#pragma once
#include "PhysicsWorld.h"
#include "BlastMesh.h"
#include "BlastVoronoi.h"
#include "BlastAsset.h"
#include "ModelRenderer.h"
#include "OutlineEffect.h"
#include <NvBlastExtAuthoring.h>
#include <NvBlastExtAuthoringBondGenerator.h>
#include <NvBlastExtAuthoringFractureTool.h>
#include <cooking/PxCooking.h>
#include <extensions/PxDefaultStreams.h>
#include <extensions/PxRigidBodyExt.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class RuntimeFractureWall
{
public:
    struct Settings
    {
        float breakingImpulse = 0.12f;
        float damageRadius = 0.65f;
        float chainRadius = 1.35f;
        unsigned int localFragments = 20;
    };

    explicit RuntimeFractureWall(PhysicsWorld& physicsWorld) : world(physicsWorld)
    {
        Material material;
        material.baseColor = glm::vec3(0.48f, 0.31f, 0.16f);
        material.specularStrength = 0.12f;
        material.shininess = 18.0f;
        wallMaterial = material;
        ModelData wall = ModelBuilder::Create(ModelType::Box);
        for (Vertex& vertex : wall.vertices) { vertex.x *= size.x; vertex.y *= size.y; vertex.z *= size.z; }
        std::uint64_t clockSeed = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::uint64_t sequenceSeed = ++wallGeneration * 0x9E3779B97F4A7C15ull;
        int32_t seed = static_cast<int32_t>((clockSeed ^ sequenceSeed) & 0x7fffffffu);
        std::vector<GeneratedChunk> initial = GenerateUniform(wall, initialPieceCount, seed);
        physx::PxCookingParams cookingParams(world.GetPhysics().getTolerancesScale());
        cookingParams.buildGPUData = world.GetCuda() != nullptr;
        pieces.reserve(initial.size());
        for (GeneratedChunk& chunk : initial)
        {
            chunk.cookedCollision = CookConvex(chunk.model, cookingParams);
            auto piece = CreatePiece(std::move(chunk.model), wallPose, 0, true, physx::PxVec3(0), physx::PxVec3(0), chunk.cookedCollision, false);
            piece->centroid = chunk.centroid;
            world.GetScene().addActor(*piece->actor);
            pieces.push_back(std::move(piece));
        }
    }

    ~RuntimeFractureWall() = default;
    RuntimeFractureWall(const RuntimeFractureWall&) = delete;
    RuntimeFractureWall& operator=(const RuntimeFractureWall&) = delete;

    bool Update(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        if (commitPlan) return CommitFracture(beforeRelease);
        if (fractureJob)
        {
            std::optional<FracturePlan> completed;
            {
                std::lock_guard<std::mutex> lock(fractureJob->mutex);
                if (!fractureJob->ready) return false;
                completed = std::move(fractureJob->plan);
            }
            fractureJob.reset();
            if (!completed) return false;
            FracturePlan plan = std::move(*completed);
            auto found = std::find_if(pieces.begin(), pieces.end(), [&](const auto& piece) { return piece->id == pending.targetId; });
            if (found == pieces.end() || plan.detached.empty()) return false;
            commitPlan.emplace(std::move(plan));
            commitIndex = 0;
            retainedCommitIndex = 0;
            retainedStaged = false;
            stagedPieces.clear();
            return CommitFracture(beforeRelease);
        }
        if (world.GetSimulationRevision() < lastFractureRevision + cooldownSteps) return false;
        Piece* target = nullptr;
        PhysicsWorld::Impact strongest;
        for (const auto& piece : pieces)
        {
            if (!CanFracture(*piece)) continue;
            PhysicsWorld::Impact impact;
            if (world.GetStrongestImpact(piece->actor, impact) && impact.impulseMagnitude > strongest.impulseMagnitude)
            {
                strongest = impact;
                target = piece.get();
            }
        }
        float normalImpactSpeed = std::abs(strongest.relativeVelocity.dot(strongest.normal));
        if (!target || strongest.impulseMagnitude < settings.breakingImpulse || normalImpactSpeed < minimumImpactSpeed) return false;
        StartFracture(*target, strongest);
        lastFractureRevision = world.GetSimulationRevision();
        return false;
    }

    void Draw(ModelRenderer& renderer, bool shadowPass) const
    {
        UpdateRenderBatch();
        if (!renderBatch) return;
        if (shadowPass) renderer.DrawShadow(*renderBatch, glm::mat4(1.0f));
        else renderer.DrawMesh(*renderBatch, glm::mat4(1.0f), wallMaterial);
    }

    void DrawOutline(OutlineEffect& outline, const Camera& camera, int width, int height, const physx::PxRigidActor* selected) const
    {
        if (!selected) return;
        for (const auto& piece : pieces) if (piece->actor == selected) outline.Draw(*piece->mesh, camera, width, height, ToMatrix(piece->actor->getGlobalPose()));
    }

    bool Resolve(const physx::PxRigidActor* actor, bool*& mesh, std::uint64_t& id)
    {
        for (const auto& piece : pieces) if (piece->actor == actor) { mesh = &piece->showMesh; id = piece->id; return true; }
        return false;
    }

    void AppendCollisionActors(std::vector<const physx::PxRigidActor*>& actors, bool all) const
    {
        for (const auto& piece : pieces) if (all || piece->showMesh) actors.push_back(piece->actor);
    }

    void SetShowCollisions(bool value)
    {
        showCollisions = value;
        for (const auto& piece : pieces) piece->showMesh = value;
    }

    std::size_t GetActorCount() const { return pieces.size(); }
    Settings GetSettings() const { return settings; }
    void SetSettings(Settings value)
    {
        if (std::isfinite(value.breakingImpulse)) settings.breakingImpulse = std::clamp(value.breakingImpulse, 0.01f, 20.0f);
        if (std::isfinite(value.damageRadius)) settings.damageRadius = std::clamp(value.damageRadius, 0.1f, 2.0f);
        if (std::isfinite(value.chainRadius)) settings.chainRadius = std::clamp(value.chainRadius, 1.0f, 3.0f);
        settings.localFragments = std::clamp(value.localFragments, 8u, 48u);
    }

private:
    struct Piece
    {
        ~Piece() { if (actor) actor->release(); }
        ModelData source;
        std::unique_ptr<Mesh> mesh;
        std::vector<physx::PxU8> cookedCollision;
        physx::PxRigidActor* actor = nullptr;
        unsigned int depth = 0;
        bool fixed = false;
        bool showMesh = false;
        std::uint64_t id = 0;
        glm::vec3 centroid{ 0.0f };
    };

    struct GeneratedChunk
    {
        ModelData model;
        glm::vec3 centroid{ 0.0f };
        std::vector<physx::PxU8> cookedCollision;
    };

    struct FracturePlan
    {
        std::vector<GeneratedChunk> detached;
        std::vector<GeneratedChunk> retained;
    };

    struct PendingFracture
    {
        std::uint64_t targetId = 0;
        glm::vec3 localHit{ 0.0f };
        physx::PxVec3 localNormal{ 0.0f,1.0f,0.0f };
        physx::PxVec3 localImpulse{ 0.0f };
        physx::PxVec3 localRelativeVelocity{ 0.0f };
        float velocityChange = 0.0f;
        float radius = 0.0f;
        float chainRadius = 0.0f;
    };

    struct AsyncFractureJob
    {
        std::mutex mutex;
        std::optional<FracturePlan> plan;
        bool ready = false;
    };

    PhysicsWorld& world;
    Settings settings;
    Material wallMaterial;
    std::vector<std::unique_ptr<Piece>> pieces;
    bool showCollisions = false;
    unsigned long long lastFractureRevision = 0;
    std::shared_ptr<AsyncFractureJob> fractureJob;
    std::optional<FracturePlan> commitPlan;
    std::vector<std::unique_ptr<Piece>> stagedPieces;
    mutable std::unique_ptr<Mesh> renderBatch;
    mutable std::vector<Vertex> renderVertices;
    mutable bool renderTopologyDirty = true;
    mutable unsigned long long renderRevision = std::numeric_limits<unsigned long long>::max();
    std::size_t commitIndex = 0;
    std::size_t retainedCommitIndex = 0;
    bool retainedStaged = false;
    PendingFracture pending;
    inline static std::uint64_t nextId = 0x4000000000000000ull;
    inline static std::uint64_t wallGeneration = 0;
    inline static const glm::vec3 size{ 16.0f,10.0f,1.0f };
    inline static const physx::PxTransform wallPose{ physx::PxVec3(0.0f,5.0f,-2.0f) };
    static constexpr unsigned int initialPieceCount = 40;
    static constexpr unsigned int maximumDepth = 2;
    static constexpr float minimumFractureExtent = 1.4f;
    static constexpr float minimumFractureVolume = 0.35f;
    static constexpr unsigned long long cooldownSteps = 4;
    static constexpr float minimumImpactSpeed = 1.0f;
    static constexpr float supportHeight = 0.08f;
    static constexpr float supportTolerance = 0.035f;
    static constexpr float unsupportedKickSpeed = 0.18f;
    static constexpr std::size_t piecesCommittedPerFrame = 8;

    physx::PxRigidStatic* CreateInitialActor(const physx::PxTransform& pose, const glm::vec3& halfExtents)
    {
        using namespace physx;
        PxRigidStatic* actor = world.GetPhysics().createRigidStatic(pose);
        if (!actor) throw std::runtime_error("Cannot create runtime fracture wall.");
        PxShape* shape = world.GetPhysics().createShape(PxBoxGeometry(halfExtents.x, halfExtents.y, halfExtents.z), world.GetMaterial(), true);
        if (!shape) { actor->release(); throw std::runtime_error("Cannot create runtime fracture wall shape."); }
        PxFilterData filter; filter.word0 = PhysicsWorld::fractureFilterTag; shape->setSimulationFilterData(filter);
        bool attached = actor->attachShape(*shape); shape->release();
        if (!attached) { actor->release(); throw std::runtime_error("Cannot attach runtime fracture wall shape."); }
        world.GetScene().addActor(*actor);
        return actor;
    }

    void StartFracture(Piece& target, const PhysicsWorld::Impact& impact)
    {
        using namespace physx;
        PxTransform pose = target.actor->getGlobalPose();
        PxVec3 localHitPx = pose.transformInv(impact.position);
        glm::vec3 localHit(localHitPx.x, localHitPx.y, localHitPx.z);
        glm::vec3 boundsCenter = Center(target.source);
        glm::vec3 towardCenter = boundsCenter - localHit;
        float towardLength = glm::length(towardCenter);
        if (towardLength > 0.0001f) localHit += towardCenter / towardLength * std::min(0.18f, towardLength * 0.25f);
        float impactEnergy = std::max(0.5f * impact.impulseMagnitude * impact.velocityChange, 0.0f);
        float energyScale = std::sqrt(impactEnergy);
        float radius = std::clamp(settings.damageRadius + energyScale * 0.02f, settings.damageRadius, 2.0f);
        float minimumCoreRadius = std::min(0.18f, radius * 0.2f);
        float maximumCoreRadius = std::min(0.55f, std::max(minimumCoreRadius, radius * 0.45f));
        float coreRadius = std::clamp(0.18f + energyScale * 0.002f, minimumCoreRadius, maximumCoreRadius);
        unsigned int coreSites = std::clamp(settings.localFragments + static_cast<unsigned int>(std::sqrt(std::max(impact.velocityChange, 0.0f)) * 1.5f), 8u, 48u);
        unsigned int outerSites = std::clamp(5u + static_cast<unsigned int>(energyScale * 0.12f), 5u, 16u);
        unsigned int guardSites = target.depth == 0 ? 12 : target.depth == 1 ? 6 : 3;
        ModelData source = target.source;
        bool fixed = target.fixed;
        int32_t seed = static_cast<int32_t>(world.GetSimulationRevision() + target.id);
        PxCookingParams cookingParams(world.GetPhysics().getTolerancesScale());
        cookingParams.buildGPUData = world.GetCuda() != nullptr;
        pending.targetId = target.id;
        pending.localHit = localHit;
        pending.localNormal = pose.q.rotateInv(impact.normal);
        pending.localImpulse = pose.q.rotateInv(impact.impulse);
        pending.localRelativeVelocity = pose.q.rotateInv(impact.relativeVelocity);
        pending.velocityChange = impact.velocityChange;
        pending.radius = radius;
        pending.chainRadius = radius * settings.chainRadius;
        auto job = std::make_shared<AsyncFractureJob>();
        fractureJob = job;
        std::thread([job, source = std::move(source), localHit, radius, coreRadius, coreSites, outerSites, guardSites, seed, fixed, cookingParams]() mutable
            {
                std::optional<FracturePlan> result;
                try { result = GeneratePartial(source, localHit, radius, coreRadius, coreSites, outerSites, guardSites, seed, fixed, cookingParams); }
                catch (...) {}
                std::lock_guard<std::mutex> lock(job->mutex);
                job->plan = std::move(result);
                job->ready = true;
            }).detach();
    }

    bool CommitFracture(const std::function<void(const physx::PxRigidActor*)>& beforeRelease)
    {
        using namespace physx;
        auto found = std::find_if(pieces.begin(), pieces.end(), [&](const auto& piece) { return piece->id == pending.targetId; });
        if (found == pieces.end()) { commitPlan.reset(); stagedPieces.clear(); return false; }
        Piece& target = **found;
        FracturePlan& plan = *commitPlan;
        PxTransform pose = target.actor->getGlobalPose();
        PxVec3 inheritedLinear(0), inheritedAngular(0);
        if (auto* dynamic = target.actor->is<PxRigidDynamic>()) { inheritedLinear = dynamic->getLinearVelocity(); inheritedAngular = dynamic->getAngularVelocity(); }
        std::size_t budget = piecesCommittedPerFrame;
        if (!retainedStaged)
        {
            while (budget > 0 && retainedCommitIndex < plan.retained.size())
            {
                GeneratedChunk& chunk = plan.retained[retainedCommitIndex++];
                auto piece = CreatePiece(std::move(chunk.model), pose, target.depth, true, inheritedLinear, inheritedAngular, chunk.cookedCollision, false);
                piece->centroid = chunk.centroid;
                stagedPieces.push_back(std::move(piece));
                --budget;
            }
            if (retainedCommitIndex < plan.retained.size()) return false;
            retainedStaged = true;
        }
        while (budget > 0 && commitIndex < plan.detached.size())
        {
            GeneratedChunk& chunk = plan.detached[commitIndex++];
            auto piece = CreatePiece(std::move(chunk.model), pose, target.depth + 1, false, inheritedLinear, inheritedAngular, chunk.cookedCollision, false);
            piece->centroid = chunk.centroid;
            stagedPieces.push_back(std::move(piece));
            --budget;
        }
        if (commitIndex < plan.detached.size()) return false;
        pose = target.actor->getGlobalPose();
        if (auto* dynamic = target.actor->is<PxRigidDynamic>()) { inheritedLinear = dynamic->getLinearVelocity(); inheritedAngular = dynamic->getAngularVelocity(); }
        PxVec3 impactPosition = pose.transform(PxVec3(pending.localHit.x, pending.localHit.y, pending.localHit.z));
        PxVec3 totalImpulse = pose.q.rotate(pending.localImpulse);
        PxVec3 relativeVelocity = pose.q.rotate(pending.localRelativeVelocity);
        float totalMass = 0.0f, totalWeight = 0.0f;
        for (const auto& piece : stagedPieces)
        {
            piece->actor->setGlobalPose(pose);
            world.GetScene().addActor(*piece->actor);
            if (piece->fixed) continue;
            auto* dynamic = static_cast<PxRigidDynamic*>(piece->actor);
            dynamic->setLinearVelocity(inheritedLinear); dynamic->setAngularVelocity(inheritedAngular);
            PxVec3 worldCenter = pose.transform(PxVec3(piece->centroid.x, piece->centroid.y, piece->centroid.z));
            float distance = (worldCenter - impactPosition).magnitude();
            float proximity = std::clamp(1.0f - distance / std::max(pending.radius * 1.25f, 0.01f), 0.0f, 1.0f);
            float mass = dynamic->getMass();
            totalMass += mass;
            totalWeight += mass * (0.1f + 0.9f * proximity * proximity);
        }
        float impulseMagnitude = totalImpulse.magnitude();
        if (impulseMagnitude <= 0.0001f && relativeVelocity.normalize() > 0.0001f) totalImpulse = -relativeVelocity * totalMass * std::min(pending.velocityChange * 0.2f, 4.0f);
        impulseMagnitude = totalImpulse.magnitude();
        float maximumImpulse = totalMass * std::clamp(pending.velocityChange, 0.0f, 25.0f);
        if (impulseMagnitude > maximumImpulse && impulseMagnitude > 0.0001f) totalImpulse *= maximumImpulse / impulseMagnitude;
        float transfer = target.fixed ? 0.75f : 0.25f;
        for (const auto& piece : stagedPieces)
        {
            if (piece->fixed) continue;
            auto* dynamic = static_cast<PxRigidDynamic*>(piece->actor);
            PxVec3 worldCenter = pose.transform(PxVec3(piece->centroid.x, piece->centroid.y, piece->centroid.z));
            PxVec3 radial = worldCenter - impactPosition;
            float distance = radial.magnitude();
            if (radial.normalize() <= 0.0001f) radial = -pose.q.rotate(pending.localNormal);
            float proximity = std::clamp(1.0f - distance / std::max(pending.radius * 1.25f, 0.01f), 0.0f, 1.0f);
            float mass = dynamic->getMass();
            float weight = mass * (0.1f + 0.9f * proximity * proximity);
            PxVec3 transferredImpulse = totalWeight > 0.0001f ? totalImpulse * (transfer * weight / totalWeight) : PxVec3(0.0f);
            PxVec3 separationImpulse = radial * mass * proximity * std::min(pending.velocityChange * 0.035f, 0.65f);
            PxVec3 forcePoint = impactPosition + (worldCenter - impactPosition) * 0.65f;
            PxRigidBodyExt::addForceAtPos(*dynamic, transferredImpulse + separationImpulse, forcePoint, PxForceMode::eIMPULSE);
        }
        if (beforeRelease) beforeRelease(target.actor);
        pieces.erase(found);
        for (auto& piece : stagedPieces) pieces.push_back(std::move(piece));
        renderTopologyDirty = true;
        ReleaseChain(impactPosition, beforeRelease);
        ReleaseUnsupported(impactPosition, beforeRelease);
        stagedPieces.clear(); commitPlan.reset();
        lastFractureRevision = world.GetSimulationRevision();
        return true;
    }

    void UpdateRenderBatch() const
    {
        unsigned long long revision = world.GetSimulationRevision();
        if (!renderTopologyDirty && renderRevision == revision) return;
        if (renderTopologyDirty)
        {
            std::size_t vertexCount = 0, indexCount = 0;
            for (const auto& piece : pieces) { vertexCount += piece->source.vertices.size(); indexCount += piece->source.indices.size(); }
            if (vertexCount == 0 || indexCount == 0) { renderBatch.reset(); renderVertices.clear(); renderTopologyDirty = false; renderRevision = revision; return; }
            if (vertexCount > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) throw std::length_error("Runtime fracture render batch has too many vertices.");
            renderVertices.clear(); renderVertices.reserve(vertexCount);
            std::vector<unsigned int> indices; indices.reserve(indexCount);
            unsigned int baseVertex = 0;
            for (const auto& piece : pieces)
            {
                renderVertices.insert(renderVertices.end(), piece->source.vertices.begin(), piece->source.vertices.end());
                for (unsigned int index : piece->source.indices) indices.push_back(baseVertex + index);
                baseVertex += static_cast<unsigned int>(piece->source.vertices.size());
            }
            renderBatch = std::make_unique<Mesh>(renderVertices, indices);
            renderTopologyDirty = false;
        }
        std::size_t output = 0;
        for (const auto& piece : pieces)
        {
            physx::PxTransform pose = piece->actor->getGlobalPose();
            for (const Vertex& source : piece->source.vertices)
            {
                Vertex& vertex = renderVertices[output++];
                physx::PxVec3 position = pose.transform(physx::PxVec3(source.x, source.y, source.z));
                physx::PxVec3 normal = pose.q.rotate(physx::PxVec3(source.nx, source.ny, source.nz));
                if (normal.normalize() <= 0.000001f) normal = physx::PxVec3(0.0f, 1.0f, 0.0f);
                vertex = source;
                vertex.x = position.x; vertex.y = position.y; vertex.z = position.z;
                vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
            }
        }
        renderBatch->UpdateVertices(renderVertices);
        renderRevision = revision;
    }

    std::unique_ptr<Piece> CreatePiece(ModelData&& model, const physx::PxTransform& pose, unsigned int depth, bool fixed, const physx::PxVec3& linearVelocity, const physx::PxVec3& angularVelocity, const std::vector<physx::PxU8>& cookedCollision, bool triangleCollision)
    {
        using namespace physx;
        if (cookedCollision.empty()) throw std::runtime_error("Runtime fracture collision data is empty.");
        auto piece = std::make_unique<Piece>();
        piece->source = std::move(model);
        piece->mesh = std::make_unique<Mesh>(piece->source.vertices, piece->source.indices);
        piece->cookedCollision = cookedCollision;
        piece->depth = depth; piece->fixed = fixed; piece->showMesh = showCollisions; piece->id = ++nextId;
        if (fixed) piece->actor = world.GetPhysics().createRigidStatic(pose);
        else piece->actor = world.GetPhysics().createRigidDynamic(pose);
        if (!piece->actor) throw std::runtime_error("Cannot create runtime fracture piece actor.");
        PxDefaultMemoryInputData input(cookedCollision.data(), static_cast<PxU32>(cookedCollision.size()));
        PxShape* shape = nullptr;
        if (triangleCollision)
        {
            PxTriangleMesh* collision = world.GetPhysics().createTriangleMesh(input);
            if (!collision) throw std::runtime_error("Cannot load runtime fracture triangle mesh.");
            PxTriangleMeshGeometry geometry(collision);
            shape = world.GetPhysics().createShape(geometry, world.GetMaterial(), true);
            collision->release();
        }
        else
        {
            PxConvexMesh* collision = world.GetPhysics().createConvexMesh(input);
            if (!collision) throw std::runtime_error("Cannot load runtime fracture convex mesh.");
            PxConvexMeshGeometry geometry(collision, PxMeshScale(), PxConvexMeshGeometryFlag::eTIGHT_BOUNDS);
            shape = world.GetPhysics().createShape(geometry, world.GetMaterial(), true);
            collision->release();
        }
        if (!shape) throw std::runtime_error("Cannot create runtime fracture piece shape.");
        PxFilterData filter; filter.word0 = PhysicsWorld::fractureFilterTag; shape->setSimulationFilterData(filter);
        bool attached = piece->actor->attachShape(*shape); shape->release();
        if (!attached) throw std::runtime_error("Cannot attach runtime fracture piece shape.");
        if (!fixed)
        {
            auto* dynamic = static_cast<PxRigidDynamic*>(piece->actor);
            if (!PxRigidBodyExt::updateMassAndInertia(*dynamic, 30.0f)) throw std::runtime_error("Cannot compute runtime fracture piece mass.");
            dynamic->setLinearDamping(0.08f); dynamic->setAngularDamping(0.15f);
            dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
            dynamic->setLinearVelocity(linearVelocity); dynamic->setAngularVelocity(angularVelocity);
        }
        return piece;
    }

    void ReleaseChain(const physx::PxVec3& impactPosition, const std::function<void(const physx::PxRigidActor*)>& beforeRelease)
    {
        using namespace physx;
        for (const auto& holder : pieces)
        {
            Piece& piece = *holder;
            if (!piece.fixed || piece.cookedCollision.empty()) continue;
            PxTransform pose = piece.actor->getGlobalPose();
            PxVec3 center = pose.transform(PxVec3(piece.centroid.x, piece.centroid.y, piece.centroid.z));
            PxVec3 radial = center - impactPosition;
            float distance = radial.magnitude();
            if (distance > pending.chainRadius) continue;
            if (radial.normalize() <= 0.0001f) radial = -pose.q.rotate(pending.localNormal);
            float proximity = std::clamp(1.0f - distance / std::max(pending.chainRadius, 0.01f), 0.0f, 1.0f);
            float kickSpeed = std::clamp(pending.velocityChange * 0.02f, 0.25f, 2.0f);
            MakeDynamic(piece, radial * kickSpeed * proximity, impactPosition, beforeRelease);
        }
    }

    bool MakeDynamic(Piece& piece, const physx::PxVec3& kickVelocity, const physx::PxVec3& forcePoint, const std::function<void(const physx::PxRigidActor*)>& beforeRelease)
    {
        using namespace physx;
        if (!piece.fixed || !piece.actor) return false;
        PxShape* sourceShape = nullptr;
        if (piece.actor->getShapes(&sourceShape, 1) != 1 || !sourceShape) return false;
        PxRigidDynamic* dynamic = world.GetPhysics().createRigidDynamic(piece.actor->getGlobalPose());
        if (!dynamic) return false;
        PxShape* shape = world.GetPhysics().createShape(sourceShape->getGeometry(), world.GetMaterial(), true);
        if (!shape) { dynamic->release(); return false; }
        shape->setLocalPose(sourceShape->getLocalPose());
        PxFilterData filter; filter.word0 = PhysicsWorld::fractureFilterTag; shape->setSimulationFilterData(filter);
        bool attached = dynamic->attachShape(*shape); shape->release();
        if (!attached || !PxRigidBodyExt::updateMassAndInertia(*dynamic, 30.0f)) { dynamic->release(); return false; }
        dynamic->setLinearDamping(0.08f); dynamic->setAngularDamping(0.15f);
        dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
        world.GetScene().addActor(*dynamic);
        if (kickVelocity.magnitudeSquared() > 0.000001f) PxRigidBodyExt::addForceAtPos(*dynamic, kickVelocity * dynamic->getMass(), forcePoint, PxForceMode::eIMPULSE);
        if (beforeRelease) beforeRelease(piece.actor);
        piece.actor->release();
        piece.actor = dynamic;
        piece.fixed = false;
        return true;
    }

    static bool CanFracture(const Piece& piece)
    {
        if (piece.depth >= maximumDepth || piece.source.vertices.size() < 12 || piece.source.indices.size() < 12) return false;
        glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
        for (const Vertex& vertex : piece.source.vertices)
        {
            glm::vec3 point(vertex.x, vertex.y, vertex.z);
            minimum = glm::min(minimum, point);
            maximum = glm::max(maximum, point);
        }
        glm::vec3 extent = maximum - minimum;
        float volume = extent.x * extent.y * extent.z;
        return std::isfinite(extent.x) && std::isfinite(extent.y) && std::isfinite(extent.z) && std::isfinite(volume) &&
            glm::length(extent) >= minimumFractureExtent && volume >= minimumFractureVolume;
    }

    void ReleaseUnsupported(const physx::PxVec3& impactPosition, const std::function<void(const physx::PxRigidActor*)>& beforeRelease)
    {
        using namespace physx;
        std::vector<Piece*> fixed;
        std::vector<PxBounds3> bounds;
        for (const auto& holder : pieces) if (holder->fixed)
        {
            fixed.push_back(holder.get());
            bounds.push_back(holder->actor->getWorldBounds(1.0f));
        }
        std::vector<bool> supported(fixed.size(), false);
        std::queue<std::size_t> pendingSupport;
        for (std::size_t index = 0; index < fixed.size(); ++index) if (bounds[index].minimum.y <= supportHeight)
        {
            supported[index] = true;
            pendingSupport.push(index);
        }
        while (!pendingSupport.empty())
        {
            std::size_t current = pendingSupport.front(); pendingSupport.pop();
            for (std::size_t candidate = 0; candidate < fixed.size(); ++candidate)
            {
                if (supported[candidate] || !BoundsTouch(bounds[current], bounds[candidate])) continue;
                supported[candidate] = true;
                pendingSupport.push(candidate);
            }
        }
        for (std::size_t index = 0; index < fixed.size(); ++index) if (!supported[index])
        {
            PxVec3 center = bounds[index].getCenter();
            PxVec3 radial = center - impactPosition;
            if (radial.normalize() <= 0.0001f) radial = PxVec3(0.0f, 0.0f, 1.0f);
            MakeDynamic(*fixed[index], radial * unsupportedKickSpeed, impactPosition, beforeRelease);
        }
    }

    static bool BoundsTouch(const physx::PxBounds3& a, const physx::PxBounds3& b)
    {
        return a.minimum.x <= b.maximum.x + supportTolerance && a.maximum.x + supportTolerance >= b.minimum.x &&
            a.minimum.y <= b.maximum.y + supportTolerance && a.maximum.y + supportTolerance >= b.minimum.y &&
            a.minimum.z <= b.maximum.z + supportTolerance && a.maximum.z + supportTolerance >= b.minimum.z;
    }

    static std::vector<GeneratedChunk> Generate(const ModelData& source, glm::vec3 hit, float radius, float coreRadius, unsigned int coreSites, unsigned int outerSites, unsigned int guardSites, int32_t seed)
    {
        BlastMesh mesh(source);
        Nv::Blast::FractureTool* tool = NvBlastExtAuthoringCreateFractureTool();
        if (!tool) throw std::runtime_error("Cannot create runtime fracture tool.");
        Nv::Blast::VoronoiSitesGenerator* generator = nullptr;
        Nv::Blast::BlastBondGenerator* bonds = nullptr;
        Nv::Blast::AuthoringResult* result = nullptr;
        BlastNullCollisionBuilder collisionBuilder;
        try
        {
            const Nv::Blast::Mesh* input = &mesh.Get();
            int32_t root = 0;
            if (!tool->setSourceMeshes(&input, 1, &root)) throw std::runtime_error("Cannot set runtime fracture source mesh.");
            BlastRandom random(seed);
            generator = NvBlastExtAuthoringCreateVoronoiSitesGenerator(&mesh.Get(), &random);
            if (!generator) throw std::runtime_error("Cannot create runtime fracture site generator.");
            generator->uniformlyGenerateSitesInMesh(guardSites);
            generator->deleteInSphere(radius, { hit.x,hit.y,hit.z });
            generator->generateInSphere(outerSites, radius, { hit.x,hit.y,hit.z });
            generator->deleteInSphere(coreRadius, { hit.x,hit.y,hit.z });
            generator->generateInSphere(coreSites, coreRadius, { hit.x,hit.y,hit.z });
            const NvcVec3* sites = nullptr;
            uint32_t siteCount = generator->getVoronoiSites(sites);
            if (!sites || siteCount < 2) throw std::runtime_error("Runtime fracture did not generate enough sites.");
            if (tool->voronoiFracturing(0, siteCount, sites, false) != 0) throw std::runtime_error("Runtime Voronoi fracture failed.");
            tool->finalizeFracturing();
            bonds = NvBlastExtAuthoringCreateBondGenerator(&collisionBuilder);
            if (!bonds) throw std::runtime_error("Cannot create runtime fracture bond generator.");
            Nv::Blast::ConvexDecompositionParams collisionParams{}; collisionParams.maximumNumberOfHulls = 1;
            result = NvBlastExtAuthoringProcessFracture(*tool, *bonds, collisionBuilder, collisionParams, -1);
            if (!result || result->chunkCount < 2) throw std::runtime_error("Runtime fracture produced no chunks.");
            std::vector<bool> hasChildren(result->chunkCount, false);
            for (uint32_t chunk = 0; chunk < result->chunkCount; ++chunk)
            {
                uint32_t parent = result->chunkDescs[chunk].parentChunkDescIndex;
                if (parent < result->chunkCount) hasChildren[parent] = true;
            }
            std::vector<GeneratedChunk> chunks;
            chunks.reserve(result->chunkCount);
            for (uint32_t chunk = 0; chunk < result->chunkCount; ++chunk)
            {
                if (hasChildren[chunk] || result->geometryOffset[chunk] == result->geometryOffset[chunk + 1]) continue;
                chunks.push_back(ConvertChunk(*result, chunk));
            }
            NvBlastExtAuthoringReleaseAuthoringResult(collisionBuilder, result); result = nullptr;
            bonds->release(); bonds = nullptr; generator->release(); generator = nullptr; tool->release();
            return chunks;
        }
        catch (...)
        {
            if (result) NvBlastExtAuthoringReleaseAuthoringResult(collisionBuilder, result);
            if (bonds) bonds->release();
            if (generator) generator->release();
            tool->release();
            throw;
        }
    }

    static std::vector<GeneratedChunk> GenerateUniform(const ModelData& source, unsigned int siteCount, int32_t seed)
    {
        BlastMesh mesh(source);
        Nv::Blast::FractureTool* tool = NvBlastExtAuthoringCreateFractureTool();
        if (!tool) throw std::runtime_error("Cannot create initial wall fracture tool.");
        Nv::Blast::VoronoiSitesGenerator* generator = nullptr;
        Nv::Blast::BlastBondGenerator* bonds = nullptr;
        Nv::Blast::AuthoringResult* result = nullptr;
        BlastNullCollisionBuilder collisionBuilder;
        try
        {
            const Nv::Blast::Mesh* input = &mesh.Get();
            int32_t root = 0;
            if (!tool->setSourceMeshes(&input, 1, &root)) throw std::runtime_error("Cannot set initial wall source mesh.");
            BlastRandom random(seed);
            generator = NvBlastExtAuthoringCreateVoronoiSitesGenerator(&mesh.Get(), &random);
            if (!generator) throw std::runtime_error("Cannot create initial wall Voronoi generator.");
            generator->uniformlyGenerateSitesInMesh(siteCount);
            const NvcVec3* sites = nullptr;
            uint32_t generatedCount = generator->getVoronoiSites(sites);
            if (!sites || generatedCount < 2) throw std::runtime_error("Cannot generate initial wall Voronoi sites.");
            if (tool->voronoiFracturing(0, generatedCount, sites, false) != 0) throw std::runtime_error("Initial wall Voronoi fracture failed.");
            tool->finalizeFracturing();
            bonds = NvBlastExtAuthoringCreateBondGenerator(&collisionBuilder);
            if (!bonds) throw std::runtime_error("Cannot create initial wall bond generator.");
            Nv::Blast::ConvexDecompositionParams collisionParams{}; collisionParams.maximumNumberOfHulls = 1;
            result = NvBlastExtAuthoringProcessFracture(*tool, *bonds, collisionBuilder, collisionParams, -1);
            if (!result || result->chunkCount < 2) throw std::runtime_error("Initial wall fracture produced no chunks.");
            std::vector<bool> hasChildren(result->chunkCount, false);
            for (uint32_t chunk = 0; chunk < result->chunkCount; ++chunk)
            {
                uint32_t parent = result->chunkDescs[chunk].parentChunkDescIndex;
                if (parent < result->chunkCount) hasChildren[parent] = true;
            }
            std::vector<GeneratedChunk> chunks;
            chunks.reserve(result->chunkCount);
            for (uint32_t chunk = 0; chunk < result->chunkCount; ++chunk)
                if (!hasChildren[chunk] && result->geometryOffset[chunk] != result->geometryOffset[chunk + 1]) chunks.push_back(ConvertChunk(*result, chunk));
            NvBlastExtAuthoringReleaseAuthoringResult(collisionBuilder, result); result = nullptr;
            bonds->release(); bonds = nullptr; generator->release(); generator = nullptr; tool->release();
            return chunks;
        }
        catch (...)
        {
            if (result) NvBlastExtAuthoringReleaseAuthoringResult(collisionBuilder, result);
            if (bonds) bonds->release();
            if (generator) generator->release();
            tool->release();
            throw;
        }
    }

    static FracturePlan GeneratePartial(const ModelData& source, glm::vec3 hit, float radius, float coreRadius, unsigned int coreSites, unsigned int outerSites, unsigned int guardSites, int32_t seed, bool retainExterior, const physx::PxCookingParams& cookingParams)
    {
        std::vector<GeneratedChunk> generated = Generate(source, hit, radius, coreRadius, coreSites, outerSites, guardSites, seed);
        FracturePlan plan;
        plan.detached.reserve(generated.size());
        plan.retained.reserve(generated.size());
        for (GeneratedChunk& chunk : generated)
        {
            bool local = !retainExterior || glm::distance(chunk.centroid, hit) <= radius;
            if (local) plan.detached.push_back(std::move(chunk));
            else plan.retained.push_back(std::move(chunk));
        }
        if (plan.detached.empty() && !plan.retained.empty())
        {
            auto nearest = std::min_element(plan.retained.begin(), plan.retained.end(), [&](const GeneratedChunk& a, const GeneratedChunk& b)
                {
                    return glm::distance(a.centroid, hit) < glm::distance(b.centroid, hit);
                });
            plan.detached.push_back(std::move(*nearest));
            plan.retained.erase(nearest);
        }
        for (GeneratedChunk& chunk : plan.detached) chunk.cookedCollision = CookConvex(chunk.model, cookingParams);
        for (GeneratedChunk& chunk : plan.retained) chunk.cookedCollision = CookConvex(chunk.model, cookingParams);
        return plan;
    }

    static std::vector<physx::PxU8> CookConvex(const ModelData& model, const physx::PxCookingParams& cookingParams)
    {
        using namespace physx;
        std::vector<PxVec3> points;
        points.reserve(model.vertices.size());
        for (const Vertex& vertex : model.vertices) points.emplace_back(vertex.x, vertex.y, vertex.z);
        PxConvexMeshDesc description;
        description.points.count = static_cast<PxU32>(points.size());
        description.points.stride = sizeof(PxVec3);
        description.points.data = points.data();
        description.flags = PxConvexFlag::eCOMPUTE_CONVEX;
        PxDefaultMemoryOutputStream stream;
        if (!PxCookConvexMesh(cookingParams, description, stream)) throw std::runtime_error("Cannot cook runtime fracture convex mesh.");
        return { stream.getData(),stream.getData() + static_cast<std::size_t>(stream.getSize()) };
    }

    static std::vector<physx::PxU8> CookTriangle(const ModelData& model, const physx::PxCookingParams& cookingParams)
    {
        using namespace physx;
        PxTriangleMeshDesc description;
        description.points.count = static_cast<PxU32>(model.vertices.size());
        description.points.stride = sizeof(Vertex);
        description.points.data = model.vertices.data();
        description.triangles.count = static_cast<PxU32>(model.indices.size() / 3);
        description.triangles.stride = sizeof(unsigned int) * 3;
        description.triangles.data = model.indices.data();
        PxDefaultMemoryOutputStream stream;
        if (!PxCookTriangleMesh(cookingParams, description, stream)) throw std::runtime_error("Cannot cook runtime fracture triangle mesh.");
        return { stream.getData(),stream.getData() + static_cast<std::size_t>(stream.getSize()) };
    }

    static GeneratedChunk ConvertChunk(Nv::Blast::AuthoringResult& authored, uint32_t chunk)
    {
        GeneratedChunk output;
        uint32_t first = authored.geometryOffset[chunk], end = authored.geometryOffset[chunk + 1];
        output.model.vertices.reserve(static_cast<size_t>(end - first) * 3);
        output.model.indices.reserve(static_cast<size_t>(end - first) * 3);
        glm::vec3 sum(0.0f); std::size_t count = 0;
        for (uint32_t triangleIndex = first; triangleIndex < end; ++triangleIndex)
        {
            const Nv::Blast::Triangle& triangle = authored.geometry[triangleIndex];
            glm::vec3 a(triangle.a.p.x, triangle.a.p.y, triangle.a.p.z), b(triangle.b.p.x, triangle.b.p.y, triangle.b.p.z), c(triangle.c.p.x, triangle.c.p.y, triangle.c.p.z);
            glm::vec3 normal = glm::cross(b - a, c - a);
            float length = glm::length(normal); normal = length > 0.000001f ? normal / length : glm::vec3(0, 1, 0);
            AddVertex(output.model, triangle.a, normal); AddVertex(output.model, triangle.b, normal); AddVertex(output.model, triangle.c, normal);
            sum += a + b + c; count += 3;
        }
        if (count) output.centroid = sum / static_cast<float>(count);
        return output;
    }

    static void AddVertex(ModelData& model, const Nv::Blast::Vertex& source, glm::vec3 normal)
    {
        model.indices.push_back(static_cast<unsigned int>(model.vertices.size()));
        model.vertices.push_back({ source.p.x,source.p.y,source.p.z,1,1,1,normal.x,normal.y,normal.z,source.uv[0].x,source.uv[0].y });
    }

    static glm::vec3 Center(const ModelData& model)
    {
        glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
        for (const Vertex& vertex : model.vertices) { glm::vec3 p(vertex.x, vertex.y, vertex.z); minimum = glm::min(minimum, p); maximum = glm::max(maximum, p); }
        return (minimum + maximum) * 0.5f;
    }

    static float DistanceToBounds(const ModelData& model, glm::vec3 point)
    {
        glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
        for (const Vertex& vertex : model.vertices) { glm::vec3 p(vertex.x, vertex.y, vertex.z); minimum = glm::min(minimum, p); maximum = glm::max(maximum, p); }
        glm::vec3 offset = glm::max(glm::max(minimum - point, point - maximum), glm::vec3(0.0f));
        return glm::length(offset);
    }

    static glm::mat4 ToMatrix(const physx::PxTransform& pose)
    {
        physx::PxMat44 source(pose); glm::mat4 result(1.0f);
        for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) result[column][row] = source[column][row];
        return result;
    }
};

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
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

class RuntimeFractureWall
{
public:
    explicit RuntimeFractureWall(PhysicsWorld& physicsWorld) : world(physicsWorld)
    {
        Material material;
        material.baseColor = glm::vec3(0.48f, 0.31f, 0.16f);
        material.specularStrength = 0.12f;
        material.shininess = 18.0f;
        wallMaterial = material;
        ModelData model = ModelBuilder::Create(ModelType::Box);
        for (Vertex& vertex : model.vertices) { vertex.x *= size.x; vertex.y *= size.y; vertex.z *= size.z; }
        auto piece = std::make_unique<Piece>();
        piece->source = std::move(model);
        piece->mesh = std::make_unique<Mesh>(piece->source.vertices, piece->source.indices);
        piece->actor = CreateInitialActor();
        piece->fixed = true;
        piece->id = ++nextId;
        pieces.push_back(std::move(piece));
    }

    ~RuntimeFractureWall() = default;
    RuntimeFractureWall(const RuntimeFractureWall&) = delete;
    RuntimeFractureWall& operator=(const RuntimeFractureWall&) = delete;

    bool Update(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        if (commitPlan) return CommitFracture(beforeRelease);
        if (jobRunning)
        {
            if (fractureJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
            FracturePlan plan;
            try { plan = fractureJob.get(); }
            catch (...) { jobRunning = false; return false; }
            jobRunning = false;
            auto found = std::find_if(pieces.begin(), pieces.end(), [&](const auto& piece) { return piece->id == pending.targetId; });
            if (found == pieces.end() || plan.detached.empty()) return false;
            commitPlan.emplace(std::move(plan));
            commitIndex = 0;
            retainedStaged = false;
            stagedPieces.clear();
            return CommitFracture(beforeRelease);
        }
        if (pieces.size() >= maximumPieces || world.GetSimulationRevision() < lastFractureRevision + cooldownSteps) return false;
        Piece* target = nullptr;
        PhysicsWorld::Impact strongest;
        for (const auto& piece : pieces)
        {
            if (piece->depth >= maximumDepth) continue;
            PhysicsWorld::Impact impact;
            if (world.GetStrongestImpact(piece->actor, impact) && impact.velocityChange > strongest.velocityChange)
            {
                strongest = impact;
                target = piece.get();
            }
        }
        if (!target || strongest.velocityChange < minimumImpactSpeed) return false;
        StartFracture(*target, strongest);
        lastFractureRevision = world.GetSimulationRevision();
        return false;
    }

    void Draw(ModelRenderer& renderer, bool shadowPass) const
    {
        for (const auto& piece : pieces)
        {
            glm::mat4 matrix = ToMatrix(piece->actor->getGlobalPose());
            if (shadowPass) renderer.DrawShadow(*piece->mesh, matrix);
            else renderer.DrawMesh(*piece->mesh, matrix, wallMaterial);
        }
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

private:
    struct Piece
    {
        ~Piece() { if (actor) actor->release(); }
        ModelData source;
        std::unique_ptr<Mesh> mesh;
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
        std::vector<physx::PxU8> retainedCollision;
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
    };

    PhysicsWorld& world;
    Material wallMaterial;
    std::vector<std::unique_ptr<Piece>> pieces;
    bool showCollisions = false;
    unsigned long long lastFractureRevision = 0;
    std::future<FracturePlan> fractureJob;
    std::optional<FracturePlan> commitPlan;
    std::vector<std::unique_ptr<Piece>> stagedPieces;
    std::size_t commitIndex = 0;
    bool retainedStaged = false;
    PendingFracture pending;
    bool jobRunning = false;
    inline static std::uint64_t nextId = 0x4000000000000000ull;
    inline static const glm::vec3 size{ 16.0f,10.0f,1.0f };
    inline static const physx::PxTransform wallPose{ physx::PxVec3(0.0f,5.0f,-2.0f) };
    static constexpr std::size_t maximumPieces = 500;
    static constexpr unsigned int maximumDepth = 3;
    static constexpr unsigned long long cooldownSteps = 4;
    static constexpr float minimumImpactSpeed = 1.0f;
    static constexpr std::size_t piecesCommittedPerFrame = 8;

    physx::PxRigidStatic* CreateInitialActor()
    {
        using namespace physx;
        PxRigidStatic* actor = world.GetPhysics().createRigidStatic(wallPose);
        if (!actor) throw std::runtime_error("Cannot create runtime fracture wall.");
        PxShape* shape = world.GetPhysics().createShape(PxBoxGeometry(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f), world.GetMaterial(), true);
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
        float radius = std::clamp(0.55f + impact.velocityChange * 0.055f, 0.7f, 1.65f);
        unsigned int localSites = target.depth == 0 ? 48 : target.depth == 1 ? 24 : 12;
        unsigned int guardSites = target.depth == 0 ? 12 : target.depth == 1 ? 6 : 3;
        ModelData source = target.source;
        bool fixed = target.fixed;
        std::size_t available = maximumPieces - (pieces.size() - 1);
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
        fractureJob = std::async(std::launch::async, [source = std::move(source), localHit, radius, localSites, guardSites, seed, fixed, available, cookingParams]() mutable
            {
                return GeneratePartial(source, localHit, radius, localSites, guardSites, seed, fixed, available, cookingParams);
            });
        jobRunning = true;
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
            retainedStaged = true;
            if (!plan.retained.empty())
            {
                ModelData retainedModel = MergeChunks(plan.retained);
                stagedPieces.push_back(CreatePiece(std::move(retainedModel), pose, target.depth, true, inheritedLinear, inheritedAngular, plan.retainedCollision, true));
                --budget;
            }
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
        stagedPieces.clear(); commitPlan.reset();
        lastFractureRevision = world.GetSimulationRevision();
        return true;
    }

    std::unique_ptr<Piece> CreatePiece(ModelData&& model, const physx::PxTransform& pose, unsigned int depth, bool fixed, const physx::PxVec3& linearVelocity, const physx::PxVec3& angularVelocity, const std::vector<physx::PxU8>& cookedCollision, bool triangleCollision)
    {
        using namespace physx;
        if (cookedCollision.empty()) throw std::runtime_error("Runtime fracture collision data is empty.");
        auto piece = std::make_unique<Piece>();
        piece->source = std::move(model);
        piece->mesh = std::make_unique<Mesh>(piece->source.vertices, piece->source.indices);
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

    static std::vector<GeneratedChunk> Generate(const ModelData& source, glm::vec3 hit, float radius, unsigned int localSites, unsigned int guardSites, int32_t seed)
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
            generator->generateInSphere(localSites, radius, { hit.x,hit.y,hit.z });
            generator->uniformlyGenerateSitesInMesh(guardSites);
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

    static FracturePlan GeneratePartial(const ModelData& source, glm::vec3 hit, float radius, unsigned int localSites, unsigned int guardSites, int32_t seed, bool retainExterior, std::size_t available, const physx::PxCookingParams& cookingParams)
    {
        std::vector<GeneratedChunk> generated = Generate(source, hit, radius, localSites, guardSites, seed);
        FracturePlan plan;
        plan.detached.reserve(generated.size());
        plan.retained.reserve(generated.size());
        for (GeneratedChunk& chunk : generated)
        {
            bool local = !retainExterior || DistanceToBounds(chunk.model, hit) <= radius * 1.2f;
            if (local) plan.detached.push_back(std::move(chunk));
            else plan.retained.push_back(std::move(chunk));
        }
        std::size_t retainedCount = plan.retained.empty() ? 0 : 1;
        std::size_t keep = available > retainedCount ? available - retainedCount : 0;
        if (plan.detached.size() > keep)
        {
            if (retainExterior) for (std::size_t index = keep; index < plan.detached.size(); ++index) plan.retained.push_back(std::move(plan.detached[index]));
            plan.detached.resize(keep);
        }
        for (GeneratedChunk& chunk : plan.detached) chunk.cookedCollision = CookConvex(chunk.model, cookingParams);
        if (!plan.retained.empty()) plan.retainedCollision = CookTriangle(MergeChunks(plan.retained), cookingParams);
        return plan;
    }

    static ModelData MergeChunks(const std::vector<GeneratedChunk>& chunks)
    {
        ModelData merged;
        std::size_t vertexCount = 0, indexCount = 0;
        for (const GeneratedChunk& chunk : chunks) { vertexCount += chunk.model.vertices.size(); indexCount += chunk.model.indices.size(); }
        merged.vertices.reserve(vertexCount);
        merged.indices.reserve(indexCount);
        for (const GeneratedChunk& chunk : chunks)
        {
            unsigned int base = static_cast<unsigned int>(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(), chunk.model.vertices.begin(), chunk.model.vertices.end());
            for (unsigned int index : chunk.model.indices) merged.indices.push_back(base + index);
        }
        return merged;
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

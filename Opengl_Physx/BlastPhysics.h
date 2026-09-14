#pragma once
#include "PhysicsWorld.h"
#include "BlastAsset.h"
#include "BlastRuntime.h"
#include <extensions/PxRigidBodyExt.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <cstdint>

class BlastPhysics
{
public:
    struct ChunkPose
    {
        uint32_t chunk = 0;
        glm::mat4 matrix{ 1.0f };
        const physx::PxRigidActor* actor = nullptr;
    };

    BlastPhysics(PhysicsWorld& physicsWorld, BlastAsset& source, BlastRuntime& blastRuntime, const std::vector<physx::PxConvexMesh*>& sharedCollisionMeshes, glm::vec3 position = glm::vec3(0.0f, 4.0f, 2.0f), glm::vec3 velocity = glm::vec3(0.0f), glm::vec3 scale = glm::vec3(3.0f)) : world(physicsWorld), authored(source.GetAuthoringResult()), runtime(blastRuntime), collisionMeshes(sharedCollisionMeshes), initialPose(physx::PxVec3(position.x, position.y, position.z)), initialVelocity(velocity.x, velocity.y, velocity.z), objectScale(scale) {}

    ~BlastPhysics()
    {
        ReleaseBodies();
    }

    BlastPhysics(const BlastPhysics&) = delete;
    BlastPhysics& operator=(const BlastPhysics&) = delete;

    void SetEnabled(bool value)
    {
        if (enabled == value) return;
        enabled = value;
        if (enabled) Sync();
        else ReleaseBodies();
    }

    void Sync()
    {
        if (!enabled) return;
        std::vector<Nv::Blast::TkActor*> blastActors = runtime.GetActors();
        if (Matches(blastActors)) return;
        std::unordered_map<uint32_t, BodyState> previous;
        BodyState fallback{ initialPose,initialVelocity,physx::PxVec3(0),showCollisions };
        bool hasPrevious = false;
        for (const Body& body : bodies)
        {
            BodyState state{ body.actor->getGlobalPose(),body.actor->getLinearVelocity(),body.actor->getAngularVelocity(),body.showMesh };
            if (!hasPrevious) { fallback = state; hasPrevious = true; }
            for (uint32_t chunk : body.chunks) previous.emplace(chunk, state);
        }
        ReleaseBodies();
        bool fractured = blastActors.size() > 1;
        for (Nv::Blast::TkActor* blastActor : blastActors)
        {
            std::vector<uint32_t> chunks = VisibleChunks(*blastActor);
            if (chunks.empty()) continue;
            BodyState state = fallback;
            for (uint32_t chunk : chunks)
            {
                auto old = previous.find(chunk);
                if (old != previous.end()) { state = old->second; break; }
            }
            bodies.push_back(CreateBody(blastActor, std::move(chunks), state, fractured));
        }
    }

    void Reset()
    {
        ReleaseBodies();
        if (enabled) Sync();
    }

    void CaptureVelocity()
    {
        for (Body& body : bodies) body.velocityBefore = body.actor->getLinearVelocity();
    }

    bool ApplyImpactDamage(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        float strongestVelocityChange = 0.0f;
        for (const Body& body : bodies)
        {
            physx::PxVec3 change = body.actor->getLinearVelocity() - body.velocityBefore;
            strongestVelocityChange = std::max(strongestVelocityChange, change.magnitude());
        }
        if (strongestVelocityChange < minimumFractureVelocityChange) return false;
        float damage = std::clamp(strongestVelocityChange / fullDamageVelocityChange, 0.2f, 1.5f);
        if (beforeRelease) for (const Body& body : bodies) beforeRelease(body.actor);
        runtime.ApplyRadialDamage(0.0f, 0.0f, 0.0f, damage, 0.0f, 3.0f);
        Sync();
        return true;
    }

    std::vector<ChunkPose> GetChunkPoses() const
    {
        std::vector<ChunkPose> poses;
        for (const Body& body : bodies)
        {
            glm::mat4 matrix = ToMatrix(body.actor->getGlobalPose());
            matrix = glm::scale(matrix, objectScale);
            for (uint32_t chunk : body.chunks) poses.push_back({ chunk,matrix,body.actor });
        }
        return poses;
    }

    bool Resolve(const physx::PxRigidActor* actor, bool*& mesh, std::uint64_t& id)
    {
        for (Body& body : bodies) if (body.actor == actor)
        {
            mesh = &body.showMesh;
            id = 0x8000000000000000ull | static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(actor));
            return true;
        }
        return false;
    }

    void AppendCollisionActors(std::vector<const physx::PxRigidActor*>& actors, bool all) const
    {
        for (const Body& body : bodies) if (all || body.showMesh) actors.push_back(body.actor);
    }

    void SetShowCollisions(bool value)
    {
        showCollisions = value;
        for (Body& body : bodies) body.showMesh = value;
    }

private:
    struct BodyState
    {
        physx::PxTransform pose;
        physx::PxVec3 linearVelocity;
        physx::PxVec3 angularVelocity;
        bool showMesh = false;
    };

    struct Body
    {
        Nv::Blast::TkActor* blastActor = nullptr;
        physx::PxRigidDynamic* actor = nullptr;
        std::vector<uint32_t> chunks;
        physx::PxVec3 velocityBefore{ 0 };
        bool showMesh = false;
    };

    PhysicsWorld& world;
    Nv::Blast::AuthoringResult& authored;
    BlastRuntime& runtime;
    const std::vector<physx::PxConvexMesh*>& collisionMeshes;
    std::vector<Body> bodies;
    bool enabled = false;
    physx::PxTransform initialPose;
    physx::PxVec3 initialVelocity;
    glm::vec3 objectScale{ 3.0f };
    bool showCollisions = false;
    static constexpr float minimumFractureVelocityChange = 2.0f;
    static constexpr float fullDamageVelocityChange = 8.0f;

    Body CreateBody(Nv::Blast::TkActor* blastActor, std::vector<uint32_t>&& chunks, const BodyState& state, bool fractured)
    {
        using namespace physx;
        PxRigidDynamic* actor = world.GetPhysics().createRigidDynamic(state.pose);
        if (!actor) throw std::runtime_error("Cannot create Blast PhysX actor.");
        try
        {
            for (uint32_t chunk : chunks)
            {
                if (chunk >= collisionMeshes.size() || !collisionMeshes[chunk]) continue;
                PxConvexMeshGeometry geometry(collisionMeshes[chunk], PxMeshScale(PxVec3(objectScale.x, objectScale.y, objectScale.z)));
                PxShape* shape = world.GetPhysics().createShape(geometry, world.GetMaterial(), true);
                if (!shape) throw std::runtime_error("Cannot create Blast collision shape.");
                bool attached = actor->attachShape(*shape);
                shape->release();
                if (!attached) throw std::runtime_error("Cannot attach Blast collision shape.");
            }
            if (!PxRigidBodyExt::updateMassAndInertia(*actor, 30.0f)) throw std::runtime_error("Cannot compute Blast chunk mass.");
            actor->setLinearDamping(0.08f);
            actor->setAngularDamping(0.15f);
            actor->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
            actor->setLinearVelocity(state.linearVelocity);
            actor->setAngularVelocity(state.angularVelocity);
            world.GetScene().addActor(*actor);
            if (fractured)
            {
                const NvBlastChunkDesc& chunk = authored.chunkDescs[chunks.front()];
                PxVec3 direction(chunk.centroid[0], chunk.centroid[1] + 0.15f, chunk.centroid[2]);
                if (direction.normalize() > 0.0001f) actor->addForce(direction * 4.0f, PxForceMode::eVELOCITY_CHANGE);
            }
        }
        catch (...)
        {
            actor->release();
            throw;
        }
        return { blastActor,actor,std::move(chunks),state.linearVelocity,state.showMesh };
    }

    bool Matches(const std::vector<Nv::Blast::TkActor*>& blastActors) const
    {
        if (blastActors.size() != bodies.size()) return false;
        for (Nv::Blast::TkActor* actor : blastActors)
        {
            bool found = false;
            for (const Body& body : bodies) if (body.blastActor == actor) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }

    static std::vector<uint32_t> VisibleChunks(Nv::Blast::TkActor& actor)
    {
        std::vector<uint32_t> chunks(actor.getVisibleChunkCount());
        if (!chunks.empty()) actor.getVisibleChunkIndices(chunks.data(), static_cast<uint32_t>(chunks.size()));
        return chunks;
    }

    void ReleaseBodies()
    {
        for (Body& body : bodies) if (body.actor) body.actor->release();
        bodies.clear();
    }

    static glm::mat4 ToMatrix(const physx::PxTransform& pose)
    {
        physx::PxMat44 source(pose);
        glm::mat4 result(1.0f);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) result[column][row] = source[column][row];
        return result;
    }
};

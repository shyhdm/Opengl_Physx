#pragma once
#include "BlastContext.h"
#include "BlastLibrary.h"
#include "BlastRuntime.h"
#include "BlastPhysics.h"
#include "BlastCollisionLibrary.h"
#include "PhysicsWorld.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <functional>

class BlastScene
{
public:
    struct RenderChunk
    {
        ModelType type = ModelType::Box;
        BlastPhysics::ChunkPose pose;
    };

    BlastScene(BlastContext& blastContext, BlastLibrary& source, PhysicsWorld& physicsWorld) : context(blastContext), library(source), world(physicsWorld), collisions(physicsWorld, source) {}

    void Spawn(ModelType type, glm::vec3 position, glm::vec3 velocity, float scale = 1.0f)
    {
        BlastAsset& asset = library.Get(type);
        auto object = std::make_unique<Object>();
        object->type = type;
        object->runtime = std::make_unique<BlastRuntime>(context, asset);
        object->physics = std::make_unique<BlastPhysics>(world, asset, *object->runtime, collisions.Get(type), position, velocity, library.GetScale(type) * scale);
        object->physics->SetShowCollisions(showCollisions);
        object->physics->SetEnabled(true);
        objects.push_back(std::move(object));
    }

    void BeforePhysics()
    {
        for (const auto& object : objects) object->physics->CaptureVelocity();
    }

    void AfterPhysics(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        for (const auto& object : objects) object->physics->ApplyImpactDamage(beforeRelease);
    }

    void Clear()
    {
        objects.clear();
    }

    std::vector<RenderChunk> GetRenderChunks() const
    {
        std::vector<RenderChunk> poses;
        for (const auto& object : objects)
        {
            std::vector<BlastPhysics::ChunkPose> objectPoses = object->physics->GetChunkPoses();
            for (const BlastPhysics::ChunkPose& pose : objectPoses) poses.push_back({ object->type,pose });
        }
        return poses;
    }

    uint32_t GetActorCount() const
    {
        uint32_t count = 0;
        for (const auto& object : objects) count += object->runtime->GetActorCount();
        return count;
    }

    uint32_t GetVisibleChunkCount() const
    {
        uint32_t count = 0;
        for (const auto& object : objects) count += object->runtime->GetVisibleChunkCount();
        return count;
    }

    uint32_t GetBondCount() const
    {
        uint32_t count = 0;
        for (const auto& object : objects) count += library.Get(object->type).GetBondCount();
        return count;
    }

    bool Resolve(const physx::PxRigidActor* actor, ModelType& type, bool*& mesh, std::uint64_t& id)
    {
        for (const auto& object : objects) if (object->physics->Resolve(actor, mesh, id))
        {
            type = object->type;
            return true;
        }
        return false;
    }

    void AppendCollisionActors(std::vector<const physx::PxRigidActor*>& actors, bool all) const
    {
        for (const auto& object : objects) object->physics->AppendCollisionActors(actors, all);
    }

    void SetShowCollisions(bool value)
    {
        showCollisions = value;
        for (const auto& object : objects) object->physics->SetShowCollisions(value);
    }

private:
    struct Object
    {
        ModelType type = ModelType::Box;
        std::unique_ptr<BlastRuntime> runtime;
        std::unique_ptr<BlastPhysics> physics;
    };

    BlastContext& context;
    BlastLibrary& library;
    PhysicsWorld& world;
    BlastCollisionLibrary collisions;
    std::vector<std::unique_ptr<Object>> objects;
    bool showCollisions = false;
};

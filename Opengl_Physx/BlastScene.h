#pragma once
#include "BlastContext.h"
#include "BlastLibrary.h"
#include "BlastRuntime.h"
#include "BlastPhysics.h"
#include "BlastCollisionLibrary.h"
#include "PhysicsWorld.h"
#include "RuntimeFractureWall.h"
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
        SpawnInternal(type, position, velocity, scale);
    }

    void BuildWall()
    {
        Clear();
        activeScene = 1;
        wall = std::make_unique<RuntimeFractureWall>(world);
        wall->SetShowCollisions(showCollisions);
    }

private:
    void SpawnInternal(ModelType type, glm::vec3 position, glm::vec3 velocity, float scale)
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

public:

    void BeforePhysics()
    {
        for (const auto& object : objects) object->physics->CaptureVelocity();
    }

    void AfterPhysics(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        for (const auto& object : objects) object->physics->ApplyImpactDamage(beforeRelease);
        if (wall) wall->Update(beforeRelease);
        RefreshRenderChunks();
    }

    void SetSceneIndex(int value)
    {
        if (activeScene == value) return;
        Clear();
        activeScene = value;
        if (activeScene == 1)
        {
            wall = std::make_unique<RuntimeFractureWall>(world);
            wall->SetShowCollisions(showCollisions);
        }
    }

    void Clear()
    {
        objects.clear();
        wall.reset();
        renderChunks.clear();
    }

    const std::vector<RenderChunk>& GetRenderChunks() const { return renderChunks; }

    uint32_t GetActorCount() const
    {
        uint32_t count = 0;
        for (const auto& object : objects) count += object->runtime->GetActorCount();
        if (wall) count += static_cast<uint32_t>(wall->GetActorCount());
        return count;
    }

    uint32_t GetVisibleChunkCount() const
    {
        uint32_t count = 0;
        for (const auto& object : objects) count += object->runtime->GetVisibleChunkCount();
        if (wall) count += static_cast<uint32_t>(wall->GetActorCount());
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
        if (wall && wall->Resolve(actor, mesh, id)) { type = ModelType::Box; return true; }
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
        if (wall) wall->AppendCollisionActors(actors, all);
    }

    void SetShowCollisions(bool value)
    {
        showCollisions = value;
        for (const auto& object : objects) object->physics->SetShowCollisions(value);
        if (wall) wall->SetShowCollisions(value);
    }

    void DrawRuntimeWall(ModelRenderer& renderer, bool shadowPass) const { if (wall) wall->Draw(renderer, shadowPass); }
    void DrawRuntimeWallOutline(OutlineEffect& outline, const Camera& camera, int width, int height, const physx::PxRigidActor* selected) const { if (wall) wall->DrawOutline(outline, camera, width, height, selected); }

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
    std::unique_ptr<RuntimeFractureWall> wall;
    std::vector<RenderChunk> renderChunks;
    bool showCollisions = false;
    int activeScene = -1;

    void RefreshRenderChunks()
    {
        renderChunks.clear();
        for (const auto& object : objects)
        {
            std::vector<BlastPhysics::ChunkPose> poses = object->physics->GetChunkPoses();
            renderChunks.reserve(renderChunks.size() + poses.size());
            for (const BlastPhysics::ChunkPose& pose : poses) renderChunks.push_back({ object->type,pose });
        }
    }
};

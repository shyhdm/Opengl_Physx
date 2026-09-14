#pragma once
#include "BlastContext.h"
#include "BlastLibrary.h"
#include "BlastRuntime.h"
#include "BlastPhysics.h"
#include "BlastCollisionLibrary.h"
#include "PhysicsWorld.h"
#include "RuntimeFractureWall.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>

class BlastScene
{
public:
    struct RenderChunk
    {
        ModelType type = ModelType::Box;
        BlastPhysics::ChunkPose pose;
    };

    BlastScene(BlastContext& blastContext, BlastLibrary& source, PhysicsWorld& physicsWorld) : context(blastContext), library(source), world(physicsWorld), collisions(physicsWorld, source) {}

    void Spawn(ModelType type, glm::vec3 position, glm::vec3 velocity, float scale = 1.0f, float mass = 1.0f)
    {
        SpawnInternal(type, position, velocity, scale, mass);
    }

    void BuildWall()
    {
        Clear();
        activeScene = 1;
        wall = std::make_unique<RuntimeFractureWall>(world);
        wall->SetSettings(wallSettings);
        wall->SetShowCollisions(showCollisions);
    }

private:
    void SpawnInternal(ModelType type, glm::vec3 position, glm::vec3 velocity, float scale, float mass)
    {
        BlastAsset& asset = library.Get(type);
        auto object = std::make_unique<Object>();
        object->type = type;
        object->runtime = std::make_unique<BlastRuntime>(context, asset);
        object->physics = std::make_unique<BlastPhysics>(world, asset, *object->runtime, collisions.Get(type), position, velocity, library.GetScale(type) * scale, mass);
        object->physics->SetShowCollisions(showCollisions);
        object->physics->SetEnabled(true);
        objects.push_back(std::move(object));
    }

public:

    void BeforePhysics()
    {
        auto started = std::chrono::steady_clock::now();
        frameUpdateMs = 0.0;
        for (const auto& object : objects) object->physics->CaptureVelocity();
        frameUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }

    void AfterPhysics(const std::function<void(const physx::PxRigidActor*)>& beforeRelease = {})
    {
        auto started = std::chrono::steady_clock::now();
        if (wall) wall->Update(beforeRelease);
        for (const auto& object : objects) object->physics->ApplyImpactDamage(beforeRelease);
        RefreshRenderChunks();
        frameUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }

    void SetSceneIndex(int value)
    {
        if (activeScene == value) return;
        Clear();
        activeScene = value;
        if (activeScene == 1)
        {
            wall = std::make_unique<RuntimeFractureWall>(world);
            wall->SetSettings(wallSettings);
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

    double GetUpdateMs() const { return frameUpdateMs; }

    RuntimeFractureWall::Settings GetWallSettings() const { return wallSettings; }
    void SetWallSettings(RuntimeFractureWall::Settings value)
    {
        wallSettings = value;
        wallSettings.breakingImpulse = std::clamp(wallSettings.breakingImpulse, 0.01f, 20.0f);
        wallSettings.damageRadius = std::clamp(wallSettings.damageRadius, 0.1f, 2.0f);
        wallSettings.chainRadius = std::clamp(wallSettings.chainRadius, 1.0f, 3.0f);
        wallSettings.localFragments = std::clamp(wallSettings.localFragments, 8u, 48u);
        if (wall) wall->SetSettings(wallSettings);
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
    double frameUpdateMs = 0.0;
    BlastCollisionLibrary collisions;
    std::vector<std::unique_ptr<Object>> objects;
    std::unique_ptr<RuntimeFractureWall> wall;
    std::vector<RenderChunk> renderChunks;
    RuntimeFractureWall::Settings wallSettings;
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

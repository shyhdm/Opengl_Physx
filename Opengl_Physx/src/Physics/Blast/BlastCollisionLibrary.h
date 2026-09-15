#pragma once
#include "BlastLibrary.h"
#include "PhysicsWorld.h"
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

class BlastCollisionLibrary
{
public:
    BlastCollisionLibrary(PhysicsWorld& physicsWorld, BlastLibrary& blastLibrary) : world(physicsWorld), library(blastLibrary) {}
    ~BlastCollisionLibrary()
    {
        for (auto& entry : entries) if (entry) for (physx::PxConvexMesh* mesh : entry->meshes) if (mesh) mesh->release();
    }
    BlastCollisionLibrary(const BlastCollisionLibrary&) = delete;
    BlastCollisionLibrary& operator=(const BlastCollisionLibrary&) = delete;

    const std::vector<physx::PxConvexMesh*>& Get(ModelType type)
    {
        size_t index = static_cast<size_t>(type);
        if (!entries.at(index)) entries[index] = Cook(library.Get(type).GetAuthoringResult());
        return entries[index]->meshes;
    }

private:
    struct Entry { std::vector<physx::PxConvexMesh*> meshes; };
    PhysicsWorld& world;
    BlastLibrary& library;
    std::array<std::unique_ptr<Entry>, static_cast<size_t>(ModelType::Count)> entries;

    std::unique_ptr<Entry> Cook(Nv::Blast::AuthoringResult& authored)
    {
        using namespace physx;
        auto entry = std::make_unique<Entry>();
        entry->meshes.resize(authored.chunkCount, nullptr);
        PxCookingParams params(world.GetPhysics().getTolerancesScale());
        params.buildGPUData = world.GetCuda() != nullptr;
        try
        {
            for (uint32_t chunk = 0; chunk < authored.chunkCount; ++chunk)
            {
                std::vector<PxVec3> points;
                uint32_t first = authored.geometryOffset[chunk], end = authored.geometryOffset[chunk + 1];
                points.reserve(static_cast<size_t>(end - first) * 3);
                for (uint32_t triangleIndex = first; triangleIndex < end; ++triangleIndex)
                {
                    const Nv::Blast::Triangle& triangle = authored.geometry[triangleIndex];
                    points.emplace_back(triangle.a.p.x, triangle.a.p.y, triangle.a.p.z);
                    points.emplace_back(triangle.b.p.x, triangle.b.p.y, triangle.b.p.z);
                    points.emplace_back(triangle.c.p.x, triangle.c.p.y, triangle.c.p.z);
                }
                PxConvexMeshDesc description;
                description.points.count = static_cast<PxU32>(points.size());
                description.points.stride = sizeof(PxVec3);
                description.points.data = points.data();
                description.flags = PxConvexFlag::eCOMPUTE_CONVEX;
                entry->meshes[chunk] = PxCreateConvexMesh(params, description, world.GetPhysics().getPhysicsInsertionCallback());
                if (!entry->meshes[chunk]) throw std::runtime_error("Cannot cook Blast chunk collision mesh.");
            }
        }
        catch (...)
        {
            for (PxConvexMesh* mesh : entry->meshes) if (mesh) mesh->release();
            throw;
        }
        return entry;
    }
};

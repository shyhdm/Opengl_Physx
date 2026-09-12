#pragma once
#include "ModelData.h"
#include <PxPhysicsAPI.h>
#include <extensions/PxDeformableVolumeExt.h>
#include <extensions/PxRemeshingExt.h>
#include <array>
#include <map>
#include <tuple>

class SoftMeshLibrary
{
public:
    explicit SoftMeshLibrary(physx::PxPhysics& physics) : physics(physics) {}
    ~SoftMeshLibrary() { for (auto& entry : meshes) if (entry.second) entry.second->release(); }
    SoftMeshLibrary(const SoftMeshLibrary&) = delete;
    SoftMeshLibrary& operator=(const SoftMeshLibrary&) = delete;

    physx::PxDeformableVolumeMesh* Get(ModelType type, unsigned int resolution = 4)
    {
        using namespace physx;
        auto index = static_cast<std::size_t>(type);
        if (index >= static_cast<std::size_t>(ModelType::Count) || resolution < 4 || resolution>12) throw std::invalid_argument("Invalid soft model.");
        auto key = std::make_pair(index, resolution);
        auto found = meshes.find(key);
        if (found != meshes.end()) return found->second;
        ModelData data = ModelBuilder::Create(type == ModelType::Plane ? ModelType::Box : type, 20, 10);
        PxArray<PxVec3> vertices;
        PxArray<PxU32> triangles;
        std::map<std::tuple<int, int, int>, PxU32> unique;
        std::vector<PxU32> remap;
        for (const auto& vertex : data.vertices)
        {
            PxVec3 p(vertex.x, vertex.y, vertex.z);
            if (type == ModelType::Plane) p.y = p.y * 0.12f - 0.06f;
            auto vertexKey = std::make_tuple(int(std::round(p.x * 100000)), int(std::round(p.y * 100000)), int(std::round(p.z * 100000)));
            auto result = unique.emplace(vertexKey, vertices.size());
            if (result.second) vertices.pushBack(p);
            remap.push_back(result.first->second);
        }
        for (auto i : data.indices) triangles.pushBack(remap[i]);
        PxRemeshingExt::limitMaxEdgeLength(triangles, vertices, 0.2f);
        PxSimpleTriangleMesh surface;
        surface.points.count = vertices.size(); surface.points.stride = sizeof(PxVec3); surface.points.data = vertices.begin();
        surface.triangles.count = triangles.size() / 3; surface.triangles.stride = 3 * sizeof(PxU32); surface.triangles.data = triangles.begin();
        PxCookingParams params(physics.getTolerancesScale());
        params.buildGPUData = true;
        params.meshWeldTolerance = 0.00001f;
        params.meshPreprocessParams = PxMeshPreprocessingFlag::eWELD_VERTICES;
        meshes[key] = PxDeformableVolumeExt::createDeformableVolumeMesh(params, surface, resolution, physics.getPhysicsInsertionCallback());
        if (!meshes[key]) { meshes.erase(key); throw std::runtime_error("Cannot cook PhysX soft-body mesh."); }
        return meshes[key];
    }
private:
    physx::PxPhysics& physics;
    std::map<std::pair<std::size_t, unsigned int>, physx::PxDeformableVolumeMesh*> meshes;
};

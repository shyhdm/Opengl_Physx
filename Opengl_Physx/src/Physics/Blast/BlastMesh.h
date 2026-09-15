#pragma once
#include "ModelData.h"
#include <NvBlastExtAuthoring.h>
#include <NvBlastExtAuthoringMesh.h>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

class BlastMesh
{
public:
    explicit BlastMesh(const ModelData& data)
    {
        if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 != 0) throw std::invalid_argument("Invalid fracture mesh.");

        std::vector<NvcVec3> positions;
        std::vector<NvcVec3> normals;
        std::vector<NvcVec2> uv;
        std::vector<uint32_t> indices;
        std::map<std::tuple<long long, long long, long long>, uint32_t> welded;

        for (unsigned int sourceIndex : data.indices)
        {
            if (sourceIndex >= data.vertices.size()) throw std::out_of_range("Fracture mesh index is invalid.");
            const Vertex& vertex = data.vertices[sourceIndex];
            auto key = std::make_tuple(std::llround(vertex.x * 100000.0), std::llround(vertex.y * 100000.0), std::llround(vertex.z * 100000.0));
            auto found = welded.find(key);
            uint32_t index = 0;

            if (found == welded.end())
            {
                index = static_cast<uint32_t>(positions.size());
                welded.emplace(key, index);
                positions.push_back({ vertex.x, vertex.y, vertex.z });
                normals.push_back({ vertex.nx, vertex.ny, vertex.nz });
                uv.push_back({ vertex.u, vertex.v });
            }
            else index = found->second;

            indices.push_back(index);
        }

        mesh = NvBlastExtAuthoringCreateMesh(positions.data(), normals.data(), uv.data(), static_cast<uint32_t>(positions.size()), indices.data(), static_cast<uint32_t>(indices.size()));
        if (!mesh) throw std::runtime_error("Cannot create Blast authoring mesh.");

        if (!mesh->isValid())
        {
            mesh->release();
            mesh = nullptr;
            throw std::runtime_error("Blast rejected the mesh. It must be closed and have valid triangle winding.");
        }
    }

    ~BlastMesh()
    {
        if (mesh) mesh->release();
    }

    BlastMesh(const BlastMesh&) = delete;
    BlastMesh& operator=(const BlastMesh&) = delete;
    Nv::Blast::Mesh& Get() { return *mesh; }

private:
    Nv::Blast::Mesh* mesh = nullptr;
};
#pragma once
#include "BlastLibrary.h"
#include "BlastScene.h"
#include "ModelRenderer.h"
#include "OutlineEffect.h"
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include <cmath>

class BlastChunkRenderer
{
public:
    explicit BlastChunkRenderer(BlastLibrary& library)
    {
        for (int index = 0; index < static_cast<int>(ModelType::Count); ++index)
        {
            ModelType type = static_cast<ModelType>(index);
            Nv::Blast::AuthoringResult& authored = library.Get(type).GetAuthoringResult();
            TypeData& data = types[index];
            data.meshes.resize(authored.chunkCount);
            data.materials.resize(authored.chunkCount);
            for (uint32_t chunk = 0; chunk < authored.chunkCount; ++chunk)
            {
                ModelData model = CreateChunkModel(authored, chunk);
                if (!model.vertices.empty()) data.meshes[chunk] = std::make_unique<Mesh>(model.vertices, model.indices);
                data.materials[chunk].baseColor = Color(type);
                data.materials[chunk].specularStrength = 0.25f;
                data.materials[chunk].shininess = 32.0f;
            }
        }
    }

    void Draw(ModelRenderer& renderer, const BlastScene& scene, bool shadowPass)
    {
        for (const BlastScene::RenderChunk& renderChunk : scene.GetRenderChunks())
        {
            const BlastPhysics::ChunkPose& pose = renderChunk.pose;
            TypeData& data = types[static_cast<size_t>(renderChunk.type)];
            if (pose.chunk >= data.meshes.size() || !data.meshes[pose.chunk]) continue;
            if (shadowPass) renderer.DrawShadow(*data.meshes[pose.chunk], pose.matrix);
            else renderer.DrawMesh(*data.meshes[pose.chunk], pose.matrix, data.materials[pose.chunk]);
        }
    }

    void DrawOutline(OutlineEffect& outline, const BlastScene& scene, const Camera& camera, int width, int height, const physx::PxRigidActor* selected)
    {
        if (!selected) return;
        for (const BlastScene::RenderChunk& renderChunk : scene.GetRenderChunks())
        {
            const BlastPhysics::ChunkPose& pose = renderChunk.pose;
            if (pose.actor != selected) continue;
            TypeData& data = types[static_cast<size_t>(renderChunk.type)];
            if (pose.chunk < data.meshes.size() && data.meshes[pose.chunk]) outline.Draw(*data.meshes[pose.chunk], camera, width, height, pose.matrix);
        }
    }

private:
    struct TypeData
    {
        std::vector<std::unique_ptr<Mesh>> meshes;
        std::vector<Material> materials;
    };

    std::array<TypeData, static_cast<size_t>(ModelType::Count)> types;

    static ModelData CreateChunkModel(Nv::Blast::AuthoringResult& authored, uint32_t chunk)
    {
        ModelData data;
        uint32_t first = authored.geometryOffset[chunk], end = authored.geometryOffset[chunk + 1];
        data.vertices.reserve(static_cast<size_t>(end - first) * 3);
        data.indices.reserve(static_cast<size_t>(end - first) * 3);
        struct Face
        {
            const Nv::Blast::Triangle* triangle = nullptr;
            glm::vec3 normal{ 0.0f,1.0f,0.0f };
            glm::vec3 weightedNormal{ 0.0f,1.0f,0.0f };
            bool interior = false;
        };
        using PositionKey = std::tuple<int64_t, int64_t, int64_t>;
        auto key = [](const Nv::Blast::Vertex& vertex)
            {
                return PositionKey(std::llround(vertex.p.x * 1000000.0), std::llround(vertex.p.y * 1000000.0), std::llround(vertex.p.z * 1000000.0));
            };
        std::vector<Face> faces;
        std::map<PositionKey, std::vector<size_t>> adjacentFaces;
        faces.reserve(end - first);
        for (uint32_t triangleIndex = first; triangleIndex < end; ++triangleIndex)
        {
            const Nv::Blast::Triangle& triangle = authored.geometry[triangleIndex];
            glm::vec3 a(triangle.a.p.x, triangle.a.p.y, triangle.a.p.z);
            glm::vec3 b(triangle.b.p.x, triangle.b.p.y, triangle.b.p.z);
            glm::vec3 c(triangle.c.p.x, triangle.c.p.y, triangle.c.p.z);
            glm::vec3 weightedNormal = glm::cross(b - a, c - a);
            float lengthSquared = glm::dot(weightedNormal, weightedNormal);
            glm::vec3 normal = lengthSquared > 0.00000001f ? weightedNormal / std::sqrt(lengthSquared) : glm::vec3(0.0f, 1.0f, 0.0f);
            size_t faceIndex = faces.size();
            faces.push_back({ &triangle,normal,weightedNormal,triangle.materialId == static_cast<int32_t>(Nv::Blast::kMaterialInteriorId) });
            adjacentFaces[key(triangle.a)].push_back(faceIndex);
            adjacentFaces[key(triangle.b)].push_back(faceIndex);
            adjacentFaces[key(triangle.c)].push_back(faceIndex);
        }
        constexpr float smoothAngleCosine = 0.75f;
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
        {
            const Face& face = faces[faceIndex];
            auto add = [&](const Nv::Blast::Vertex& vertex)
                {
                    glm::vec3 normal = face.normal;
                    if (!face.interior)
                    {
                        glm::vec3 sum(0.0f);
                        for (size_t adjacentIndex : adjacentFaces[key(vertex)])
                        {
                            const Face& adjacent = faces[adjacentIndex];
                            if (!adjacent.interior && glm::dot(face.normal, adjacent.normal) >= smoothAngleCosine) sum += adjacent.weightedNormal;
                        }
                        float lengthSquared = glm::dot(sum, sum);
                        if (lengthSquared > 0.00000001f) normal = sum / std::sqrt(lengthSquared);
                    }
                    AddVertex(data, vertex, normal);
                };
            add(face.triangle->a);
            add(face.triangle->b);
            add(face.triangle->c);
        }
        return data;
    }

    static void AddVertex(ModelData& data, const Nv::Blast::Vertex& source, glm::vec3 normal)
    {
        data.indices.push_back(static_cast<unsigned int>(data.vertices.size()));
        data.vertices.push_back({ source.p.x,source.p.y,source.p.z,1.0f,1.0f,1.0f,normal.x,normal.y,normal.z,source.uv[0].x,source.uv[0].y });
    }

    static glm::vec3 Color(ModelType type)
    {
        switch (type)
        {
        case ModelType::Box: return { 0.75f,0.18f,0.10f };
        case ModelType::Plane: return { 0.55f,0.60f,0.65f };
        case ModelType::Sphere: return { 0.12f,0.35f,0.85f };
        case ModelType::Cylinder: return { 0.16f,0.60f,0.28f };
        case ModelType::Cone: return { 0.85f,0.55f,0.12f };
        case ModelType::Capsule: return { 0.55f,0.22f,0.75f };
        case ModelType::Torus: return { 0.12f,0.65f,0.65f };
        default: return glm::vec3(0.65f);
        }
    }
};

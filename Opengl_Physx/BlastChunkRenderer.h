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
#include <bit>
#include <limits>
#include <glm/gtc/matrix_inverse.hpp>

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
            data.models.resize(authored.chunkCount);
            data.materials.resize(authored.chunkCount);
            ModelData combined;
            for (uint32_t chunk = 0; chunk < authored.chunkCount; ++chunk)
            {
                ModelData model = CreateChunkModel(authored, chunk);
                if (!model.vertices.empty())
                {
                    AppendModel(combined, model);
                    data.models[chunk] = model;
                    data.meshes[chunk] = std::make_unique<Mesh>(model.vertices, model.indices);
                }
                data.materials[chunk].baseColor = Color(type);
                data.materials[chunk].specularStrength = 0.25f;
                data.materials[chunk].shininess = 32.0f;
            }
            if (!combined.vertices.empty()) { data.combinedModel = combined; data.combined = std::make_unique<Mesh>(combined.vertices, combined.indices); }
        }
    }

    void Draw(ModelRenderer& renderer, const BlastScene& scene, bool shadowPass)
    {
        if (shadowPass) UpdateBatches(scene);
        for (std::size_t type = 0; type < batches.size(); ++type)
        {
            Batch& batch = batches[type];
            if (!batch.mesh) continue;
            if (shadowPass) renderer.DrawShadow(*batch.mesh, glm::mat4(1.0f));
            else renderer.DrawMesh(*batch.mesh, glm::mat4(1.0f), types[type].materials[0]);
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
            if (pose.whole && data.combined) { outline.Draw(*data.combined, camera, width, height, pose.matrix); continue; }
            if (pose.chunk < data.meshes.size() && data.meshes[pose.chunk]) outline.Draw(*data.meshes[pose.chunk], camera, width, height, pose.matrix);
        }
    }

private:
    struct TypeData
    {
        std::vector<std::unique_ptr<Mesh>> meshes;
        std::vector<ModelData> models;
        std::vector<Material> materials;
        std::unique_ptr<Mesh> combined;
        ModelData combinedModel;
    };
    struct Batch { std::vector<std::size_t> renders; std::vector<Vertex> vertices; std::unique_ptr<Mesh> mesh; };

    std::array<TypeData, static_cast<size_t>(ModelType::Count)> types;
    std::array<Batch, static_cast<size_t>(ModelType::Count)> batches;
    std::uint64_t batchSignature = 0;

    const ModelData* Source(const BlastScene::RenderChunk& render) const
    {
        const TypeData& data = types[static_cast<std::size_t>(render.type)];
        if (render.pose.whole) return data.combinedModel.vertices.empty() ? nullptr : &data.combinedModel;
        return render.pose.chunk < data.models.size() && !data.models[render.pose.chunk].vertices.empty() ? &data.models[render.pose.chunk] : nullptr;
    }

    void UpdateBatches(const BlastScene& scene)
    {
        const auto& renders = scene.GetRenderChunks();
        std::uint64_t signature = 0xCBF29CE484222325ull;
        for (const auto& render : renders)
        {
            signature ^= reinterpret_cast<std::uintptr_t>(render.pose.actor) + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
            signature ^= (static_cast<std::uint64_t>(render.type) << 33) ^ (static_cast<std::uint64_t>(render.pose.chunk) << 1) ^ static_cast<std::uint64_t>(render.pose.whole);
        }
        if (signature != batchSignature)
        {
            for (Batch& batch : batches) { batch.renders.clear(); batch.vertices.clear(); batch.mesh.reset(); }
            for (std::size_t i = 0; i < renders.size(); ++i) if (Source(renders[i])) batches[static_cast<std::size_t>(renders[i].type)].renders.push_back(i);
            for (Batch& batch : batches)
            {
                std::vector<unsigned int> indices;
                unsigned int base = 0;
                for (std::size_t renderIndex : batch.renders)
                {
                    const ModelData& source = *Source(renders[renderIndex]);
                    batch.vertices.insert(batch.vertices.end(), source.vertices.begin(), source.vertices.end());
                    for (unsigned int index : source.indices) indices.push_back(base + index);
                    base += static_cast<unsigned int>(source.vertices.size());
                }
                if (!batch.vertices.empty()) batch.mesh = std::make_unique<Mesh>(batch.vertices, indices);
            }
            batchSignature = signature;
        }
        for (Batch& batch : batches)
        {
            std::size_t output = 0;
            for (std::size_t renderIndex : batch.renders)
            {
                const auto& render = renders[renderIndex];
                const ModelData& source = *Source(render);
                glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(render.pose.matrix)));
                for (const Vertex& input : source.vertices)
                {
                    Vertex& vertex = batch.vertices[output++];
                    glm::vec4 position = render.pose.matrix * glm::vec4(input.x, input.y, input.z, 1.0f);
                    glm::vec3 normal = normalMatrix * glm::vec3(input.nx, input.ny, input.nz);
                    float length = glm::length(normal); normal = length > 0.000001f ? normal / length : glm::vec3(0, 1, 0);
                    vertex = input; vertex.x = position.x; vertex.y = position.y; vertex.z = position.z;
                    vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
                }
            }
            if (batch.mesh) batch.mesh->UpdateVertices(batch.vertices);
        }
    }

    static void AppendModel(ModelData& target, const ModelData& source)
    {
        unsigned int base = static_cast<unsigned int>(target.vertices.size());
        target.vertices.insert(target.vertices.end(), source.vertices.begin(), source.vertices.end());
        target.indices.reserve(target.indices.size() + source.indices.size());
        for (unsigned int index : source.indices) target.indices.push_back(base + index);
    }

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

#pragma once
#include "BlastMesh.h"
#include "BlastVoronoi.h"
#include "BlastAsset.h"
#include "ModelData.h"
#include <array>
#include <memory>

class BlastLibrary
{
public:
    BlastLibrary()
    {
        for (int index = 0; index < static_cast<int>(ModelType::Count); ++index)
        {
            ModelType type = static_cast<ModelType>(index);
            ModelData data = ModelBuilder::Create(type == ModelType::Plane ? ModelType::Box : type);
            auto entry = std::make_unique<Entry>();
            entry->mesh = std::make_unique<BlastMesh>(data);
            entry->voronoi = std::make_unique<BlastVoronoi>(*entry->mesh, 16, 12345 + index * 97);
            entry->asset = std::make_unique<BlastAsset>(*entry->voronoi);
            entries[index] = std::move(entry);
        }
    }

    BlastAsset& Get(ModelType type)
    {
        return *entries.at(static_cast<size_t>(type))->asset;
    }

    glm::vec3 GetScale(ModelType type) const
    {
        return type == ModelType::Plane ? glm::vec3(1.0f, 0.04f, 1.0f) : glm::vec3(1.0f);
    }

private:
    struct Entry
    {
        std::unique_ptr<BlastMesh> mesh;
        std::unique_ptr<BlastVoronoi> voronoi;
        std::unique_ptr<BlastAsset> asset;
    };

    std::array<std::unique_ptr<Entry>, static_cast<size_t>(ModelType::Count)> entries;
};

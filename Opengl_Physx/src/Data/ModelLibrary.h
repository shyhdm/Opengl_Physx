#pragma once
#include "ModelData.h"
#include <array>
#include <memory>

// GPU 模型缓存。首次 Get 时上传，之后共享；必须在 OpenGL 上下文存活时使用。
class ModelLibrary
{
public:
    const Mesh& Get(ModelType type)
    {
        std::size_t index = static_cast<std::size_t>(type);
        if (index >= models.size()) throw std::invalid_argument("Unknown model type.");
        if (!models[index])
        {
            ModelData data = ModelBuilder::Create(type);
            models[index] = std::make_unique<Mesh>(data.vertices, data.indices);
        }
        return *models[index];
    }

private:
    std::array<std::unique_ptr<Mesh>, static_cast<std::size_t>(ModelType::Count)> models;
};

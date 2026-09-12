#pragma once
#include <glm/glm.hpp>
#include "Texture.h"

// 纯外观数据，可直接复制。不会改变刚体的质量、摩擦或碰撞形状。
struct Material
{
    glm::vec3 baseColor = glm::vec3(0.65f);
    float specularStrength = 0.25f;
    float shininess = 32.0f;
    std::shared_ptr<Texture> baseTexture;
    glm::vec2 textureTiling = glm::vec2(1.0f);
};

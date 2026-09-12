#pragma once
#include "Mesh.h"
#include <glm/glm.hpp>
#include <cmath>

// CPU 模型数据：可以在创建窗口之前生成、修改，之后再上传为 Mesh。
struct ModelData
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
};

enum class ModelType { Box, Plane, Sphere, Cylinder, Cone, Capsule, Torus, Count };

class ModelBuilder
{
public:
    // 模型以原点为中心，Y 朝上。尺寸通过 Transform.scale 修改。
    static ModelData Create(ModelType type, unsigned int segments = 32, unsigned int rings = 16)
    {
        if (segments < 3 || segments > 256 || rings < 2 || rings > 256)
            throw std::invalid_argument("Model segments must be 3..256 and rings 2..256.");
        switch (type)
        {
        case ModelType::Box: return Box();
        case ModelType::Plane: return Plane();
        case ModelType::Sphere: return Sphere(segments, rings);
        case ModelType::Cylinder: return Lathe({ {0,-0.5f,0,-1}, {0.5f,-0.5f,0,-1}, {0.5f,-0.5f,1,0}, {0.5f,0.5f,1,0}, {0.5f,0.5f,0,1}, {0,0.5f,0,1} }, segments);
        case ModelType::Cone: return Lathe({ {0,-0.5f,0,-1}, {0.5f,-0.5f,0,-1}, {0.5f,-0.5f,0.89442719f,0.44721360f}, {0,0.5f,0.89442719f,0.44721360f} }, segments);
        case ModelType::Capsule: return Capsule(segments, rings);
        case ModelType::Torus: return Torus(segments, rings);
        default: throw std::invalid_argument("Unknown model type.");
        }
    }

private:
    static constexpr float pi = 3.14159265358979323846f;
    struct Profile { float radius, y, normalRadius, normalY; };

    static void VertexAt(ModelData& data, glm::vec3 p, glm::vec3 n, float u, float v)
    {
        // 法线同时用于给模型生成基础顶点颜色，之后可自行修改 r/g/b。
        glm::vec3 color = glm::vec3(0.55f) + n * 0.25f;
        data.vertices.push_back({ p.x,p.y,p.z,color.r,color.g,color.b,n.x,n.y,n.z,u,v });
    }

    static void Triangle(ModelData& data, unsigned int a, unsigned int b, unsigned int c)
    {
        data.indices.insert(data.indices.end(), { a,b,c });
    }

    static void Face(ModelData& data, glm::vec3 center, glm::vec3 u, glm::vec3 v, glm::vec3 normal)
    {
        unsigned int first = static_cast<unsigned int>(data.vertices.size());
        VertexAt(data, center - u - v, normal, 0, 0);
        VertexAt(data, center + u - v, normal, 1, 0);
        VertexAt(data, center + u + v, normal, 1, 1);
        VertexAt(data, center - u + v, normal, 0, 1);
        Triangle(data, first, first + 1, first + 2);
        Triangle(data, first, first + 2, first + 3);
    }

    static ModelData Box()
    {
        ModelData data;
        for (glm::vec3 normal : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)})
        {
            glm::vec3 u = std::abs(normal.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::cross(glm::vec3(0, 1, 0), normal);
            Face(data, normal * 0.5f, u * 0.5f, glm::cross(normal, u) * 0.5f, normal);
        }
        return data;
    }

    static ModelData Plane()
    {
        ModelData data;
        Face(data, glm::vec3(0), glm::vec3(0.5f, 0, 0), glm::vec3(0, 0, -0.5f), glm::vec3(0, 1, 0));
        return data;
    }

    // 将纵向轮廓绕 Y 轴旋转，缝合成网格。硬边用重复顶点保留独立法线。
    static ModelData Lathe(const std::vector<Profile>& profile, unsigned int segments)
    {
        ModelData data;
        unsigned int rows = static_cast<unsigned int>(profile.size()), stride = segments + 1;
        for (unsigned int i = 0; i < rows; ++i)
        {
            const auto& p = profile[i];
            for (unsigned int j = 0; j <= segments; ++j)
            {
                float u = static_cast<float>(j) / segments;
                float angle = j == segments ? 0.0f : 2 * pi * u;
                float c = std::cos(angle), s = std::sin(angle);
                VertexAt(data, { p.radius * c,p.y,p.radius * s }, { p.normalRadius * c,p.normalY,p.normalRadius * s }, u, static_cast<float>(i) / (rows - 1));
            }
        }
        for (unsigned int i = 0; i + 1 < rows; ++i)
        {
            if (profile[i].radius == profile[i + 1].radius && profile[i].y == profile[i + 1].y) continue;
            for (unsigned int j = 0; j < segments; ++j)
            {
                unsigned int a = i * stride + j, b = a + stride;
                if (profile[i + 1].radius > 0) Triangle(data, a, b, b + 1);
                if (profile[i].radius > 0) Triangle(data, a, b + 1, a + 1);
            }
        }
        return data;
    }

    static ModelData Sphere(unsigned int segments, unsigned int rings)
    {
        std::vector<Profile> profile;
        for (unsigned int i = 0; i <= rings; ++i)
        {
            float angle = -pi * 0.5f + pi * static_cast<float>(i) / rings;
            float c = (i == 0 || i == rings) ? 0.0f : std::cos(angle), s = std::sin(angle);
            profile.push_back({ 0.5f * c,0.5f * s,c,s });
        }
        return Lathe(profile, segments);
    }

    static ModelData Capsule(unsigned int segments, unsigned int rings)
    {
        std::vector<Profile> profile;
        for (unsigned int half = 0; half < 2; ++half)
            for (unsigned int i = 0; i <= rings; ++i)
            {
                float angle = (half == 0 ? -pi * 0.5f : 0.0f) + pi * 0.5f * static_cast<float>(i) / rings;
                float c = (half == 0 && i == 0) || (half == 1 && i == rings) ? 0.0f : std::cos(angle), s = std::sin(angle);
                profile.push_back({ 0.5f * c,(half == 0 ? -0.5f : 0.5f) + 0.5f * s,c,s });
            }
        return Lathe(profile, segments);
    }

    static ModelData Torus(unsigned int segments, unsigned int rings)
    {
        if (rings < 3) throw std::invalid_argument("Torus requires at least 3 tube rings.");
        ModelData data;
        for (unsigned int i = 0; i <= segments; ++i)
            for (unsigned int j = 0; j <= rings; ++j)
            {
                float u = static_cast<float>(i) / segments, v = static_cast<float>(j) / rings;
                float a = i == segments ? 0.0f : 2 * pi * u, b = j == rings ? 0.0f : 2 * pi * v;
                float c = std::cos(a), s = std::sin(a), cb = std::cos(b), sb = std::sin(b);
                VertexAt(data, { (0.65f + 0.2f * cb) * c,0.2f * sb,(0.65f + 0.2f * cb) * s }, { cb * c,sb,cb * s }, u, v);
            }
        for (unsigned int i = 0; i < segments; ++i)
            for (unsigned int j = 0; j < rings; ++j)
            {
                unsigned int a = i * (rings + 1) + j, b = a + rings + 1;
                Triangle(data, a, a + 1, b + 1);
                Triangle(data, a, b + 1, b);
            }
        return data;
    }
};

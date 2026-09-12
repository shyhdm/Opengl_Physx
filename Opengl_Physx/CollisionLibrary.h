#pragma once
#include <PxPhysicsAPI.h>
#include "ModelData.h"
#include <array>
#include <stdexcept>

// 缓存单位尺寸的凸包。同一个物理世界内共享，不能比 PxPhysics 活得更久。
class CollisionLibrary
{
public:
    explicit CollisionLibrary(physx::PxPhysics& physics) : physics(physics) {}
    ~CollisionLibrary() { for (auto* mesh : meshes) if (mesh) mesh->release(); }
    CollisionLibrary(const CollisionLibrary&) = delete;
    CollisionLibrary& operator=(const CollisionLibrary&) = delete;

    void Attach(physx::PxRigidActor& actor, ModelType type, glm::vec3 scale, physx::PxMaterial& material)
    {
        using namespace physx;
        switch (type)
        {
        case ModelType::Box:
            Add(actor, PxBoxGeometry(scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f), material); break;
        case ModelType::Plane:
            // 有限平面用薄盒子碰撞，顶面位于模型的局部 y=0。
            Add(actor, PxBoxGeometry(scale.x * 0.5f, 0.02f, scale.z * 0.5f), material, PxTransform(PxVec3(0, -0.02f, 0))); break;
        case ModelType::Sphere:
            UniformScale(scale);
            Add(actor, PxSphereGeometry(0.5f * scale.x), material); break;
        case ModelType::Capsule:
            UniformScale(scale);
            // PhysX 原生胶囊沿 X 轴，模型沿 Y 轴，形状局部旋转 90 度。
            Add(actor, PxCapsuleGeometry(0.5f * scale.x, 0.5f * scale.x), material, PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1)))); break;
        case ModelType::Cylinder:
        case ModelType::Cone:
            Add(actor, PxConvexMeshGeometry(Get(type), PxMeshScale(PxVec3(scale.x, scale.y, scale.z))), material); break;
        case ModelType::Torus:
            UniformScale(scale);
            for (unsigned int i = 0; i < 16; ++i)
                Add(actor, PxConvexMeshGeometry(Get(type), PxMeshScale(scale.x)), material, PxTransform(PxQuat(PxTwoPi * static_cast<float>(i) / 16.0f, PxVec3(0, 1, 0))));
            break;
        default: throw std::invalid_argument("Unknown collision model.");
        }
    }

private:
    physx::PxPhysics& physics;
    std::array<physx::PxConvexMesh*, 3> meshes = {};

    static void UniformScale(glm::vec3 scale)
    {
        if (std::abs(scale.x - scale.y) > 0.00001f || std::abs(scale.x - scale.z) > 0.00001f)
            throw std::invalid_argument("Sphere, capsule and torus require uniform scale.");
    }

    void Add(physx::PxRigidActor& actor, const physx::PxGeometry& geometry, physx::PxMaterial& material, physx::PxTransform pose = physx::PxTransform(physx::PxIdentity))
    {
        auto* shape = physics.createShape(geometry, material, true);
        if (!shape) throw std::runtime_error("Cannot create collision shape.");
        shape->setLocalPose(pose);
        bool attached = actor.attachShape(*shape);
        shape->release();
        if (!attached) throw std::runtime_error("Cannot attach collision shape.");
    }

    physx::PxConvexMesh* Get(ModelType type)
    {
        using namespace physx;
        std::size_t index = type == ModelType::Cylinder ? 0 : type == ModelType::Cone ? 1 : 2;
        if (meshes[index]) return meshes[index];
        std::vector<PxVec3> points;
        if (type == ModelType::Torus)
        {
            // 一段环管的凸包，两端各 16 点；16 段共享同一个网格。
            for (int end : {-1, 1})
                for (unsigned int j = 0; j < 16; ++j)
                {
                    float a = end * PxPi / 16.0f, b = PxTwoPi * static_cast<float>(j) / 16.0f;
                    float r = 0.65f + 0.2f * std::cos(b);
                    points.emplace_back(r * std::cos(a), 0.2f * std::sin(b), r * std::sin(a));
                }
        }
        else
        {
            for (unsigned int i = 0; i < 32; ++i)
            {
                float angle = PxTwoPi * static_cast<float>(i) / 32.0f;
                points.emplace_back(0.5f * std::cos(angle), -0.5f, 0.5f * std::sin(angle));
                if (type == ModelType::Cylinder) points.emplace_back(points.back().x, 0.5f, points.back().z);
            }
            if (type == ModelType::Cone) points.emplace_back(0.0f, 0.5f, 0.0f);
        }
        PxConvexMeshDesc description;
        description.points.count = static_cast<PxU32>(points.size());
        description.points.stride = sizeof(PxVec3);
        description.points.data = points.data();
        description.flags = PxConvexFlag::eCOMPUTE_CONVEX;
        PxCookingParams params(physics.getTolerancesScale());
        params.buildGPUData = true;
        meshes[index] = PxCreateConvexMesh(params, description, physics.getPhysicsInsertionCallback());
        if (!meshes[index]) throw std::runtime_error("Cannot cook convex collision mesh.");
        return meshes[index];
    }
};

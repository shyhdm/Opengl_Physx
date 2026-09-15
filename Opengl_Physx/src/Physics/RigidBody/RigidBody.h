#pragma once
#include "PhysicsWorld.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 支持动态刚体和用于场景结构的静态刚体；平面使用薄盒碰撞体。尺寸参数是模型缩放。
class RigidBody
{
public:
    RigidBody(PhysicsWorld& world, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f), float density = 10.0f,
        bool isStatic = false, glm::vec3 rotationDegrees = glm::vec3(0.0f))
        : RigidBody(world, ModelType::Box, position, scale, density, isStatic, rotationDegrees) {}

    RigidBody(PhysicsWorld& world, ModelType type, glm::vec3 position, glm::vec3 scale = glm::vec3(1.0f), float density = 10.0f,
        bool isStatic = false, glm::vec3 rotationDegrees = glm::vec3(0.0f)) : type(type), scale(scale)
    {
        using namespace physx;
        if (!PxVec3(position.x, position.y, position.z).isFinite() || !PxVec3(scale.x, scale.y, scale.z).isFinite() ||
            !PxVec3(rotationDegrees.x, rotationDegrees.y, rotationDegrees.z).isFinite() ||
            scale.x <= 0 || scale.y <= 0 || scale.z <= 0 || !std::isfinite(density) || density <= 0)
            throw std::invalid_argument("Invalid rigid body position, scale, rotation or density.");
        try
        {
            PxQuat rotation = PxQuat(glm::radians(rotationDegrees.z), PxVec3(0, 0, 1)) *
                PxQuat(glm::radians(rotationDegrees.y), PxVec3(0, 1, 0)) *
                PxQuat(glm::radians(rotationDegrees.x), PxVec3(1, 0, 0));
            PxTransform pose(PxVec3(position.x, position.y, position.z), rotation);
            if (isStatic) actor = world.GetPhysics().createRigidStatic(pose);
            else
            {
                dynamic = world.GetPhysics().createRigidDynamic(pose);
                actor = dynamic;
            }
            if (!actor) throw std::runtime_error("Cannot create PhysX actor.");
            physicalMaterial = world.GetPhysics().createMaterial(0.6f, 0.5f, 0.15f);
            if (!physicalMaterial) throw std::runtime_error("Cannot create body material.");
            world.GetCollisions().Attach(*actor, type, scale, *physicalMaterial);
            if (dynamic)
            {
                if (!PxRigidBodyExt::updateMassAndInertia(*dynamic, density)) throw std::runtime_error("Cannot compute body mass.");
                dynamic->setAngularDamping(0.2f);
                dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
            }
            world.GetScene().addActor(*actor);
        }
        catch (...) { if (actor) actor->release(); if (physicalMaterial) physicalMaterial->release(); throw; }
    }

    ~RigidBody() { actor->release(); physicalMaterial->release(); }
    RigidBody(const RigidBody&) = delete;
    RigidBody& operator=(const RigidBody&) = delete;

    struct Properties
    {
        float mass = 1, staticFriction = 0.6f, dynamicFriction = 0.5f, restitution = 0.15f, linearDamping = 0, angularDamping = 0.2f;
    };
    Properties GetProperties() const
    {
        if (!dynamic)
            return { 0.0f,physicalMaterial->getStaticFriction(),physicalMaterial->getDynamicFriction(),
                physicalMaterial->getRestitution(),0.0f,0.0f };
        return { dynamic->getMass(),physicalMaterial->getStaticFriction(),physicalMaterial->getDynamicFriction(),
            physicalMaterial->getRestitution(),dynamic->getLinearDamping(),dynamic->getAngularDamping() };
    }
    void SetProperties(const Properties& p)
    {
        if (!dynamic) throw std::logic_error("Static bodies do not have dynamic physical properties.");
        auto valid = [](float v, float low, float high) {return std::isfinite(v) && v >= low && v <= high; };
        if (!valid(p.mass, 0.01f, 100000.0f) || !valid(p.staticFriction, 0, 10) || !valid(p.dynamicFriction, 0, 10) ||
            !valid(p.restitution, 0, 1) || !valid(p.linearDamping, 0, 100) || !valid(p.angularDamping, 0, 100))
            throw std::invalid_argument("Invalid physical properties.");
        // 相同形状的惯性与质量成正比，一起更新，避免只改质量导致旋转异常。
        float ratio = p.mass / dynamic->getMass();
        dynamic->setMassSpaceInertiaTensor(dynamic->getMassSpaceInertiaTensor() * ratio);
        dynamic->setMass(p.mass);
        physicalMaterial->setStaticFriction(p.staticFriction);
        physicalMaterial->setDynamicFriction(p.dynamicFriction);
        physicalMaterial->setRestitution(p.restitution);
        dynamic->setLinearDamping(p.linearDamping);
        dynamic->setAngularDamping(p.angularDamping);
        dynamic->wakeUp();
    }

    ModelType GetModelType() const { return type; }
    const physx::PxRigidActor* GetActor() const { return actor; }
    bool IsDynamic() const { return dynamic != nullptr; }
    unsigned int GetShapeCount() const { return actor->getNbShapes(); }

    glm::vec3 GetPosition() const
    {
        auto p = actor->getGlobalPose().p;
        return { p.x,p.y,p.z };
    }

    void SetVelocity(glm::vec3 velocity)
    {
        if (!dynamic) throw std::logic_error("Body is not dynamic.");
        physx::PxVec3 value(velocity.x, velocity.y, velocity.z);
        if (!value.isFinite()) throw std::invalid_argument("Invalid velocity.");
        dynamic->setLinearVelocity(value);
    }

    glm::mat4 GetMatrix() const
    {
        physx::PxMat44 pose(actor->getGlobalPose());
        glm::mat4 matrix(1.0f);
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) matrix[column][row] = pose[column][row];
        return glm::scale(matrix, scale);
    }

    void Reset(glm::vec3 position)
    {
        if (!physx::PxVec3(position.x, position.y, position.z).isFinite()) throw std::invalid_argument("Invalid reset position.");
        actor->setGlobalPose(physx::PxTransform(physx::PxVec3(position.x, position.y, position.z)));
        if (dynamic)
        {
            dynamic->setLinearVelocity(physx::PxVec3(0));
            dynamic->setAngularVelocity(physx::PxVec3(0));
            dynamic->clearForce();
            dynamic->clearTorque();
            dynamic->wakeUp();
        }
    }

private:
    ModelType type;
    glm::vec3 scale;
    physx::PxRigidActor* actor = nullptr;
    physx::PxRigidDynamic* dynamic = nullptr;
    physx::PxMaterial* physicalMaterial = nullptr;
};

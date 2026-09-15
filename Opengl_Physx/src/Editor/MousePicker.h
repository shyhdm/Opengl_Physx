#pragma once
#include "PhysicsWorld.h"
#include "Camera.h"

// 必须先于被选中的刚体销毁；使用世界锚点的弹簧关节，不直接修改刚体位置。
class MousePicker
{
public:
    ~MousePicker() { Clear(); }
    MousePicker() = default;
    MousePicker(const MousePicker&) = delete;
    MousePicker& operator=(const MousePicker&) = delete;

    static bool MakeRay(const Camera& camera, double x, double y, int width, int height, glm::vec3& direction)
    {
        if (width <= 0 || height <= 0 || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= width || y >= height) return false;
        glm::vec4 p = glm::inverse(camera.GetProjectionMatrix(float(width) / height) * camera.GetViewMatrix()) *
            glm::vec4(float(2 * x / width - 1), float(1 - 2 * y / height), 1, 1);
        if (std::abs(p.w) < 0.000001f) return false;
        direction = glm::normalize(glm::vec3(p) / p.w - camera.position);
        return std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
    }

    bool Begin(PhysicsWorld& world, glm::vec3 origin, glm::vec3 direction)
    {
        using namespace physx;
        Clear();
        PxVec3 ray(direction.x, direction.y, direction.z);
        if (!ray.isFinite() || ray.magnitudeSquared() < 0.000001f) return false;
        ray.normalize();
        PxRaycastBuffer hit;
        // 包括静态地面，使被地面挡住的物体不能穿透选中。
        if (!world.GetScene().raycast(PxVec3(origin.x, origin.y, origin.z), ray, 100.0f, hit) || !hit.hasBlock) return false;
        auto* body = hit.block.actor->is<PxRigidDynamic>();
        if (!body || body->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) return false;
        PxTransform pose = body->getGlobalPose();
        joint = PxD6JointCreate(world.GetPhysics(), nullptr, PxTransform(hit.block.position), body, PxTransform(pose.transformInv(hit.block.position)));
        if (!joint) throw std::runtime_error("Cannot create mouse drag joint.");
        for (int axis = 0; axis < 6; ++axis) joint->setMotion(static_cast<PxD6Axis::Enum>(axis), PxD6Motion::eFREE);
        float mass = body->getMass();
        PxD6JointDrive drive(120.0f * mass, 22.0f * mass, 200.0f * mass);
        joint->setDrive(PxD6Drive::eX, drive);
        joint->setDrive(PxD6Drive::eY, drive);
        joint->setDrive(PxD6Drive::eZ, drive);
        selected = body;
        distance = hit.block.distance;
        body->wakeUp();
        return true;
    }

    void Move(glm::vec3 origin, glm::vec3 direction)
    {
        if (!joint) return;
        glm::vec3 target = origin + direction * distance;
        physx::PxVec3 p(target.x, target.y, target.z);
        if (!p.isFinite()) return;
        joint->setLocalPose(physx::PxJointActorIndex::eACTOR0, physx::PxTransform(p));
        selected->wakeUp();
    }

    void ReleaseDrag() { if (joint) { joint->release(); joint = nullptr; } }
    void Clear() { ReleaseDrag(); selected = nullptr; }
    void Forget(const physx::PxRigidActor* actor) { if (selected == actor) Clear(); }
    const physx::PxRigidActor* GetSelected() const { return selected; }
    bool IsDragging() const { return joint != nullptr; }

private:
    physx::PxRigidDynamic* selected = nullptr;
    physx::PxD6Joint* joint = nullptr;
    float distance = 0;
};

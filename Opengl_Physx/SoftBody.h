#pragma once
#include "PhysicsWorld.h"
#include "SoftMeshLibrary.h"
#include "SoftGpuBuffer.h"
#include <PxDeformableAttachment.h>
#include <cudamanager/PxCudaContext.h>
#include <extensions/PxCudaHelpersExt.h>
#include <map>
#include <array>
#include <cfloat>
#include <algorithm>

class SoftBody
{
public:
    SoftBody(PhysicsWorld& world, SoftMeshLibrary& library, ModelType type, glm::vec3 position, glm::vec3 velocity = glm::vec3(0), float scale = 1, unsigned int resolution = 4)
        : world(world), type(type), cuda(world.GetCuda()), bodyScale(scale)
    {
        using namespace physx;
        if (!cuda) throw std::runtime_error("PhysX GPU is unavailable. Copy PhysXGpu_64.dll beside the executable.");
        if (!std::isfinite(scale) || scale <= 0) throw std::invalid_argument("Invalid soft-body scale.");
        PxVec4* simPositions = nullptr, * velocities = nullptr, * rest = nullptr;
        PxShape* shape = nullptr;
        try
        {
            auto* cooked = library.Get(type, resolution, scale);
            material = world.AcquireSoftMaterial();
            if (!material) throw std::runtime_error("Cannot create soft-body material.");
            ApplyMaterial();
            actor = world.GetPhysics().createDeformableVolume(*cuda);
            if (!actor) throw std::runtime_error("Cannot create soft body.");
            PxTetrahedronMeshGeometry geometry(cooked->getCollisionMesh());
            shape = world.GetPhysics().createShape(geometry, &material, 1, true, PxShapeFlag::eSIMULATION_SHAPE | PxShapeFlag::eVISUALIZATION);
            if (!shape || !actor->attachShape(*shape)) throw std::runtime_error("Cannot attach soft-body shape.");
            shape->release(); shape = nullptr;
            if (!actor->attachSimulationMesh(*cooked->getSimulationMesh(), *cooked->getDeformableVolumeAuxData()))
                throw std::runtime_error("Cannot attach soft simulation mesh.");
            actor->setSolverIterationCounts(8);
            actor->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, true);
            float initialSpeed = PxVec3(velocity.x, velocity.y, velocity.z).magnitude();
            actor->setMaxLinearVelocity(std::max(20.0f, initialSpeed * 1.25f));
            actor->setMaxDepenetrationVelocity(10.0f);
            PxDeformableVolumeExt::allocateAndInitializeHostMirror(*actor, cuda, simPositions, velocities, positions, rest);
            if (!simPositions || !velocities || !positions || !rest) throw std::runtime_error("Cannot allocate soft-body buffers.");
            PxDeformableVolumeExt::transform(*actor, PxTransform(PxVec3(position.x, position.y, position.z)), 1.0f, simPositions, velocities, positions, rest);
            PxDeformableVolumeExt::updateMass(*actor, 100.0f, 50.0f, simPositions);
            for (PxU32 i = 0; i < actor->getSimulationMesh()->getNbVertices(); ++i) velocities[i] = PxVec4(velocity.x, velocity.y, velocity.z, 0);
            PxDeformableVolumeExt::copyToDevice(*actor, PxDeformableVolumeDataFlag::eALL, simPositions, velocities, positions, rest);
            world.GetScene().addActor(*actor);
            BuildSurface();
            UpdateSurface();
            mesh = std::make_unique<Mesh>(vertices, indices);
            gpu = std::make_unique<SoftGpuBuffer>(*cuda, *mesh, vertices, indices);
            revision = world.GetSimulationRevision();
            cpuRevision = revision;
            birthRevision = revision;
        }
        catch (...)
        {
            if (shape) shape->release();
            Free(simPositions); Free(velocities); Free(rest);
            Release(); throw;
        }
        Free(simPositions); Free(velocities); Free(rest);
    }
    ~SoftBody() { Release(); }
    SoftBody(const SoftBody&) = delete;
    SoftBody& operator=(const SoftBody&) = delete;
    ModelType GetModelType() const { return type; }
    const Mesh& GetMesh() const { return *mesh; }
    glm::vec3 GetPosition() const
    {
        if (world.GetSimulationRevision() == birthRevision) return center;
        auto p = actor->getWorldBounds().getCenter();
        return glm::vec3(p.x, p.y, p.z);
    }
    physx::PxBounds3 GetBounds() const
    {
        if (world.GetSimulationRevision() != birthRevision) return actor->getWorldBounds(1.0f);
        auto bounds = physx::PxBounds3::empty();
        for (const auto& v : vertices) bounds.include(physx::PxVec3(v.x, v.y, v.z));
        return bounds;
    }

    const std::vector<Vertex>& GetVertices() const { const_cast<SoftBody*>(this)->ReadForPicking(); return vertices; }
    const std::vector<unsigned int>& GetIndices() const { return indices; }
    physx::PxDeformableVolumeMaterial& GetPhysicalMaterial() { return *material; }
    float GetBaseStiffness() const { return baseStiffness; }

    void SetMaterial(float stiffness, float poisson)
    {
        if (!std::isfinite(stiffness) || !std::isfinite(poisson)) return;
        baseStiffness = std::clamp(stiffness, 0.001f, 10.0f);
        ApplyMaterial();
        material->setPoissons(std::clamp(poisson, 0.0f, 0.49f));
        actor->setWakeCounter(0.4f);
    }

    void SetSimulationSettings(unsigned int iterations, bool selfCollision)
    {
        actor->setSolverIterationCounts(std::clamp(iterations, 4u, 16u));
        actor->setDeformableBodyFlag(physx::PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, !selfCollision);
        actor->setWakeCounter(0.4f);
    }

    std::size_t GetSimulationTetrahedronCount() const { return actor->getSimulationMesh()->getNbTetrahedrons(); }

    void Sync()
    {
        if (revision == world.GetSimulationRevision()) return;
        gpu->Update(actor->getPositionInvMassBufferD());
        revision = world.GetSimulationRevision();
    }

    static void SyncBatch(const std::vector<SoftBody*>& bodies)
    {
        std::vector<SoftGpuBuffer::UpdateRequest> requests;
        for (auto* body : bodies)
            if (body->revision != body->world.GetSimulationRevision())
                requests.push_back({ body->gpu.get(),body->actor->getPositionInvMassBufferD() });
        SoftGpuBuffer::UpdateBatch(requests);
        for (auto* body : bodies) body->revision = body->world.GetSimulationRevision();
    }

    void BeginDrag(glm::vec3 origin, glm::vec3 direction, float distance)
    {
        using namespace physx;
        StopDrag();
        const PxU32 count = actor->getSimulationMesh()->getNbVertices();
        if (!dragPositions) dragPositions = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, *cuda, count);
        if (!dragPositions) throw std::runtime_error("Cannot allocate soft picking buffer.");
        {
            PxScopedCudaLock lock(*cuda);
            if (cuda->getCudaContext()->memcpyDtoH(dragPositions, reinterpret_cast<CUdeviceptr>(actor->getSimPositionInvMassBufferD()), count * sizeof(PxVec4)) != 0)
                throw std::runtime_error("Cannot read soft drag patch.");
        }
        dragDistance = distance;
        dragTarget = dragDesired = origin + direction * distance;
        dragMass = 0;
        std::vector<std::pair<float, PxU32>> nearest;
        for (PxU32 i = 0; i < count; ++i)
        {
            auto& p = dragPositions[i];
            if (p.w <= 0) continue;
            auto delta = glm::vec3(p.x, p.y, p.z) - dragTarget;
            nearest.push_back({ glm::dot(delta,delta),i });
        }
        if (nearest.empty()) return;
        std::sort(nearest.begin(), nearest.end());
        std::vector<PxU32> ids;
        std::vector<PxVec4> offsets;
        float patchRadiusSquared = 0.0001f;
        const std::size_t pointCount = std::min<std::size_t>(16, nearest.size());
        for (std::size_t i = 0; i < pointCount; ++i)
        {
            auto& candidate = nearest[i];
            auto& p = dragPositions[candidate.second];
            auto offset = glm::vec3(p.x, p.y, p.z) - dragTarget;
            ids.push_back(candidate.second);
            offsets.emplace_back(offset.x, offset.y, offset.z, 0.0f);
            dragMass += 1.0f / p.w;
            patchRadiusSquared = std::max(patchRadiusSquared, candidate.first);
        }
        try
        {
            PxTransform pose(PxVec3(dragTarget.x, dragTarget.y, dragTarget.z));
            dragAnchor = world.GetPhysics().createRigidDynamic(pose);
            if (!dragAnchor) throw std::runtime_error("Cannot create soft drag anchor.");
            float mass = std::max(dragMass, 0.001f);
            dragAnchor->setMass(mass);
            dragAnchor->setMassSpaceInertiaTensor(PxVec3(std::max(0.4f * mass * patchRadiusSquared, 0.00001f)));
            dragAnchor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
            dragAnchor->setMaxLinearVelocity(100.0f);
            dragAnchor->setMaxAngularVelocity(20.0f);
            world.GetScene().addActor(*dragAnchor);
            dragJoint = PxD6JointCreate(world.GetPhysics(), nullptr, pose, dragAnchor, PxTransform(PxIdentity));
            if (!dragJoint) throw std::runtime_error("Cannot create soft drag joint.");
            for (int axis = 0; axis < 6; ++axis) dragJoint->setMotion(static_cast<PxD6Axis::Enum>(axis), PxD6Motion::eFREE);
            PxDeformableAttachmentData desc;
            desc.actor[0] = actor;
            desc.type[0] = PxDeformableAttachmentTargetType::eVERTEX;
            desc.indices[0].data = ids.data();
            desc.indices[0].count = static_cast<PxU32>(ids.size());
            desc.actor[1] = dragAnchor;
            desc.type[1] = PxDeformableAttachmentTargetType::eRIGID;
            desc.coords[1].data = offsets.data();
            desc.coords[1].count = static_cast<PxU32>(offsets.size());
            dragAttachment = world.GetPhysics().createDeformableAttachment(desc);
            if (!dragAttachment) throw std::runtime_error("Cannot attach soft drag region.");
            UpdateDrag(1.0f / 60.0f);
        }
        catch (...) { StopDrag(); throw; }
        actor->setWakeCounter(0.4f);
    }

    void MoveDrag(glm::vec3 origin, glm::vec3 direction)
    {
        auto target = origin + direction * dragDistance;
        if (std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z)) dragDesired = target;
    }

    void UpdateDrag(float deltaTime)
    {
        if (!IsDragging() || !std::isfinite(deltaTime) || deltaTime <= 0) return;
        dragTarget = dragDesired;
        float mass = dragMass + dragAnchor->getMass();
        physx::PxD6JointDrive drive(120.0f * mass, 22.0f * mass, 200.0f * mass);
        dragJoint->setDrive(physx::PxD6Drive::eX, drive);
        dragJoint->setDrive(physx::PxD6Drive::eY, drive);
        dragJoint->setDrive(physx::PxD6Drive::eZ, drive);
        dragJoint->setLocalPose(physx::PxJointActorIndex::eACTOR0, physx::PxTransform(physx::PxVec3(dragTarget.x, dragTarget.y, dragTarget.z)));
        dragAnchor->wakeUp();
        actor->setWakeCounter(0.4f);
    }

    void StopDrag()
    {
        if (dragAttachment) { dragAttachment->release(); dragAttachment = nullptr; }
        if (dragJoint) { dragJoint->release(); dragJoint = nullptr; }
        if (dragAnchor) { dragAnchor->release(); dragAnchor = nullptr; }
        if (actor) actor->setWakeCounter(0.4f);
    }

    bool IsDragging() const { return dragAttachment && dragJoint; }

    bool Raycast(glm::vec3 origin, glm::vec3 direction, float& distance) const
    {
        const_cast<SoftBody*>(this)->ReadForPicking();
        bool found = false;
        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            auto point = [&](unsigned int n) {auto& v = vertices[n]; return glm::vec3(v.x, v.y, v.z); };
            auto a = point(indices[i]), e1 = point(indices[i + 1]) - a, e2 = point(indices[i + 2]) - a;
            auto p = glm::cross(direction, e2); float determinant = glm::dot(e1, p);
            if (std::abs(determinant) < 1e-7f) continue;
            float inverse = 1 / determinant;
            auto t = origin - a; float u = glm::dot(t, p) * inverse;
            if (u < 0 || u>1) continue;
            auto q = glm::cross(t, e1); float v = glm::dot(direction, q) * inverse;
            if (v < 0 || u + v>1) continue;
            float d = glm::dot(e2, q) * inverse;
            if (d >= 0 && d < distance) { distance = d; found = true; }
        }
        return found;
    }
private:
    PhysicsWorld& world;
    ModelType type;
    physx::PxCudaContextManager* cuda = nullptr;
    float bodyScale = 1.0f;
    float baseStiffness = 0.2f;
    physx::PxDeformableVolume* actor = nullptr;
    physx::PxDeformableVolumeMaterial* material = nullptr;
    physx::PxVec4* positions = nullptr;
    physx::PxVec4* dragPositions = nullptr;
    physx::PxRigidDynamic* dragAnchor = nullptr;
    physx::PxD6Joint* dragJoint = nullptr;
    physx::PxDeformableAttachment* dragAttachment = nullptr;
    glm::vec3 dragTarget = glm::vec3(0), dragDesired = glm::vec3(0);
    float dragDistance = 0, dragMass = 0;
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<SoftGpuBuffer> gpu;
    glm::vec3 center = glm::vec3(0);
    unsigned long long revision = 0, cpuRevision = 0, birthRevision = 0;

    void Free(physx::PxVec4* memory) { if (memory) PX_EXT_PINNED_MEMORY_FREE(*cuda, memory); }
    void ApplyMaterial()
    {
        material->setYoungsModulus(baseStiffness * 100000.0f * bodyScale);
    }
    void Release()
    {
        StopDrag();
        gpu.reset();
        mesh.reset();
        if (actor) { actor->release(); actor = nullptr; }
        Free(positions); positions = nullptr;
        Free(dragPositions); dragPositions = nullptr;
        if (material) { world.ReturnSoftMaterial(material); material = nullptr; }
    }
    void ReadForPicking()
    {
        if (cpuRevision == world.GetSimulationRevision()) return;
        physx::PxScopedCudaLock lock(*cuda);
        auto result = cuda->getCudaContext()->memcpyDtoH(positions, reinterpret_cast<CUdeviceptr>(actor->getPositionInvMassBufferD()),
            actor->getCollisionMesh()->getNbVertices() * sizeof(physx::PxVec4));
        if (result != 0) throw std::runtime_error("Cannot read soft-body selection geometry.");
        UpdateSurface();
        cpuRevision = world.GetSimulationRevision();
    }

    void BuildSurface()
    {
        using namespace physx;
        auto* tetrahedra = actor->getCollisionMesh();
        auto* raw = tetrahedra->getTetrahedrons();
        bool indices16Bit = tetrahedra->getTetrahedronMeshFlags().isSet(PxTetrahedronMeshFlag::e16_BIT_INDICES);
        auto read = [&](PxU32 i) {return indices16Bit ? PxU32(static_cast<const PxU16*>(raw)[i]) : static_cast<const PxU32*>(raw)[i]; };
        struct Face { std::array<PxU32, 3> oriented; int count = 0; };
        std::map<std::array<PxU32, 3>, Face> faces;
        const auto* rest = tetrahedra->getVertices();
        for (PxU32 t = 0; t < tetrahedra->getNbTetrahedrons(); ++t)
        {
            PxU32 v[4] = { read(t * 4),read(t * 4 + 1),read(t * 4 + 2),read(t * 4 + 3) };
            for (int opposite = 0; opposite < 4; ++opposite)
            {
                std::array<PxU32, 3> face = {}; int k = 0;
                for (int j = 0; j < 4; ++j) if (j != opposite) face[k++] = v[j];
                if ((rest[face[1]] - rest[face[0]]).cross(rest[face[2]] - rest[face[0]]).dot(rest[v[opposite]] - rest[face[0]]) > 0)
                    std::swap(face[1], face[2]);
                auto key = face; std::sort(key.begin(), key.end());
                auto& value = faces[key]; value.oriented = face; ++value.count;
            }
        }
        for (auto& pair : faces) if (pair.second.count == 1)
            indices.insert(indices.end(), pair.second.oriented.begin(), pair.second.oriented.end());
        vertices.resize(tetrahedra->getNbVertices());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            vertices[i] = { rest[i].x,rest[i].y,rest[i].z,1,1,1,0,0,0,rest[i].x + 0.5f,rest[i].z + 0.5f };
        }
        if (indices.empty()) throw std::runtime_error("Soft body has no surface.");
    }
    void UpdateSurface()
    {
        center = glm::vec3(0);
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            auto& p = positions[i]; auto& v = vertices[i];
            v.x = p.x; v.y = p.y; v.z = p.z; v.nx = v.ny = v.nz = 0;
            center += glm::vec3(p.x, p.y, p.z);
        }
        center /= static_cast<float>(vertices.size());
        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            auto& a = vertices[indices[i]]; auto& b = vertices[indices[i + 1]]; auto& c = vertices[indices[i + 2]];
            auto n = glm::cross(glm::vec3(b.x - a.x, b.y - a.y, b.z - a.z), glm::vec3(c.x - a.x, c.y - a.y, c.z - a.z));
            for (int j = 0; j < 3; ++j) { auto& v = vertices[indices[i + j]]; v.nx += n.x; v.ny += n.y; v.nz += n.z; }
        }
        for (auto& v : vertices)
        {
            float length = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
            if (length > 1e-8f) { v.nx /= length; v.ny /= length; v.nz /= length; }
            else v.ny = 1;
        }
    }
};

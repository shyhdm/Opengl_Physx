#pragma once
#include <PxPhysicsAPI.h>
#include <gpu/PxGpu.h>
#include <cudamanager/PxCudaContextManager.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "CollisionLibrary.h"
#include <memory>
#include <functional>
#include <chrono>
#include <vector>

// 一个程序先创建一个物理世界；刚体必须在世界销毁之前销毁。
class PhysicsWorld
{
public:
    explicit PhysicsWorld(bool useGpu = true)
    {
        using namespace physx;
        try
        {
            foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errors);
            if (!foundation) throw std::runtime_error("Cannot create PhysX foundation.");
            physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale());
            if (!physics) throw std::runtime_error("Cannot create PhysX physics.");
            extensions = PxInitExtensions(*physics, nullptr);
            if (!extensions) throw std::runtime_error("Cannot initialize PhysX extensions.");
            PxCudaContextManagerDesc cudaDesc;
            if (useGpu) cuda = PxCreateCudaContextManager(*foundation, cudaDesc, PxGetProfilerCallback());
            if (cuda && !cuda->contextIsValid()) { cuda->release(); cuda = nullptr; }
            collisions = std::make_unique<CollisionLibrary>(*physics);
            dispatcher = PxDefaultCpuDispatcherCreate(2);
            if (!dispatcher) throw std::runtime_error("Cannot create PhysX dispatcher.");
            PxSceneDesc description(physics->getTolerancesScale());
            description.gravity = PxVec3(0.0f, -9.81f, 0.0f);
            description.cpuDispatcher = dispatcher;
            description.filterShader = Filter;
            description.flags |= PxSceneFlag::eENABLE_CCD;
            description.solverType = PxSolverType::eTGS;
            if (cuda)
            {
                description.cudaContextManager = cuda;
                description.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS | PxSceneFlag::eENABLE_PCM;
                description.broadPhaseType = PxBroadPhaseType::eGPU;
                description.solverType = PxSolverType::eTGS;
                description.flags |= PxSceneFlag::eENABLE_EXTERNAL_FORCES_EVERY_ITERATION_TGS;
            }
            scene = physics->createScene(description);
            if (!scene) throw std::runtime_error("Cannot create PhysX scene.");
            material = physics->createMaterial(0.6f, 0.5f, 0.15f);
            if (!material) throw std::runtime_error("Cannot create PhysX material.");
            // 地面厚 1 米，顶面位于 y = 0，与显示的地面一致。
            ground = PxCreateStatic(*physics, PxTransform(PxVec3(0.0f, -0.5f, 0.0f)), PxBoxGeometry(10.0f, 0.5f, 10.0f), *material);
            if (!ground) throw std::runtime_error("Cannot create PhysX ground.");
            scene->addActor(*ground);
        }
        catch (...)
        {
            Release();
            throw;
        }
    }

    ~PhysicsWorld() { Release(); }
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // 渲染帧率可以变化，物理始终按每秒 60 步计算。
    void Update(float deltaTime, const std::function<void(float)>& beforeStep = {})
    {
        lastSimulationMs = 0; lastSteps = 0;
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) return;
        accumulator += std::min(static_cast<double>(deltaTime), 0.1);
        while (accumulator >= step)
        {
            if (beforeStep) beforeStep(static_cast<float>(step));
            auto started = std::chrono::steady_clock::now();
            scene->simulate(static_cast<float>(step));
            scene->fetchResults(true);
            lastSimulationMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            ++lastSteps;
            ++simulationRevision;
            accumulator -= step;
        }
    }

    void SingleStep()
    {
        scene->simulate(static_cast<float>(step));
        scene->fetchResults(true);
        ++simulationRevision;
        accumulator = 0;
    }
    double GetLastSimulationMs() const { return lastSimulationMs; }
    unsigned int GetLastSteps() const { return lastSteps; }

    void ClearAccumulator() { accumulator = 0; }

    physx::PxDeformableVolumeMaterial* AcquireSoftMaterial()
    {
        physx::PxDeformableVolumeMaterial* value = nullptr;
        if (freeSoftMaterials.empty())
        {
            value = physics->createDeformableVolumeMaterial(20000.0f, 0.35f, 0.2f, 0.05f);
            if (!value) throw std::runtime_error("Cannot create soft-body material.");
            try
            {
                if (freeSoftMaterials.capacity() < softMaterials.size() + 1)
                    freeSoftMaterials.reserve(std::max<std::size_t>(8, freeSoftMaterials.capacity() * 2));
                softMaterials.push_back(value);
            }
            catch (...) { value->release(); throw; }
        }
        else
        {
            value = freeSoftMaterials.back();
            freeSoftMaterials.pop_back();
            value->setYoungsModulus(20000.0f);
            value->setPoissons(0.35f);
            value->setDynamicFriction(0.2f);
            value->setElasticityDamping(0.05f);
        }
        return value;
    }

    void ReturnSoftMaterial(physx::PxDeformableVolumeMaterial* value)
    {
        if (value) freeSoftMaterials.push_back(value);
    }

    physx::PxPhysics& GetPhysics() { return *physics; }
    physx::PxScene& GetScene() { return *scene; }
    physx::PxMaterial& GetMaterial() { return *material; }
    physx::PxCudaContextManager* GetCuda() const { return cuda; }
    unsigned long long GetSimulationRevision() const { return simulationRevision; }
    CollisionLibrary& GetCollisions() { return *collisions; }

private:
    physx::PxDefaultAllocator allocator;
    physx::PxDefaultErrorCallback errors;
    physx::PxFoundation* foundation = nullptr;
    physx::PxPhysics* physics = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher = nullptr;
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* material = nullptr;
    physx::PxRigidStatic* ground = nullptr;
    std::unique_ptr<CollisionLibrary> collisions;
    physx::PxCudaContextManager* cuda = nullptr;
    std::vector<physx::PxDeformableVolumeMaterial*> softMaterials, freeSoftMaterials;
    bool extensions = false;
    unsigned long long simulationRevision = 0;
    double lastSimulationMs = 0;
    unsigned int lastSteps = 0;
    double accumulator = 0.0;
    static constexpr double step = 1.0 / 60.0;

    static physx::PxFilterFlags Filter(physx::PxFilterObjectAttributes a, physx::PxFilterData ad, physx::PxFilterObjectAttributes b, physx::PxFilterData bd, physx::PxPairFlags& pair, const void* data, physx::PxU32 size)
    {
        auto flags = physx::PxDefaultSimulationFilterShader(a, ad, b, bd, pair, data, size);
        if (!physx::PxFilterObjectIsTrigger(a) && !physx::PxFilterObjectIsTrigger(b)) pair |= physx::PxPairFlag::eDETECT_CCD_CONTACT;
        return flags;
    }

    void Release()
    {
        if (ground) { ground->release(); ground = nullptr; }
        if (scene) { scene->release(); scene = nullptr; }
        if (dispatcher) { dispatcher->release(); dispatcher = nullptr; }
        if (material) { material->release(); material = nullptr; }
        for (auto* value : softMaterials) value->release();
        softMaterials.clear();
        freeSoftMaterials.clear();
        collisions.reset();
        if (extensions) { PxCloseExtensions(); extensions = false; }
        if (physics) { physics->release(); physics = nullptr; }
        if (cuda) { cuda->release(); cuda = nullptr; }
        if (foundation) { foundation->release(); foundation = nullptr; }
    }
};

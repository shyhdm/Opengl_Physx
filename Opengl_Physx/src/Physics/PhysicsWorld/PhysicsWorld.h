#pragma once
#include <PxPhysicsAPI.h>
#include <gpu/PxGpu.h>
#include <cudamanager/PxCudaContextManager.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include "CollisionLibrary.h"
#include "../Particle/SharedParticleSimulation.h"

class PhysicsWorld
{
public:
    std::shared_ptr<SharedParticleSimulation> AcquireParticleSimulation() {
        auto shared = particleSimulation_.lock();
        if (!shared) {
            if (!cuda) throw std::runtime_error("GPU particles require CUDA");
            shared = std::make_shared<SharedParticleSimulation>(*physics, *scene, *cuda, particleConfigurations_);
            particleSimulation_ = shared;
        }
        return shared;
    }
    // Settings survive releasing the last particle buffer, without retaining GPU allocations.
    void ConfigureParticleSimulation(bool sand, float spacing, float adhesion, float adhesionScale, float adhesionRadius, bool active) {
        particleConfigurations_[sand ? 1 : 0] = { spacing, adhesion, adhesionScale, adhesionRadius };
        if (auto shared = particleSimulation_.lock()) shared->Configure(sand, spacing, adhesion, adhesionScale, adhesionRadius, active);
    }
    static constexpr unsigned ParticleLimit = 1000000;
    unsigned TotalParticleCount() const {
        unsigned total = 0;
        for (const auto& entry : particleCounts_) total += entry.second;
        return total;
    }
    void SetParticleCount(const void* owner, unsigned count) {
        if (count) particleCounts_[owner] = count;
        else particleCounts_.erase(owner);
    }
    struct Impact
    {
        physx::PxVec3 position{ 0 };
        physx::PxVec3 normal{ 0,1,0 };
        physx::PxVec3 impulse{ 0 };
        physx::PxVec3 relativeVelocity{ 0 };
        float impulseMagnitude = 0.0f;
        float velocityChange = 0.0f;
        float colliderVolume = 0.0f;
    };

    explicit PhysicsWorld(bool useGpu = true, bool particleSolver = false)
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
            scene = CreateScene(particleSolver);
            material = physics->createMaterial(0.6f, 0.5f, 0.15f);
            if (!material) throw std::runtime_error("Cannot create PhysX material.");
            ground = PxCreateStatic(*physics, PxTransform(PxVec3(0.0f, -0.5f, 0.0f)), PxBoxGeometry(10.0f, 0.5f, 10.0f), *material);
            if (!ground) throw std::runtime_error("Cannot create PhysX ground.");
            scene->addActor(*ground);
        }
        catch (...) { Release(); throw; }
    }

    ~PhysicsWorld() { Release(); }
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Call after removing the old preset's bodies and detaching its liquid actors.
    // Rebuild only PxScene; the physics SDK, CUDA context and user particle storage survive.
    void SetParticleSolver(bool particleSolver)
    {
        using namespace physx;
        const auto wanted = particleSolver ? PxSolverType::ePGS : PxSolverType::eTGS;
        if (scene->getSolverType() == wanted) return;
        if (scene->getNbPBDParticleSystems() ||
            scene->getNbActors(PxActorTypeFlag::eRIGID_STATIC | PxActorTypeFlag::eRIGID_DYNAMIC) != 1)
            throw std::runtime_error("Clear scene actors before changing the solver.");
        auto* replacement = CreateScene(particleSolver);
        scene->removeActor(*ground);
        replacement->addActor(*ground);
        auto* previous = scene;
        scene = replacement;
        previous->release();
        contacts.Clear();
        accumulator = 0;
        lastSimulationMs = 0;
        lastSteps = 0;
    }

    void Update(float deltaTime, const std::function<void(float)>& beforeStep = {})
    {
        lastSimulationMs = lastSubmitMs = lastFetchMs = lastParticleFetchMs = 0; lastSteps = 0; contacts.Clear();
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) return;
        accumulator += std::min(static_cast<double>(deltaTime), step);
        while (accumulator >= step)
        {
            if (beforeStep) beforeStep(static_cast<float>(step));
            auto started = std::chrono::steady_clock::now();
            scene->simulate(static_cast<float>(step));
            auto submitted = std::chrono::steady_clock::now();
            scene->fetchResults(true);
            auto fetched = std::chrono::steady_clock::now();
            if (cuda && scene->getNbPBDParticleSystems()) scene->fetchResultsParticleSystem();
            auto finished = std::chrono::steady_clock::now();
            lastSubmitMs += std::chrono::duration<double, std::milli>(submitted - started).count();
            lastFetchMs += std::chrono::duration<double, std::milli>(fetched - submitted).count();
            lastParticleFetchMs += std::chrono::duration<double, std::milli>(finished - fetched).count();
            lastSimulationMs += std::chrono::duration<double, std::milli>(finished - started).count();
            ++lastSteps; ++simulationRevision; accumulator -= step;
        }
    }

    void SingleStep()
    {
        contacts.Clear();
        scene->simulate(static_cast<float>(step));
        scene->fetchResults(true);
        if (cuda && scene->getNbPBDParticleSystems()) scene->fetchResultsParticleSystem();
        ++simulationRevision; accumulator = 0;
    }

    bool GetStrongestImpact(const physx::PxRigidActor* actor, Impact& result) const
    {
        std::lock_guard<std::mutex> lock(contacts.mutex);
        bool found = false;
        for (const auto& event : contacts.events)
            if (event.actor == actor && (!found || event.impact.impulseMagnitude > result.impulseMagnitude))
            {
                result = event.impact;
                found = true;
            }
        return found;
    }

    double GetLastSubmitMs() const { return lastSubmitMs; }
    double GetLastFetchMs() const { return lastFetchMs; }
    double GetLastParticleFetchMs() const { return lastParticleFetchMs; }
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
                if (freeSoftMaterials.capacity() < softMaterials.size() + 1) freeSoftMaterials.reserve(std::max<std::size_t>(8, freeSoftMaterials.capacity() * 2));
                softMaterials.push_back(value);
            }
            catch (...) { value->release(); throw; }
        }
        else
        {
            value = freeSoftMaterials.back(); freeSoftMaterials.pop_back();
            value->setYoungsModulus(20000.0f); value->setPoissons(0.35f); value->setDynamicFriction(0.2f); value->setElasticityDamping(0.05f);
        }
        return value;
    }

    void ReturnSoftMaterial(physx::PxDeformableVolumeMaterial* value) { if (value) freeSoftMaterials.push_back(value); }
    physx::PxPhysics& GetPhysics() { return *physics; }
    physx::PxScene& GetScene() { return *scene; }
    physx::PxMaterial& GetMaterial() { return *material; }
    physx::PxCudaContextManager* GetCuda() const { return cuda; }
    unsigned long long GetSimulationRevision() const { return simulationRevision; }
    CollisionLibrary& GetCollisions() { return *collisions; }
    static constexpr physx::PxU32 fractureFilterTag = 0x46524143u;

private:
    class ContactCollector final : public physx::PxSimulationEventCallback
    {
    public:
        struct Event { const physx::PxRigidActor* actor = nullptr; Impact impact; };
        std::vector<Event> events;
        mutable std::mutex mutex;
        void Clear() { std::lock_guard<std::mutex> lock(mutex); events.clear(); }
        void onConstraintBreak(physx::PxConstraintInfo*, physx::PxU32) override {}
        void onWake(physx::PxActor**, physx::PxU32) override {}
        void onSleep(physx::PxActor**, physx::PxU32) override {}
        void onTrigger(physx::PxTriggerPair*, physx::PxU32) override {}
        void onAdvance(const physx::PxRigidBody* const*, const physx::PxTransform*, const physx::PxU32) override {}
        void onContact(const physx::PxContactPairHeader& header, const physx::PxContactPair* pairs, physx::PxU32 pairCount) override
        {
            using namespace physx;
            if (!header.actors[0] || !header.actors[1]) return;
            float effectiveMass = std::min(Mass(header.actors[0]), Mass(header.actors[1]));
            if (!std::isfinite(effectiveMass)) return;
            PxVec3 relativeVelocity = Velocity(header.actors[0]) - Velocity(header.actors[1]);
            std::lock_guard<std::mutex> lock(mutex);
            for (PxU32 pairIndex = 0; pairIndex < pairCount; ++pairIndex)
            {
                const PxContactPair& pair = pairs[pairIndex];
                if (pair.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;
                bool fractureActor0 = pair.shapes[0] && pair.shapes[0]->getSimulationFilterData().word0 == fractureFilterTag;
                bool fractureActor1 = pair.shapes[1] && pair.shapes[1]->getSimulationFilterData().word0 == fractureFilterTag;
                if (!fractureActor0 && !fractureActor1) continue;
                PxContactPairPoint points[32];
                PxU32 count = pair.extractContacts(points, 32);
                PxVec3 totalImpulse(0.0f), weightedPosition(0.0f), weightedNormal(0.0f);
                float impulseMagnitude = 0.0f;
                for (PxU32 i = 0; i < count; ++i)
                {
                    float magnitude = points[i].impulse.magnitude();
                    if (!std::isfinite(magnitude) || magnitude <= 0.0001f) continue;
                    totalImpulse += points[i].impulse;
                    weightedPosition += points[i].position * magnitude;
                    weightedNormal += points[i].normal * magnitude;
                    impulseMagnitude += magnitude;
                }
                if (!std::isfinite(impulseMagnitude) || impulseMagnitude <= 0.0001f) continue;
                PxVec3 position = weightedPosition / impulseMagnitude;
                PxVec3 normal = weightedNormal;
                if (normal.normalize() <= 0.0001f) normal = PxVec3(0.0f, 1.0f, 0.0f);
                float velocityChange = impulseMagnitude / std::max(effectiveMass, 0.01f);
                float colliderVolume0 = ColliderVolume(header.actors[0]);
                float colliderVolume1 = ColliderVolume(header.actors[1]);
                if (fractureActor0) events.push_back({ header.actors[0]->is<PxRigidActor>(),{position,normal,totalImpulse,relativeVelocity,impulseMagnitude,velocityChange,colliderVolume1} });
                if (fractureActor1) events.push_back({ header.actors[1]->is<PxRigidActor>(),{position,-normal,-totalImpulse,-relativeVelocity,impulseMagnitude,velocityChange,colliderVolume0} });
            }
        }
    private:
        static float Mass(const physx::PxActor* actor)
        {
            const auto* dynamic = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
            if (!dynamic || dynamic->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC)) return std::numeric_limits<float>::infinity();
            return std::max(dynamic->getMass(), 0.01f);
        }
        static physx::PxVec3 Velocity(const physx::PxActor* actor)
        {
            const auto* dynamic = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
            return dynamic && !dynamic->getRigidBodyFlags().isSet(physx::PxRigidBodyFlag::eKINEMATIC) ? dynamic->getLinearVelocity() : physx::PxVec3(0.0f);
        }
        static float ColliderVolume(const physx::PxActor* actor)
        {
            using namespace physx;
            const auto* dynamic = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
            if (!dynamic || dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) return 0.0f;
            PxShape* shapes[32]{};
            PxU32 shapeCount = dynamic->getShapes(shapes, 32);
            float volume = 0.0f;
            constexpr float pi = 3.14159265358979323846f;
            for (PxU32 index = 0; index < shapeCount; ++index)
            {
                PxGeometryHolder geometry = shapes[index]->getGeometry();
                switch (geometry.getType())
                {
                case PxGeometryType::eBOX:
                {
                    const PxVec3& half = geometry.box().halfExtents;
                    volume += 8.0f * half.x * half.y * half.z;
                    break;
                }
                case PxGeometryType::eSPHERE:
                {
                    float radius = geometry.sphere().radius;
                    volume += 4.0f * pi * radius * radius * radius / 3.0f;
                    break;
                }
                case PxGeometryType::eCAPSULE:
                {
                    const PxCapsuleGeometry& capsule = geometry.capsule();
                    volume += pi * capsule.radius * capsule.radius * (2.0f * capsule.halfHeight) + 4.0f * pi * capsule.radius * capsule.radius * capsule.radius / 3.0f;
                    break;
                }
                case PxGeometryType::eCONVEXMESH:
                {
                    const PxConvexMeshGeometry& convex = geometry.convexMesh();
                    float unitMass = 0.0f; PxMat33 inertia(PxIdentity); PxVec3 center(0.0f);
                    if (convex.convexMesh) convex.convexMesh->getMassInformation(unitMass, inertia, center);
                    const PxVec3& scale = convex.scale.scale;
                    volume += std::abs(unitMass * scale.x * scale.y * scale.z);
                    break;
                }
                default: break;
                }
            }
            if (volume <= 0.0f)
            {
                PxVec3 extents = dynamic->getWorldBounds(1.0f).getExtents();
                volume = 8.0f * extents.x * extents.y * extents.z;
            }
            return std::isfinite(volume) ? std::clamp(volume, 0.0f, 4096.0f) : 0.0f;
        }
    };

    physx::PxDefaultAllocator allocator;
    physx::PxDefaultErrorCallback errors;
    ContactCollector contacts;
    physx::PxFoundation* foundation = nullptr;
    physx::PxPhysics* physics = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher = nullptr;
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* material = nullptr;
    std::unordered_map<const void*, unsigned> particleCounts_;
    SharedParticleSimulation::Configurations particleConfigurations_ = SharedParticleSimulation::Defaults();
    std::weak_ptr<SharedParticleSimulation> particleSimulation_;
    physx::PxRigidStatic* ground = nullptr;
    std::unique_ptr<CollisionLibrary> collisions;
    physx::PxCudaContextManager* cuda = nullptr;
    std::vector<physx::PxDeformableVolumeMaterial*> softMaterials, freeSoftMaterials;
    bool extensions = false;
    unsigned long long simulationRevision = 0;
    double lastSubmitMs = 0, lastFetchMs = 0, lastParticleFetchMs = 0;
    double lastSimulationMs = 0, accumulator = 0.0;
    unsigned int lastSteps = 0;
    static constexpr double step = 1.0 / 60.0;

    static physx::PxFilterFlags Filter(physx::PxFilterObjectAttributes a, physx::PxFilterData ad, physx::PxFilterObjectAttributes b, physx::PxFilterData bd, physx::PxPairFlags& pair, const void* data, physx::PxU32 size)
    {
        if (physx::PxFilterObjectIsTrigger(a) || physx::PxFilterObjectIsTrigger(b))
        {
            pair = physx::PxPairFlag::eTRIGGER_DEFAULT;
            return physx::PxFilterFlag::eDEFAULT;
        }
        pair = physx::PxPairFlag::eCONTACT_DEFAULT | physx::PxPairFlag::eDETECT_CCD_CONTACT;
        if (ad.word0 == fractureFilterTag || bd.word0 == fractureFilterTag) pair |= physx::PxPairFlag::eNOTIFY_TOUCH_FOUND | physx::PxPairFlag::eNOTIFY_CONTACT_POINTS;
        return physx::PxFilterFlag::eDEFAULT;
    }

    physx::PxScene* CreateScene(bool particleSolver)
    {
        using namespace physx;
        PxSceneDesc description(physics->getTolerancesScale());
        description.gravity = PxVec3(0.0f, -9.81f, 0.0f);
        description.cpuDispatcher = dispatcher;
        description.filterShader = Filter;
        description.simulationEventCallback = &contacts;
        description.flags |= PxSceneFlag::eENABLE_CCD;
        description.solverType = particleSolver ? PxSolverType::ePGS : PxSolverType::eTGS;
        if (cuda)
        {
            description.cudaContextManager = cuda;
            description.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS | PxSceneFlag::eENABLE_PCM;
            description.broadPhaseType = PxBroadPhaseType::eGPU;
        }
        auto* result = physics->createScene(description);
        if (!result) throw std::runtime_error("Cannot create PhysX scene.");
        return result;
    }

    void Release()
    {
        if (ground) { ground->release(); ground = nullptr; }
        if (scene) { scene->release(); scene = nullptr; }
        if (dispatcher) { dispatcher->release(); dispatcher = nullptr; }
        if (material) { material->release(); material = nullptr; }
        for (auto* value : softMaterials) value->release();
        softMaterials.clear(); freeSoftMaterials.clear(); collisions.reset();
        if (extensions) { PxCloseExtensions(); extensions = false; }
        if (physics) { physics->release(); physics = nullptr; }
        if (cuda) { cuda->release(); cuda = nullptr; }
        if (foundation) { foundation->release(); foundation = nullptr; }
    }
};

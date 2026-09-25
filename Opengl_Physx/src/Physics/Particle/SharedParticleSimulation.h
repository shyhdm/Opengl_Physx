#pragma once
#include <PxPhysicsAPI.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

// Shared simulation only: each liquid/sand owner keeps its own buffer and renderer.
// Owners must detach their buffers before releasing their shared reference.
class SharedParticleSimulation {
public:
    static constexpr float DefaultWaterRadius = .1f;
    static constexpr float DefaultSandRadius = DefaultWaterRadius / .6f;
    SharedParticleSimulation(physx::PxPhysics& physics, physx::PxScene& scene, physx::PxCudaContextManager& cuda) : physics_(physics) {
        system_ = physics.createPBDParticleSystem(cuda, 96);
        if (!system_) throw std::runtime_error("Cannot create shared GPU particle system");
        // Reserve compatible physical scales for BOTH materials before any simulation.
        // Joining/leaving a phase must not change an existing pile's contact geometry.
        slots_[0].spacing = 2.f * DefaultWaterRadius;
        slots_[0].adhesion = .07f;
        slots_[1].spacing = 2.f * DefaultSandRadius;
        ApplyOffsets();
        scene.addActor(*system_);
    }
    ~SharedParticleSimulation() {
        if (system_) system_->release();
        for (auto& slot : slots_) if (slot.material) slot.material->release();
    }
    SharedParticleSimulation(const SharedParticleSimulation&) = delete;
    SharedParticleSimulation& operator=(const SharedParticleSimulation&) = delete;
    physx::PxPBDParticleSystem* System() const { return system_; }
    physx::PxPBDMaterial* Material(bool sand) {
        auto& slot = slots_[sand ? 1 : 0];
        if (!slot.material) {
            slot.material = physics_.createPBDMaterial(sand ? .6f : .05f, .05f, 0, sand ? 0.f : 1.f, sand ? 0.f : .5f, 0, 0, 0, 0);
            if (!slot.material) throw std::runtime_error("Cannot create particle phase material");
            auto flags = physx::PxParticlePhaseFlags(physx::PxParticlePhaseFlag::eParticlePhaseSelfCollide);
            if (!sand) flags |= physx::PxParticlePhaseFlag::eParticlePhaseFluid;
            slot.phase = system_->createPhase(slot.material, flags);
        }
        return slot.material;
    }
    unsigned Phase(bool sand) { Material(sand); return slots_[sand ? 1 : 0].phase; }
    void Configure(bool sand, float spacing, float adhesion, float adhesionScale, float adhesionRadius) {
        auto& slot = slots_[sand ? 1 : 0];
        slot.active = true; slot.spacing = spacing;
        slot.adhesion = adhesion; slot.adhesionScale = adhesionScale; slot.adhesionRadius = adhesionRadius;
        ApplyOffsets();
    }
    void Deactivate(bool sand) { slots_[sand ? 1 : 0].active = false; }
private:
    struct Slot {
        physx::PxPBDMaterial* material = nullptr; unsigned phase = 0;
        bool active = false; float spacing = .08f, adhesion = 0, adhesionScale = 1, adhesionRadius = 2;
    };
    void ApplyOffsets() {
        const auto& water = slots_[0]; const auto& sand = slots_[1];
        float wallRest = std::numeric_limits<float>::max(), wallContact = 0, particleContact = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& s = slots_[i];
            const float rest = s.spacing * .5f / (i ? 1.f : .6f);
            wallRest = std::min(wallRest, rest);
            const float solidContact = std::max(rest + .01f, s.adhesion > 0 && s.adhesionScale > 0 ? rest * s.adhesionRadius : 0.f);
            wallContact = std::max(wallContact, i ? solidContact : rest + (s.adhesion > 0 ? std::max(.01f, rest) : .01f));
            particleContact = std::max(particleContact, std::max(rest + .01f,
                i && s.adhesion > 0 && s.adhesionScale > 0 ? rest * s.adhesionRadius : 0.f));
        }
        const float solidRest = sand.spacing * .5f;
        const float fluidRest = water.spacing * .5f;
        // The SDK exposes one collider rest distance for the whole system.
        // Use both configured phase scales, even while one phase has no particles.
        // This SDK initializes GPU mGridCellWidth when the actor enters the scene.
        // Updating particleContactOffset alone changes the kernel radius but not the
        // search grid. Reinsert the same actor between completed simulation steps;
        // buffers, phases, materials and particle state remain owned by this system.
        const float nextContact = std::max(particleContact, std::max(solidRest, fluidRest) + .01f);
        auto* scene = system_->getScene();
        const bool rebuildGrid = scene && system_->getParticleContactOffset() != nextContact;
        if (rebuildGrid) scene->removeActor(*system_);
        system_->setRestOffset(wallRest);
        system_->setSolidRestOffset(solidRest);
        system_->setFluidRestOffset(fluidRest);
        system_->setContactOffset(std::max(wallContact, std::max(solidRest, fluidRest) + .01f));
        system_->setParticleContactOffset(nextContact);
        if (rebuildGrid) scene->addActor(*system_);
    }
    physx::PxPhysics& physics_;
    physx::PxPBDParticleSystem* system_ = nullptr;
    Slot slots_[2];
};

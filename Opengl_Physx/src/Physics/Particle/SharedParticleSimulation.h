#pragma once
#include <PxPhysicsAPI.h>
#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include <stdexcept>

// Shared simulation only: each liquid/sand owner keeps its own buffer and renderer.
// Owners must detach their buffers before releasing their shared reference.
class SharedParticleSimulation {
public:
    static constexpr float DefaultWaterRadius = .1f;
    static constexpr float DefaultSandRadius = DefaultWaterRadius / .6f;
    struct Configuration {
        float spacing, adhesion = 0, adhesionScale = 1, adhesionRadius = 2;
    };
    using Configurations = std::array<Configuration, 2>;
    static Configurations Defaults() {
        return { Configuration{2.f * DefaultWaterRadius, .07f}, Configuration{2.f * DefaultSandRadius} };
    }
    SharedParticleSimulation(physx::PxPhysics& physics, physx::PxScene& scene, physx::PxCudaContextManager& cuda,
        const Configurations& configurations) : physics_(physics) {
        system_ = physics.createPBDParticleSystem(cuda, 96);
        if (!system_) throw std::runtime_error("Cannot create shared GPU particle system");
        // Retain both configurations, but only populated phases contribute to solver scales.
        for (unsigned i = 0; i < 2; ++i) static_cast<Configuration&>(slots_[i]) = configurations[i];
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
    void AttachBuffer(physx::PxParticleBuffer* buffer) {
        buffers_.push_back(buffer);
        system_->addParticleBuffer(buffer);
    }
    void DetachBuffer(physx::PxParticleBuffer* buffer) {
        system_->removeParticleBuffer(buffer);
        buffers_.erase(std::remove(buffers_.begin(), buffers_.end(), buffer), buffers_.end());
    }
    // A count change shifts later buffers' offsets in PhysX's shared flat arrays.
    // Republish their unchanged user data; do not reorder, recreate, or reset particles.
    void NotifyParticleCountChanged() {
        for (auto* buffer : buffers_) {
            buffer->raiseFlags(physx::PxParticleBufferFlag::eUPDATE_POSITION);
            buffer->raiseFlags(physx::PxParticleBufferFlag::eUPDATE_VELOCITY);
            buffer->raiseFlags(physx::PxParticleBufferFlag::eUPDATE_PHASE);
        }
    }

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
    void Configure(bool sand, float spacing, float adhesion, float adhesionScale, float adhesionRadius, bool active) {
        auto& slot = slots_[sand ? 1 : 0];
        slot.active = active; slot.spacing = spacing;
        slot.adhesion = adhesion; slot.adhesionScale = adhesionScale; slot.adhesionRadius = adhesionRadius;
        ApplyOffsets();
    }
    void Deactivate(bool sand) { slots_[sand ? 1 : 0].active = false; ApplyOffsets(); }
private:
    struct Slot : Configuration {
        physx::PxPBDMaterial* material = nullptr; unsigned phase = 0;
        bool active = false;
    };
    void ApplyOffsets() {
        const auto& water = slots_[0]; const auto& sand = slots_[1];
        if (!water.active && !sand.active) return;
        float wallRest = std::numeric_limits<float>::max(), wallContact = 0, particleContact = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& s = slots_[i];
            if (!s.active) continue;
            const float rest = s.spacing * .5f / (i ? 1.f : .6f);
            wallRest = std::min(wallRest, rest);
            const float margin = rest * .06f; // 0.01 at the reference sand radius; scale with particle size.
            const float solidContact = std::max(rest + margin, s.adhesion > 0 && s.adhesionScale > 0 ? rest * s.adhesionRadius : 0.f);
            wallContact = std::max(wallContact, i ? solidContact : rest + (s.adhesion > 0 ? std::max(margin, rest) : margin));
            particleContact = std::max(particleContact, std::max(rest + margin,
                i && s.adhesion > 0 && s.adhesionScale > 0 ? rest * s.adhesionRadius : 0.f));
        }
        // Inactive-phase offsets are compatible placeholders, not saved settings.
        // Activating that phase restores its independently configured radius.
        const float solidRest = sand.active ? sand.spacing * .5f : water.spacing * .5f / .6f;
        const float fluidRest = water.active ? water.spacing * .5f : solidRest * .6f;
        // The SDK exposes one collider rest distance for the whole system.
        // Derive the shared collider/search scales from populated phases only.
        // This SDK initializes GPU mGridCellWidth when the actor enters the scene.
        // Updating particleContactOffset alone changes the kernel radius but not the
        // search grid. Reinsert the same actor between completed simulation steps;
        // buffers, phases, materials and particle state remain owned by this system.
        const float nextContact = std::max(particleContact, std::max(solidRest, fluidRest) * 1.06f);
        auto* scene = system_->getScene();
        const bool rebuildGrid = scene && system_->getParticleContactOffset() != nextContact;
        if (rebuildGrid) scene->removeActor(*system_);
        // Expand bounds before changing rest distances; shrink only after both phases are updated.
        const float nextWallContact = std::max(wallContact, std::max(solidRest, fluidRest) * 1.06f);
        system_->setContactOffset(std::max(system_->getContactOffset(), nextWallContact));
        system_->setParticleContactOffset(std::max(system_->getParticleContactOffset(), nextContact));
        system_->setRestOffset(wallRest);
        system_->setSolidRestOffset(solidRest);
        system_->setFluidRestOffset(fluidRest);
        system_->setContactOffset(nextWallContact);
        system_->setParticleContactOffset(nextContact);
        if (rebuildGrid) scene->addActor(*system_);
    }
    physx::PxPhysics& physics_;
    physx::PxPBDParticleSystem* system_ = nullptr;
    Slot slots_[2];
    std::vector<physx::PxParticleBuffer*> buffers_;
};

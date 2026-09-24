#pragma once
#include "PhysicsWorld.h"
#include "Shader.h"
#include "Camera.h"
#include "LiquidSurface.h"
#include "LiquidGpuTimer.h"
#include "SandRenderer.h"
#include "LiquidParticleCleanup.h"
#include <memory>
#include <extensions/PxParticleExt.h>
#include <extensions/PxCudaHelpersExt.h>
#include <cudamanager/PxCudaContext.h>
#include <array>
#include <limits>
#include <vector>

class LiquidGpu
{
public:
    static constexpr unsigned MaxParticles = 1000000;
    static constexpr float WaterDensity = 10.f;
    static constexpr float SandDensity = 16.f;
    struct Parameters
    {
        float viscosity = .05f, damping = .05f, surfaceTension = .77f, cohesion = 5.06f;
        float vorticity = 1.0f, friction = .05f, adhesion = .07f, gravityScale = 1.0f;
    };
    const Parameters& GetParameters() const { return parameters_; }
    void SetParameters(Parameters value)
    {
        auto bounded = [](float v, float low, float high, float fallback) {return std::isfinite(v) ? glm::clamp(v, low, high) : fallback; };
        value.viscosity = bounded(value.viscosity, 0, 100, .05f);
        value.damping = bounded(value.damping, 0, 10, .05f);
        value.surfaceTension = bounded(value.surfaceTension, 0, 10, .77f);
        value.cohesion = bounded(value.cohesion, 0, 10, 5.06f);
        value.vorticity = bounded(value.vorticity, 0, 10, 1.0f);
        value.friction = bounded(value.friction, 0, 2, .05f);
        value.adhesion = bounded(value.adhesion, 0, 10, .07f);
        value.gravityScale = bounded(value.gravityScale, -2, 5, 1);
        parameters_ = value;
        if (!material_ || granular_)return;
        material_->setViscosity(value.viscosity); material_->setDamping(value.damping);
        material_->setSurfaceTension(value.surfaceTension); material_->setCohesion(value.cohesion);
        material_->setVorticityConfinement(value.vorticity); material_->setFriction(value.friction);
        material_->setAdhesion(value.adhesion); material_->setAdhesionRadiusScale(2.0f); material_->setGravityScale(value.gravityScale);
        if (system_) { const float rest = system_->getRestOffset(); system_->setContactOffset(rest + (value.adhesion > 0 ? std::max(.01f, rest) : .01f)); }
    }
    glm::vec3 position{ 0,5.55f,0 }, size{ 8.40f,4.50f,9.30f };
    float spacing = .2f;
    explicit LiquidGpu(PhysicsWorld& world, bool defaultWater = true, bool granular = false) :granular_(granular), world_(world), cuda_(*world.GetCuda()), shader_("Assets/Shaders/liquid_particles.glsl")
    {
        try {
            driver_ = LoadLibraryW(L"nvcuda.dll");
            if (!driver_) throw std::runtime_error("CUDA driver unavailable");
            reg_ = Load<Register>("cuGraphicsGLRegisterBuffer"); unregister_ = Load<Unregister>("cuGraphicsUnregisterResource");
            map_ = Load<Map>("cuGraphicsMapResources"); unmap_ = Load<Map>("cuGraphicsUnmapResources"); pointer_ = Load<Pointer>("cuGraphicsResourceGetMappedPointer_v2");
            GL::GenVertexArrays(1, &vao_); GL::GenBuffers(1, &vbo_);
            GL::GenVertexArrays(1, &boxVao_); GL::GenBuffers(1, &boxVbo_);
            Configure(vao_, vbo_); Configure(boxVao_, boxVbo_);
            containerEnabled_ = defaultWater && !granular_;
            if (granular_) { position = glm::vec3(0, 5, 0); size = glm::vec3(4); showDebugBounds_ = true; }
            if (defaultWater) { if (containerEnabled_) SetContainer(containerPosition_, containerSize_); Reset(); }
        }
        catch (...) { Release(); throw; }
    }
    ~LiquidGpu() { Release(); }
    LiquidGpu(const LiquidGpu&) = delete;
    LiquidGpu& operator=(const LiquidGpu&) = delete;
    const LiquidSurface::RenderParameters& GetRenderParameters() const { return renderParameters_; }
    void SetRenderParameters(LiquidSurface::RenderParameters value) { renderParameters_ = LiquidSurface::ClampRenderParameters(value); if (surface_)surface_->SetRenderParameters(renderParameters_); }
    float SandRenderScale() const { return sandRenderScale_; }
    void SetSandRenderScale(float value) {
        if (std::isfinite(value)) sandRenderScale_ = glm::clamp(value, .5f, 2.5f);
    }
    const SandRenderer::Parameters& GetSandRenderParameters() const { return sandParameters_; }
    void SetSandRenderParameters(SandRenderer::Parameters value) { sandParameters_ = SandRenderer::Clamp(value); }
    int DisplayMode() const { return displayMode_; }
    void SetDisplayMode(int mode) { if (mode == 0 || mode == 1) { if (displayMode_ != mode && surface_)surface_->Invalidate(); displayMode_ = mode; } }
    unsigned int Count() const { return count_; }
    unsigned int Capacity() const { return particles_ ? particles_->getMaxParticles() : 0; }
    // Drop scene contents, but retain the user buffers, interop registration and cleanup storage.
    void ClearForSceneChange()
    {
        if (particles_) particles_->setNbActiveParticles(0);
        count_ = 0;
        containerEnabled_ = false;
        UpdateSimulationMembership();
        revision_ = std::numeric_limits<unsigned long long>::max();
        if (surface_) surface_->Invalidate();
    }
    // Regenerate scene contents using the user's shared settings, without resetting preferences.
    void ResetForScene()
    {
        containerEnabled_ = !granular_;
        if (containerEnabled_) SetContainer(containerPosition_, containerSize_);
        // Invalid pending region edits must not make a scene switch throw.
        if (RequestedCount()) Reset();
    }
    void ResetSandTest()
    {
        if (!granular_) return;
        ClearForSceneChange();
        containerEnabled_ = false; showDebugBounds_ = true;
        spacing = .2f;
        position = glm::vec3(0, 5, 0); size = glm::vec3(4);
        Reset();
    }
    // Keep the same world-space kill plane as fallen rigid bodies.
    static constexpr float FallenParticleHeight = -30.0f;
    static constexpr unsigned long long CleanupIntervalSteps = 30;
    void CleanupFallenParticles()
    {
        if (!particles_ || !count_) return;
        const auto current = world_.GetSimulationRevision();
        if (current - lastCleanupRevision_ < CleanupIntervalSteps) return;
        if (!cleanup_) cleanup_ = std::make_unique<LiquidParticleCleanup>(cuda_);
        const unsigned kept = cleanup_->RemoveBelow(*particles_, FallenParticleHeight);
        lastCleanupRevision_ = current;
        if (kept == count_) return;
        count_ = kept;
        if (!count_) { UpdateSimulationMembership(); if (surface_) surface_->Invalidate(); }
        revision_ = std::numeric_limits<unsigned long long>::max();
        // Keep live foam: spatial lookup and lighting refresh on the new simulation revision.
    }
    double GetRenderGpuMs() const { return renderTimer_.Milliseconds(); }
    bool HasRenderTiming() const { return renderTimer_.HasResult(); }
    // Uncapped preview; saturate only at uint64 overflow, not at GPU capacity.
    uint64_t PreviewCount() const
    {
        if (!std::isfinite(spacing) || spacing < .08f || spacing > .5f) return 0;
        uint64_t total = 1;
        const uint64_t limit = std::numeric_limits<uint64_t>::max();
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(position[i]) || !std::isfinite(size[i]) || size[i] < spacing) return 0;
        }
        for (int i = 0; i < 3; ++i) {
            // Match the float division used when Reset constructs grid dimensions.
            const float axis = std::floor(size[i] / spacing);
            if (!std::isfinite(axis) || static_cast<long double>(axis) >= static_cast<long double>(limit)) return limit;
            const uint64_t n = static_cast<uint64_t>(axis);
            if (n && total > limit / n) return limit;
            total *= n;
        }
        return total;
    }
    unsigned int RequestedCount() const
    {
        const auto total = PreviewCount();
        return total <= MaxParticles ? static_cast<unsigned int>(total) : 0;
    }
    bool GetShowDebugBounds() const { return showDebugBounds_; }
    void SetShowDebugBounds(bool value) { showDebugBounds_ = value; }
    glm::vec3 ContainerPosition() const { return containerPosition_; }
    glm::vec3 ContainerSize() const { return containerSize_; }
    void SetContainer(glm::vec3 center, glm::vec3 innerSize)
    {
        using namespace physx;
        for (int i = 0; i < 3; ++i) if (!std::isfinite(center[i]) || !std::isfinite(innerSize[i]) || innerSize[i] < 1.0f) return;
        const float thickness = .25f;
        if (!container_)
        {
            container_ = world_.GetPhysics().createRigidStatic(PxTransform(PxVec3(center.x, center.y, center.z)));
            if (!container_)throw std::runtime_error("Cannot create liquid container");
            try {
                for (int axis = 0; axis < 3; ++axis)for (int side = 0; side < 2; ++side)
                {
                    glm::vec3 half = innerSize * .5f + glm::vec3(thickness); half[axis] = thickness * .5f;
                    auto* shape = world_.GetPhysics().createShape(PxBoxGeometry(half.x, half.y, half.z), world_.GetMaterial(), true);
                    if (!shape)throw std::runtime_error("Cannot create liquid container wall");
                    glm::vec3 offset(0); offset[axis] = (side ? 1.0f : -1.0f) * (innerSize[axis] + thickness) * .5f;
                    shape->setLocalPose(PxTransform(PxVec3(offset.x, offset.y, offset.z)));
                    bool attached = container_->attachShape(*shape); shape->release();
                    if (!attached)throw std::runtime_error("Cannot attach liquid container wall");
                }
                world_.GetScene().addActor(*container_);
            }
            catch (...) { container_->release(); container_ = nullptr; throw; }
        }
        else
        {
            PxShape* shapes[6]{}; container_->getShapes(shapes, 6);
            for (int axis = 0; axis < 3; ++axis)for (int side = 0; side < 2; ++side)
            {
                glm::vec3 half = innerSize * .5f + glm::vec3(thickness); half[axis] = thickness * .5f;
                glm::vec3 offset(0); offset[axis] = (side ? 1.0f : -1.0f) * (innerSize[axis] + thickness) * .5f;
                shapes[axis * 2 + side]->setGeometry(PxBoxGeometry(half.x, half.y, half.z));
                shapes[axis * 2 + side]->setLocalPose(PxTransform(PxVec3(offset.x, offset.y, offset.z)));
            }
            container_->setGlobalPose(PxTransform(PxVec3(center.x, center.y, center.z)));
        }
        containerPosition_ = center; containerSize_ = innerSize;
    }
    void Reset()
    {
        using namespace physx;
        const auto count = RequestedCount(); if (!count)throw std::runtime_error("Liquid region exceeds particle limit");
        const bool reuse = particles_ && count <= particles_->getMaxParticles() && spacing == simulationSpacing_;
        if (!reuse)ClearParticles();
        PxVec4* positions = nullptr; PxVec4* velocities = nullptr; PxU32* phases = nullptr;
        try {
            EnsureSystem();
            positions = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, cuda_, count); velocities = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, cuda_, count); phases = PX_EXT_PINNED_MEMORY_ALLOC(PxU32, cuda_, count);
            if (!positions || !velocities || !phases)throw std::runtime_error("Liquid initialization allocation failed");
            const glm::ivec3 dims(glm::floor(size / spacing));
            const glm::vec3 first = position - .5f * glm::vec3(dims - glm::ivec3(1)) * spacing;
            unsigned int n = 0; const float mass = (granular_ ? SandDensity : WaterDensity) * spacing * spacing * spacing;
            for (int z = 0; z < dims.z; ++z)for (int y = 0; y < dims.y; ++y)for (int x = 0; x < dims.x; ++x) { auto p = first + glm::vec3(x, y, z) * spacing; positions[n] = PxVec4(p.x, p.y, p.z, 1.0f / mass); velocities[n] = PxVec4(0.0f); phases[n++] = phase_; }
            ExtGpu::PxParticleBufferDesc desc; desc.maxParticles = MaxParticles; desc.numActiveParticles = count; desc.positions = positions; desc.velocities = velocities; desc.phases = phases;
            if (reuse)
            {
                { PxScopedCudaLock lock(cuda_); Check(cuda_.getCudaContext()->streamSynchronize(nullptr)); }
                {
                    PxScopedCudaLock lock(cuda_); auto* context = cuda_.getCudaContext();
                    Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getPositionInvMasses()), positions, count * sizeof(PxVec4)));
                    Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getVelocities()), velocities, count * sizeof(PxVec4)));
                    Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getPhases()), phases, count * sizeof(PxU32)));
                }
                particles_->setNbActiveParticles(count);
                particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
                particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_VELOCITY);
                particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_PHASE);
            }
            else {
                particles_ = ExtGpu::PxCreateAndPopulateParticleBuffer(desc, &cuda_);
                if (!particles_)throw std::runtime_error("Cannot create liquid particle buffer");
                system_->addParticleBuffer(particles_); bufferAttached_ = true;
                GL::BindBuffer(GL::ArrayBuffer, vbo_); GL::BufferData(GL::ArrayBuffer, (2 * MaxParticles * sizeof(PxVec4) + (granular_ ? MaxParticles * sizeof(unsigned) : 0)), nullptr, 0x88E8); GL::BindBuffer(GL::ArrayBuffer, 0);
                { PxScopedCudaLock lock(cuda_); Check(reg_(&resource_, vbo_, 2)); }
            }
            // Load/JIT the cleanup kernel during reset, not during a simulation frame.
            if (!cleanup_) cleanup_ = std::make_unique<LiquidParticleCleanup>(cuda_);
            cleanup_->Prepare(MaxParticles);
            if (granular_) { cleanup_->AssignIds(0, count, 0); nextSandId_ = count; }
            if (surface_)surface_->Invalidate();
            simulationSpacing_ = spacing; world_.ClearAccumulator(); lastCleanupRevision_ = world_.GetSimulationRevision();
            count_ = count; renderRadius_ = spacing * (granular_ ? .5f : .55f); revision_ = std::numeric_limits<unsigned long long>::max();
            UpdateSimulationMembership();
        }
        catch (...) {
            if (positions)PX_EXT_PINNED_MEMORY_FREE(cuda_, positions); if (velocities)PX_EXT_PINNED_MEMORY_FREE(cuda_, velocities); if (phases)PX_EXT_PINNED_MEMORY_FREE(cuda_, phases);
            ClearParticles(); throw;
        }
        PX_EXT_PINNED_MEMORY_FREE(cuda_, positions); PX_EXT_PINNED_MEMORY_FREE(cuda_, velocities); PX_EXT_PINNED_MEMORY_FREE(cuda_, phases);
    }
    // Append only the new particles; existing simulated positions and velocities are retained.
    unsigned Emit(glm::vec3 origin, glm::vec3 direction, float speed, float radius, unsigned requested, float duration)
    {
        using namespace physx;
        const unsigned amount = std::min(requested, MaxParticles - count_);
        if (!amount) return 0;
        if (!std::isfinite(speed) || !std::isfinite(radius) || !std::isfinite(duration) ||
            !std::isfinite(glm::dot(origin, origin)) || !std::isfinite(glm::dot(direction, direction)) ||
            glm::dot(direction, direction) < .0001f || speed < 0 || radius <= 0 || duration < 0) return 0;
        // A spacing edit made before leaving scene 4 also applies to the next emission.
        if (!count_ && particles_ && simulationSpacing_ != spacing) ClearParticles();
        EnsureSystem();
        if (!particles_) {
            particles_ = world_.GetPhysics().createParticleBuffer(MaxParticles, &cuda_);
            if (!particles_) throw std::runtime_error("Cannot allocate emission buffer");
            GL::BindBuffer(GL::ArrayBuffer, vbo_);
            GL::BufferData(GL::ArrayBuffer, (2 * MaxParticles * sizeof(PxVec4) + (granular_ ? MaxParticles * sizeof(unsigned) : 0)), nullptr, 0x88E8);
            GL::BindBuffer(GL::ArrayBuffer, 0);
            { PxScopedCudaLock lock(cuda_); Check(reg_(&resource_, vbo_, 2)); }
            simulationSpacing_ = spacing;
            if (!cleanup_) cleanup_ = std::make_unique<LiquidParticleCleanup>(cuda_);
            cleanup_->Prepare(MaxParticles);
            lastCleanupRevision_ = world_.GetSimulationRevision();
        }
        emissionPositions_.resize(amount); emissionVelocities_.resize(amount); emissionPhases_.resize(amount);
        direction = glm::normalize(direction);
        const auto right = glm::normalize(glm::cross(direction, std::abs(direction.y) < .95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
        const auto up = glm::cross(right, direction);
        const float inverseMass = 1.0f / ((granular_ ? SandDensity : WaterDensity) * simulationSpacing_ * simulationSpacing_ * simulationSpacing_);
        for (unsigned i = 0; i < amount; ++i) {
            const double sequence = double(emittedSequence_++);
            const float radial = radius * std::sqrt(float(std::fmod(sequence * .7548776662466927 + .5, 1.0)));
            const float angle = float(std::fmod(sequence * .5698402909980532, 1.0)) * 6.28318530718f;
            const auto pos = origin + radial * (std::cos(angle) * right + std::sin(angle) * up)
                + direction * (speed * duration * (float(i) + .5f) / amount);
            emissionPositions_[i] = PxVec4(pos.x, pos.y, pos.z, inverseMass);
            emissionVelocities_[i] = PxVec4(direction.x * speed, direction.y * speed, direction.z * speed, 0);
            emissionPhases_[i] = phase_;
        }
        {
            PxScopedCudaLock lock(cuda_); auto* context = cuda_.getCudaContext();
            Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getPositionInvMasses() + count_), emissionPositions_.data(), amount * sizeof(PxVec4)));
            Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getVelocities() + count_), emissionVelocities_.data(), amount * sizeof(PxVec4)));
            Check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(particles_->getPhases() + count_), emissionPhases_.data(), amount * sizeof(PxU32)));
        }
        if (granular_) { cleanup_->AssignIds(count_, amount, nextSandId_); nextSandId_ += amount; }
        count_ += amount;
        particles_->setNbActiveParticles(count_);
        particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
        particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_VELOCITY);
        particles_->raiseFlags(PxParticleBufferFlag::eUPDATE_PHASE);
        renderRadius_ = simulationSpacing_ * .55f;
        revision_ = std::numeric_limits<unsigned long long>::max();
        UpdateSimulationMembership();
        return amount;
    }
    void Draw(const Camera& camera, int width, int height)
    {
        if (!count_ || width <= 0 || height <= 0)return;
        if (count_ && revision_ != world_.GetSimulationRevision()) {
            physx::PxScopedCudaLock lock(cuda_); Check(map_(1, &resource_, nullptr));
            try { CUdeviceptr dst = 0; size_t bytes = 0; Check(pointer_(&dst, &bytes, resource_)); if (bytes < 2 * count_ * sizeof(physx::PxVec4))throw std::runtime_error("Liquid GL buffer too small"); Check(cuda_.getCudaContext()->memcpyDtoDAsync(dst, reinterpret_cast<CUdeviceptr>(particles_->getPositionInvMasses()), count_ * sizeof(physx::PxVec4), nullptr)); Check(cuda_.getCudaContext()->memcpyDtoDAsync(dst + count_ * sizeof(physx::PxVec4), reinterpret_cast<CUdeviceptr>(particles_->getVelocities()), count_ * sizeof(physx::PxVec4), nullptr)); if (granular_) { if (bytes < 2 * MaxParticles * sizeof(physx::PxVec4) + count_ * sizeof(unsigned)) throw std::runtime_error("Sand ID GL buffer too small"); Check(cuda_.getCudaContext()->memcpyDtoDAsync(dst + 2 * MaxParticles * sizeof(physx::PxVec4), cleanup_->Ids(), count_ * sizeof(unsigned), nullptr)); } }
            catch (...) { unmap_(1, &resource_, nullptr); throw; }Check(unmap_(1, &resource_, nullptr)); revision_ = world_.GetSimulationRevision();
        }
        renderTimer_.Begin();
        if (granular_ && displayMode_ == 0) {
            if (!sandRenderer_) sandRenderer_ = std::make_unique<SandRenderer>();
            sandRenderer_->Draw(vbo_, 2 * MaxParticles * sizeof(physx::PxVec4), count_, renderRadius_ * sandRenderScale_, camera, width, height, sandParameters_);
            renderTimer_.End(); return;
        }
        if (!granular_ && displayMode_ == 0 && count_) { if (!surface_)surface_ = std::make_unique<LiquidSurface>(); surface_->SetRenderParameters(renderParameters_); surface_->Draw(vbo_, count_, simulationSpacing_, camera, width, height, world_.GetSimulationRevision(), parameters_.gravityScale, containerEnabled_ ? containerPosition_ - containerSize_ * .5f : glm::vec3(-100, -30, -100), containerEnabled_ ? containerPosition_ + containerSize_ * .5f : glm::vec3(100)); }
        shader_.Use(); shader_.SetMatrix4("view", camera.GetViewMatrix()); shader_.SetMatrix4("projection", camera.GetProjectionMatrix(float(width) / height));
        shader_.SetFloat("granular", granular_ ? 1.0f : 0.0f); shader_.SetFloat("sandRender", granular_ && displayMode_ == 0 ? 1.0f : 0.0f); shader_.SetFloat("radius", renderRadius_); shader_.SetFloat("sandShapeScale", sandRenderScale_); shader_.SetFloat("viewportHeight", float(height)); shader_.SetFloat("region", 0);
        const bool pointSize = glIsEnabled(0x8642) != 0; glEnable(0x8642); GL::BindVertexArray(vao_); if (granular_ || displayMode_ == 1)glDrawArrays(GL_POINTS, 0, count_); if (!pointSize)glDisable(0x8642);
        renderTimer_.End();
        GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0);
    }
    void DrawDebugBounds(const Camera& camera, int width, int height)
    {
        if (!showDebugBounds_ || width <= 0 || height <= 0) return;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(position[i]) || !std::isfinite(size[i]) || size[i] <= 0) return;
        shader_.Use();
        shader_.SetMatrix4("view", camera.GetViewMatrix());
        shader_.SetMatrix4("projection", camera.GetProjectionMatrix(float(width) / height));
        std::array<physx::PxVec4, 24> lines{}; const int edges[24] = { 0,1,0,2,0,4,1,3,1,5,2,3,2,6,3,7,4,5,4,6,5,7,6,7 };
        for (int i = 0; i < 24; ++i) { int c = edges[i]; auto p = position + size * glm::vec3(c & 1 ? .5f : -.5f, c & 2 ? .5f : -.5f, c & 4 ? .5f : -.5f); lines[i] = physx::PxVec4(p.x, p.y, p.z, 1); }
        GL::BindBuffer(GL::ArrayBuffer, boxVbo_); GL::BufferData(GL::ArrayBuffer, sizeof(lines), lines.data(), 0x88E8);
        shader_.SetFloat("region", 1); GL::BindVertexArray(boxVao_); glDrawArrays(GL_LINES, 0, 24);
        if (containerEnabled_) {
            for (int i = 0; i < 24; ++i) { int c = edges[i]; auto p = containerPosition_ + containerSize_ * glm::vec3(c & 1 ? .5f : -.5f, c & 2 ? .5f : -.5f, c & 4 ? .5f : -.5f); lines[i] = physx::PxVec4(p.x, p.y, p.z, 1); }
            GL::BufferData(GL::ArrayBuffer, sizeof(lines), lines.data(), 0x88E8); shader_.SetFloat("region", 2); glDrawArrays(GL_LINES, 0, 24);
        }
        GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0);
    }

private:
    void EnsureSystem()
    {
        using namespace physx;
        if (!system_) {
            material_ = world_.GetPhysics().createPBDMaterial(granular_ ? .6f : .05f, .05f, 0, granular_ ? 0.f : 1.0f, granular_ ? 0.f : .5f, 0, 0, 0, 0);
            if (!material_)throw std::runtime_error("Cannot create liquid material");
            SetParameters(parameters_);
            system_ = world_.GetPhysics().createPBDParticleSystem(cuda_, 96);
            if (!system_)throw std::runtime_error("Cannot create GPU liquid system");
            const float rest = spacing * .5f / (granular_ ? 1.0f : .6f);
            system_->setRestOffset(rest); system_->setContactOffset(rest + .01f); system_->setParticleContactOffset(rest + .01f);
            system_->setSolidRestOffset(rest); system_->setFluidRestOffset(spacing * .5f);
            SetParameters(parameters_);

            world_.GetScene().addActor(*system_);
            phase_ = system_->createPhase(material_, PxParticlePhaseFlags(granular_ ? PxParticlePhaseFlag::eParticlePhaseSelfCollide : (PxParticlePhaseFlag::eParticlePhaseFluid | PxParticlePhaseFlag::eParticlePhaseSelfCollide)));

        }
    }
    void UpdateSimulationMembership()
    {
        auto& scene = world_.GetScene();
        if (count_)
        {
            if (system_ && !system_->getScene()) scene.addActor(*system_);
            if (system_ && particles_ && !bufferAttached_) { system_->addParticleBuffer(particles_); bufferAttached_ = true; }
            if (containerEnabled_ && container_ && !container_->getScene()) scene.addActor(*container_);
        }
        else
        {
            // Only user storage persists; solver state belongs to the old scene.
            if (system_) { if (particles_ && bufferAttached_) system_->removeParticleBuffer(particles_); bufferAttached_ = false; system_->release(); system_ = nullptr; }
            if (material_) { material_->release(); material_ = nullptr; }
            if (container_ && container_->getScene()) scene.removeActor(*container_);
        }
    }
    using Register = int(WINAPI*)(void**, unsigned int, unsigned int); using Unregister = int(WINAPI*)(void*);
    using Map = int(WINAPI*)(unsigned int, void**, CUstream); using Pointer = int(WINAPI*)(CUdeviceptr*, size_t*, void*);
    template<class T>T Load(const char* name) { auto f = reinterpret_cast<T>(GetProcAddress(driver_, name)); if (!f)throw std::runtime_error("CUDA interop unavailable"); return f; }
    static void Check(int code) { if (code)throw std::runtime_error("CUDA liquid transfer failed"); }
    static void Configure(GLuint vao, GLuint buffer) { GL::BindVertexArray(vao); GL::BindBuffer(GL::ArrayBuffer, buffer); GL::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(physx::PxVec4), nullptr); GL::EnableVertexAttribArray(0); GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0); }
    void ClearParticles() { cleanup_.reset(); if (resource_) { glFinish(); physx::PxScopedCudaLock lock(cuda_); cuda_.getCudaContext()->streamSynchronize(nullptr); }if (resource_) { physx::PxScopedCudaLock lock(cuda_); unregister_(resource_); resource_ = nullptr; }if (particles_) { if (system_ && bufferAttached_)system_->removeParticleBuffer(particles_); bufferAttached_ = false; particles_->release(); particles_ = nullptr; }if (system_) { system_->release(); system_ = nullptr; }if (material_) { material_->release(); material_ = nullptr; }count_ = 0; }
    void Release() { ClearParticles(); if (container_) { container_->release(); container_ = nullptr; }GL::DeleteBuffers(1, &vbo_); GL::DeleteBuffers(1, &boxVbo_); GL::DeleteVertexArrays(1, &vao_); GL::DeleteVertexArrays(1, &boxVao_); if (driver_) { FreeLibrary(driver_); driver_ = nullptr; } }
    const bool granular_;
    bool containerEnabled_ = true;
    std::vector<physx::PxVec4> emissionPositions_, emissionVelocities_;
    std::vector<physx::PxU32> emissionPhases_;
    unsigned long long emittedSequence_ = 0;
    bool bufferAttached_ = false;
    unsigned int phase_ = 0; float simulationSpacing_ = 0;
    int displayMode_ = 0;
    float sandRenderScale_ = 1.82f; // Visual polygon extent only; no simulation changes.
    LiquidSurface::RenderParameters renderParameters_;
    std::unique_ptr<LiquidSurface> surface_;
    std::unique_ptr<SandRenderer> sandRenderer_;
    SandRenderer::Parameters sandParameters_;
    unsigned nextSandId_ = 0;
    Parameters parameters_;
    std::unique_ptr<LiquidParticleCleanup> cleanup_;
    unsigned long long lastCleanupRevision_ = 0;
    LiquidGpuTimer renderTimer_;
    bool showDebugBounds_ = true;
    glm::vec3 containerPosition_{ 0,4,0 }, containerSize_{ 10,8,10 };
    physx::PxRigidStatic* container_ = nullptr;
    PhysicsWorld& world_; physx::PxCudaContextManager& cuda_; Shader shader_;
    physx::PxPBDParticleSystem* system_ = nullptr; physx::PxParticleBuffer* particles_ = nullptr; physx::PxPBDMaterial* material_ = nullptr;
    HMODULE driver_ = nullptr; void* resource_ = nullptr; Register reg_ = nullptr; Unregister unregister_ = nullptr; Map map_ = nullptr, unmap_ = nullptr; Pointer pointer_ = nullptr;
    GLuint vao_ = 0, vbo_ = 0, boxVao_ = 0, boxVbo_ = 0; unsigned int count_ = 0; float renderRadius_ = .088f; unsigned long long revision_ = 0;
};

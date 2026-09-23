#pragma once
#include "PhysicsWorld.h"
#include "Shader.h"
#include "Camera.h"
#include "LiquidSurface.h"
#include <memory>
#include <extensions/PxParticleExt.h>
#include <extensions/PxCudaHelpersExt.h>
#include <cudamanager/PxCudaContext.h>
#include <array>
#include <limits>

class LiquidGpu
{
public:
    static constexpr unsigned MaxParticles = 1048576;
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
        if (!material_)return;
        material_->setViscosity(value.viscosity); material_->setDamping(value.damping);
        material_->setSurfaceTension(value.surfaceTension); material_->setCohesion(value.cohesion);
        material_->setVorticityConfinement(value.vorticity); material_->setFriction(value.friction);
        material_->setAdhesion(value.adhesion); material_->setAdhesionRadiusScale(2.0f); material_->setGravityScale(value.gravityScale);
        if (system_) { const float rest = system_->getRestOffset(); system_->setContactOffset(rest + (value.adhesion > 0 ? std::max(.01f, rest) : .01f)); }
    }
    glm::vec3 position{ 0,5.55f,0 }, size{ 8.40f,4.50f,9.30f };
    float spacing = .2f;
    explicit LiquidGpu(PhysicsWorld& world) :world_(world), cuda_(*world.GetCuda()), shader_("Assets/Shaders/liquid_particles.glsl")
    {
        try {
            driver_ = LoadLibraryW(L"nvcuda.dll");
            if (!driver_) throw std::runtime_error("CUDA driver unavailable");
            reg_ = Load<Register>("cuGraphicsGLRegisterBuffer"); unregister_ = Load<Unregister>("cuGraphicsUnregisterResource");
            map_ = Load<Map>("cuGraphicsMapResources"); unmap_ = Load<Map>("cuGraphicsUnmapResources"); pointer_ = Load<Pointer>("cuGraphicsResourceGetMappedPointer_v2");
            GL::GenVertexArrays(1, &vao_); GL::GenBuffers(1, &vbo_);
            GL::GenVertexArrays(1, &boxVao_); GL::GenBuffers(1, &boxVbo_);
            Configure(vao_, vbo_); Configure(boxVao_, boxVbo_); SetContainer(containerPosition_, containerSize_); Reset();
        }
        catch (...) { Release(); throw; }
    }
    ~LiquidGpu() { Release(); }
    LiquidGpu(const LiquidGpu&) = delete;
    LiquidGpu& operator=(const LiquidGpu&) = delete;
    int DisplayMode() const { return displayMode_; }
    void SetDisplayMode(int mode) { if (mode == 0 || mode == 1) { if (displayMode_ != mode && surface_)surface_->Invalidate(); displayMode_ = mode; } }
    unsigned int Count() const { return count_; }
    unsigned int RequestedCount() const
    {
        if (!std::isfinite(spacing) || spacing < .08f || spacing>.5f) return 0;
        uint64_t count = 1;
        for (int i = 0; i < 3; ++i) { if (!std::isfinite(size[i]) || size[i] < spacing || size[i]>20 || !std::isfinite(position[i]))return 0; count *= static_cast<unsigned int>(std::floor(size[i] / spacing)); }
        return count <= MaxParticles ? static_cast<unsigned int>(count) : 0;
    }
    glm::vec3 ContainerPosition() const { return containerPosition_; }
    glm::vec3 ContainerSize() const { return containerSize_; }
    void SetContainer(glm::vec3 center, glm::vec3 innerSize)
    {
        using namespace physx;
        for (int i = 0; i < 3; ++i) if (!std::isfinite(center[i]) || !std::isfinite(innerSize[i]) || innerSize[i] < 1.0f || innerSize[i]>30.0f) return;
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
            if (!reuse) {
                material_ = world_.GetPhysics().createPBDMaterial(.05f, .05f, 0, 1.0f, .5f, 0, 0, 0, 0);
                if (!material_)throw std::runtime_error("Cannot create liquid material");
                SetParameters(parameters_);
                system_ = world_.GetPhysics().createPBDParticleSystem(cuda_, 96);
                if (!system_)throw std::runtime_error("Cannot create GPU liquid system");
                const float rest = spacing * .5f / .6f;
                system_->setRestOffset(rest); system_->setContactOffset(rest + .01f); system_->setParticleContactOffset(rest + .01f);
                system_->setSolidRestOffset(rest); system_->setFluidRestOffset(spacing * .5f);
                SetParameters(parameters_);
                world_.GetScene().addActor(*system_);
                phase_ = system_->createPhase(material_, PxParticlePhaseFlags(PxParticlePhaseFlag::eParticlePhaseFluid | PxParticlePhaseFlag::eParticlePhaseSelfCollide));
            }
            positions = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, cuda_, count); velocities = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, cuda_, count); phases = PX_EXT_PINNED_MEMORY_ALLOC(PxU32, cuda_, count);
            if (!positions || !velocities || !phases)throw std::runtime_error("Liquid initialization allocation failed");
            const glm::ivec3 dims(glm::floor(size / spacing));
            const glm::vec3 first = position - .5f * glm::vec3(dims - glm::ivec3(1)) * spacing;
            unsigned int n = 0; const float mass = 10.0f * spacing * spacing * spacing;
            for (int z = 0; z < dims.z; ++z)for (int y = 0; y < dims.y; ++y)for (int x = 0; x < dims.x; ++x) { auto p = first + glm::vec3(x, y, z) * spacing; positions[n] = PxVec4(p.x, p.y, p.z, 1.0f / mass); velocities[n] = PxVec4(0.0f); phases[n++] = phase_; }
            ExtGpu::PxParticleBufferDesc desc; desc.maxParticles = desc.numActiveParticles = count; desc.positions = positions; desc.velocities = velocities; desc.phases = phases;
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
                system_->addParticleBuffer(particles_);
                GL::BindBuffer(GL::ArrayBuffer, vbo_); GL::BufferData(GL::ArrayBuffer, 2 * count * sizeof(PxVec4), nullptr, 0x88E8); GL::BindBuffer(GL::ArrayBuffer, 0);
                { PxScopedCudaLock lock(cuda_); Check(reg_(&resource_, vbo_, 2)); }
            }
            if (surface_)surface_->Invalidate();
            simulationSpacing_ = spacing; world_.ClearAccumulator();
            count_ = count; renderRadius_ = spacing * .55f; revision_ = std::numeric_limits<unsigned long long>::max();
        }
        catch (...) {
            if (positions)PX_EXT_PINNED_MEMORY_FREE(cuda_, positions); if (velocities)PX_EXT_PINNED_MEMORY_FREE(cuda_, velocities); if (phases)PX_EXT_PINNED_MEMORY_FREE(cuda_, phases);
            ClearParticles(); throw;
        }
        PX_EXT_PINNED_MEMORY_FREE(cuda_, positions); PX_EXT_PINNED_MEMORY_FREE(cuda_, velocities); PX_EXT_PINNED_MEMORY_FREE(cuda_, phases);
    }
    void Draw(const Camera& camera, int width, int height)
    {
        if (width <= 0 || height <= 0)return;
        if (count_ && revision_ != world_.GetSimulationRevision()) {
            physx::PxScopedCudaLock lock(cuda_); Check(map_(1, &resource_, nullptr));
            try { CUdeviceptr dst = 0; size_t bytes = 0; Check(pointer_(&dst, &bytes, resource_)); if (bytes < 2 * count_ * sizeof(physx::PxVec4))throw std::runtime_error("Liquid GL buffer too small"); Check(cuda_.getCudaContext()->memcpyDtoDAsync(dst, reinterpret_cast<CUdeviceptr>(particles_->getPositionInvMasses()), count_ * sizeof(physx::PxVec4), nullptr)); Check(cuda_.getCudaContext()->memcpyDtoDAsync(dst + count_ * sizeof(physx::PxVec4), reinterpret_cast<CUdeviceptr>(particles_->getVelocities()), count_ * sizeof(physx::PxVec4), nullptr)); }
            catch (...) { unmap_(1, &resource_, nullptr); throw; }Check(unmap_(1, &resource_, nullptr)); revision_ = world_.GetSimulationRevision();
        }
        if (displayMode_ == 0 && count_) { if (!surface_)surface_ = std::make_unique<LiquidSurface>(); surface_->Draw(vbo_, count_, simulationSpacing_, camera, width, height, world_.GetSimulationRevision(), parameters_.gravityScale, glm::min(position - size * .5f, containerPosition_ - containerSize_ * .5f), glm::max(position + size * .5f, containerPosition_ + containerSize_ * .5f)); }
        shader_.Use(); shader_.SetMatrix4("view", camera.GetViewMatrix()); shader_.SetMatrix4("projection", camera.GetProjectionMatrix(float(width) / height));
        shader_.SetFloat("radius", renderRadius_); shader_.SetFloat("viewportHeight", float(height)); shader_.SetFloat("region", 0);
        const bool pointSize = glIsEnabled(0x8642) != 0; glEnable(0x8642); GL::BindVertexArray(vao_); if (displayMode_ == 1)glDrawArrays(GL_POINTS, 0, count_); if (!pointSize)glDisable(0x8642);
        std::array<physx::PxVec4, 24> lines{}; const int edges[24] = { 0,1,0,2,0,4,1,3,1,5,2,3,2,6,3,7,4,5,4,6,5,7,6,7 };
        for (int i = 0; i < 24; ++i) { int c = edges[i]; auto p = position + size * glm::vec3(c & 1 ? .5f : -.5f, c & 2 ? .5f : -.5f, c & 4 ? .5f : -.5f); lines[i] = physx::PxVec4(p.x, p.y, p.z, 1); }
        GL::BindBuffer(GL::ArrayBuffer, boxVbo_); GL::BufferData(GL::ArrayBuffer, sizeof(lines), lines.data(), 0x88E8);
        shader_.SetFloat("region", 1); GL::BindVertexArray(boxVao_); glDrawArrays(GL_LINES, 0, 24);
        for (int i = 0; i < 24; ++i) { int c = edges[i]; auto p = containerPosition_ + containerSize_ * glm::vec3(c & 1 ? .5f : -.5f, c & 2 ? .5f : -.5f, c & 4 ? .5f : -.5f); lines[i] = physx::PxVec4(p.x, p.y, p.z, 1); }
        GL::BufferData(GL::ArrayBuffer, sizeof(lines), lines.data(), 0x88E8); shader_.SetFloat("region", 2); glDrawArrays(GL_LINES, 0, 24);
        GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0);
    }
private:
    using Register = int(WINAPI*)(void**, unsigned int, unsigned int); using Unregister = int(WINAPI*)(void*);
    using Map = int(WINAPI*)(unsigned int, void**, CUstream); using Pointer = int(WINAPI*)(CUdeviceptr*, size_t*, void*);
    template<class T>T Load(const char* name) { auto f = reinterpret_cast<T>(GetProcAddress(driver_, name)); if (!f)throw std::runtime_error("CUDA interop unavailable"); return f; }
    static void Check(int code) { if (code)throw std::runtime_error("CUDA liquid transfer failed"); }
    static void Configure(GLuint vao, GLuint buffer) { GL::BindVertexArray(vao); GL::BindBuffer(GL::ArrayBuffer, buffer); GL::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(physx::PxVec4), nullptr); GL::EnableVertexAttribArray(0); GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0); }
    void ClearParticles() { if (resource_) { glFinish(); physx::PxScopedCudaLock lock(cuda_); cuda_.getCudaContext()->streamSynchronize(nullptr); }if (resource_) { physx::PxScopedCudaLock lock(cuda_); unregister_(resource_); resource_ = nullptr; }if (particles_) { if (system_)system_->removeParticleBuffer(particles_); particles_->release(); particles_ = nullptr; }if (system_) { system_->release(); system_ = nullptr; }if (material_) { material_->release(); material_ = nullptr; }count_ = 0; }
    void Release() { ClearParticles(); if (container_) { container_->release(); container_ = nullptr; }GL::DeleteBuffers(1, &vbo_); GL::DeleteBuffers(1, &boxVbo_); GL::DeleteVertexArrays(1, &vao_); GL::DeleteVertexArrays(1, &boxVao_); if (driver_) { FreeLibrary(driver_); driver_ = nullptr; } }
    unsigned int phase_ = 0; float simulationSpacing_ = 0;
    int displayMode_ = 0;
    std::unique_ptr<LiquidSurface> surface_;
    Parameters parameters_;
    glm::vec3 containerPosition_{ 0,4,0 }, containerSize_{ 10,8,10 };
    physx::PxRigidStatic* container_ = nullptr;
    PhysicsWorld& world_; physx::PxCudaContextManager& cuda_; Shader shader_;
    physx::PxPBDParticleSystem* system_ = nullptr; physx::PxParticleBuffer* particles_ = nullptr; physx::PxPBDMaterial* material_ = nullptr;
    HMODULE driver_ = nullptr; void* resource_ = nullptr; Register reg_ = nullptr; Unregister unregister_ = nullptr; Map map_ = nullptr, unmap_ = nullptr; Pointer pointer_ = nullptr;
    GLuint vao_ = 0, vbo_ = 0, boxVao_ = 0, boxVbo_ = 0; unsigned int count_ = 0; float renderRadius_ = .088f; unsigned long long revision_ = 0;
};


#pragma once
#include "Shader.h"
#include "Camera.h"
#include <PxIsosurfaceExtraction.h>
#include <PxSmoothing.h>
#include <PxAnisotropy.h>
#include <gpu/PxPhysicsGpu.h>
#include <extensions/PxCudaHelpersExt.h>
#include <algorithm>
#include <limits>
#include <fstream>
#include <vector>
#include <cmath>

class LiquidSurface : public physx::PxParticleSystemCallback
{
public:
    struct DebugInfo {
        unsigned vertices = 0, triangles = 0, maxVertices = 0, maxTriangles = 0, subgrids = 0, particles = 0, emptyFrames = 0;
        unsigned glBefore = 0, glAfter = 0, invalidParticles = 0, invalidVertices = 0, rawTriangles = 0;
        unsigned long long revision = 0, snapshotRevision = 0;
        bool processed = false, snapshot = false, logOk = false;
        glm::vec3 particleMin{ 0 }, particleMax{ 0 }, vertexMin{ 0 }, vertexMax{ 0 };
    };
    const DebugInfo& Debug() const { return debug_; }
    void RequestSnapshot() { snapshotRequested_ = true; }
    LiquidSurface(physx::PxCudaContextManager& cuda, unsigned capacity, float spacing) :cuda_(cuda), shader_("Assets/Shaders/liquid_surface.glsl")
    {
        try {
            driver_ = LoadLibraryW(L"nvcuda.dll"); if (!driver_)throw std::runtime_error("CUDA driver unavailable");
            reg_ = Load<Register>("cuGraphicsGLRegisterBuffer"); unregister_ = Load<Unregister>("cuGraphicsUnregisterResource");
            map_ = Load<Map>("cuGraphicsMapResources"); unmap_ = Load<Map>("cuGraphicsUnmapResources"); pointer_ = Load<Pointer>("cuGraphicsResourceGetMappedPointer_v2");
            maxVertices_ = std::max(65536u, capacity * 16u); maxTriangles_ = maxVertices_ * 2u;
            physx::PxSparseGridParams grid; grid.subgridSizeX = grid.subgridSizeY = grid.subgridSizeZ = 16; grid.haloSize = 0;
            grid.maxNumSubgrids = capacity > 20000 ? 2048 : 512; debug_.subgrids = grid.maxNumSubgrids; debug_.maxVertices = maxVertices_; debug_.maxTriangles = maxTriangles_; grid.gridSpacing = spacing * .75f;
            physx::PxIsosurfaceParams params; params.particleCenterToIsosurfaceDistance = spacing * .8f; params.numMeshSmoothingPasses = 4; params.numMeshNormalSmoothingPasses = 4;
            extractor_ = PxGetPhysicsGpu()->createSparseGridIsosurfaceExtractor(&cuda_, grid, params, capacity, maxVertices_, maxTriangles_);
            if (!extractor_)throw std::runtime_error("Cannot create native PhysX water surface");
            smoother_ = PxGetPhysicsGpu()->createSmoothedPositionGenerator(&cuda_, capacity, .5f);
            anisotropy_ = PxGetPhysicsGpu()->createAnisotropyGenerator(&cuda_, capacity, 5.0f, 1.0f, 2.0f);
            if (!smoother_ || !anisotropy_)throw std::runtime_error("Cannot create native water smoothing");
            for (auto& buffer : processed_) { buffer = PX_EXT_DEVICE_MEMORY_ALLOC(physx::PxVec4, cuda_, capacity); if (!buffer)throw std::runtime_error("Cannot allocate water smoothing buffer"); }
            smoother_->setResultBufferDevice(processed_[0]);
            anisotropy_->setResultBufferDevice(processed_[1], processed_[2], processed_[3]);
            GL::GenVertexArrays(1, &vao_); GL::GenBuffers(3, buffers_); GL::BindVertexArray(vao_);
            for (unsigned i = 0; i < 3; ++i) {
                const auto target = i == 2 ? 0x8893 : GL::ArrayBuffer;
                GL::BindBuffer(target, buffers_[i]); const size_t bytes = i == 2 ? size_t(maxTriangles_) * 3 * sizeof(unsigned) : size_t(maxVertices_) * sizeof(physx::PxVec4);
                GL::BufferData(target, bytes, nullptr, 0x88E8);
                if (i < 2) { GL::VertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(physx::PxVec4), nullptr); GL::EnableVertexAttribArray(i); }
                physx::PxScopedCudaLock lock(cuda_); Check(reg_(&resources_[i], buffers_[i], 2));
            }
            GL::BindVertexArray(0); GL::BindBuffer(GL::ArrayBuffer, 0);
        }
        catch (...) { Release(); throw; }
    }
    ~LiquidSurface() { Release(); }
    LiquidSurface(const LiquidSurface&) = delete;
    LiquidSurface& operator=(const LiquidSurface&) = delete;
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    void Invalidate() { ready_ = false; revision_ = std::numeric_limits<unsigned long long>::max(); }
    void onBegin(const physx::PxGpuMirroredPointer<physx::PxGpuParticleSystem>&, CUstream) override {}
    void onAdvance(const physx::PxGpuMirroredPointer<physx::PxGpuParticleSystem>&, CUstream) override {}
    void onPostSolve(const physx::PxGpuMirroredPointer<physx::PxGpuParticleSystem>& system, CUstream stream) override
    {
        if (!enabled_) { ready_ = false; return; }
        const auto& data = *system.mHostPtr;
        anisotropy_->generateAnisotropy(system.mDevicePtr, data.mCommonData.mMaxParticles, stream);
        smoother_->generateSmoothedPositions(system.mDevicePtr, data.mCommonData.mMaxParticles, stream);
        anisotropyFactor_ = data.mCommonData.mParticleContactDistance; ready_ = true;
    }
    unsigned Triangles() const { return triangles_; }
    void Draw(physx::PxParticleBuffer& particles, unsigned long long revision, const Camera& camera, int width, int height)
    {
        debug_.glBefore = glGetError();
        if (revision_ != revision || snapshotRequested_) {
            physx::PxScopedCudaLock lock(cuda_); Check(map_(3, resources_, nullptr));
            try {
                CUdeviceptr ptr[3]{}; size_t bytes = 0;
                for (int i = 0; i < 3; ++i)Check(pointer_(&ptr[i], &bytes, resources_[i]));
                extractor_->setResultBufferDevice(reinterpret_cast<physx::PxVec4*>(ptr[0]), reinterpret_cast<unsigned*>(ptr[2]), reinterpret_cast<physx::PxVec4*>(ptr[1]));
                extractor_->extractIsosurface(ready_ ? processed_[0] : particles.getPositionInvMasses(), particles.getNbActiveParticles(), nullptr, particles.getPhases(), physx::PxParticlePhaseFlag::eParticlePhaseFluid, nullptr, ready_ ? processed_[1] : nullptr, ready_ ? processed_[2] : nullptr, ready_ ? processed_[3] : nullptr, anisotropyFactor_);
                Check(cuda_.getCudaContext()->streamSynchronize(nullptr));
                triangles_ = std::min(extractor_->getNumTriangles(), maxTriangles_);
                debug_.vertices = extractor_->getNumVertices(); debug_.triangles = triangles_; debug_.particles = particles.getNbActiveParticles(); debug_.revision = revision; debug_.processed = ready_;
                if (!triangles_)++debug_.emptyFrames;
                if (snapshotRequested_) {
                    Inspect(reinterpret_cast<CUdeviceptr>(particles.getPositionInvMasses()), debug_.particles, debug_.particleMin, debug_.particleMax, debug_.invalidParticles);
                    Inspect(ptr[0], debug_.vertices, debug_.vertexMin, debug_.vertexMax, debug_.invalidVertices);
                    extractor_->extractIsosurface(particles.getPositionInvMasses(), particles.getNbActiveParticles(), nullptr, particles.getPhases());
                    Check(cuda_.getCudaContext()->streamSynchronize(nullptr)); debug_.rawTriangles = extractor_->getNumTriangles();
                    extractor_->extractIsosurface(ready_ ? processed_[0] : particles.getPositionInvMasses(), particles.getNbActiveParticles(), nullptr, particles.getPhases(), physx::PxParticlePhaseFlag::eParticlePhaseFluid, nullptr, ready_ ? processed_[1] : nullptr, ready_ ? processed_[2] : nullptr, ready_ ? processed_[3] : nullptr, anisotropyFactor_);
                    Check(cuda_.getCudaContext()->streamSynchronize(nullptr)); triangles_ = std::min(extractor_->getNumTriangles(), maxTriangles_);
                    debug_.snapshot = true; debug_.snapshotRevision = revision; snapshotRequested_ = false; WriteLog("snapshot");
                }
                const bool saturated = debug_.vertices >= maxVertices_ || triangles_ >= maxTriangles_;
                if ((triangles_ == 0) != wasEmpty_ || saturated != wasSaturated_) { WriteLog("mesh-state"); wasEmpty_ = triangles_ == 0; wasSaturated_ = saturated; }
            }
            catch (...) { unmap_(3, resources_, nullptr); throw; }
            Check(unmap_(3, resources_, nullptr)); revision_ = revision;
        }
        shader_.Use(); shader_.SetMatrix4("view", camera.GetViewMatrix()); shader_.SetMatrix4("projection", camera.GetProjectionMatrix(float(width) / height)); shader_.SetVector3("eye", camera.position);
        const bool cull = glIsEnabled(GL_CULL_FACE) != 0; glDisable(GL_CULL_FACE);
        GL::BindVertexArray(vao_); glDrawElements(GL_TRIANGLES, triangles_ * 3, GL_UNSIGNED_INT, nullptr); GL::BindVertexArray(0);
        if (cull)glEnable(GL_CULL_FACE);
        debug_.glAfter = glGetError(); if ((debug_.glBefore || debug_.glAfter) && (debug_.glBefore != lastGlBefore_ || debug_.glAfter != lastGlAfter_))WriteLog("OpenGL");
        lastGlBefore_ = debug_.glBefore; lastGlAfter_ = debug_.glAfter;
    }
private:
    void Inspect(CUdeviceptr ptr, unsigned count, glm::vec3& low, glm::vec3& high, unsigned& invalid) {
        std::vector<physx::PxVec4> data(count); if (count)Check(cuda_.getCudaContext()->memcpyDtoH(data.data(), ptr, size_t(count) * sizeof(physx::PxVec4)));
        low = glm::vec3(std::numeric_limits<float>::max()); high = -low; invalid = 0; unsigned valid = 0;
        for (const auto& p : data) { if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) { ++invalid; continue; }glm::vec3 v(p.x, p.y, p.z); low = glm::min(low, v); high = glm::max(high, v); ++valid; }
        if (!valid)low = high = glm::vec3(0);
    }
    void WriteLog(const char* event) {
        std::ofstream file("liquid_debug.log", std::ios::app); debug_.logOk = bool(file); if (!file)return;
        file << "event=" << event << " revision=" << debug_.revision << " particles=" << debug_.particles << " vertices=" << debug_.vertices << "/" << maxVertices_ << " triangles=" << debug_.triangles << "/" << maxTriangles_ << " subgrid_capacity=" << debug_.subgrids << " processed=" << debug_.processed << " gl_before=" << debug_.glBefore << " gl_after=" << debug_.glAfter;
        if (debug_.snapshot) {
            file << " snapshot_revision=" << debug_.snapshotRevision << " raw_triangles=" << debug_.rawTriangles << " invalid_particles=" << debug_.invalidParticles << " invalid_vertices=" << debug_.invalidVertices;
            for (int i = 0; i < 3; ++i)file << " particle_min" << i << "=" << debug_.particleMin[i] << " particle_max" << i << "=" << debug_.particleMax[i] << " vertex_min" << i << "=" << debug_.vertexMin[i] << " vertex_max" << i << "=" << debug_.vertexMax[i];
        }
        file << "\n";
    }
    DebugInfo debug_; bool snapshotRequested_ = false, wasEmpty_ = false, wasSaturated_ = false; unsigned lastGlBefore_ = 0, lastGlAfter_ = 0;
    using Register = int(WINAPI*)(void**, unsigned, unsigned); using Unregister = int(WINAPI*)(void*);
    using Map = int(WINAPI*)(unsigned, void**, CUstream); using Pointer = int(WINAPI*)(CUdeviceptr*, size_t*, void*);
    template<class T>T Load(const char* name) { auto f = reinterpret_cast<T>(GetProcAddress(driver_, name)); if (!f)throw std::runtime_error("CUDA surface interop unavailable"); return f; }
    static void Check(int code) { if (code) { std::ofstream("liquid_debug.log", std::ios::app) << "CUDA error=" << code << "\n"; throw std::runtime_error("CUDA surface operation failed: " + std::to_string(code)); } }
    void Release() {
        glFinish();
        if (extractor_) { physx::PxScopedCudaLock lock(cuda_); cuda_.getCudaContext()->streamSynchronize(nullptr); extractor_->release(); delete extractor_; extractor_ = nullptr; }
        if (smoother_) { smoother_->release(); smoother_ = nullptr; }
        if (anisotropy_) { anisotropy_->release(); anisotropy_ = nullptr; }
        for (auto& buffer : processed_)if (buffer) { PX_EXT_DEVICE_MEMORY_FREE(cuda_, buffer); buffer = nullptr; }
        for (auto& resource : resources_)if (resource) { physx::PxScopedCudaLock lock(cuda_); unregister_(resource); resource = nullptr; }
        GL::DeleteBuffers(3, buffers_); GL::DeleteVertexArrays(1, &vao_); if (driver_)FreeLibrary(driver_);
    }
    physx::PxCudaContextManager& cuda_; Shader shader_; physx::PxIsosurfaceExtractor* extractor_ = nullptr;
    physx::PxSmoothedPositionGenerator* smoother_ = nullptr; physx::PxAnisotropyGenerator* anisotropy_ = nullptr;
    physx::PxVec4* processed_[4]{}; bool ready_ = false, enabled_ = true; float anisotropyFactor_ = 1;
    HMODULE driver_ = nullptr; Register reg_ = nullptr; Unregister unregister_ = nullptr; Map map_ = nullptr, unmap_ = nullptr; Pointer pointer_ = nullptr;
    GLuint vao_ = 0, buffers_[3]{}; void* resources_[3]{}; unsigned maxVertices_ = 0, maxTriangles_ = 0, triangles_ = 0;
    unsigned long long revision_ = std::numeric_limits<unsigned long long>::max();
};


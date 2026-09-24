#pragma once
#include <PxPhysicsAPI.h>
#include <extensions/PxCudaHelpersExt.h>
#include <cudamanager/PxCudaContext.h>
#include <stdexcept>
#include "LiquidParticleCleanupPtx.h"

// Call only between completed simulation steps, after fetchResultsParticleSystem().
// This compacts an ordinary fluid buffer with no cloth, attachments or springs.
class LiquidParticleCleanup
{
public:
    explicit LiquidParticleCleanup(physx::PxCudaContextManager& cuda) : cuda_(cuda) {}
    ~LiquidParticleCleanup()
    {
        physx::PxScopedCudaLock lock(cuda_);
        auto* context = cuda_.getCudaContext();
        context->streamSynchronize(nullptr);
        if (scratch_) context->memFree(scratch_);
        if (counter_) context->memFree(counter_);
        if (module_) context->moduleUnload(module_);
    }
    LiquidParticleCleanup(const LiquidParticleCleanup&) = delete;
    LiquidParticleCleanup& operator=(const LiquidParticleCleanup&) = delete;

    void Prepare(unsigned count)
    {
        physx::PxScopedCudaLock lock(cuda_);
        Ensure(count);
    }

    unsigned RemoveBelow(physx::PxParticleBuffer& particles, float killHeight)
    {
        using namespace physx;
        const unsigned count = particles.getNbActiveParticles();
        if (!count) return 0;
        unsigned kept = count;
        {
            PxScopedCudaLock lock(cuda_);
            auto* context = cuda_.getCudaContext();
            Ensure(count);
            Check(context->memsetD32Async(counter_, 0, 1, nullptr));
            CUdeviceptr positions = reinterpret_cast<CUdeviceptr>(particles.getPositionInvMasses());
            CUdeviceptr velocities = reinterpret_cast<CUdeviceptr>(particles.getVelocities());
            CUdeviceptr phases = reinterpret_cast<CUdeviceptr>(particles.getPhases());
            CUdeviceptr outPositions = scratch_;
            CUdeviceptr outVelocities = scratch_ + size_t(capacity_) * sizeof(PxVec4);
            CUdeviceptr outPhases = scratch_ + size_t(capacity_) * 2 * sizeof(PxVec4);
            unsigned inputCount = count;
            void* args[] = { &positions, &velocities, &phases, &outPositions,
                &outVelocities, &outPhases, &counter_, &inputCount, &killHeight };
            Check(context->launchKernel(kernel_, (count + 255) / 256, 1, 1,
                256, 1, 1, 0, nullptr, args, nullptr, __FILE__, __LINE__));
            Check(context->streamSynchronize(nullptr));
            // Only four bytes cross from GPU to CPU, not the particle arrays.
            Check(context->memcpyDtoH(&kept, counter_, sizeof(kept)));
            if (kept > count) throw std::runtime_error("Invalid liquid cleanup count");
            if (kept == count) return count;
            if (kept)
            {
                Check(context->memcpyDtoDAsync(positions, outPositions, size_t(kept) * sizeof(PxVec4), nullptr));
                Check(context->memcpyDtoDAsync(velocities, outVelocities, size_t(kept) * sizeof(PxVec4), nullptr));
                Check(context->memcpyDtoDAsync(phases, outPhases, size_t(kept) * sizeof(PxU32), nullptr));
                Check(context->streamSynchronize(nullptr));
            }
        }
        // The first 'kept' entries are now the entire active simulation population.
        particles.setNbActiveParticles(kept);
        particles.raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
        particles.raiseFlags(PxParticleBufferFlag::eUPDATE_VELOCITY);
        particles.raiseFlags(PxParticleBufferFlag::eUPDATE_PHASE);
        return kept;
    }

private:
    static void Check(physx::PxCUresult result)
    {
        if (result) throw std::runtime_error("CUDA liquid cleanup failed");
    }
    void Ensure(unsigned count)
    {
        auto* context = cuda_.getCudaContext();
        if (!module_) Check(context->moduleLoadDataEx(&module_, LiquidParticleCleanupPtx, 0, nullptr, nullptr));
        if (!kernel_) Check(context->moduleGetFunction(&kernel_, module_, "compactLiquidParticles"));
        if (!counter_) Check(context->memAlloc(&counter_, sizeof(unsigned)));
        if (count > capacity_)
        {
            CUdeviceptr next = 0;
            Check(context->memAlloc(&next, size_t(count) * (2 * sizeof(physx::PxVec4) + sizeof(physx::PxU32))));
            if (scratch_) context->memFree(scratch_);
            scratch_ = next;
            capacity_ = count;
        }
    }
    physx::PxCudaContextManager& cuda_;
    CUmodule module_ = nullptr;
    CUfunction kernel_ = nullptr;
    CUdeviceptr scratch_ = 0, counter_ = 0;
    unsigned capacity_ = 0;
};

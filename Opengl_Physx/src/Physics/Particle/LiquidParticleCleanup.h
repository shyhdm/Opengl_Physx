#pragma once
#include <PxPhysicsAPI.h>
#include <extensions/PxCudaHelpersExt.h>
#include <cudamanager/PxCudaContext.h>
#include <stdexcept>
#include <vector>
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
        if (ids_) context->memFree(ids_);
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

    CUdeviceptr Ids() const { return ids_; }
    void AssignIds(unsigned offset, unsigned count, unsigned first) {
        physx::PxScopedCudaLock lock(cuda_);
        if (offset > capacity_ || count > capacity_ - offset) throw std::runtime_error("Sand ID capacity exceeded");
        auto* context = cuda_.getCudaContext();
        if (!ids_) Check(context->memAlloc(&ids_, size_t(capacity_) * sizeof(unsigned)));
        std::vector<unsigned> values(count);
        for (unsigned i = 0; i < count; ++i) values[i] = first + i;
        if (count) Check(context->memcpyHtoD(ids_ + size_t(offset) * sizeof(unsigned), values.data(), size_t(count) * sizeof(unsigned)));
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
            unsigned inputCount = count;
            void* args[] = { &positions, &velocities, &counter_, &inputCount, &killHeight };
            Check(context->launchKernel(kernel_, (count + 255) / 256, 1, 1,
                256, 1, 1, 0, nullptr, args, nullptr, __FILE__, __LINE__));
            Check(context->streamSynchronize(nullptr));
            unsigned removed = 0;
            Check(context->memcpyDtoH(&removed, counter_, sizeof(removed)));
            if (removed > count) throw std::runtime_error("Invalid liquid cleanup count");
            if (!removed) return count;
            kept = count - removed;
            if (kept) {
                // Destinations are holes in [0, kept); donors are read-only entries in [kept, count).
                // Surviving prefix entries never move. IDs travel only with the tail donors.
                CUdeviceptr tail = counter_ + sizeof(unsigned);
                Check(context->memsetD32Async(tail, count, 1, nullptr));
                void* fillArgs[] = { &positions, &velocities, &phases, &ids_, &tail, &kept, &killHeight };
                Check(context->launchKernel(fillKernel_, (kept + 255) / 256, 1, 1,
                    256, 1, 1, 0, nullptr, fillArgs, nullptr, __FILE__, __LINE__));
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
        if (!kernel_) Check(context->moduleGetFunction(&kernel_, module_, "countRemovedParticles"));
        if (!fillKernel_) Check(context->moduleGetFunction(&fillKernel_, module_, "fillParticleHoles"));
        if (!counter_) Check(context->memAlloc(&counter_, 2 * sizeof(unsigned)));
        if (count > capacity_)
        {
            CUdeviceptr nextIds = 0;
            Check(context->streamSynchronize(nullptr));
            try {
                if (ids_) {
                    Check(context->memAlloc(&nextIds, size_t(count) * sizeof(unsigned)));
                    Check(context->memcpyDtoDAsync(nextIds, ids_, size_t(capacity_) * sizeof(unsigned), nullptr));
                    Check(context->streamSynchronize(nullptr));
                }
            }
            catch (...) {
                context->streamSynchronize(nullptr);
                if (nextIds) context->memFree(nextIds);
                throw;
            }
            if (ids_) { context->memFree(ids_); ids_ = nextIds; }
            capacity_ = count;
        }
    }
    physx::PxCudaContextManager& cuda_;
    CUmodule module_ = nullptr;
    CUfunction kernel_ = nullptr, fillKernel_ = nullptr;
    CUdeviceptr counter_ = 0, ids_ = 0;
    unsigned capacity_ = 0;
};

#pragma once
#include "FlowContext.h"
#include <nvflow/../../shared/NvFlowMath.h>
#include "Gpu/FlowVoxelCS_vulkan.hlsl.h"
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>

class FlowVoxelRenderer
{
public:
    explicit FlowVoxelRenderer(FlowContext& flow) : flow_(flow) {}
    ~FlowVoxelRenderer()
    {
        ReleaseResources();
    }
    void ReleaseResources() noexcept
    {
        if (constants_.empty() && !pipeline_) return;
        auto* context = flow_.Context();
        flow_.Loader().deviceInterface.waitIdle(flow_.DeviceQueue());
        for (auto& slot : constants_) flow_.Interface().destroyBuffer(context, slot.buffer);
        if (pipeline_) flow_.Interface().destroyComputePipeline(context, pipeline_);
        constants_.clear();
        pipeline_ = nullptr;
    }
    FlowVoxelRenderer(const FlowVoxelRenderer&) = delete;
    FlowVoxelRenderer& operator=(const FlowVoxelRenderer&) = delete;

    NvFlowTextureTransient* Draw(const NvFlowGridRenderData& data,
        const NvFlowFloat4x4& view, const NvFlowFloat4x4& projection,
        NvFlowUint width, NvFlowUint height, NvFlowTextureTransient* depth, NvFlowTextureTransient* color,
        bool smokeMode, bool smoothSmoke, const std::array<std::array<float, 3>, 3>& smokeColors,
        float densityMax, float opacity, float volumeDensity, float brightness, int raySteps)
    {
        if (!data.densityTexture || !data.sparseBuffer || !data.sparseParams.layerCount || !data.sparseParams.levelCount)
            return color;
        const auto& layer = data.sparseParams.layers[0];
        const auto& level = data.sparseParams.levels[0];
        if (!layer.numLocations || !level.numLocations) return color;
        auto& api = flow_.Interface();
        auto* context = flow_.Context();
        const NvFlowDescriptorType types[] = { eNvFlowDescriptorType_constantBuffer,
            eNvFlowDescriptorType_structuredBuffer, eNvFlowDescriptorType_texture,
            eNvFlowDescriptorType_texture, eNvFlowDescriptorType_texture, eNvFlowDescriptorType_rwTexture };
        if (!pipeline_)
        {
            NvFlowBindingDesc bindings[6]{};
            for (unsigned i = 0; i < 6; ++i) { bindings[i].type = types[i]; bindings[i].bindingDesc.vulkan = { i,1,0 }; }
            NvFlowComputePipelineDesc desc{};
            desc.numBindingDescs = 6; desc.bindingDescs = bindings;
            desc.bytecode = { FlowVoxelCS_vulkan_hlsl,sizeof(FlowVoxelCS_vulkan_hlsl) };
            pipeline_ = api.createComputePipeline(context, &desc);
            if (!pipeline_) throw std::runtime_error("Flow GPU voxel pipeline creation failed");
        }
        Params params{};
        params.viewInverse = NvFlowMath::matrixTranspose(NvFlowMath::matrixInverse(view));
        params.projectionInverse = NvFlowMath::matrixTranspose(NvFlowMath::matrixInverse(projection));
        params.minimum = { (layer.locationMin.x - .5f) * layer.blockSizeWorld.x,
            (layer.locationMin.y - .5f) * layer.blockSizeWorld.y,(layer.locationMin.z - .5f) * layer.blockSizeWorld.z,0 };
        params.maximum = { (layer.locationMax.x + .5f) * layer.blockSizeWorld.x,
            (layer.locationMax.y + .5f) * layer.blockSizeWorld.y,(layer.locationMax.z + .5f) * layer.blockSizeWorld.z,0 };
        params.cellSize = { layer.blockSizeWorld.x / float(level.blockDimLessOne.x + 1),
            layer.blockSizeWorld.y / float(level.blockDimLessOne.y + 1),layer.blockSizeWorld.z / float(level.blockDimLessOne.z + 1),0 };
        params.viewport = { width,height,static_cast<NvFlowUint>(layer.layerAndLevel),0 };
        params.level = level;
        params.viewport.w = smokeMode ? (smoothSmoke ? 2u : 1u) : 0u;
        auto safe = [](float v, float fallback, float low, float high) {return std::clamp(std::isfinite(v) ? v : fallback, low, high); };
        for (size_t i = 0; i < 3; ++i) params.smokeColors[i] = { safe(smokeColors[i][0],.5f,0,1),safe(smokeColors[i][1],.5f,0,1),safe(smokeColors[i][2],.5f,0,1),0 };
        params.smokeControls = { safe(densityMax,2,.01f,10),safe(opacity,1,0,5) * safe(volumeDensity,6,.1f,10) * .2f,
            safe(brightness,.8f,0,20),float(std::clamp(raySteps,32,256)) };
        const auto completed = api.getLastFrameCompleted(context);
        size_t index = 0;
        while (index<constants_.size() && constants_[index].frame>completed) ++index;
        if (index == constants_.size())
        {
            NvFlowBufferDesc desc{}; desc.usageFlags = eNvFlowBufferUsage_constantBuffer;
            desc.sizeInBytes = sizeof(Params); desc.structureStride = 0;
            auto* buffer = api.createBuffer(context, eNvFlowMemoryType_upload, &desc);
            if (!buffer) throw std::runtime_error("Flow voxel constants allocation failed");
            constants_.push_back({ buffer,0 });
        }
        auto& slot = constants_[index];
        void* mapped = api.mapBuffer(context, slot.buffer);
        if (!mapped) throw std::runtime_error("Flow voxel constants upload failed");
        std::memcpy(mapped, &params, sizeof(params)); api.unmapBuffer(context, slot.buffer);
        slot.frame = api.getCurrentFrame(context);
        NvFlowTextureDesc desc{}; desc.textureType = eNvFlowTextureType_2d;
        desc.usageFlags = eNvFlowTextureUsage_texture | eNvFlowTextureUsage_rwTexture | eNvFlowTextureUsage_textureCopySrc;
        desc.format = eNvFlowFormat_r32g32b32a32_float; desc.width = width; desc.height = height; desc.depth = 1; desc.mipLevels = 1;
        auto* output = api.getTextureTransient(context, &desc);
        NvFlowDescriptorWrite writes[6]{};
        for (unsigned i = 0; i < 6; ++i) { writes[i].type = types[i]; writes[i].write.vulkan = { i,0,0 }; }
        NvFlowResource resources[6]{};
        resources[0].bufferTransient = api.registerBufferAsTransient(context, slot.buffer);
        resources[1].bufferTransient = data.sparseBuffer;
        resources[2].textureTransient = data.densityTexture;
        resources[3].textureTransient = depth;
        resources[4].textureTransient = color;
        resources[5].textureTransient = output;
        NvFlowPassComputeParams pass{}; pass.pipeline = pipeline_; pass.gridDim = { (width + 7) / 8,(height + 7) / 8,1 };
        pass.numDescriptorWrites = 6; pass.descriptorWrites = writes; pass.resources = resources; pass.debugLabel = "Flow GPU voxels";
        api.addPassCompute(context, &pass);
        return output;
    }
private:
    struct Params
    {
        NvFlowFloat4x4 viewInverse, projectionInverse;
        NvFlowFloat4 minimum, maximum, cellSize;
        NvFlowUint4 viewport;
        NvFlowSparseLevelParams level;
        NvFlowFloat4 smokeColors[3];
        NvFlowFloat4 smokeControls;
    };
    struct Constants { NvFlowBuffer* buffer; NvFlowUint64 frame; };
    FlowContext& flow_;
    NvFlowComputePipeline* pipeline_ = nullptr;
    std::vector<Constants> constants_;
};

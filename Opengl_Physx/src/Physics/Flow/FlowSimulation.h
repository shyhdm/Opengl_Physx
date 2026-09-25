#pragma once

#include "FlowContext.h"
#include "FlowVoxelRenderer.h"
#include "FlowRigidColliders.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <memory>
#include <vector>
#include <limits>

class FlowSimulation
{
public:
    struct Settings
    {
        bool emitting = true;
        float position[3] = { 0.0f, 0.32f, 0.0f };
        float radius = 0.8f;
        float upwardVelocity = 2.0f;
        float temperature = 3.750f;
        float fuel = 1.660f;
        float smoke = 1.350f;
        float coolingRate = 1.5f;
        float ignitionTemperature = 0.000f;
        float burnRate = 3.950f;
        float temperatureBuoyancy = 1.080f;
        float vorticityStrength = 1.700f;
        float velocityDamping = 0.205f;
        float smokeDissipation = 0.880f;
        float smokePerBurn = 10.000f;
        float cellSize = 0.10f;
        float renderDensity = 3.200f;
        float fireBrightness = 14.750f;
        int raySteps = 128;
        int displayMode = 0;
        float smokeColorDensity = 2.0f;
        float smokeOpacity = 1.0f;
        std::array<std::array<float, 3>, 3> smokeColors{ {
            {{1.0f, 1.0f, 1.0f}}, {{0.414f, 0.414f, 0.414f}}, {{0.245f, 0.245f, 0.245f}}
        } };
        float colormapMaxTemperature = 1.0f;
        std::array<float, 6> colormapPositions{ 0.0f, 0.05f, 0.15f, 0.818f, 0.819f, 1.0f };
        std::array<std::array<float, 4>, 6> colormapColors{ {
            {{0.0154f, 0.0177f, 0.0154f, 0.004902f}},
            {{0.26f, 0.26f, 0.26f, 0.504902f}},
            {{0.0f, 0.0f, 0.0f, 0.504902f}},
            {{1.0f, 0.28f, 0.0f, 0.8f}},
            {{1.0f, 0.21f, 0.0f, 0.8f}},
            {{1.0f, 0.87f, 0.11f, 0.7f}}
        } };
        std::array<float, 6> colormapIntensities{ 1.0f, 11.45f, 3.35f, 9.55f, 100.0f, 33.9f };
    };

    struct Readback
    {
        const NvFlowUint8* temperature = nullptr;
        NvFlowUint64 temperatureSize = 0u;
        const NvFlowUint8* smoke = nullptr;
        NvFlowUint64 smokeSize = 0u;
        NvFlowUint64 frame = 0u;
        NvFlowUint64 generation = 0u;
        std::shared_ptr<const std::vector<NvFlowUint8>> temperatureOwner;
        std::shared_ptr<const std::vector<NvFlowUint8>> smokeOwner;

        bool IsValid() const
        {
            return temperature && temperatureSize > 0u &&
                smoke && smokeSize > 0u;
        }
    };

    explicit FlowSimulation(FlowContext& flowContext)
        : flowContext_(flowContext), voxelRenderer_(flowContext)
    {
        // Allocate the simulation grid only when its scene becomes active.
    }

    ~FlowSimulation()
    {
        Shutdown();
    }

    FlowSimulation(const FlowSimulation&) = delete;
    FlowSimulation& operator=(const FlowSimulation&) = delete;

    Settings& GetSettings()
    {
        return settings_;
    }

    const Settings& GetSettings() const
    {
        return settings_;
    }

    bool IsSmoke() const { return smokeMode_; }
    Settings DefaultSettings() const
    {
        Settings value;
        if (smokeMode_)
        {
            value.radius = 1.6f;
            value.temperature = value.fuel = value.burnRate = value.smokePerBurn = 0.0f;
            value.upwardVelocity = 4.0f;
            value.smoke = 4.0f;
            value.smokeDissipation = 0.08f;
            value.velocityDamping = 0.05f;
            value.renderDensity = 6.0f;
            value.fireBrightness = 0.8f;
            value.colormapPositions = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
            for (auto& color : value.colormapColors) color = { 0.5f, 0.5f, 0.5f, 1.0f };
            value.colormapIntensities.fill(1.0f);
        }
        return value;
    }
    void SetSmoke(bool smoke)
    {
        if (smokeMode_ == smoke) return;
        const int displayMode = settings_.displayMode;
        if (smokeMode_) { smokeSettings_ = settings_; smokeSettingsSaved_ = true; }
        else fireSettings_ = settings_;
        smokeMode_ = smoke;
        settings_ = smoke ? (smokeSettingsSaved_ ? smokeSettings_ : DefaultSettings()) : fireSettings_;
        settings_.displayMode = displayMode;
        Reset();
    }

    void SetSceneActive(bool active)
    {
        if (sceneActive_ == active)
        {
            return;
        }
        sceneActive_ = active;
        Reset();
    }

    bool IsSceneActive() const
    {
        return sceneActive_;
    }

    void Reset()
    {
        // A force-clear only erases density; rebuilding also releases grid storage.
        Shutdown();
        colliders_.Reset();
        absoluteSimTime_ = 0.0;
        forceClearNextFrame_ = true;
        latestReadback_ = {};
        latestReadback_.generation = ++generation_;
        minimumReadbackFrame_ = std::numeric_limits<NvFlowUint64>::max();
        lastSubmittedFrame_ = 0;
        if (sceneActive_) Initialize();
    }

    void SyncRigidBodies(physx::PxScene& scene, const std::vector<physx::PxRigidActor*>& proxies = {}) { colliders_.Update(scene, settings_.cellSize, proxies); }

    void Update(float deltaTime, bool = false)
    {
        if (!sceneActive_ || !grid_ || !gridParams_)
        {
            return;
        }

        const float safeDeltaTime = std::clamp(
            std::isfinite(deltaTime) ? deltaTime : 0.0f,
            0.0f,
            1.0f / 15.0f
        );
        if (safeDeltaTime <= 0.0f && !forceClearNextFrame_) return;
        simulateParams_.nanoVdbExport.enabled = NV_FLOW_FALSE;
        simulateParams_.nanoVdbExport.readbackEnabled = NV_FLOW_FALSE;
        simulateParams_.simulateWhenPaused = NV_FLOW_FALSE;
        ApplySettings();
        absoluteSimTime_ += static_cast<double>(safeDeltaTime);
        ++version_;

        parameterPointers_ = { reinterpret_cast<NvFlowUint8*>(&simulateParams_),
            reinterpret_cast<NvFlowUint8*>(&emitterParams_), reinterpret_cast<NvFlowUint8*>(&renderParams_),
            reinterpret_cast<NvFlowUint8*>(&offscreenParams_) };
        typeSnapshots_ = { {
            {version_, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType, &parameterPointers_[0], 1u},
            {version_, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType, &parameterPointers_[1], 1u},
            {version_, &NvFlowGridRenderLayerParams_NvFlowReflectDataType, &parameterPointers_[2], 1u},
            {version_, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType, &parameterPointers_[3], 1u},
            {version_, &NvFlowGridEmitterBoxParams_NvFlowReflectDataType, colliders_.Data(), colliders_.Count()}
        } };

        NvFlowDatabaseSnapshot databaseSnapshot =
        {
            version_,
            typeSnapshots_.data(),
            5u
        };

        NvFlowGridParamsDescSnapshot commitSnapshot =
        {
            databaseSnapshot,
            absoluteSimTime_,
            safeDeltaTime,
            static_cast<NvFlowBool32>(forceClearNextFrame_ ? NV_FLOW_TRUE : NV_FLOW_FALSE),
            nullptr,
            0u
        };

        auto& loader = flowContext_.Loader();
        loader.gridParamsInterface.commitParams(gridParams_, &commitSnapshot);

        NvFlowGridParamsSnapshot* paramsSnapshot =
            loader.gridParamsInterface.getParamsSnapshot(
                gridParams_,
                absoluteSimTime_,
                0u
            );

        NvFlowGridParamsDesc paramsDesc{};
        if (paramsSnapshot &&
            loader.gridParamsInterface.mapParamsDesc(
                gridParams_,
                paramsSnapshot,
                &paramsDesc
            ))
        {
            loader.gridInterface.simulate(
                flowContext_.Context(),
                grid_,
                &paramsDesc,
                forceClearNextFrame_ ? NV_FLOW_TRUE : NV_FLOW_FALSE
            );

            loader.gridParamsInterface.unmapParamsDesc(
                gridParams_,
                paramsSnapshot
            );
        }

        else
        {
            throw std::runtime_error("NVIDIA Flow could not map the simulation parameters.");
        }

        forceClearNextFrame_ = false;

        NvFlowUint64 flushedFrame = 0u;
        const int deviceReset = loader.deviceInterface.flush(
            flowContext_.DeviceQueue(),
            &flushedFrame,
            nullptr,
            nullptr
        );
        if (deviceReset)
        {
            throw std::runtime_error("NVIDIA Flow Vulkan device was reset.");
        }
        lastSubmittedFrame_ = flushedFrame;

    }
    NvFlowUint64 LastSubmittedFrame() const
    {
        return lastSubmittedFrame_;
    }

    const Readback& LatestReadback() const
    {
        return latestReadback_;
    }

    NvFlowTextureTransient* RenderNative(const NvFlowFloat4x4& view, const NvFlowFloat4x4& projection,
        NvFlowUint width, NvFlowUint height, NvFlowTextureTransient* depth, NvFlowTextureTransient* color)
    {
        if (!sceneActive_ || !grid_ || absoluteSimTime_ <= 0.0) return color;
        ApplySettings();
        auto& loader = flowContext_.Loader();
        if (settings_.displayMode == 1)
        {
            NvFlowGridRenderData data{};
            loader.gridInterface.getRenderData(flowContext_.Context(), grid_, &data);
            return voxelRenderer_.Draw(data, view, projection, width, height, depth, color,
                smokeMode_, settings_.displayMode == 0, settings_.smokeColors, settings_.smokeColorDensity,
                settings_.smokeOpacity, settings_.renderDensity, settings_.fireBrightness, settings_.raySteps);
        }
        auto* snapshot = loader.gridParamsInterface.getParamsSnapshot(gridParams_, absoluteSimTime_, 0u);
        NvFlowGridParamsDesc params{};
        if (!snapshot || !loader.gridParamsInterface.mapParamsDesc(gridParams_, snapshot, &params))
            throw std::runtime_error("Flow native render parameters unavailable");
        NvFlowTextureTransient* output = nullptr;
        loader.gridInterface.offscreen(flowContext_.Context(), grid_, &params);
        loader.gridInterface.render(flowContext_.Context(), grid_, &params, &view, &projection, &projection,
            width, height, width, height, 1.0f, depth, eNvFlowFormat_r32g32b32a32_float, color, &output);
        loader.gridParamsInterface.unmapParamsDesc(gridParams_, snapshot);
        if (!output) throw std::runtime_error("Flow native renderer returned no output");
        return output;
    }

private:
    void Initialize()
    {
        auto& loader = flowContext_.Loader();
        NvFlowContext* context = flowContext_.Context();
        if (!context)
        {
            throw std::runtime_error("NVIDIA Flow context is unavailable.");
        }

        NvFlowGridDesc gridDesc = NvFlowGridDesc_default;
        gridDesc.maxLocations = 8192u;

        grid_ = loader.gridInterface.createGrid(
            &flowContext_.Interface(),
            context,
            loader.opList_orig,
            loader.extOpList_orig,
            &gridDesc
        );
        if (!grid_)
        {
            throw std::runtime_error("NVIDIA Flow failed to create a simulation grid.");
        }

        gridParams_ = loader.gridParamsInterface.createGridParams();
        if (!gridParams_)
        {
            Shutdown();
            throw std::runtime_error("NVIDIA Flow failed to create grid parameters.");
        }

        simulateParams_ = NvFlowGridSimulateLayerParams_default;
        emitterParams_ = NvFlowEmitterSphereParams_default;

        simulateParams_.nanoVdbExport.enabled = NV_FLOW_FALSE;
        simulateParams_.nanoVdbExport.readbackEnabled = NV_FLOW_FALSE;
        simulateParams_.nanoVdbExport.temperatureEnabled = NV_FLOW_TRUE;
        ApplySettings();
    }

    void ApplySettings()
    {
        settings_.radius = std::clamp(settings_.radius, 0.1f, 5.0f);
        settings_.upwardVelocity = std::clamp(settings_.upwardVelocity, 0.0f, 30.0f);
        settings_.temperature = std::clamp(settings_.temperature, 0.0f, 10.0f);
        settings_.fuel = std::clamp(settings_.fuel, 0.0f, 5.0f);
        settings_.smoke = std::clamp(settings_.smoke, 0.0f, 5.0f);
        settings_.cellSize = std::clamp(settings_.cellSize, 0.08f, 1.0f);
        settings_.renderDensity = std::clamp(settings_.renderDensity, 0.1f, 10.0f);
        settings_.fireBrightness = std::clamp(settings_.fireBrightness, 0.0f, 20.0f);
        settings_.raySteps = std::clamp(settings_.raySteps, 32, 256);

        settings_.coolingRate = std::isfinite(settings_.coolingRate) ?
            std::clamp(settings_.coolingRate, 0.0f, 10.0f) : Settings{}.coolingRate;
        simulateParams_.advection.coolingRate = settings_.coolingRate;
        settings_.ignitionTemperature = std::isfinite(settings_.ignitionTemperature) ?
            std::clamp(settings_.ignitionTemperature, 0.0f, 5.0f) : Settings{}.ignitionTemperature;
        simulateParams_.advection.ignitionTemp = settings_.ignitionTemperature;
        settings_.burnRate = std::isfinite(settings_.burnRate) ?
            std::clamp(settings_.burnRate, 0.0f, 20.0f) : Settings{}.burnRate;
        simulateParams_.advection.burnPerTemp = smokeMode_ ? 0.0f : settings_.burnRate;
        settings_.temperatureBuoyancy = std::isfinite(settings_.temperatureBuoyancy) ?
            std::clamp(settings_.temperatureBuoyancy, 0.0f, 10.0f) : Settings{}.temperatureBuoyancy;
        simulateParams_.advection.buoyancyPerTemp = settings_.temperatureBuoyancy;
        simulateParams_.advection.buoyancyPerSmoke = smokeMode_ ? 0.15f : 0.0f;
        settings_.vorticityStrength = std::isfinite(settings_.vorticityStrength) ?
            std::clamp(settings_.vorticityStrength, 0.0f, 5.0f) : Settings{}.vorticityStrength;
        simulateParams_.vorticity.forceScale = settings_.vorticityStrength;
        settings_.velocityDamping = std::isfinite(settings_.velocityDamping) ?
            std::clamp(settings_.velocityDamping, 0.0f, 0.99f) : Settings{}.velocityDamping;
        simulateParams_.advection.velocity.damping = settings_.velocityDamping;
        settings_.smokeDissipation = std::isfinite(settings_.smokeDissipation) ?
            std::clamp(settings_.smokeDissipation, 0.0f, 5.0f) : Settings{}.smokeDissipation;
        simulateParams_.advection.smoke.fade = settings_.smokeDissipation;
        simulateParams_.advection.smoke.damping = smokeMode_ ? 0.08f : 0.30f;
        settings_.smokePerBurn = std::isfinite(settings_.smokePerBurn) ?
            std::clamp(settings_.smokePerBurn, 0.0f, 10.0f) : Settings{}.smokePerBurn;
        simulateParams_.advection.smokePerBurn = settings_.smokePerBurn;

        renderParams_.rayMarch.colorScale = settings_.fireBrightness / 3.0f;
        renderParams_.rayMarch.attenuation = (smokeMode_ ? 0.4f : 0.05f) * settings_.renderDensity / 2.0f;
        renderParams_.rayMarch.stepSizeScale = 0.75f * 128.0f / float(settings_.raySteps);
        settings_.colormapMaxTemperature = std::isfinite(settings_.colormapMaxTemperature) ?
            std::clamp(settings_.colormapMaxTemperature, 0.01f, 10.0f) : 1.0f;
        for (size_t i = 0; i < settings_.colormapPositions.size(); ++i)
        {
            const float minimum = i ? settings_.colormapPositions[i - 1] + 0.001f : 0.0f;
            const float maximum = 1.0f - float(5 - i) * 0.001f;
            auto& position = settings_.colormapPositions[i];
            position = std::clamp(std::isfinite(position) ? position : minimum, minimum, maximum);
            auto& intensity = settings_.colormapIntensities[i];
            intensity = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 100.0f) : 1.0f;
            auto& color = settings_.colormapColors[i];
            for (auto& component : color)
                component = std::isfinite(component) ? std::clamp(component, 0.0f, 1.0f) : 0.0f;
            colormapRgba_[i] = { color[0] * intensity, color[1] * intensity, color[2] * intensity, color[3] };
        }
        renderParams_.rayMarch.colormapXMin = 0.0f;
        renderParams_.rayMarch.colormapXMax = settings_.colormapMaxTemperature;
        offscreenParams_.colormap.xPoints = settings_.colormapPositions.data();
        offscreenParams_.colormap.xPointCount = settings_.colormapPositions.size();
        offscreenParams_.colormap.rgbaPoints = colormapRgba_.data();
        offscreenParams_.colormap.rgbaPointCount = colormapRgba_.size();
        offscreenParams_.colormap.colorScale = smokeMode_ ? 2.5f : 0.2f;
        offscreenParams_.debugVolume = NvFlowDebugVolumeParams_default;
        if (smokeMode_)
        {
            auto safe = [](float v, float fallback, float low, float high) {return std::clamp(std::isfinite(v) ? v : fallback, low, high); };
            settings_.smokeColorDensity = safe(settings_.smokeColorDensity, 2.0f, .01f, 10.0f);
            settings_.smokeOpacity = safe(settings_.smokeOpacity, 1.0f, 0.0f, 5.0f);
            for (auto& color : settings_.smokeColors) for (auto& v : color) v = safe(v, .5f, 0.0f, 1.0f);
            for (size_t i = 0; i < 6; ++i)
            {
                float t = float(i) / 5.0f;
                smokeColorPositions_[i] = t;
                float blend = t < .5f ? t * 2.0f : (t - .5f) * 2.0f;
                size_t a = t < .5f ? 0 : 1;
                const auto& lo = settings_.smokeColors[a]; const auto& hi = settings_.smokeColors[a + 1];
                colormapRgba_[i] = { lo[0] + (hi[0] - lo[0]) * blend,lo[1] + (hi[1] - lo[1]) * blend,lo[2] + (hi[2] - lo[2]) * blend,1 };
            }
            offscreenParams_.colormap.xPoints = smokeColorPositions_.data();
            offscreenParams_.colormap.colorScale = 1.0f;
            renderParams_.rayMarch.colormapXMax = settings_.smokeColorDensity;
            renderParams_.rayMarch.colorScale = settings_.fireBrightness;
            renderParams_.rayMarch.attenuation = .2f * settings_.renderDensity * settings_.smokeOpacity;
            offscreenParams_.debugVolume.enabled = NV_FLOW_TRUE;
            offscreenParams_.debugVolume.applyPreShadow = NV_FLOW_TRUE;
            offscreenParams_.debugVolume.outputTemperatureScale = 0.0f;
            offscreenParams_.debugVolume.outputTemperatureOffset = 1.0f;
            offscreenParams_.debugVolume.outputScaleBySmoke = NV_FLOW_TRUE;
        }

        simulateParams_.physicsCollisionEnabled = NV_FLOW_TRUE;
        simulateParams_.advection.gravity = { 0.0f, -50.0f, 0.0f };
        simulateParams_.densityCellSize = settings_.cellSize;
        emitterParams_.enabled = settings_.emitting ? NV_FLOW_TRUE : NV_FLOW_FALSE;
        emitterParams_.position =
        {
            settings_.position[0],
            settings_.position[1],
            settings_.position[2]
        };
        emitterParams_.radius = settings_.radius;
        emitterParams_.radiusIsWorldSpace = NV_FLOW_TRUE;
        emitterParams_.velocity = { 0.0f, settings_.upwardVelocity, 0.0f };
        emitterParams_.velocityIsWorldSpace = NV_FLOW_TRUE;
        emitterParams_.temperature = smokeMode_ ? 0.0f : settings_.temperature;
        emitterParams_.fuel = smokeMode_ ? 0.0f : settings_.fuel;
        emitterParams_.burn = 0.0f;
        emitterParams_.smoke = settings_.smoke;
        emitterParams_.coupleRateVelocity = 2.0f;
        emitterParams_.coupleRateTemperature = 2.0f;
        emitterParams_.coupleRateFuel = 2.0f;
        emitterParams_.coupleRateSmoke = 2.0f;
    }

    void Shutdown() noexcept
    {
        if (!grid_ && !gridParams_) return;
        auto& loader = flowContext_.Loader();
        if (flowContext_.DeviceQueue() && loader.deviceInterface.waitIdle)
        {
            loader.deviceInterface.waitIdle(flowContext_.DeviceQueue());
        }

        voxelRenderer_.ReleaseResources();

        if (grid_ && loader.gridInterface.destroyGrid)
        {
            loader.gridInterface.destroyGrid(flowContext_.Context(), grid_);
            grid_ = nullptr;
        }

        if (gridParams_ && loader.gridParamsInterface.destroyGridParams)
        {
            loader.gridParamsInterface.destroyGridParams(gridParams_);
            gridParams_ = nullptr;
        }
        flowContext_.CollectUnusedResources();
    }

    FlowContext& flowContext_;
    FlowVoxelRenderer voxelRenderer_;
    std::array<float, 6> smokeColorPositions_{};
    bool smokeMode_ = false, smokeSettingsSaved_ = false;
    Settings fireSettings_, smokeSettings_;
    FlowRigidColliders colliders_;
    Settings settings_{};
    std::array<NvFlowFloat4, 6> colormapRgba_{};
    std::array<NvFlowUint8*, 4> parameterPointers_{};
    std::array<NvFlowDatabaseTypeSnapshot, 5> typeSnapshots_{};
    NvFlowGrid* grid_ = nullptr;
    NvFlowGridParams* gridParams_ = nullptr;
    NvFlowGridOffscreenLayerParams offscreenParams_ = NvFlowGridOffscreenLayerParams_default;
    NvFlowGridRenderLayerParams renderParams_ = NvFlowGridRenderLayerParams_default;
    NvFlowGridSimulateLayerParams simulateParams_ = NvFlowGridSimulateLayerParams_default;
    NvFlowGridEmitterSphereParams emitterParams_ = NvFlowEmitterSphereParams_default;
    NvFlowUint64 version_ = 1u;
    NvFlowUint64 lastSubmittedFrame_ = 0u;
    Readback latestReadback_{};
    double absoluteSimTime_ = 0.0;
    NvFlowUint64 generation_ = 0u;
    NvFlowUint64 minimumReadbackFrame_ = 0u;
    bool sceneActive_ = false;
    bool forceClearNextFrame_ = true;
};

#pragma once

#include "FlowContext.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

class FlowSimulation
{
public:
    struct Settings
    {
        bool emitting = true;
        float position[3] = { 0.0f, 0.75f, 0.0f };
        float radius = 0.75f;
        float upwardVelocity = 4.0f;
        float temperature = 2.0f;
        float fuel = 1.0f;
        float smoke = 0.15f;
        float cellSize = 0.20f;
        float renderDensity = 2.0f;
        float fireBrightness = 3.0f;
        int raySteps = 128;
    };

    struct Readback
    {
        const NvFlowUint8* temperature = nullptr;
        NvFlowUint64 temperatureSize = 0u;
        const NvFlowUint8* smoke = nullptr;
        NvFlowUint64 smokeSize = 0u;
        NvFlowUint64 frame = 0u;

        bool IsValid() const
        {
            return temperature && temperatureSize > 0u &&
                smoke && smokeSize > 0u;
        }
    };

    explicit FlowSimulation(FlowContext& flowContext)
        : flowContext_(flowContext)
    {
        Initialize();
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

    void SetSceneActive(bool active)
    {
        if (sceneActive_ == active)
        {
            return;
        }
        sceneActive_ = active;
        if (sceneActive_)
        {
            Reset();
        }
    }

    bool IsSceneActive() const
    {
        return sceneActive_;
    }

    void Reset()
    {
        absoluteSimTime_ = 0.0;
        forceClearNextFrame_ = true;
        latestReadback_ = {};
    }

    void Update(float deltaTime)
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
        if (safeDeltaTime <= 0.0f)
        {
            return;
        }

        ApplySettings();
        absoluteSimTime_ += static_cast<double>(safeDeltaTime);
        ++version_;

        NvFlowGridSimulateLayerParams* simulatePointer = &simulateParams_;
        NvFlowGridEmitterSphereParams* emitterPointer = &emitterParams_;

        NvFlowDatabaseTypeSnapshot typeSnapshots[2] =
        {
            {
                version_,
                &NvFlowGridSimulateLayerParams_NvFlowReflectDataType,
                reinterpret_cast<NvFlowUint8**>(&simulatePointer),
                1u
            },
            {
                version_,
                &NvFlowGridEmitterSphereParams_NvFlowReflectDataType,
                reinterpret_cast<NvFlowUint8**>(&emitterPointer),
                1u
            }
        };

        NvFlowDatabaseSnapshot databaseSnapshot =
        {
            version_,
            typeSnapshots,
            2u
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

        NvFlowGridRenderData renderData{};
        loader.gridInterface.getRenderData(
            flowContext_.Context(),
            grid_,
            &renderData
        );

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

        readbackAccumulator_ += safeDeltaTime;
        if (readbackAccumulator_ >= 1.0f / 15.0f)
        {
            readbackAccumulator_ = 0.0f;
            loader.deviceInterface.waitForFrame(
                flowContext_.DeviceQueue(),
                flushedFrame
            );

            const NvFlowUint64 lastCompleted =
                flowContext_.Interface().getLastFrameCompleted(
                    flowContext_.Context()
                );

            for (NvFlowUint64 index = renderData.nanoVdb.readbacks ? renderData.nanoVdb.readbackCount : 0u;
                index > 0u;
                --index)
            {
                const NvFlowGridRenderDataNanoVdbReadback& candidate =
                    renderData.nanoVdb.readbacks[index - 1u];
                if (candidate.globalFrameCompleted <= lastCompleted &&
                    candidate.temperatureNanoVdbReadback &&
                    candidate.smokeNanoVdbReadback)
                {
                    latestReadback_.temperature = candidate.temperatureNanoVdbReadback;
                    latestReadback_.temperatureSize = candidate.temperatureNanoVdbReadbackSize;
                    latestReadback_.smoke = candidate.smokeNanoVdbReadback;
                    latestReadback_.smokeSize = candidate.smokeNanoVdbReadbackSize;
                    latestReadback_.frame = candidate.globalFrameCompleted;
                    break;
                }
            }
        }
    }

    NvFlowUint64 LastSubmittedFrame() const
    {
        return lastSubmittedFrame_;
    }

    const Readback& LatestReadback() const
    {
        return latestReadback_;
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

        simulateParams_.nanoVdbExport.enabled = NV_FLOW_TRUE;
        simulateParams_.nanoVdbExport.readbackEnabled = NV_FLOW_TRUE;
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
        emitterParams_.temperature = settings_.temperature;
        emitterParams_.fuel = settings_.fuel;
        emitterParams_.burn = 0.0f;
        emitterParams_.smoke = settings_.smoke;
        emitterParams_.coupleRateVelocity = 2.0f;
        emitterParams_.coupleRateTemperature = 2.0f;
        emitterParams_.coupleRateFuel = 2.0f;
        emitterParams_.coupleRateSmoke = 2.0f;
    }

    void Shutdown() noexcept
    {
        auto& loader = flowContext_.Loader();
        if (flowContext_.DeviceQueue() && loader.deviceInterface.waitIdle)
        {
            loader.deviceInterface.waitIdle(flowContext_.DeviceQueue());
        }

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
    }

    FlowContext& flowContext_;
    Settings settings_{};
    NvFlowGrid* grid_ = nullptr;
    NvFlowGridParams* gridParams_ = nullptr;
    NvFlowGridSimulateLayerParams simulateParams_ = NvFlowGridSimulateLayerParams_default;
    NvFlowGridEmitterSphereParams emitterParams_ = NvFlowEmitterSphereParams_default;
    NvFlowUint64 version_ = 1u;
    NvFlowUint64 lastSubmittedFrame_ = 0u;
    Readback latestReadback_{};
    double absoluteSimTime_ = 0.0;
    float readbackAccumulator_ = 0.0f;
    bool sceneActive_ = false;
    bool forceClearNextFrame_ = true;
};

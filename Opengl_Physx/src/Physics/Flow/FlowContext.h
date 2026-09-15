#pragma once

#include <Windows.h>
#include <nvflowext/NvFlowLoader.h>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

class FlowContext
{
public:
    explicit FlowContext(NvFlowUint deviceIndex = 0u)
    {
        try
        {
            Initialize(deviceIndex);
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    ~FlowContext()
    {
        Shutdown();
    }

    FlowContext(const FlowContext&) = delete;
    FlowContext& operator=(const FlowContext&) = delete;
    FlowContext(FlowContext&&) = delete;
    FlowContext& operator=(FlowContext&&) = delete;

    bool IsReady() const
    {
        return deviceManager_ != nullptr &&
            device_ != nullptr &&
            deviceQueue_ != nullptr;
    }

    NvFlowLoader& Loader()
    {
        return loader_;
    }

    NvFlowContextInterface& Interface()
    {
        return contextInterface_;
    }

    NvFlowDeviceQueue* DeviceQueue() const
    {
        return deviceQueue_;
    }

    NvFlowContext* Context() const
    {
        if (!deviceQueue_ || !loader_.deviceInterface.getContext)
        {
            return nullptr;
        }
        return loader_.deviceInterface.getContext(deviceQueue_);
    }

private:
    static void LoaderError(const char* message, void*)
    {
        OutputDebugStringA("NVIDIA Flow loader error: ");
        OutputDebugStringA(message ? message : "unknown error");
        OutputDebugStringA("\n");
    }

    static void LogPrint(NvFlowLogLevel level, const char* format, ...)
    {
        char message[2048]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        switch (level)
        {
        case eNvFlowLogLevel_error:
            OutputDebugStringA("NVIDIA Flow error: ");
            break;
        case eNvFlowLogLevel_warning:
            OutputDebugStringA("NVIDIA Flow warning: ");
            break;
        default:
            OutputDebugStringA("NVIDIA Flow: ");
            break;
        }
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }

    void Initialize(NvFlowUint deviceIndex)
    {
        NvFlowLoaderInitDeviceAPI(
            &loader_,
            LoaderError,
            nullptr,
            eNvFlowContextApi_vulkan
        );

        if (!loader_.module_nvflow || !loader_.module_nvflowext)
        {
            throw std::runtime_error(
                "NVIDIA Flow could not load nvflow.dll or nvflowext.dll."
            );
        }

        if (!loader_.deviceInterface.createDeviceManager ||
            !loader_.deviceInterface.createDevice ||
            !loader_.deviceInterface.getDeviceQueue ||
            !loader_.deviceInterface.getContextInterface ||
            !loader_.deviceInterface.getContext)
        {
            throw std::runtime_error(
                "NVIDIA Flow Vulkan device interface is incomplete."
            );
        }

        deviceManager_ = loader_.deviceInterface.createDeviceManager(
            NV_FLOW_FALSE,
            nullptr,
            0u
        );
        if (!deviceManager_)
        {
            throw std::runtime_error(
                "NVIDIA Flow failed to create the Vulkan device manager."
            );
        }

        NvFlowDeviceDesc deviceDesc{};
        deviceDesc.deviceIndex = deviceIndex;
        deviceDesc.enableExternalUsage = NV_FLOW_TRUE;
        deviceDesc.logPrint = LogPrint;

        device_ = loader_.deviceInterface.createDevice(
            deviceManager_,
            &deviceDesc
        );
        if (!device_)
        {
            throw std::runtime_error(
                "NVIDIA Flow failed to create the Vulkan device."
            );
        }

        deviceQueue_ = loader_.deviceInterface.getDeviceQueue(device_);
        if (!deviceQueue_)
        {
            throw std::runtime_error(
                "NVIDIA Flow failed to obtain its Vulkan device queue."
            );
        }

        NvFlowContextInterface* sourceInterface =
            loader_.deviceInterface.getContextInterface(deviceQueue_);
        if (!sourceInterface || !loader_.deviceInterface.getContext(deviceQueue_))
        {
            throw std::runtime_error(
                "NVIDIA Flow failed to create its Vulkan context."
            );
        }

        NvFlowContextInterface_duplicate(&contextInterface_, sourceInterface);
    }

    void Shutdown() noexcept
    {
        if (deviceQueue_ && loader_.deviceInterface.waitIdle)
        {
            loader_.deviceInterface.waitIdle(deviceQueue_);
        }

        if (device_ && deviceManager_ && loader_.deviceInterface.destroyDevice)
        {
            loader_.deviceInterface.destroyDevice(deviceManager_, device_);
        }
        deviceQueue_ = nullptr;
        device_ = nullptr;

        if (deviceManager_ && loader_.deviceInterface.destroyDeviceManager)
        {
            loader_.deviceInterface.destroyDeviceManager(deviceManager_);
        }
        deviceManager_ = nullptr;

        if (loader_.module_nvflow || loader_.module_nvflowext)
        {
            NvFlowLoaderDestroy(&loader_);
        }
        loader_.module_nvflow = nullptr;
        loader_.module_nvflowext = nullptr;
    }

    NvFlowLoader loader_{};
    NvFlowContextInterface contextInterface_{};
    NvFlowDeviceManager* deviceManager_ = nullptr;
    NvFlowDevice* device_ = nullptr;
    NvFlowDeviceQueue* deviceQueue_ = nullptr;
};

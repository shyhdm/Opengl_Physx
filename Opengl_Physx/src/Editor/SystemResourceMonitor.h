#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <vector>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

// Windows CPU utility, system RAM, busiest GPU engine and dedicated VRAM.
// CUDA is used only to identify the current OpenGL adapter LUID, not for metrics.
// Driver queries run off the render thread, once per second. No log files.
class SystemResourceMonitor
{
public:
    struct Reading { double percent = -1; unsigned long long used = 0, total = 0; };
    using Snapshot = std::array<Reading, 4>;
    SystemResourceMonitor()
    {
        const Adapter adapter = FindOpenGlDevice();
        worker = std::thread([this, adapter]() { Run(adapter); });
    }
    ~SystemResourceMonitor()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    SystemResourceMonitor(const SystemResourceMonitor&) = delete;
    SystemResourceMonitor& operator=(const SystemResourceMonitor&) = delete;
    Snapshot Read() const { std::lock_guard<std::mutex> lock(mutex); return latest; }
    static std::string Format(size_t index, const Reading& reading)
    {
        const char* labels[] = {"CPU", "RAM", "GPU", "VRAM"};
        char text[128];
        if (reading.percent < 0) std::snprintf(text,sizeof(text),"%s N/A",labels[index]);
        else if (index == 1 || index == 3)
            std::snprintf(text,sizeof(text),"%s %.1f/%.1f GiB (%.0f%%)",labels[index],
                reading.used/1073741824.0,reading.total/1073741824.0,reading.percent);
        else std::snprintf(text,sizeof(text),"%s %.0f%%",labels[index],reading.percent);
        return text;
    }
    static bool High(size_t index, const Reading& reading)
    { return reading.percent >= ((index == 1 || index == 3) ? 85.0 : 90.0); }
private:

    struct Adapter { std::wstring token; unsigned long long capacity = 0; };
    static Adapter FindOpenGlDevice()
    {
        Adapter result;
        HMODULE cuda=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!cuda) return result;
        using Init=int(WINAPI*)(unsigned);
        using Devices=int(WINAPI*)(unsigned*,int*,unsigned,unsigned);
        using GetLuid=int(WINAPI*)(char*,unsigned*,int);
        auto init=reinterpret_cast<Init>(GetProcAddress(cuda,"cuInit"));
        auto devices=reinterpret_cast<Devices>(GetProcAddress(cuda,"cuGLGetDevices_v2"));
        if (!devices) devices=reinterpret_cast<Devices>(GetProcAddress(cuda,"cuGLGetDevices"));
        auto luidFn=reinterpret_cast<GetLuid>(GetProcAddress(cuda,"cuDeviceGetLuid"));
        unsigned count=0, mask=0; int device=0; LUID luid{};
        bool valid=init && devices && luidFn && init(0)==0 && devices(&count,&device,1,1)==0 && count==1 &&
            luidFn(reinterpret_cast<char*>(&luid),&mask,device)==0;
        FreeLibrary(cuda);
        if (!valid) return result;
        wchar_t token[64];
        swprintf_s(token,L"luid_0x%08x_0x%08x_",static_cast<unsigned>(luid.HighPart),luid.LowPart);
        result.token=token;
        IDXGIFactory1* factory=nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory))))
        {
            for (UINT i=0;;++i)
            {
                IDXGIAdapter1* adapter=nullptr;
                if (factory->EnumAdapters1(i,&adapter)!=S_OK) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.HighPart==luid.HighPart && desc.AdapterLuid.LowPart==luid.LowPart)
                    result.capacity=desc.DedicatedVideoMemory;
                adapter->Release();
            }
            factory->Release();
        }
        return result;
    }
    static bool Good(DWORD status) { return status==PDH_CSTATUS_VALID_DATA || status==PDH_CSTATUS_NEW_DATA; }
    template<class Callback> static bool Each(PDH_HCOUNTER counter, Callback callback)
    {
        if (!counter) return false;
        for (int retry=0;retry<3;++retry)
        {
            DWORD bytes=0,count=0;
            auto status=PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,nullptr);
            if (status!=PDH_MORE_DATA || !bytes) return false;
            // Explicitly aligned storage for the counter structs and trailing names.
            std::vector<unsigned long long> storage((bytes+7)/8);
            auto* items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
            status=PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,items);
            if (status==PDH_MORE_DATA) continue; // Instances may appear during sampling.
            if (status!=ERROR_SUCCESS) return false;
            for (DWORD i=0;i<count;++i)
                if (Good(items[i].FmtValue.CStatus) && std::isfinite(items[i].FmtValue.doubleValue))
                {
                    std::wstring name(items[i].szName);
                    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
                    callback(name,items[i].FmtValue.doubleValue);
                }
            return true;
        }
        return false;
    }
    void Run(const Adapter& adapter)
    {
        PDH_HQUERY query=nullptr;
        PDH_HCOUNTER cpu=nullptr,engine=nullptr,memory=nullptr;
        if (PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS)
        {
            auto add=[&](const wchar_t* path,PDH_HCOUNTER& counter) {
                if (PdhAddEnglishCounterW(query,path,0,&counter)!=ERROR_SUCCESS) counter=nullptr;
            };
            add(L"\\Processor Information(_Total)\\% Processor Utility",cpu);
            if (!adapter.token.empty())
            {
                add(L"\\GPU Engine(*)\\Utilization Percentage",engine);
                add(L"\\GPU Adapter Memory(*)\\Dedicated Usage",memory);
            }
            PdhCollectQueryData(query); // Rate counters need a baseline sample.
        }
        for (;;)
        {
            { std::unique_lock<std::mutex> lock(mutex);
              if (wake.wait_for(lock,std::chrono::seconds(1),[this](){return stopping;})) break; }
            Snapshot snapshot{};
            MEMORYSTATUSEX ram{}; ram.dwLength=sizeof(ram);
            if (GlobalMemoryStatusEx(&ram) && ram.ullTotalPhys)
                snapshot[1]={100.0*(ram.ullTotalPhys-ram.ullAvailPhys)/ram.ullTotalPhys,
                    ram.ullTotalPhys-ram.ullAvailPhys,ram.ullTotalPhys};
            if (query && PdhCollectQueryData(query)==ERROR_SUCCESS)
            {
                PDH_FMT_COUNTERVALUE value{};
                if (cpu && PdhGetFormattedCounterValue(cpu,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,nullptr,&value)==ERROR_SUCCESS &&
                    Good(value.CStatus) && std::isfinite(value.doubleValue))
                    snapshot[0].percent=std::clamp(value.doubleValue,0.0,100.0);
                // GPU Engine instances are per process. Sum processes sharing the
                // SAME physical engine, then take the busiest engine, not their sum.
                std::map<std::wstring,double> engines;
                Each(engine,[&](const std::wstring& name,double percent) {
                    auto pos=name.find(adapter.token);
                    if (pos!=std::wstring::npos && percent>=0) engines[name.substr(pos)]+=percent;
                });
                if (!engines.empty())
                {
                    double busiest=0;
                    for (const auto& item:engines) busiest=std::max(busiest,item.second);
                    snapshot[2].percent=std::clamp(busiest,0.0,100.0);
                }
                double used=0; bool found=false;
                Each(memory,[&](const std::wstring& name,double bytes) {
                    if (name.find(adapter.token)!=std::wstring::npos && bytes>=0) {used+=bytes;found=true;}
                });
                if (found && adapter.capacity && used<=static_cast<double>(adapter.capacity))
                    snapshot[3]={100.0*used/adapter.capacity,static_cast<unsigned long long>(used),adapter.capacity};
            }
            std::lock_guard<std::mutex> lock(mutex);
            latest=snapshot;
        }
        if (query) PdhCloseQuery(query);
    }
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping=false;
    Snapshot latest{};
    std::thread worker;
};

#pragma once
#include <NvBlastTk.h>
#include <stdexcept>

class BlastContext
{
public:
    BlastContext()
    {
        framework = NvBlastTkFrameworkCreate();
        if (!framework) throw std::runtime_error("Cannot initialize NVIDIA Blast.");

        Nv::Blast::TkGroupDesc desc{};
        desc.workerCount = 1;
        group = framework->createGroup(desc);
        if (!group)
        {
            framework->release();
            framework = nullptr;
            throw std::runtime_error("Cannot create NVIDIA Blast group.");
        }
    }

    ~BlastContext()
    {
        if (group) group->release();
        if (framework) framework->release();
    }

    BlastContext(const BlastContext&) = delete;
    BlastContext& operator=(const BlastContext&) = delete;

    Nv::Blast::TkFramework& GetFramework() { return *framework; }
    Nv::Blast::TkGroup& GetGroup() { return *group; }
    void Process() { group->process(); }

private:
    Nv::Blast::TkFramework* framework = nullptr;
    Nv::Blast::TkGroup* group = nullptr;
};
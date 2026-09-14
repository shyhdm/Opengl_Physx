#pragma once
#include "BlastContext.h"
#include "BlastAsset.h"
#include <NvBlastTkActor.h>
#include <NvBlastTkAsset.h>
#include <NvBlastTkFamily.h>
#include <NvBlastTkFramework.h>
#include <NvBlastTkGroup.h>
#include <NvBlastExtDamageShaders.h>
#include <stdexcept>
#include <vector>

class BlastRuntime
{
public:
    BlastRuntime(BlastContext& blastContext, BlastAsset& source) : context(blastContext)
    {
        asset = context.GetFramework().createAsset(&source.GetLowLevelAsset(), nullptr, 0, false);
        if (!asset) throw std::runtime_error("Cannot create Blast TkAsset.");
        try { CreateInitialActor(); }
        catch (...)
        {
            asset->release();
            asset = nullptr;
            throw;
        }
    }

    ~BlastRuntime()
    {
        std::vector<Nv::Blast::TkActor*> actors = GetActors();
        family = nullptr;
        for (Nv::Blast::TkActor* actor : actors) actor->release();
        if (asset) asset->release();
    }

    BlastRuntime(const BlastRuntime&) = delete;
    BlastRuntime& operator=(const BlastRuntime&) = delete;

    void Reset()
    {
        std::vector<Nv::Blast::TkActor*> actors = GetActors();
        family = nullptr;
        for (Nv::Blast::TkActor* actor : actors) actor->release();
        CreateInitialActor();
    }

    void ApplyRadialDamage(float x, float y, float z, float damage, float minRadius, float maxRadius)
    {
        if (!family) return;
        uint32_t actorCount = family->getActorCount();
        if (actorCount == 0) return;
        NvBlastExtRadialDamageDesc damageDesc{};
        damageDesc.damage = damage;
        damageDesc.position[0] = x;
        damageDesc.position[1] = y;
        damageDesc.position[2] = z;
        damageDesc.minRadius = minRadius;
        damageDesc.maxRadius = maxRadius;
        NvBlastExtProgramParams params(&damageDesc);
        NvBlastDamageProgram program{};
        program.graphShaderFunction = NvBlastExtFalloffGraphShader;
        program.subgraphShaderFunction = NvBlastExtFalloffSubgraphShader;
        std::vector<Nv::Blast::TkActor*> actors(actorCount);
        family->getActors(actors.data(), actorCount);
        for (Nv::Blast::TkActor* actor : actors) actor->damage(program, &params);
        context.Process();
    }

    uint32_t GetActorCount() const
    {
        return family ? family->getActorCount() : 0;
    }

    std::vector<Nv::Blast::TkActor*> GetActors() const
    {
        std::vector<Nv::Blast::TkActor*> actors;
        if (!family) return actors;
        actors.resize(family->getActorCount());
        if (!actors.empty()) family->getActors(actors.data(), static_cast<uint32_t>(actors.size()));
        return actors;
    }

    uint32_t GetVisibleChunkCount() const
    {
        if (!family) return 0;
        uint32_t actorCount = family->getActorCount();
        if (actorCount == 0) return 0;
        std::vector<Nv::Blast::TkActor*> actors(actorCount);
        family->getActors(actors.data(), actorCount);
        uint32_t visibleCount = 0;
        for (Nv::Blast::TkActor* actor : actors) visibleCount += actor->getVisibleChunkCount();
        return visibleCount;
    }

    std::vector<uint32_t> GetVisibleChunkIndices() const
    {
        std::vector<uint32_t> visibleChunks;
        if (!family) return visibleChunks;
        uint32_t actorCount = family->getActorCount();
        if (actorCount == 0) return visibleChunks;
        std::vector<Nv::Blast::TkActor*> actors(actorCount);
        family->getActors(actors.data(), actorCount);
        for (Nv::Blast::TkActor* actor : actors)
        {
            uint32_t visibleCount = actor->getVisibleChunkCount();
            size_t first = visibleChunks.size();
            visibleChunks.resize(first + visibleCount);
            actor->getVisibleChunkIndices(visibleChunks.data() + first, visibleCount);
        }
        return visibleChunks;
    }

private:
    BlastContext& context;
    Nv::Blast::TkAsset* asset = nullptr;
    Nv::Blast::TkFamily* family = nullptr;

    void CreateInitialActor()
    {
        Nv::Blast::TkActorDesc actorDesc(asset);
        actorDesc.uniformInitialBondHealth = 1.0f;
        actorDesc.uniformInitialLowerSupportChunkHealth = 1.0f;
        Nv::Blast::TkActor* actor = context.GetFramework().createActor(actorDesc);
        if (!actor) throw std::runtime_error("Cannot create Blast TkActor.");
        family = &actor->getFamily();
        if (!context.GetGroup().addActor(*actor))
        {
            actor->release();
            family = nullptr;
            throw std::runtime_error("Cannot add Blast actor to group.");
        }
    }
};

#pragma once
#include "BlastMesh.h"
#include <NvBlastExtAuthoringFractureTool.h>
#include <cstdint>
#include <random>
#include <stdexcept>

class BlastRandom final : public Nv::Blast::RandomGeneratorBase
{
public:
    explicit BlastRandom(int32_t value = 12345) { seed(value); }
    float getRandomValue() override { return distribution(engine); }
    void seed(int32_t value) override { engine.seed(static_cast<uint32_t>(value)); }

private:
    std::mt19937 engine;
    std::uniform_real_distribution<float> distribution{ 0.0f, 1.0f };
};

class BlastVoronoi
{
public:
    BlastVoronoi(BlastMesh& sourceMesh, uint32_t siteCount = 16, int32_t seed = 12345)
    {
        if (siteCount < 2) throw std::invalid_argument("Voronoi requires at least two sites.");
        tool = NvBlastExtAuthoringCreateFractureTool();
        if (!tool) throw std::runtime_error("Cannot create Blast fracture tool.");

        Nv::Blast::VoronoiSitesGenerator* generator = nullptr;
        try
        {
            const Nv::Blast::Mesh* source = &sourceMesh.Get();
            int32_t rootId = 0;
            if (!tool->setSourceMeshes(&source, 1, &rootId)) throw std::runtime_error("Cannot set Blast source mesh.");

            BlastRandom random(seed);
            generator = NvBlastExtAuthoringCreateVoronoiSitesGenerator(&sourceMesh.Get(), &random);
            if (!generator) throw std::runtime_error("Cannot create Voronoi site generator.");

            generator->uniformlyGenerateSitesInMesh(siteCount);
            const NvcVec3* sites = nullptr;
            uint32_t generatedCount = generator->getVoronoiSites(sites);
            if (!sites || generatedCount < 2) throw std::runtime_error("Cannot generate Voronoi sites.");

            int32_t result = tool->voronoiFracturing(0, generatedCount, sites, false);
            if (result != 0) throw std::runtime_error("Blast Voronoi fracture failed.");

            generator->release();
            generator = nullptr;
            tool->finalizeFracturing();
            chunkCount = tool->getChunkCount();
            if (chunkCount < 2) throw std::runtime_error("Blast did not create fracture chunks.");
        }
        catch (...)
        {
            if (generator) generator->release();
            Release();
            throw;
        }
    }

    ~BlastVoronoi() { Release(); }
    BlastVoronoi(const BlastVoronoi&) = delete;
    BlastVoronoi& operator=(const BlastVoronoi&) = delete;
    uint32_t GetChunkCount() const { return chunkCount; }
    Nv::Blast::FractureTool& GetTool() { return *tool; }

private:
    Nv::Blast::FractureTool* tool = nullptr;
    uint32_t chunkCount = 0;

    void Release()
    {
        if (tool)
        {
            tool->release();
            tool = nullptr;
        }
    }
};
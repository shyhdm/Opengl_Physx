#pragma once
#include "BlastVoronoi.h"
#include <NvBlastExtAuthoring.h>
#include <NvBlastExtAuthoringBondGenerator.h>
#include <NvBlastExtAuthoringConvexMeshBuilder.h>
#include <stdexcept>

class BlastNullCollisionBuilder final : public Nv::Blast::ConvexMeshBuilder
{
public:
    void release() override {}
    Nv::Blast::CollisionHull* buildCollisionGeometry(uint32_t verticesCount, const NvcVec3* vertexData) override
    {
        return nullptr;
    }
    void releaseCollisionHull(Nv::Blast::CollisionHull* hull) const override {}
};

class BlastAsset
{
public:
    explicit BlastAsset(BlastVoronoi& voronoi)
    {
        bondGenerator = NvBlastExtAuthoringCreateBondGenerator(&collisionBuilder);
        if (!bondGenerator) throw std::runtime_error("Cannot create Blast bond generator.");

        Nv::Blast::ConvexDecompositionParams collisionParams{};
        collisionParams.maximumNumberOfHulls = 1;
        result = NvBlastExtAuthoringProcessFracture(voronoi.GetTool(), *bondGenerator, collisionBuilder, collisionParams, -1);

        if (!result || !result->asset)
        {
            Release();
            throw std::runtime_error("Cannot create Blast asset.");
        }
        if (result->chunkCount < 2)
        {
            Release();
            throw std::runtime_error("Blast asset does not contain fractured chunks.");
        }
    }

    ~BlastAsset()
    {
        Release();
    }

    BlastAsset(const BlastAsset&) = delete;
    BlastAsset& operator=(const BlastAsset&) = delete;

    NvBlastAsset& GetLowLevelAsset()
    {
        return *result->asset;
    }

    Nv::Blast::AuthoringResult& GetAuthoringResult()
    {
        return *result;
    }

    uint32_t GetChunkCount() const
    {
        return result ? result->chunkCount : 0;
    }

    uint32_t GetBondCount() const
    {
        return result ? result->bondCount : 0;
    }

private:
    BlastNullCollisionBuilder collisionBuilder;
    Nv::Blast::BlastBondGenerator* bondGenerator = nullptr;
    Nv::Blast::AuthoringResult* result = nullptr;

    void Release()
    {
        if (result)
        {
            NvBlastExtAuthoringReleaseAuthoringResult(collisionBuilder, result);
            result = nullptr;
        }
        if (bondGenerator)
        {
            bondGenerator->release();
            bondGenerator = nullptr;
        }
    }
};
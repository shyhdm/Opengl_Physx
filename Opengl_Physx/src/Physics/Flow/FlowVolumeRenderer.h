#pragma once
#include "Camera.h"
#include "FlowSimulation.h"
#include "Shader.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>
#ifndef PNANOVDB_C
#define PNANOVDB_C
#endif
#ifndef PNANOVDB_BUF_BOUNDS_CHECK
#define PNANOVDB_BUF_BOUNDS_CHECK
#endif
#include <nvflow/nanovdb/PNanoVDB.h>

class FlowVolumeRenderer
{
public:
    FlowVolumeRenderer() : shader_("Assets/Shaders/flow_volume.glsl")
    {
        glGetIntegerv(0x8073, &maxTextureSize_);
        GL::LoadFunction(blendFuncSeparate_, "glBlendFuncSeparate");
        glGenTextures(1, &volumeTexture_);
        GL::GenVertexArrays(1, &vertexArray_);
    }

    ~FlowVolumeRenderer()
    {
        // The worker owns only CPU data and must finish before member destruction.
        if (cancel_) cancel_->store(true);
        if (pending_.valid()) pending_.wait();
        glDeleteTextures(1, &volumeTexture_);
        glDeleteTextures(1, &depthTexture_);
        GL::DeleteVertexArrays(1, &vertexArray_);
    }
    FlowVolumeRenderer(const FlowVolumeRenderer&) = delete;
    FlowVolumeRenderer& operator=(const FlowVolumeRenderer&) = delete;

    bool CanAcceptReadback() const
    {
        return !pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    void Clear()
    {
        if (cancel_) cancel_->store(true);
        hasVolume_ = false;
        lastScheduledFrame_ = std::numeric_limits<NvFlowUint64>::max();
        ++jobEpoch_;
    }

    void Deactivate()
    {
        if (hasVolume_ || lastScheduledFrame_ != std::numeric_limits<NvFlowUint64>::max()) Clear();
        if (pending_.valid() && CanAcceptReadback())
        {
            pending_.get();
            cancel_.reset();
        }
        if (!reusableVoxels_.empty()) std::vector<float>().swap(reusableVoxels_);
        if (textureSize_ != glm::ivec3(0))
        {
            glDeleteTextures(1, &volumeTexture_);
            volumeTexture_ = 0;
            textureSize_ = {};
        }
    }

    void Update(const FlowSimulation::Readback& readback)
    {
        if (generation_ != readback.generation)
        {
            Clear();
            generation_ = readback.generation;
        }
        if (pending_.valid() && CanAcceptReadback())
        {
            VolumeData data = pending_.get();
            if (data.epoch == jobEpoch_)
            {
                hasVolume_ = data.valid;
                if (data.valid) Upload(data);
                reusableVoxels_ = std::move(data.voxels);
            }
        }
        if (!pending_.valid() && readback.IsValid() && readback.frame != lastScheduledFrame_)
        {
            lastScheduledFrame_ = readback.frame;
            const auto epoch = jobEpoch_;
            const int textureLimit = maxTextureSize_;
            cancel_ = std::make_shared<std::atomic_bool>(false);
            pending_ = std::async(std::launch::async,
                [readback, epoch, textureLimit, cancel = cancel_, storage = std::move(reusableVoxels_)]() mutable
                { return Convert(readback, epoch, std::move(storage), textureLimit, cancel); });
        }
    }

    void Draw(const Camera& camera, int width, int height, const FlowSimulation::Settings& settings)
    {
        if (!hasVolume_ || width <= 0 || height <= 0) return;
        const GLboolean blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), depth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean depthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        GLint active = 0, program = 0, vao = 0, volumeBinding = 0, depthBinding = 0;
        GLint srcRgb = 0, dstRgb = 0, srcAlpha = 0, dstAlpha = 0;
        glGetIntegerv(0x84E0, &active);
        glGetIntegerv(0x8B8D, &program);
        glGetIntegerv(0x85B5, &vao);
        glGetIntegerv(0x80C9, &srcRgb); glGetIntegerv(0x80C8, &dstRgb);
        glGetIntegerv(0x80CB, &srcAlpha); glGetIntegerv(0x80CA, &dstAlpha);
        GL::ActiveTexture(0x84C0);
        glGetIntegerv(0x806A, &volumeBinding);
        GL::ActiveTexture(0x84C1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &depthBinding);
        EnsureDepthTexture(width, height);
        glBindTexture(GL_TEXTURE_2D, depthTexture_);
        // Scene renders into the single-sampled default framebuffer before this pass.
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        shader_.Use();
        shader_.SetMatrix4("inverseViewProjection", glm::inverse(camera.GetProjectionMatrix(
            static_cast<float>(width) / height) * camera.GetViewMatrix()));
        shader_.SetVector3("cameraPosition", camera.position);
        shader_.SetVector3("boundsMinimum", boundsMinimum_);
        shader_.SetVector3("boundsMaximum", boundsMaximum_);
        shader_.SetFloat("densityScale", settings.renderDensity);
        shader_.SetFloat("fireBrightness", settings.fireBrightness);
        shader_.SetInt("raySteps", settings.raySteps);
        shader_.SetInt("displayMode", settings.displayMode);
        shader_.SetInt("volumeTexture", 0);
        shader_.SetInt("sceneDepth", 1);
        GL::ActiveTexture(0x84C0);
        glBindTexture(Texture3D, volumeTexture_);
        GL::BindVertexArray(vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        GL::BindVertexArray(static_cast<GLuint>(vao));
        GL::UseProgram(static_cast<GLuint>(program));
        glBindTexture(Texture3D, static_cast<GLuint>(volumeBinding));
        GL::ActiveTexture(0x84C1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(depthBinding));
        GL::ActiveTexture(static_cast<GLenum>(active));
        blendFuncSeparate_(srcRgb, dstRgb, srcAlpha, dstAlpha);
        glDepthMask(depthMask);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        if (!blend) glDisable(GL_BLEND);
    }

private:
    struct VolumeData
    {
        glm::vec3 minimum{}, maximum{};
        glm::ivec3 size{};
        std::vector<float> voxels;
        NvFlowUint64 epoch = 0;
        bool valid = false;
    };

    struct NanoGrid
    {
        pnanovdb_buf_t buffer{};
        pnanovdb_grid_handle_t grid{};
        pnanovdb_root_handle_t root{};
        pnanovdb_readaccessor_t accessor{};
        pnanovdb_grid_type_t type = PNANOVDB_GRID_TYPE_UNKNOWN;
        glm::vec3 minimum{};
        glm::vec3 maximum{};
        float voxelSize = 0.0f;
        bool valid = false;
        std::array<float, 9> inverse{};
        glm::vec3 translation{};
        std::vector<glm::vec3> xTerms, yTerms, zTerms;

        NanoGrid(const NvFlowUint8* data, NvFlowUint64 size)
        {
            if (!data || size < PNANOVDB_GRID_SIZE || size % sizeof(std::uint32_t) != 0u)
            {
                return;
            }

            buffer = pnanovdb_make_buf(
                const_cast<std::uint32_t*>(reinterpret_cast<const std::uint32_t*>(data)),
                size / sizeof(std::uint32_t)
            );
            grid.address = pnanovdb_address_null();
            type = pnanovdb_grid_get_grid_type(buffer, grid);
            if (type != PNANOVDB_GRID_TYPE_FLOAT &&
                type != PNANOVDB_GRID_TYPE_HALF &&
                type != PNANOVDB_GRID_TYPE_FP4 &&
                type != PNANOVDB_GRID_TYPE_FP8 &&
                type != PNANOVDB_GRID_TYPE_FP16 &&
                type != PNANOVDB_GRID_TYPE_FPN)
            {
                return;
            }

            minimum = glm::vec3(
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 0u)),
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 1u)),
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 2u))
            );
            maximum = glm::vec3(
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 3u)),
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 4u)),
                static_cast<float>(pnanovdb_grid_get_world_bbox(buffer, grid, 5u))
            );
            voxelSize = static_cast<float>(pnanovdb_grid_get_voxel_size(buffer, grid, 0u));
            root = pnanovdb_tree_get_root(buffer, pnanovdb_grid_get_tree(buffer, grid));
            pnanovdb_readaccessor_init(&accessor, root);
            const auto map = pnanovdb_grid_get_map(buffer, grid);
            for (int i = 0; i < 9; ++i) inverse[i] = pnanovdb_map_get_invmatf(buffer, map, i);
            for (int i = 0; i < 3; ++i) translation[i] = pnanovdb_map_get_vecf(buffer, map, i);
            valid = true;
        }

        float Sample(const glm::vec3& world)
        {
            const glm::vec3 p = world - translation;
            return SampleIndex(glm::vec3(
                p.x * inverse[0] + p.y * inverse[1] + p.z * inverse[2],
                p.x * inverse[3] + p.y * inverse[4] + p.z * inverse[5],
                p.x * inverse[6] + p.y * inverse[7] + p.z * inverse[8]));
        }

        void PrepareLookup(const glm::vec3& minimum, const glm::vec3& extent, const glm::ivec3& size)
        {
            xTerms.resize(size.x); yTerms.resize(size.y); zTerms.resize(size.z);
            for (int axis = 0; axis < 3; ++axis)
                for (int i = 0; i < size[axis]; ++i)
                {
                    const float world = minimum[axis] + extent[axis] *
                        ((static_cast<float>(i) + 0.5f) / static_cast<float>(size[axis]));
                    const float p = world - translation[axis];
                    auto& terms = axis == 0 ? xTerms : (axis == 1 ? yTerms : zTerms);
                    terms[i] = glm::vec3(p * inverse[axis], p * inverse[axis + 3], p * inverse[axis + 6]);
                }
        }

        float SampleGrid(int x, int y, int z)
        {
            return SampleIndex((xTerms[x] + yTerms[y]) + zTerms[z]);
        }

        float SampleIndex(const glm::vec3& indexPosition)
        {
            const int ix = static_cast<int>(std::floor(indexPosition.x));
            const int iy = static_cast<int>(std::floor(indexPosition.y));
            const int iz = static_cast<int>(std::floor(indexPosition.z));
            const float fx = indexPosition.x - static_cast<float>(ix);
            const float fy = indexPosition.y - static_cast<float>(iy);
            const float fz = indexPosition.z - static_cast<float>(iz);
            float value = 0.0f;
            for (int z = 0; z < 2; ++z)
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x)
                    {
                        pnanovdb_coord_t coordinate{ ix + x, iy + y, iz + z };
                        value += ReadVoxel(coordinate) * (x ? fx : 1.0f - fx) *
                            (y ? fy : 1.0f - fy) * (z ? fz : 1.0f - fz);
                    }
            return value;
        }

        float ReadVoxel(pnanovdb_coord_t coordinate)
        {
            pnanovdb_uint32_t level = 0u;
            const pnanovdb_address_t address =
                pnanovdb_readaccessor_get_value_address_and_level(
                    type,
                    buffer,
                    &accessor,
                    &coordinate,
                    &level
                );

            switch (type)
            {
            case PNANOVDB_GRID_TYPE_FLOAT:
                return pnanovdb_read_float(buffer, address);
            case PNANOVDB_GRID_TYPE_HALF:
                return pnanovdb_read_half(buffer, address);
            case PNANOVDB_GRID_TYPE_FP4:
                return pnanovdb_root_fp4_read_float(buffer, address, &coordinate, level);
            case PNANOVDB_GRID_TYPE_FP8:
                return pnanovdb_root_fp8_read_float(buffer, address, &coordinate, level);
            case PNANOVDB_GRID_TYPE_FP16:
                return pnanovdb_root_fp16_read_float(buffer, address, &coordinate, level);
            case PNANOVDB_GRID_TYPE_FPN:
                return pnanovdb_root_fpn_read_float(buffer, address, &coordinate, level);
            default:
                return 0.0f;
            }
        }
    };


    static bool Finite(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }
    static float Sanitize(float v)
    {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 65504.0f) : 0.0f;
    }
    static VolumeData Convert(const FlowSimulation::Readback& readback, NvFlowUint64 epoch, std::vector<float> storage = {}, int textureLimit = 2048, const std::shared_ptr<std::atomic_bool>& cancel = {})
    {
        VolumeData data;
        data.voxels = std::move(storage);
        data.epoch = epoch;
        if (cancel && cancel->load()) return data;
        NanoGrid temperature(readback.temperature, readback.temperatureSize);
        NanoGrid smoke(readback.smoke, readback.smokeSize);
        if (!temperature.valid || !smoke.valid) return data;
        data.minimum = glm::min(temperature.minimum, smoke.minimum);
        data.maximum = glm::max(temperature.maximum, smoke.maximum);
        glm::vec3 extent = data.maximum - data.minimum;
        if (!Finite(data.minimum) || !Finite(data.maximum) ||
            glm::any(glm::lessThanEqual(extent, glm::vec3(0.001f))) ||
            glm::any(glm::greaterThan(extent, glm::vec3(500.0f)))) return data;
        float cellSize = std::min(temperature.voxelSize, smoke.voxelSize);
        if (!std::isfinite(cellSize) || cellSize <= 0.0f) return data;
        const glm::vec3 origin = temperature.translation;
        const glm::vec3 first = glm::floor((data.minimum - origin) / cellSize) - 2.0f;
        const glm::vec3 last = glm::ceil((data.maximum - origin) / cellSize) + 2.0f;
        const glm::vec3 dimensions = last - first + 1.0f;
        if (!Finite(dimensions) || glm::any(glm::greaterThan(dimensions, glm::vec3(textureLimit))))
            throw std::runtime_error("Flow volume exceeds GL_MAX_3D_TEXTURE_SIZE at native voxel resolution");
        data.minimum = origin + (first - 0.5f) * cellSize;
        data.size = glm::ivec3(dimensions);
        data.maximum = data.minimum + glm::vec3(data.size) * cellSize;
        extent = data.maximum - data.minimum;
        if (cancel && cancel->load()) return data;
        data.voxels.resize(static_cast<size_t>(data.size.x) * data.size.y * data.size.z * 2);
        smoke.PrepareLookup(data.minimum, extent, data.size);
        temperature.PrepareLookup(data.minimum, extent, data.size);
        for (int z = 0; z < data.size.z; ++z)
            for (int y = 0; y < data.size.y; ++y)
            {
                if (cancel && cancel->load()) return data;
                for (int x = 0; x < data.size.x; ++x)
                {
                    const size_t i = (static_cast<size_t>(z) * data.size.y * data.size.x +
                        static_cast<size_t>(y) * data.size.x + x) * 2;
                    data.voxels[i] = Sanitize(smoke.SampleGrid(x, y, z));
                    data.voxels[i + 1] = Sanitize(temperature.SampleGrid(x, y, z));
                }
            }
        data.valid = true;
        return data;
    }
    void Upload(const VolumeData& data)
    {
        GLint active = 0, binding = 0, alignment = 0;
        glGetIntegerv(0x84E0, &active);
        GL::ActiveTexture(0x84C0);
        glGetIntegerv(0x806A, &binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        if (!volumeTexture_) glGenTextures(1, &volumeTexture_);
        glBindTexture(Texture3D, volumeTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (data.size != textureSize_)
        {
            GL::TexImage3D(Texture3D, 0, 0x822F /* RG16F */, data.size.x, data.size.y, data.size.z,
                0, 0x8227 /* RG */, GL_FLOAT, data.voxels.data());
            textureSize_ = data.size;
            glTexParameteri(Texture3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(Texture3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(Texture3D, GL_TEXTURE_WRAP_S, 0x812D /* CLAMP_TO_BORDER */);
            glTexParameteri(Texture3D, GL_TEXTURE_WRAP_T, 0x812D);
            glTexParameteri(Texture3D, 0x8072 /* WRAP_R */, 0x812D);
            const float border[4]{};
            glTexParameterfv(Texture3D, GL_TEXTURE_BORDER_COLOR, border);
        }
        else
            GL::TexSubImage3D(Texture3D, 0, 0, 0, 0, data.size.x, data.size.y, data.size.z,
                0x8227, GL_FLOAT, data.voxels.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glBindTexture(Texture3D, static_cast<GLuint>(binding));
        GL::ActiveTexture(static_cast<GLenum>(active));
        boundsMinimum_ = data.minimum;
        boundsMaximum_ = data.maximum;
    }
    void EnsureDepthTexture(int width, int height)
    {
        if (depthSize_ == glm::ivec2(width, height)) return;
        if (!depthTexture_) glGenTextures(1, &depthTexture_);
        glBindTexture(GL_TEXTURE_2D, depthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /* DEPTH_COMPONENT24 */, width, height,
            0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        depthSize_ = { width, height };
    }
    static constexpr GLenum Texture3D = 0x806F;
    using BlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
    BlendFuncSeparate blendFuncSeparate_ = nullptr;
    Shader shader_;
    GLuint volumeTexture_ = 0, depthTexture_ = 0, vertexArray_ = 0;
    glm::vec3 boundsMinimum_{}, boundsMaximum_{};
    glm::ivec3 textureSize_{};
    glm::ivec2 depthSize_{};
    std::future<VolumeData> pending_;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::vector<float> reusableVoxels_;
    GLint maxTextureSize_ = 0;
    NvFlowUint64 generation_ = std::numeric_limits<NvFlowUint64>::max();
    NvFlowUint64 lastScheduledFrame_ = std::numeric_limits<NvFlowUint64>::max();
    NvFlowUint64 jobEpoch_ = 0;
    bool hasVolume_ = false;
};

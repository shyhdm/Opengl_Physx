#pragma once

#include "Camera.h"
#include "FlowSimulation.h"
#include "Shader.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
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
    FlowVolumeRenderer()
        : shader_("Assets/Shaders/flow_volume.glsl")
    {
        CreateVolumeTexture();
        CreateCube();
    }

    ~FlowVolumeRenderer()
    {
        if (volumeTexture_)
        {
            glDeleteTextures(1, &volumeTexture_);
        }
        if (vertexBuffer_)
        {
            GL::DeleteBuffers(1, &vertexBuffer_);
        }
        if (vertexArray_)
        {
            GL::DeleteVertexArrays(1, &vertexArray_);
        }
    }

    FlowVolumeRenderer(const FlowVolumeRenderer&) = delete;
    FlowVolumeRenderer& operator=(const FlowVolumeRenderer&) = delete;

    void Update(const FlowSimulation::Readback& readback)
    {
        if (!readback.IsValid() || readback.frame == lastReadbackFrame_)
        {
            return;
        }

        NanoGrid temperature(readback.temperature, readback.temperatureSize);
        NanoGrid smoke(readback.smoke, readback.smokeSize);
        if (!temperature.valid || !smoke.valid)
        {
            return;
        }

        glm::vec3 minimum = glm::min(temperature.minimum, smoke.minimum);
        glm::vec3 maximum = glm::max(temperature.maximum, smoke.maximum);
        glm::vec3 extent = maximum - minimum;
        if (!Finite(minimum) || !Finite(maximum) ||
            extent.x <= 0.001f || extent.y <= 0.001f || extent.z <= 0.001f ||
            extent.x > 500.0f || extent.y > 500.0f || extent.z > 500.0f)
        {
            return;
        }

        const float padding = std::max(temperature.voxelSize, smoke.voxelSize) * 2.0f;
        minimum -= glm::vec3(padding);
        maximum += glm::vec3(padding);
        extent = maximum - minimum;

        std::vector<std::uint8_t> voxels(
            static_cast<std::size_t>(Resolution) * Resolution * Resolution * 2u,
            0u
        );

        for (int z = 0; z < Resolution; ++z)
        {
            const float tz = (static_cast<float>(z) + 0.5f) / static_cast<float>(Resolution);
            for (int y = 0; y < Resolution; ++y)
            {
                const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(Resolution);
                for (int x = 0; x < Resolution; ++x)
                {
                    const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(Resolution);
                    const glm::vec3 world = minimum + extent * glm::vec3(tx, ty, tz);

                    const float smokeValue = std::max(0.0f, smoke.Sample(world));
                    const float temperatureValue = std::max(0.0f, temperature.Sample(world));

                    const std::size_t index =
                        (static_cast<std::size_t>(x) +
                            static_cast<std::size_t>(Resolution) *
                            (static_cast<std::size_t>(y) +
                                static_cast<std::size_t>(Resolution) * static_cast<std::size_t>(z))) * 2u;

                    voxels[index] = ToByte(smokeValue);
                    voxels[index + 1u] = ToByte(temperatureValue * 0.35f);
                }
            }
        }

        glBindTexture(Texture3D, volumeTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        GL::TexSubImage3D(
            Texture3D,
            0,
            0,
            0,
            0,
            Resolution,
            Resolution,
            Resolution,
            RedGreen,
            GL_UNSIGNED_BYTE,
            voxels.data()
        );
        glBindTexture(Texture3D, 0);

        boundsMinimum_ = minimum;
        boundsMaximum_ = maximum;
        lastReadbackFrame_ = readback.frame;
        hasVolume_ = true;
    }

    void Clear()
    {
        hasVolume_ = false;
        lastReadbackFrame_ = std::numeric_limits<NvFlowUint64>::max();
    }

    void Draw(
        const Camera& camera,
        int width,
        int height,
        const FlowSimulation::Settings& settings
    )
    {
        if (!hasVolume_ || width <= 0 || height <= 0)
        {
            return;
        }

        const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
        const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean depthWriteWasEnabled = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasEnabled);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        shader_.Use();
        shader_.SetMatrix4("view", camera.GetViewMatrix());
        shader_.SetMatrix4(
            "projection",
            camera.GetProjectionMatrix(static_cast<float>(width) / static_cast<float>(height))
        );
        shader_.SetVector3("cameraPosition", camera.position);
        shader_.SetVector3("boundsMinimum", boundsMinimum_);
        shader_.SetVector3("boundsMaximum", boundsMaximum_);
        shader_.SetFloat("densityScale", settings.renderDensity);
        shader_.SetFloat("fireBrightness", settings.fireBrightness);
        shader_.SetInt("raySteps", settings.raySteps);
        shader_.SetInt("volumeTexture", 0);

        GL::ActiveTexture(Texture0);
        glBindTexture(Texture3D, volumeTexture_);
        GL::BindVertexArray(vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        GL::BindVertexArray(0);
        glBindTexture(Texture3D, 0);

        glDepthMask(depthWriteWasEnabled);
        if (!depthWasEnabled) glDisable(GL_DEPTH_TEST);
        if (!cullWasEnabled) glDisable(GL_CULL_FACE);
        if (!blendWasEnabled) glDisable(GL_BLEND);
    }

private:
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
            valid = true;
        }

        float Sample(const glm::vec3& world)
        {
            pnanovdb_vec3_t worldPosition{};
            worldPosition.x = world.x;
            worldPosition.y = world.y;
            worldPosition.z = world.z;
            const pnanovdb_vec3_t indexPosition =
                pnanovdb_grid_world_to_indexf(buffer, grid, &worldPosition);

            pnanovdb_coord_t coordinate{};
            coordinate.x = static_cast<pnanovdb_int32_t>(std::floor(indexPosition.x + 0.5f));
            coordinate.y = static_cast<pnanovdb_int32_t>(std::floor(indexPosition.y + 0.5f));
            coordinate.z = static_cast<pnanovdb_int32_t>(std::floor(indexPosition.z + 0.5f));

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

    static constexpr int Resolution = 64;
    static constexpr GLenum Texture3D = 0x806F;
    static constexpr GLenum Texture0 = 0x84C0;
    static constexpr GLenum TextureWrapR = 0x8072;
    static constexpr GLint ClampToEdge = 0x812F;
    static constexpr GLenum RedGreen = 0x8227;
    static constexpr GLint RedGreen8 = 0x822B;

    static bool Finite(const glm::vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    static std::uint8_t ToByte(float value)
    {
        value = std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f);
        return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    }

    void CreateVolumeTexture()
    {
        glGenTextures(1, &volumeTexture_);
        if (!volumeTexture_)
        {
            throw std::runtime_error("Cannot create the Flow OpenGL 3D texture.");
        }
        glBindTexture(Texture3D, volumeTexture_);
        glTexParameteri(Texture3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(Texture3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(Texture3D, GL_TEXTURE_WRAP_S, ClampToEdge);
        glTexParameteri(Texture3D, GL_TEXTURE_WRAP_T, ClampToEdge);
        glTexParameteri(Texture3D, TextureWrapR, ClampToEdge);
        GL::TexImage3D(
            Texture3D,
            0,
            RedGreen8,
            Resolution,
            Resolution,
            Resolution,
            0,
            RedGreen,
            GL_UNSIGNED_BYTE,
            nullptr
        );
        glBindTexture(Texture3D, 0);
    }

    void CreateCube()
    {
        constexpr float vertices[] =
        {
            0,0,0, 0,1,0, 1,1,0, 1,1,0, 1,0,0, 0,0,0,
            0,0,1, 1,0,1, 1,1,1, 1,1,1, 0,1,1, 0,0,1,
            0,1,1, 0,1,0, 0,0,0, 0,0,0, 0,0,1, 0,1,1,
            1,1,1, 1,0,0, 1,1,0, 1,0,0, 1,1,1, 1,0,1,
            0,0,0, 1,0,0, 1,0,1, 1,0,1, 0,0,1, 0,0,0,
            0,1,0, 1,1,1, 1,1,0, 1,1,1, 0,1,0, 0,1,1
        };

        GL::GenVertexArrays(1, &vertexArray_);
        GL::GenBuffers(1, &vertexBuffer_);
        GL::BindVertexArray(vertexArray_);
        GL::BindBuffer(GL::ArrayBuffer, vertexBuffer_);
        GL::BufferData(GL::ArrayBuffer, sizeof(vertices), vertices, GL::StaticDraw);
        GL::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        GL::EnableVertexAttribArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
        GL::BindVertexArray(0);
    }

    Shader shader_;
    GLuint volumeTexture_ = 0;
    GLuint vertexArray_ = 0;
    GLuint vertexBuffer_ = 0;
    glm::vec3 boundsMinimum_{};
    glm::vec3 boundsMaximum_{};
    NvFlowUint64 lastReadbackFrame_ = std::numeric_limits<NvFlowUint64>::max();
    bool hasVolume_ = false;
};

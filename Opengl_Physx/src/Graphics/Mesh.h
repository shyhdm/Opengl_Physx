#pragma once
#include "OpenGL.h"
#include <vector>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <map>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>

// 位置、颜色、法线、UV。默认值兼容之前只填写六个数字的顶点。
struct Vertex
{
    float x, y, z;
    float r, g, b;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

static_assert(std::is_standard_layout<Vertex>::value, "Vertex must have a standard layout.");

// 一个 Mesh 拥有一组 GPU 顶点、索引和顶点格式。
// 创建和释放 Mesh 时，OpenGL 上下文都必须仍然有效。
class Mesh
{
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices)
    {
        if (vertices.empty() || indices.empty() || indices.size() % 3 != 0)
        {
            throw std::invalid_argument("Mesh requires vertices and complete triangle indices.");
        }

        if (indices.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()))
        {
            throw std::length_error("Too many mesh indices.");
        }

        constexpr size_t maxBytes = static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max());
        if (vertices.size() > maxBytes / sizeof(Vertex) || indices.size() > maxBytes / sizeof(unsigned int))
        {
            throw std::length_error("Mesh buffers are too large.");
        }

        for (unsigned int index : indices)
        {
            if (index >= vertices.size())
            {
                throw std::out_of_range("Mesh index is outside the vertex array.");
            }
        }

        indexCount = static_cast<GLsizei>(indices.size());

        GL::GenVertexArrays(1, &vao);
        GL::GenBuffers(1, &vbo);
        GL::GenBuffers(1, &ebo);

        if (!vao || !vbo || !ebo)
        {
            Release();
            throw std::runtime_error("Cannot create mesh GPU objects.");
        }

        GL::BindVertexArray(vao);

        // VBO：把顶点数据复制到 GPU。
        GL::BindBuffer(GL::ArrayBuffer, vbo);
        GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(vertices.size() * sizeof(Vertex)), vertices.data(), GL::StaticDraw);

        // EBO：把组成三角形的顶点编号复制到 GPU。
        // EBO 的绑定会记录在当前 VAO 中。
        GL::BindBuffer(GL::ElementArrayBuffer, ebo);
        GL::BufferData(GL::ElementArrayBuffer, static_cast<std::ptrdiff_t>(indices.size() * sizeof(unsigned int)), indices.data(), GL::StaticDraw);

        // location = 0：每个顶点的前三个 float 是位置。
        GL::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));
        GL::EnableVertexAttribArray(0);

        // location = 1：接下来的三个 float 是颜色。
        GL::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, r)));
        GL::EnableVertexAttribArray(1);

        GL::VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, nx)));
        GL::EnableVertexAttribArray(2);
        GL::VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, u)));
        GL::EnableVertexAttribArray(3);

        GL::BindVertexArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }

    ~Mesh()
    {
        Release();
    }

    // 禁止复制，避免同一份 GPU 资源被释放两次。
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void UpdateVertices(const std::vector<Vertex>& vertices)
    {
        GL::BindBuffer(GL::ArrayBuffer, vbo);
        GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(vertices.size() * sizeof(Vertex)), vertices.data(), 0x88E8);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }

    void SetGpuPositions(GLuint buffer, const GLuint* textures)
    {
        GL::BindVertexArray(vao);
        GL::BindBuffer(GL::ArrayBuffer, buffer);
        GL::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        GL::BindVertexArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
        for (int i = 0; i < 3; ++i) gpuTextures[i] = textures[i];
    }

    void SetInstances(const std::vector<glm::mat4>& matrices)
    {
        if (matrices.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) throw std::length_error("Too many mesh instances.");
        instanceCount = static_cast<GLsizei>(matrices.size());
        if (matrices.empty()) return;
        if (!instanceVbo) GL::GenBuffers(1, &instanceVbo);
        if (!instanceVbo) throw std::runtime_error("Cannot create instance GPU buffer.");
        using DivisorFunction = void(APIENTRY*)(GLuint, GLuint);
        static DivisorFunction divisor = reinterpret_cast<DivisorFunction>(glfwGetProcAddress("glVertexAttribDivisor"));
        if (!divisor) throw std::runtime_error("Cannot load OpenGL function: glVertexAttribDivisor");
        GL::BindVertexArray(vao);
        GL::BindBuffer(GL::ArrayBuffer, instanceVbo);
        GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(matrices.size() * sizeof(glm::mat4)), matrices.data(), 0x88E8);
        for (GLuint column = 0; column < 4; ++column)
        {
            GLuint location = 4 + column;
            GL::VertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(column * sizeof(glm::vec4))));
            GL::EnableVertexAttribArray(location);
            divisor(location, 1);
        }
        GL::BindVertexArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }

    void SetTransformIndices(const std::vector<float>& indices)
    {
        if (!transformIndexVbo) GL::GenBuffers(1, &transformIndexVbo);
        if (!transformIndexVbo) throw std::runtime_error("Cannot create transform-index GPU buffer.");
        GL::BindVertexArray(vao);
        GL::BindBuffer(GL::ArrayBuffer, transformIndexVbo);
        GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(indices.size() * sizeof(float)), indices.data(), GL::StaticDraw);
        GL::VertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
        GL::EnableVertexAttribArray(8);
        GL::BindVertexArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }

    void SetTransformMatrices(const std::vector<glm::mat4>& matrices)
    {
        if (matrices.empty()) return;
        if (!transformMatrixBuffer) GL::GenBuffers(1, &transformMatrixBuffer);
        if (!transformMatrixTexture) glGenTextures(1, &transformMatrixTexture);
        if (!transformMatrixBuffer || !transformMatrixTexture) throw std::runtime_error("Cannot create indexed-transform GPU objects.");
        GL::BindBuffer(0x8C2A, transformMatrixBuffer);
        GL::BufferData(0x8C2A, static_cast<std::ptrdiff_t>(matrices.size() * sizeof(glm::mat4)), matrices.data(), 0x88E8);
        glBindTexture(0x8C2A, transformMatrixTexture);
        using TexBufferFunction = void(APIENTRY*)(GLenum, GLenum, GLuint);
        static TexBufferFunction texBuffer = reinterpret_cast<TexBufferFunction>(glfwGetProcAddress("glTexBuffer"));
        if (!texBuffer) throw std::runtime_error("Cannot load OpenGL function: glTexBuffer");
        texBuffer(0x8C2A, 0x8814, transformMatrixBuffer);
        glBindTexture(0x8C2A, 0);
        GL::BindBuffer(0x8C2A, 0);
    }

    void Draw(GLuint program = 0) const
    {
        GLint active = 0;
        if (!program)
        {
            GLint current = 0; glGetIntegerv(0x8B8D, &current); program = static_cast<GLuint>(current);
        }
        auto found = uniforms.find(program);
        if (found == uniforms.end())
        {
            std::array<GLint, 4> locations;
            const char* names[] = { "softGpu","softPositions","softRanges","softFaces" };
            for (int i = 0; i < 4; ++i) locations[i] = GL::GetUniformLocation(program, names[i]);
            found = uniforms.emplace(program, locations).first;
        }
        const auto& locations = found->second;
        GLint enabled = locations[0];
        if (enabled >= 0)
        {
            GL::Uniform1i(enabled, gpuTextures[0] != 0);
            for (int i = 0; i < 3; ++i) GL::Uniform1i(locations[i + 1], 8 + i);
        }
        if (enabled >= 0 && gpuTextures[0])
        {
            glGetIntegerv(0x84E0, &active);
            for (int i = 0; i < 3; ++i)
            {
                GL::ActiveTexture(0x84C0 + 8 + i);
                glBindTexture(0x8C2A, gpuTextures[i]);

            }
            GL::ActiveTexture(static_cast<GLenum>(active));
        }
        GL::BindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
        if (enabled >= 0) GL::Uniform1i(enabled, 0);
        GL::BindVertexArray(0);
    }

    void DrawInstanced(GLuint program = 0) const
    {
        if (instanceCount <= 0) return;
        GLint enabled = program ? GL::GetUniformLocation(program, "softGpu") : -1;
        if (enabled >= 0) GL::Uniform1i(enabled, 0);
        using DrawInstancedFunction = void(APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
        static DrawInstancedFunction drawInstanced = reinterpret_cast<DrawInstancedFunction>(glfwGetProcAddress("glDrawElementsInstanced"));
        if (!drawInstanced) throw std::runtime_error("Cannot load OpenGL function: glDrawElementsInstanced");
        GL::BindVertexArray(vao);
        drawInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr, instanceCount);
        GL::BindVertexArray(0);
    }

    void DrawIndexedTransforms(GLuint program = 0) const
    {
        if (!transformMatrixTexture) return;
        if (!program)
        {
            GLint current = 0;
            glGetIntegerv(0x8B8D, &current);
            program = static_cast<GLuint>(current);
        }
        GLint softEnabled = GL::GetUniformLocation(program, "softGpu");
        if (softEnabled >= 0) GL::Uniform1i(softEnabled, 0);
        GLint matrixSampler = GL::GetUniformLocation(program, "transformMatrices");
        if (matrixSampler < 0) return;
        GLint active = 0;
        glGetIntegerv(0x84E0, &active);
        GL::Uniform1i(matrixSampler, 11);
        GL::ActiveTexture(0x84C0 + 11);
        glBindTexture(0x8C2A, transformMatrixTexture);
        GL::BindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
        GL::BindVertexArray(0);
        glBindTexture(0x8C2A, 0);
        GL::ActiveTexture(static_cast<GLenum>(active));
    }

private:
    GLuint vao = 0, vbo = 0, ebo = 0, instanceVbo = 0, transformIndexVbo = 0, transformMatrixBuffer = 0, transformMatrixTexture = 0;
    GLsizei indexCount = 0, instanceCount = 0;
    GLuint gpuTextures[3] = {};
    mutable std::map<GLuint, std::array<GLint, 4>> uniforms;

    void Release()
    {
        if (vao) GL::DeleteVertexArrays(1, &vao);
        if (vbo) GL::DeleteBuffers(1, &vbo);
        if (ebo) GL::DeleteBuffers(1, &ebo);
        if (instanceVbo) GL::DeleteBuffers(1, &instanceVbo);
        if (transformIndexVbo) GL::DeleteBuffers(1, &transformIndexVbo);
        if (transformMatrixBuffer) GL::DeleteBuffers(1, &transformMatrixBuffer);
        if (transformMatrixTexture) glDeleteTextures(1, &transformMatrixTexture);
        vao = 0;
        vbo = 0;
        ebo = 0;
        instanceVbo = 0;
        transformIndexVbo = 0;
        transformMatrixBuffer = 0;
        transformMatrixTexture = 0;
    }
};


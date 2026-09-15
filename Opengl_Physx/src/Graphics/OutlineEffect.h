#pragma once
#include "Shader.h"
#include "Mesh.h"
#include "Camera.h"

// 完整模型掩码用于找轮廓；独立的模型深度与场景深度只用于判断可见性。
class OutlineEffect
{
public:
    OutlineEffect() : shader("Assets/Shaders/model.glsl", { "selection","outline" })
    {
        GL::GenFramebuffers(1, &framebuffer);
        GL::GenVertexArrays(1, &vao);
        glGenTextures(3, textures);
    }
    ~OutlineEffect()
    {
        GL::DeleteFramebuffers(1, &framebuffer);
        GL::DeleteVertexArrays(1, &vao);
        glDeleteTextures(3, textures);
    }
    OutlineEffect(const OutlineEffect&) = delete;
    OutlineEffect& operator=(const OutlineEffect&) = delete;

    void Draw(const Mesh& mesh, const Camera& camera, int width, int height, const glm::mat4& model)
    {
        if (width <= 0 || height <= 0) return;
        State saved; // 即使发生异常，也恢复当前场景的渲染状态。
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GL::ActiveTexture(0x84C2);
        if (width != storedWidth || height != storedHeight)
        {
            Allocate(textures[0], 0x8229, GL_RED, GL_UNSIGNED_BYTE, width, height);
            Allocate(textures[1], 0x8CAC, GL_DEPTH_COMPONENT, GL_FLOAT, width, height);
            Allocate(textures[2], 0x8CAC, GL_DEPTH_COMPONENT, GL_FLOAT, width, height);
            storedWidth = width; storedHeight = height;
        }
        GL::BindFramebuffer(0x8CA8, static_cast<GLuint>(saved.draw));
        glBindTexture(GL_TEXTURE_2D, textures[2]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

        GL::BindFramebuffer(0x8D40, framebuffer);
        GL::FramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, textures[0], 0);
        GL::FramebufferTexture2D(0x8D40, 0x8D00, GL_TEXTURE_2D, textures[1], 0);
        glDrawBuffer(0x8CE0);
        glReadBuffer(0x8CE0);
        if (GL::CheckFramebufferStatus(0x8D40) != 0x8CD5)
            throw std::runtime_error("Outline framebuffer is incomplete.");

        glViewport(0, 0, width, height);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        shader.UsePass("selection");
        shader.SetMatrix4("mvp", camera.GetProjectionMatrix(float(width) / height) * camera.GetViewMatrix() * model);
        mesh.Draw();

        GL::BindFramebuffer(0x8CA9, static_cast<GLuint>(saved.draw));
        GL::BindFramebuffer(0x8CA8, static_cast<GLuint>(saved.read));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        shader.UsePass("outline");
        shader.SetInt("selectionMask", 2);
        shader.SetInt("selectionDepth", 3);
        shader.SetInt("sceneDepth", 4);
        shader.SetVector3("outlineColor", glm::vec3(0.15f, 0.85f, 1.0f));
        for (int i = 0; i < 3; ++i)
        {
            GL::ActiveTexture(0x84C2 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
        }
        GL::BindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
private:
    Shader shader;
    GLuint framebuffer = 0, vao = 0, textures[3] = {};
    int storedWidth = 0, storedHeight = 0;

    struct State
    {
        GLint draw = 0, read = 0, viewport[4] = {}, depthFunc = 0, program = 0, vao = 0, activeTexture = 0, bindings[3] = {};
        GLboolean depthWrite = GL_TRUE, colorWrite[4] = {};
        GLfloat clearColor[4] = {};
        GLdouble clearDepth = 1;
        static constexpr GLenum capabilities[6] = { GL_DEPTH_TEST,GL_BLEND,GL_SCISSOR_TEST,GL_STENCIL_TEST,GL_CULL_FACE,GL_POLYGON_OFFSET_FILL };
        bool enabled[6] = {};
        State()
        {
            glGetIntegerv(0x8CA6, &draw); glGetIntegerv(0x8CAA, &read);
            glGetIntegerv(GL_VIEWPORT, viewport); glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glGetIntegerv(0x8B8D, &program); glGetIntegerv(0x85B5, &vao);
            glGetIntegerv(0x84E0, &activeTexture);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
            glGetBooleanv(GL_COLOR_WRITEMASK, colorWrite);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
            glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clearDepth);
            for (int i = 0; i < 6; ++i) enabled[i] = glIsEnabled(capabilities[i]) != 0;
            for (int i = 0; i < 3; ++i)
            {
                GL::ActiveTexture(0x84C2 + i);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bindings[i]);
            }
        }
        ~State()
        {
            GL::BindFramebuffer(0x8CA9, static_cast<GLuint>(draw));
            GL::BindFramebuffer(0x8CA8, static_cast<GLuint>(read));
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glDepthFunc(static_cast<GLenum>(depthFunc)); glDepthMask(depthWrite);
            glColorMask(colorWrite[0], colorWrite[1], colorWrite[2], colorWrite[3]);
            glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            glClearDepth(clearDepth);
            for (int i = 0; i < 6; ++i) { if (enabled[i]) glEnable(capabilities[i]); else glDisable(capabilities[i]); }
            for (int i = 0; i < 3; ++i) { GL::ActiveTexture(0x84C2 + i); glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bindings[i])); }
            GL::ActiveTexture(static_cast<GLenum>(activeTexture));
            GL::UseProgram(static_cast<GLuint>(program));
            GL::BindVertexArray(static_cast<GLuint>(vao));
        }
    };

    static void Allocate(GLuint texture, GLint format, GLenum external, GLenum type, int width, int height)
    {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, external, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    }
};

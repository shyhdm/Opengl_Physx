#pragma once
#include "OpenGL.h"
#include <stdexcept>

// 深度纹理及其帧缓冲；必须在 OpenGL 上下文存活时创建和销毁。
class ShadowMap
{
public:
    explicit ShadowMap(int resolution = 2048) : resolution(resolution)
    {
        GLint maximum = 0, previousTexture = 0, previousDraw = 0, previousRead = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        if (resolution <= 0 || resolution > maximum) throw std::invalid_argument("Unsupported shadow map size.");
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(0x8CA6, &previousDraw);
        glGetIntegerv(0x8CAA, &previousRead);
        glGenTextures(1, &depth);
        GL::GenFramebuffers(1, &framebuffer);
        if (!depth || !framebuffer) { Release(); throw std::runtime_error("Cannot allocate shadow map."); }
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6, resolution, resolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
        const GLfloat border[4] = { 1,1,1,1 };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        GL::BindFramebuffer(0x8D40, framebuffer);
        GL::FramebufferTexture2D(0x8D40, 0x8D00, GL_TEXTURE_2D, depth, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        GLenum status = GL::CheckFramebufferStatus(0x8D40);
        GL::BindFramebuffer(0x8CA9, static_cast<GLuint>(previousDraw));
        GL::BindFramebuffer(0x8CA8, static_cast<GLuint>(previousRead));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        if (status != 0x8CD5) { Release(); throw std::runtime_error("Shadow framebuffer is incomplete."); }
    }

    ~ShadowMap() { Release(); }
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    void Begin()
    {
        if (active) throw std::logic_error("Shadow pass already started.");
        glGetIntegerv(0x8CA6, &savedDraw);
        glGetIntegerv(0x8CAA, &savedRead);
        glGetIntegerv(GL_VIEWPORT, savedViewport);
        GL::BindFramebuffer(0x8D40, framebuffer);
        glViewport(0, 0, resolution, resolution);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        active = true;
    }

    void End()
    {
        if (!active) return;
        GL::BindFramebuffer(0x8CA9, static_cast<GLuint>(savedDraw));
        GL::BindFramebuffer(0x8CA8, static_cast<GLuint>(savedRead));
        glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
        active = false;
    }

    void Bind() const
    {
        GL::ActiveTexture(0x84C1); // 阴影固定使用单元1，底色贴图使用单元0。
        glBindTexture(GL_TEXTURE_2D, depth);
    }

private:
    int resolution;
    GLuint depth = 0, framebuffer = 0;
    GLint savedDraw = 0, savedRead = 0, savedViewport[4] = {};
    bool active = false;
    void Release()
    {
        End();
        if (framebuffer) GL::DeleteFramebuffers(1, &framebuffer);
        if (depth) glDeleteTextures(1, &depth);
        framebuffer = 0; depth = 0;
    }
};

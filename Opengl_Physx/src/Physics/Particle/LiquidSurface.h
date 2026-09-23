#pragma once
#include "Shader.h"
#include "Camera.h"
#include "LiquidWhitewater.h"
#include "LiquidLighting.h"
#include <algorithm>
#include <cmath>

class LiquidSurface
{
public:
    struct RenderParameters {
        glm::vec3 color{ 5.f / 255.f,9.f / 255.f,49.f / 255.f };
        glm::vec3 thinColor{ 9.f / 255.f,42.f / 255.f,70.f / 255.f };
        glm::vec2 colorRange{ 0.f,.001f };
        float absorption = 1.61f, reflection = 1.f, refraction = .24f, thickness = .8f;
        float particleScale = .71f, smoothRadius = .65f, smoothSharpness = .43f, depthRejection = 3.7f;
        int smoothIterations = 3;
    };
    static RenderParameters ClampRenderParameters(RenderParameters value) {
        const RenderParameters defaults;
        auto limit = [](float v, float lo, float hi, float fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
        for (int i = 0; i < 3; ++i)value.color[i] = limit(value.color[i], 0, 1, defaults.color[i]);
        for (int i = 0; i < 3; ++i)value.thinColor[i] = limit(value.thinColor[i], 0, 1, defaults.thinColor[i]);
        value.colorRange.x = limit(value.colorRange.x, 0, 19.99f, defaults.colorRange.x);
        value.colorRange.y = limit(value.colorRange.y, value.colorRange.x + .001f, 20, std::max(defaults.colorRange.y, value.colorRange.x + .001f));
        value.absorption = limit(value.absorption, 0, 5, defaults.absorption);
        value.reflection = limit(value.reflection, 0, 2, defaults.reflection);
        value.refraction = limit(value.refraction, 0, 3, defaults.refraction);
        value.thickness = limit(value.thickness, .1f, 3, defaults.thickness);
        value.particleScale = limit(value.particleScale, .5f, 1.5f, defaults.particleScale);
        value.smoothRadius = limit(value.smoothRadius, 0, 1, defaults.smoothRadius);
        value.smoothSharpness = limit(value.smoothSharpness, .1f, 2, defaults.smoothSharpness);
        value.depthRejection = limit(value.depthRejection, 0, 20, defaults.depthRejection);
        value.smoothIterations = std::clamp(value.smoothIterations, 0, 5);
        return value;
    }
    void SetRenderParameters(RenderParameters value) { renderParameters_ = ClampRenderParameters(value); }
    LiquidSurface() :shader_("Assets/Shaders/liquid_screen.glsl", { "DEPTH","THICKNESS","PACK","SMOOTH","NORMALS","VIEW_DEPTH","COMPOSITE" })
    {
        GL::LoadFunction(blit_, "glBlitFramebuffer");
        GL::LoadFunction(divisor_, "glVertexAttribDivisor");
        GL::LoadFunction(instanced_, "glDrawArraysInstanced");
        GL::LoadFunction(blendEquation_, "glBlendEquationSeparate");
        GL::LoadFunction(blendFunc_, "glBlendFuncSeparate");
        GL::GenVertexArrays(1, &particleVao_); GL::GenVertexArrays(1, &screenVao_);
        GL::GenFramebuffers(3, fbos_); glGenTextures(8, textures_);
    }
    ~LiquidSurface() { GL::DeleteFramebuffers(3, fbos_); glDeleteTextures(8, textures_); GL::DeleteVertexArrays(1, &particleVao_); GL::DeleteVertexArrays(1, &screenVao_); }
    LiquidSurface(const LiquidSurface&) = delete;
    LiquidSurface& operator=(const LiquidSurface&) = delete;
    void Invalidate() { whitewater_.Invalidate(); lighting_.Invalidate(); }
    void Draw(GLuint positions, unsigned count, float spacing, const Camera& camera, int width, int height, unsigned long long revision = 0, float gravityScale = 1, glm::vec3 boundsLow = glm::vec3(-10, 0, -10), glm::vec3 boundsHigh = glm::vec3(10, 15, 10))
    {
        if (!count || width <= 0 || height <= 0)return;
        State state(*this);
        Resize(width, height); lighting_.Prepare(width, height); glDisable(GL_SCISSOR_TEST);
        GL::BindFramebuffer(0x8CA8, state.readFbo); GL::BindFramebuffer(0x8CA9, fbos_[0]);
        blit_(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        blit_(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glViewport(0, 0, width, height); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(0x8DB9);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearDepth(1);
        GL::BindVertexArray(particleVao_); GL::BindBuffer(GL::ArrayBuffer, positions);
        GL::VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr); GL::EnableVertexAttribArray(0); divisor_(0, 1);
        GL::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void*>(size_t(count) * 4 * sizeof(float))); GL::EnableVertexAttribArray(1); divisor_(1, 1);
        const auto projection = camera.GetProjectionMatrix(float(width) / height);
        const auto view = camera.GetViewMatrix();
        const float radius = spacing * renderParameters_.particleScale;
        glViewport(0, 0, width, height);
        shader_.UsePass("DEPTH"); Common(projection, view, radius, width, height); Bind(0, textures_[1], "sceneDepth");
        GL::BindFramebuffer(0x8D40, fbos_[1]); Attach(textures_[6]);
        glClearColor(10000000.f, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        instanced_(GL_TRIANGLE_STRIP, 0, 4, count);
        const int thicknessWidth = (width + 1) / 2, thicknessHeight = (height + 1) / 2;
        glViewport(0, 0, thicknessWidth, thicknessHeight);
        shader_.UsePass("THICKNESS"); Common(projection, view, radius, thicknessWidth, thicknessHeight); Bind(0, textures_[1], "sceneDepth");
        GL::BindFramebuffer(0x8D40, fbos_[2]); Attach(textures_[4]); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glEnable(GL_BLEND); blendEquation_(0x8006, 0x8006); blendFunc_(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        instanced_(GL_TRIANGLE_STRIP, 0, 4, count); glDisable(GL_BLEND);
        glViewport(0, 0, width, height);
        GL::BindVertexArray(screenVao_);
        Attach(textures_[2]); shader_.UsePass("PACK"); Common(projection, view, radius, width, height);
        Bind(0, textures_[6], "waterDepth"); Bind(1, textures_[4], "waterThickness"); glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int pass = 0; pass < renderParameters_.smoothIterations * 2; ++pass) {
            const unsigned src = pass % 2 ? 3 : 2, dst = pass % 2 ? 2 : 3;
            Attach(textures_[dst]); shader_.UsePass("SMOOTH"); Common(projection, view, radius, width, height);
            shader_.SetVector2("axis", pass % 2 ? glm::vec2(0, 1) : glm::vec2(1, 0)); Bind(0, textures_[src], "waterDepth"); glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        Attach(textures_[7]); shader_.UsePass("NORMALS"); Common(projection, view, radius, width, height);
        Bind(0, textures_[2], "waterDepth"); glDrawArrays(GL_TRIANGLES, 0, 3);
        Attach(textures_[6]); shader_.UsePass("VIEW_DEPTH"); Common(projection, view, radius, width, height);
        Bind(0, textures_[2], "waterDepth"); glDrawArrays(GL_TRIANGLES, 0, 3);
        const GLuint background = lighting_.Render(particleVao_, screenVao_, count, radius, projection, view, textures_[0], textures_[1], width, height, boundsLow, boundsHigh, revision);
        GL::BindFramebuffer(0x8CA8, fbos_[2]); GL::FramebufferTexture2D(0x8CA8, 0x8CE0, GL_TEXTURE_2D, background, 0);
        GL::BindFramebuffer(0x8CA9, state.drawFbo); blit_(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glViewport(0, 0, width, height); GL::BindFramebuffer(0x8D40, state.drawFbo); glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
        shader_.UsePass("COMPOSITE"); Common(projection, view, radius, width, height);
        shader_.SetVector3("sceneBackgroundColor", glm::vec3(state.clearColor[0], state.clearColor[1], state.clearColor[2]));
        shader_.SetVector3("boundsLow", boundsLow); shader_.SetVector3("boundsHigh", boundsHigh);
        Bind(0, textures_[2], "waterDepth"); Bind(1, background, "sceneColor"); Bind(2, textures_[1], "sceneDepth"); Bind(3, textures_[7], "surfaceNormals");
        GL::BindVertexArray(screenVao_); glDrawArrays(GL_TRIANGLES, 0, 3);
        GL::ActiveTexture(0x84C0); glBindTexture(GL_TEXTURE_2D, textures_[0]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        whitewater_.Draw(positions, count, spacing, revision, view, projection, textures_[6], textures_[1], state.drawFbo, width, height, gravityScale, textures_[0], glm::vec3(state.clearColor[0], state.clearColor[1], state.clearColor[2]));
    }
private:
    RenderParameters renderParameters_;
    using Blit = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    using Divisor = void(APIENTRY*)(GLuint, GLuint);
    using Instanced = void(APIENTRY*)(GLenum, GLint, GLsizei, GLsizei);
    using BlendEquation = void(APIENTRY*)(GLenum, GLenum);
    using BlendFunc = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
    struct State {
        LiquidSurface& owner; GLint drawFbo, readFbo, viewport[4], program, vao, buffer, active, tex[7], cube[7], volume[7], textureBuffer, depthFunc, srcRgb, dstRgb, srcAlpha, dstAlpha, eqRgb, eqAlpha;
        GLboolean depth, cull, blend, scissor, srgb, depthWrite, colorWrite[4]; GLfloat clearColor[4]; GLdouble clearDepth;
        explicit State(LiquidSurface& value) :owner(value) {
            glGetIntegerv(0x8CA6, &drawFbo); glGetIntegerv(0x8CAA, &readFbo); glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(0x8B8D, &program); glGetIntegerv(0x85B5, &vao); glGetIntegerv(0x8894, &buffer); glGetIntegerv(0x84E0, &active);
            for (unsigned i = 0; i < 7; ++i) { GL::ActiveTexture(0x84C0 + i); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]); glGetIntegerv(0x8514, &cube[i]); glGetIntegerv(0x806A, &volume[i]); }
            GL::ActiveTexture(0x84C2); glGetIntegerv(0x8C2C, &textureBuffer);
            depth = glIsEnabled(GL_DEPTH_TEST); cull = glIsEnabled(GL_CULL_FACE); blend = glIsEnabled(GL_BLEND); scissor = glIsEnabled(GL_SCISSOR_TEST); srgb = glIsEnabled(0x8DB9);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite); glGetBooleanv(GL_COLOR_WRITEMASK, colorWrite); glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glGetIntegerv(0x80C9, &srcRgb); glGetIntegerv(0x80C8, &dstRgb); glGetIntegerv(0x80CB, &srcAlpha); glGetIntegerv(0x80CA, &dstAlpha);
            glGetIntegerv(0x8009, &eqRgb); glGetIntegerv(0x883D, &eqAlpha); glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor); glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clearDepth);
        }
        ~State() {
            GL::BindFramebuffer(0x8CA9, drawFbo); GL::BindFramebuffer(0x8CA8, readFbo); glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            GL::UseProgram(program); GL::BindVertexArray(vao); GL::BindBuffer(GL::ArrayBuffer, buffer);
            for (unsigned i = 0; i < 7; ++i) { GL::ActiveTexture(0x84C0 + i); glBindTexture(GL_TEXTURE_2D, tex[i]); glBindTexture(0x8513, cube[i]); glBindTexture(0x806F, volume[i]); }GL::ActiveTexture(0x84C2); glBindTexture(0x8C2A, textureBuffer); GL::ActiveTexture(active);
            Restore(GL_DEPTH_TEST, depth); Restore(GL_CULL_FACE, cull); Restore(GL_BLEND, blend); Restore(GL_SCISSOR_TEST, scissor); Restore(0x8DB9, srgb);
            glDepthMask(depthWrite); glColorMask(colorWrite[0], colorWrite[1], colorWrite[2], colorWrite[3]); glDepthFunc(depthFunc);
            owner.blendEquation_(eqRgb, eqAlpha); owner.blendFunc_(srcRgb, dstRgb, srcAlpha, dstAlpha);
            glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]); glClearDepth(clearDepth);
        }
        static void Restore(GLenum cap, bool enabled) { if (enabled)glEnable(cap); else glDisable(cap); }
    };
    void Common(const glm::mat4& projection, const glm::mat4& view, float radius, int w, int h) {
        shader_.SetMatrix4("projection", projection); shader_.SetMatrix4("inverseProjection", glm::inverse(projection)); shader_.SetMatrix4("view", view); shader_.SetMatrix4("inverseView", glm::inverse(view));
        shader_.SetVector3("waterColor", renderParameters_.color);
        shader_.SetVector3("waterThinColor", renderParameters_.thinColor);
        shader_.SetVector2("colorTransition", renderParameters_.colorRange);
        shader_.SetFloat("absorptionStrength", renderParameters_.absorption); shader_.SetFloat("reflectionStrength", renderParameters_.reflection);
        shader_.SetFloat("refractionStrength", renderParameters_.refraction); shader_.SetFloat("thicknessStrength", renderParameters_.thickness);
        shader_.SetFloat("smoothRadius", renderParameters_.smoothRadius); shader_.SetFloat("smoothSharpness", renderParameters_.smoothSharpness); shader_.SetFloat("depthRejection", renderParameters_.depthRejection);
        shader_.SetFloat("radius", radius); shader_.SetFloat("waterTime", float(glfwGetTime())); shader_.SetVector2("resolution", glm::vec2(w, h));
    }
    void Bind(unsigned unit, GLuint texture, const char* name) { GL::ActiveTexture(0x84C0 + unit); glBindTexture(GL_TEXTURE_2D, texture); shader_.SetInt(name, unit); }
    void Attach(GLuint texture) { GL::FramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, texture, 0); }
    void Resize(int w, int h) {
        if (width_ == w && height_ == h)return;
        for (int i = 0; i < 8; ++i) {
            glBindTexture(GL_TEXTURE_2D, textures_[i]);
            bool depth = i == 1 || i == 5;
            const bool scalar = i == 4 || i == 6;
            const GLint format = depth ? 0x81A6 : (i == 0 ? GL_RGBA8 : (scalar ? 0x822E : 0x8814));
            glTexImage2D(GL_TEXTURE_2D, 0, format, i == 4 ? (w + 1) / 2 : w, i == 4 ? (h + 1) / 2 : h, 0, depth ? GL_DEPTH_COMPONENT : (scalar ? 0x1903 : GL_RGBA), GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, depth ? GL_NEAREST : GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, depth ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        }
        for (unsigned i = 0; i < 3; ++i) {
            GL::BindFramebuffer(0x8D40, fbos_[i]); Attach(textures_[i == 0 ? 0 : (i == 1 ? 6 : 4)]);
            if (i < 2)GL::FramebufferTexture2D(0x8D40, 0x8D00, GL_TEXTURE_2D, textures_[i == 0 ? 1 : 5], 0);
            glDrawBuffer(0x8CE0); glReadBuffer(0x8CE0);
            if (GL::CheckFramebufferStatus(0x8D40) != 0x8CD5)throw std::runtime_error("Cannot allocate screen-space water targets");
        }
        width_ = w; height_ = h;
    }
    LiquidLighting lighting_;
    LiquidWhitewater whitewater_;
    Shader shader_; GLuint fbos_[3]{}, textures_[8]{}, particleVao_ = 0, screenVao_ = 0; int width_ = 0, height_ = 0;
    Blit blit_ = nullptr; Divisor divisor_ = nullptr; Instanced instanced_ = nullptr; BlendEquation blendEquation_ = nullptr; BlendFunc blendFunc_ = nullptr;
};

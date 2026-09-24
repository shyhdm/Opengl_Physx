#pragma once
#include "Shader.h"

// A depth-only prepass supplies neighboring grains to screen-space lighting.
// No CPU particle readback. Targets are allocated only when sand is rendered.
class SandLighting {
public:
    SandLighting() {
        GL::GenFramebuffers(1, &fbo_);
        glGenTextures(1, &depth_);
    }
    ~SandLighting() { GL::DeleteFramebuffers(1, &fbo_); glDeleteTextures(1, &depth_); }
    SandLighting(const SandLighting&) = delete;
    SandLighting& operator=(const SandLighting&) = delete;

    void Draw(Shader& shader, unsigned count, int width, int height, const glm::mat4& projection) {
        State state;
        GL::ActiveTexture(0x84C0 + 15);
        glBindTexture(GL_TEXTURE_2D, depth_);
        GL::BindFramebuffer(0x8D40, fbo_);
        if (width != width_ || height != height_) {
            glTexImage2D(GL_TEXTURE_2D, 0, 0x8CAC, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
            GL::FramebufferTexture2D(0x8D40, 0x8D00, GL_TEXTURE_2D, depth_, 0);
            glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
            if (GL::CheckFramebufferStatus(0x8D40) != 0x8CD5)
                throw std::runtime_error("Sand lighting depth allocation failed");
            width_ = width; height_ = height;
        }
        // Never sample a texture while it is attached to the active draw FBO.
        glBindTexture(GL_TEXTURE_2D, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
        glClearDepth(1); glClear(GL_DEPTH_BUFFER_BIT);
        shader.SetInt("sandDepthPass", 1);
        shader.SetInt("sandLightingEnabled", 0);
        glDrawArrays(GL_POINTS, 0, count);

        GL::BindFramebuffer(0x8CA8, state.readFbo);
        GL::BindFramebuffer(0x8CA9, state.drawFbo);
        glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
        State::Enable(GL_SCISSOR_TEST, state.scissor);
        glBindTexture(GL_TEXTURE_2D, depth_);
        shader.SetInt("sandDepth", 15);
        shader.SetInt("sandDepthPass", 0);
        shader.SetInt("sandLightingEnabled", 1);
        shader.SetMatrix4("sandInverseProjection", glm::inverse(projection));
        shader.SetVector2("sandViewportOrigin", glm::vec2(state.viewport[0], state.viewport[1]));
        glDrawArrays(GL_POINTS, 0, count);
        shader.SetInt("sandLightingEnabled", 0);
    }
private:
    struct State {
        GLint readFbo{}, drawFbo{}, active{}, texture{}, viewport[4]{}, depthFunc{};
        GLboolean depthMask{}, depthTest{}, scissor{}, blend{};
        GLdouble clearDepth{};
        State() {
            glGetIntegerv(0x8CAA, &readFbo); glGetIntegerv(0x8CA6, &drawFbo);
            glGetIntegerv(0x84E0, &active);
            GL::ActiveTexture(0x84C0 + 15); glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGetIntegerv(GL_VIEWPORT, viewport); glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask); glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clearDepth);
            depthTest = glIsEnabled(GL_DEPTH_TEST); scissor = glIsEnabled(GL_SCISSOR_TEST); blend = glIsEnabled(GL_BLEND);
        }
        static void Enable(GLenum cap, GLboolean value) { if (value)glEnable(cap); else glDisable(cap); }
        ~State() {
            GL::BindFramebuffer(0x8CA8, readFbo); GL::BindFramebuffer(0x8CA9, drawFbo);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glDepthFunc(depthFunc); glDepthMask(depthMask); glClearDepth(clearDepth);
            Enable(GL_DEPTH_TEST, depthTest); Enable(GL_SCISSOR_TEST, scissor); Enable(GL_BLEND, blend);
            GL::ActiveTexture(0x84C0 + 15); glBindTexture(GL_TEXTURE_2D, texture); GL::ActiveTexture(active);
        }
    };
    GLuint fbo_{}, depth_{};
    int width_{}, height_{};
};

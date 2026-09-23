#pragma once
#include "Shader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
class LiquidLighting {
public:
    LiquidLighting() :shader_("Assets/Shaders/liquid_lighting.glsl", { "DEPTH","THICKNESS","SMOOTH","PHOTONS","RECEIVERS" }) {
        GL::LoadFunction(instanced_, "glDrawArraysInstanced"); GL::GenFramebuffers(2, fbo_); glGenTextures(6, tex_); glGenTextures(1, &environment_); glGenTextures(1, &noise_);
    }
    ~LiquidLighting() { GL::DeleteFramebuffers(2, fbo_); glDeleteTextures(6, tex_); glDeleteTextures(1, &environment_); glDeleteTextures(1, &noise_); }
    LiquidLighting(const LiquidLighting&) = delete;
    LiquidLighting& operator=(const LiquidLighting&) = delete;
    GLuint Environment()const { return environment_; }
    GLuint Noise()const { return noise_; }
    void Prepare(int w, int h) { Allocate(w, h); }
    GLuint Render(GLuint particleVao, GLuint screenVao, unsigned count, float radius, const glm::mat4& projection, const glm::mat4& view, GLuint sceneColor, GLuint sceneDepth, int w, int h, glm::vec3 low, glm::vec3 high) {
        Allocate(w, h);
        const glm::vec3 center = (low + high) * .5f, light = glm::normalize(glm::vec3(-.4f, .8f, .3f));
        const float span = std::max(glm::length(high - low) * .65f, 4.f);
        auto lightView = glm::lookAt(center + light * span * 2.f, center, glm::vec3(0, 1, 0));
        auto lightProjection = glm::ortho(-span, span, -span, span, .1f, span * 4);
        auto common = [&](const char* pass) {shader_.UsePass(pass); shader_.SetMatrix4("lightView", lightView); shader_.SetMatrix4("lightProjection", lightProjection); shader_.SetMatrix4("inverseLightView", glm::inverse(lightView)); shader_.SetMatrix4("inverseLightProjection", glm::inverse(lightProjection)); shader_.SetMatrix4("inverseView", glm::inverse(view)); shader_.SetMatrix4("inverseProjection", glm::inverse(projection)); shader_.SetVector3("regionCenter", center); shader_.SetFloat("span", span); shader_.SetFloat("radius", radius); };
        GL::BindFramebuffer(0x8D40, fbo_[0]); Attach(tex_[0]); glViewport(0, 0, 512, 512); glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS); glDisable(GL_BLEND); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        GL::BindVertexArray(particleVao); common("DEPTH"); instanced_(GL_TRIANGLE_STRIP, 0, 4, count);
        GL::BindFramebuffer(0x8D40, fbo_[1]); Attach(tex_[3]); glViewport(0, 0, 128, 128); glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
        common("THICKNESS"); instanced_(GL_TRIANGLE_STRIP, 0, 4, count); glDisable(GL_BLEND);
        glViewport(0, 0, 512, 512); GL::BindVertexArray(screenVao);
        for (int i = 0; i < 4; ++i) { Attach(tex_[i % 2 ? 0 : 1]); common("SMOOTH"); Bind(0, tex_[i % 2 ? 1 : 0], "lightDepth"); shader_.SetVector2("axis", i % 2 ? glm::vec2(0, 1) : glm::vec2(1, 0)); glDrawArrays(GL_TRIANGLES, 0, 3); }
        Attach(tex_[4]); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); common("PHOTONS"); Bind(0, tex_[0], "lightDepth"); Bind(1, tex_[3], "lightThickness"); instanced_(GL_TRIANGLE_STRIP, 0, 4, 512 * 512); glDisable(GL_BLEND);
        Attach(tex_[5]); glViewport(0, 0, w, h); common("RECEIVERS"); Bind(0, sceneColor, "sceneColor"); Bind(1, sceneDepth, "sceneDepth"); Bind(2, tex_[0], "lightDepth"); Bind(3, tex_[3], "lightThickness"); Bind(4, tex_[4], "causticMap"); glDrawArrays(GL_TRIANGLES, 0, 3);
        return tex_[5];
    }
private:
    void Bind(unsigned unit, GLuint texture, const char* name) { GL::ActiveTexture(0x84C0 + unit); glBindTexture(GL_TEXTURE_2D, texture); shader_.SetInt(name, unit); }
    static void Attach(GLuint texture) { GL::FramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, texture, 0); }
    void Allocate(int w, int h) {
        if (!ready_) {
            for (int i = 0; i < 5; ++i) { glBindTexture(GL_TEXTURE_2D, tex_[i]); int size = i == 3 ? 128 : 512; glTexImage2D(GL_TEXTURE_2D, 0, i == 2 ? 0x81A6 : (i < 2 ? 0x8814 : 0x881A), size, size, 0, i == 2 ? GL_DEPTH_COMPONENT : GL_RGBA, GL_FLOAT, nullptr); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 || i == 1 || i == 2 ? GL_NEAREST : GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, i == 0 || i == 1 || i == 2 ? GL_NEAREST : GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F); }
            GL::BindFramebuffer(0x8D40, fbo_[0]); Attach(tex_[0]); GL::FramebufferTexture2D(0x8D40, 0x8D00, GL_TEXTURE_2D, tex_[2], 0);
            if (GL::CheckFramebufferStatus(0x8D40) != 0x8CD5)throw std::runtime_error("Liquid light target allocation failed");
            glBindTexture(0x8513, environment_); std::array<glm::vec3, 32 * 32> pixels;
            for (int face = 0; face < 6; ++face) {
                for (int y = 0; y < 32; ++y)for (int x = 0; x < 32; ++x) {
                    float a = 2 * (x + .5f) / 32 - 1, b = 2 * (y + .5f) / 32 - 1; glm::vec3 d;
                switch (face) { case 0:d = { 1,-b,-a }; break; case 1:d = { -1,-b,a }; break; case 2:d = { a,1,b }; break; case 3:d = { a,-1,-b }; break; case 4:d = { a,-b,1 }; break; default:d = { -a,-b,-1 }; break; }
                                      d = glm::normalize(d); float elevation = glm::smoothstep(-.15f, .8f, d.y); pixels[y * 32 + x] = glm::mix(glm::vec3(.30f, .38f, .44f), glm::vec3(.12f, .23f, .38f), elevation);
                }glTexImage2D(0x8515 + face, 0, 0x881B, 32, 32, 0, GL_RGB, GL_FLOAT, pixels.data());
            }
            glTexParameteri(0x8513, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(0x8513, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexParameteri(0x8513, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(0x8513, GL_TEXTURE_WRAP_T, 0x812F); glTexParameteri(0x8513, 0x8072, 0x812F);
            std::array<unsigned char, 32 * 32 * 32 * 4> noise; unsigned seed = 1729;
            for (auto& value : noise) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; value = static_cast<unsigned char>(seed & 255); }
            void(APIENTRY * image3d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr; GL::LoadFunction(image3d, "glTexImage3D");
            glBindTexture(0x806F, noise_); image3d(0x806F, 0, GL_RGBA8, 32, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, noise.data());
            glTexParameteri(0x806F, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(0x806F, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexParameteri(0x806F, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(0x806F, GL_TEXTURE_WRAP_T, GL_REPEAT); glTexParameteri(0x806F, 0x8072, GL_REPEAT); ready_ = true;
        }
        if (w != width_ || h != height_) { glBindTexture(GL_TEXTURE_2D, tex_[5]); glTexImage2D(GL_TEXTURE_2D, 0, 0x881A, w, h, 0, GL_RGBA, GL_FLOAT, nullptr); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F); width_ = w; height_ = h; }
    }
    Shader shader_; GLuint fbo_[2]{}, tex_[6]{}, environment_ = 0, noise_ = 0; bool ready_ = false; int width_ = 0, height_ = 0;
    void(APIENTRY* instanced_)(GLenum, GLint, GLsizei, GLsizei) = nullptr;
};

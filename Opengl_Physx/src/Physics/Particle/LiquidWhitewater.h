#pragma once
#include "Shader.h"
#include <limits>
#include <array>

class LiquidWhitewater
{
public:
    LiquidWhitewater() :shader_("Assets/Shaders/liquid_whitewater.glsl") {
        GLint major = 0, minor = 0; glGetIntegerv(0x821B, &major); glGetIntegerv(0x821C, &minor);
        if (major < 4 || (major == 4 && minor < 3))throw std::runtime_error("GPU whitewater requires OpenGL 4.3");
        GL::LoadFunction(dispatch_, "glDispatchCompute"); GL::LoadFunction(barrier_, "glMemoryBarrier"); GL::LoadFunction(bindBase_, "glBindBufferBase"); GL::LoadFunction(getIndexed_, "glGetIntegeri_v");
        GL::LoadFunction(uniformUInt_, "glUniform1ui"); GL::LoadFunction(instanced_, "glDrawArraysInstanced"); GL::LoadFunction(divisor_, "glVertexAttribDivisor");
        try { for (int i = 0; i < 7; ++i) { compute_[i] = BuildCompute(i); const char* names[] = { "particleCount","poolCapacity","hashSize","tick","spacing","deltaTime","gravityScale" }; for (int j = 0; j < 7; ++j)locations_[i][j] = GL::GetUniformLocation(compute_[i], names[j]); }GL::GenBuffers(6, buffers_); GL::GenVertexArrays(1, &vao_); }
        catch (...) { Release(); throw; }
    }
    ~LiquidWhitewater() { Release(); }
    void Invalidate() { clear_ = true; lastRevision_ = std::numeric_limits<unsigned long long>::max(); }
    void Draw(GLuint source, unsigned count, float spacing, unsigned long long revision, const glm::mat4& view, const glm::mat4& projection, GLuint water, GLuint scene, GLuint destination, int width, int height, float gravityScale = 1) {
        Simulate(source, count, spacing, revision, gravityScale);
        GL::BindFramebuffer(0x8D40, destination); glViewport(0, 0, width, height); glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        shader_.Use(); shader_.SetMatrix4("view", view); shader_.SetMatrix4("projection", projection); shader_.SetMatrix4("inverseProjection", glm::inverse(projection)); shader_.SetVector2("resolution", glm::vec2(width, height)); shader_.SetFloat("spacing", spacing);
        GL::ActiveTexture(0x84C2); glBindTexture(GL_TEXTURE_2D, water); shader_.SetInt("waterDepth", 2);
        GL::ActiveTexture(0x84C3); glBindTexture(GL_TEXTURE_2D, scene); shader_.SetInt("sceneDepth", 3);
        GL::BindVertexArray(vao_); GL::BindBuffer(GL::ArrayBuffer, buffers_[3]);
        for (unsigned i = 0; i < 3; ++i) { GL::VertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 48, reinterpret_cast<const void*>(size_t(i) * 16)); GL::EnableVertexAttribArray(i); divisor_(i, 1); }
        glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); instanced_(GL_TRIANGLE_STRIP, 0, 4, PoolCapacity); glDisable(GL_BLEND);
    }
    void Simulate(GLuint source, unsigned count, float spacing, unsigned long long revision, float gravityScale = 1) {
        if (!count || (!clear_ && lastRevision_ == revision))return;
        StorageState state(*this); Ensure(count);
        bindBase_(0x90D2, 0, source); for (unsigned i = 0; i < 6; ++i)bindBase_(0x90D2, i + 1, buffers_[i]);
        count_ = count; spacing_ = spacing; gravityScale_ = gravityScale;
        if (clear_) { Run(5, PoolCapacity); clear_ = false; }
        Run(0, HashSize); Run(1, count); Run(2, count);
        unsigned steps = lastRevision_ == std::numeric_limits<unsigned long long>::max() ? 1u : unsigned(std::min(revision > lastRevision_ ? revision - lastRevision_ : 1ull, 6ull));
        for (unsigned i = 0; i < steps; ++i) { tick_ = unsigned(revision - steps + i + 1); Run(6, 1); Run(3, PoolCapacity); Run(4, count); }
        barrier_(0x2000 | 0x0001); lastRevision_ = revision;
    }
private:
    static constexpr unsigned PoolCapacity = 262144, HashSize = 1048576;
    using Dispatch = void(APIENTRY*)(GLuint, GLuint, GLuint); using Barrier = void(APIENTRY*)(GLbitfield); using BindBase = void(APIENTRY*)(GLenum, GLuint, GLuint); using GetIndexed = void(APIENTRY*)(GLenum, GLuint, GLint*);
    using UniformUInt = void(APIENTRY*)(GLint, GLuint); using Instanced = void(APIENTRY*)(GLenum, GLint, GLsizei, GLsizei); using Divisor = void(APIENTRY*)(GLuint, GLuint);
    struct StorageState { LiquidWhitewater& owner; GLint generic, program, bindings[7]; StorageState(LiquidWhitewater& o) :owner(o) { glGetIntegerv(0x8B8D, &program); glGetIntegerv(0x90D3, &generic); for (unsigned i = 0; i < 7; ++i)o.getIndexed_(0x90D3, i, &bindings[i]); }~StorageState() { for (unsigned i = 0; i < 7; ++i)owner.bindBase_(0x90D2, i, bindings[i]); GL::BindBuffer(0x90D2, generic); GL::UseProgram(program); } };
    void Run(int pass, unsigned count) { GL::UseProgram(compute_[pass]); auto* p = locations_[pass]; uniformUInt_(p[0], count_); uniformUInt_(p[1], PoolCapacity); uniformUInt_(p[2], HashSize); uniformUInt_(p[3], tick_); GL::Uniform1f(p[4], spacing_); GL::Uniform1f(p[5], 1.f / 60.f); GL::Uniform1f(p[6], gravityScale_); dispatch_((count + 127) / 128, 1, 1); barrier_(0x2000); }
    void Allocate(unsigned index, size_t bytes) { GL::BindBuffer(0x90D2, buffers_[index]); GL::BufferData(0x90D2, bytes, nullptr, 0x88E8); }
    void Ensure(unsigned count) {
        if (!allocated_) { Allocate(0, size_t(HashSize) * 4); Allocate(3, size_t(PoolCapacity) * 48); Allocate(4, size_t(PoolCapacity) * 4); Allocate(5, 8 * 4); allocated_ = true; }
        if (count > capacity_) { Allocate(1, size_t(count) * 4); Allocate(2, size_t(count) * 16); capacity_ = count; }
    }
    static GLuint BuildCompute(int stage) {
        std::vector<wchar_t> path(32768); DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())); if (!length || length >= path.size())throw std::runtime_error("Cannot locate whitewater shader");
        auto file = std::filesystem::path(std::wstring(path.data(), length)).parent_path() / "Assets/Shaders/liquid_whitewater.comp"; std::ifstream input(file, std::ios::binary); if (!input)throw std::runtime_error("Missing Assets/Shaders/liquid_whitewater.comp");
        std::string source((std::istreambuf_iterator<char>(input)), {}); if (source.compare(0, 3, "\xef\xbb\xbf") == 0)source.erase(0, 3); source.insert(source.find('\n') + 1, "#define KERNEL_STAGE " + std::to_string(stage) + "\n");
        GLuint shader = GL::CreateShader(0x91B9), program = 0; try { const char* text = source.c_str(); GL::ShaderSource(shader, 1, &text, nullptr); GL::CompileShader(shader); GLint ok = 0; GL::GetShaderiv(shader, GL::CompileStatus, &ok); if (!ok) { char log[8192]{}; GL::GetShaderInfoLog(shader, sizeof(log), nullptr, log); throw std::runtime_error(std::string("Whitewater compute: ") + log); }program = GL::CreateProgram(); GL::AttachShader(program, shader); GL::LinkProgram(program); GL::GetProgramiv(program, GL::LinkStatus, &ok); if (!ok) { char log[8192]{}; GL::GetProgramInfoLog(program, sizeof(log), nullptr, log); throw std::runtime_error(std::string("Whitewater link: ") + log); }GL::DeleteShader(shader); return program; }
        catch (...) { GL::DeleteShader(shader); if (program)GL::DeleteProgram(program); throw; }
    }
    void Release() { for (auto p : compute_)if (p)GL::DeleteProgram(p); GL::DeleteBuffers(6, buffers_); GL::DeleteVertexArrays(1, &vao_); }
    Shader shader_; GLuint compute_[7]{}, buffers_[6]{}, vao_ = 0; unsigned count_ = 0, tick_ = 0; float spacing_ = 0, gravityScale_ = 1; GLint locations_[7][7]{}; unsigned capacity_ = 0; bool allocated_ = false, clear_ = true;
    unsigned long long lastRevision_ = std::numeric_limits<unsigned long long>::max(); Dispatch dispatch_ = nullptr; Barrier barrier_ = nullptr; BindBase bindBase_ = nullptr; GetIndexed getIndexed_ = nullptr; UniformUInt uniformUInt_ = nullptr; Instanced instanced_ = nullptr; Divisor divisor_ = nullptr;
};

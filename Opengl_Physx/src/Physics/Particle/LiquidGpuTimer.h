#pragma once
#include "OpenGL.h"
#include <array>

class LiquidGpuTimer {
public:
    ~LiquidGpuTimer() { if (initialized_) remove_(8, queries_.data()); }
    LiquidGpuTimer() = default;
    LiquidGpuTimer(const LiquidGpuTimer&) = delete;
    LiquidGpuTimer& operator=(const LiquidGpuTimer&) = delete;
    double Milliseconds() const { return milliseconds_; }
    bool HasResult() const { return hasResult_; }
    void Begin() {
        if (!initialized_) {
            GL::LoadFunction(generate_, "glGenQueries");
            GL::LoadFunction(remove_, "glDeleteQueries");
            GL::LoadFunction(counter_, "glQueryCounter");
            GL::LoadFunction(available_, "glGetQueryObjectiv");
            GL::LoadFunction(result_, "glGetQueryObjectui64v");
            generate_(8, queries_.data()); initialized_ = true;
        }
        while (pending_) {
            GLint ready = 0; available_(queries_[read_ * 2 + 1], 0x8867, &ready);
            if (!ready)break;
            unsigned long long start = 0, end = 0;
            result_(queries_[read_ * 2], 0x8866, &start);
            result_(queries_[read_ * 2 + 1], 0x8866, &end);
            const double sample = double(end - start) / 1000000.0;
            milliseconds_ = hasResult_ ? milliseconds_ * .9 + sample * .1 : sample;
            hasResult_ = true; read_ = (read_ + 1) % 4; --pending_;
        }
        measuring_ = pending_ < 4;
        if (measuring_)counter_(queries_[write_ * 2], 0x8E28);
    }
    void End() {
        if (!measuring_)return;
        counter_(queries_[write_ * 2 + 1], 0x8E28);
        write_ = (write_ + 1) % 4; ++pending_; measuring_ = false;
    }
private:
    std::array<GLuint, 8> queries_{};
    unsigned read_ = 0, write_ = 0, pending_ = 0;
    bool initialized_ = false, measuring_ = false, hasResult_ = false;
    double milliseconds_ = 0;
    void(APIENTRY* generate_)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* remove_)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* counter_)(GLuint, GLenum) = nullptr;
    void(APIENTRY* available_)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* result_)(GLuint, GLenum, unsigned long long*) = nullptr;
};

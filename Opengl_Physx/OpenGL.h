#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <GLFW/glfw3.h>
#include <cstddef>

// Small loader for the functions used by this example.
// Replace this module with GLAD when the renderer grows.
namespace GL
{
    constexpr GLenum VertexShader = 0x8B31;
    constexpr GLenum FragmentShader = 0x8B30;
    constexpr GLenum CompileStatus = 0x8B81;
    constexpr GLenum LinkStatus = 0x8B82;
    constexpr GLenum ArrayBuffer = 0x8892;
    constexpr GLenum ElementArrayBuffer = 0x8893;
    constexpr GLenum StaticDraw = 0x88E4;

    using CreateShaderFunction = GLuint(APIENTRY*)(GLenum);
    using ShaderSourceFunction = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
    using CompileShaderFunction = void(APIENTRY*)(GLuint);
    using GetShaderivFunction = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using GetShaderInfoLogFunction = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using DeleteShaderFunction = void(APIENTRY*)(GLuint);
    using CreateProgramFunction = GLuint(APIENTRY*)();
    using AttachShaderFunction = void(APIENTRY*)(GLuint, GLuint);
    using LinkProgramFunction = void(APIENTRY*)(GLuint);
    using GetProgramivFunction = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using GetProgramInfoLogFunction = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using UseProgramFunction = void(APIENTRY*)(GLuint);
    using DeleteProgramFunction = void(APIENTRY*)(GLuint);
    using GenVertexArraysFunction = void(APIENTRY*)(GLsizei, GLuint*);
    using BindVertexArrayFunction = void(APIENTRY*)(GLuint);
    using DeleteVertexArraysFunction = void(APIENTRY*)(GLsizei, const GLuint*);
    using GenBuffersFunction = void(APIENTRY*)(GLsizei, GLuint*);
    using BindBufferFunction = void(APIENTRY*)(GLenum, GLuint);
    using BufferDataFunction = void(APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
    using DeleteBuffersFunction = void(APIENTRY*)(GLsizei, const GLuint*);
    using VertexAttribPointerFunction = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    using EnableVertexAttribArrayFunction = void(APIENTRY*)(GLuint);
    using GetUniformLocationFunction = GLint(APIENTRY*)(GLuint, const char*);
    using UniformMatrix4fvFunction = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);

    using GenFramebuffersFunction = void(APIENTRY*)(GLsizei, GLuint*);
    using DeleteFramebuffersFunction = void(APIENTRY*)(GLsizei, const GLuint*);
    using BindFramebufferFunction = void(APIENTRY*)(GLenum, GLuint);
    using FramebufferTexture2DFunction = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using CheckFramebufferStatusFunction = GLenum(APIENTRY*)(GLenum);
    using ActiveTextureFunction = void(APIENTRY*)(GLenum);
    using GenerateMipmapFunction = void(APIENTRY*)(GLenum);
    using Uniform1iFunction = void(APIENTRY*)(GLint, GLint);
    using Uniform1fFunction = void(APIENTRY*)(GLint, GLfloat);
    using Uniform2fFunction = void(APIENTRY*)(GLint, GLfloat, GLfloat);
    using Uniform3fFunction = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
    using Uniform4fFunction = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    using GetActiveUniformFunction = void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
    inline CreateShaderFunction CreateShader = nullptr;
    inline ShaderSourceFunction ShaderSource = nullptr;
    inline CompileShaderFunction CompileShader = nullptr;
    inline GetShaderivFunction GetShaderiv = nullptr;
    inline GetShaderInfoLogFunction GetShaderInfoLog = nullptr;
    inline DeleteShaderFunction DeleteShader = nullptr;
    inline CreateProgramFunction CreateProgram = nullptr;
    inline AttachShaderFunction AttachShader = nullptr;
    inline LinkProgramFunction LinkProgram = nullptr;
    inline GetProgramivFunction GetProgramiv = nullptr;
    inline GetProgramInfoLogFunction GetProgramInfoLog = nullptr;
    inline UseProgramFunction UseProgram = nullptr;
    inline DeleteProgramFunction DeleteProgram = nullptr;
    inline GenVertexArraysFunction GenVertexArrays = nullptr;
    inline BindVertexArrayFunction BindVertexArray = nullptr;
    inline DeleteVertexArraysFunction DeleteVertexArrays = nullptr;
    inline GenBuffersFunction GenBuffers = nullptr;
    inline BindBufferFunction BindBuffer = nullptr;
    inline BufferDataFunction BufferData = nullptr;
    inline DeleteBuffersFunction DeleteBuffers = nullptr;
    inline VertexAttribPointerFunction VertexAttribPointer = nullptr;
    inline EnableVertexAttribArrayFunction EnableVertexAttribArray = nullptr;
    inline GetUniformLocationFunction GetUniformLocation = nullptr;
    inline UniformMatrix4fvFunction UniformMatrix4fv = nullptr;

    inline GenFramebuffersFunction GenFramebuffers = nullptr;
    inline DeleteFramebuffersFunction DeleteFramebuffers = nullptr;
    inline BindFramebufferFunction BindFramebuffer = nullptr;
    inline FramebufferTexture2DFunction FramebufferTexture2D = nullptr;
    inline CheckFramebufferStatusFunction CheckFramebufferStatus = nullptr;
    inline ActiveTextureFunction ActiveTexture = nullptr;
    inline GenerateMipmapFunction GenerateMipmap = nullptr;
    inline Uniform1iFunction Uniform1i = nullptr;
    inline Uniform1fFunction Uniform1f = nullptr;
    inline Uniform2fFunction Uniform2f = nullptr;
    inline Uniform3fFunction Uniform3f = nullptr;
    inline Uniform4fFunction Uniform4f = nullptr;
    inline GetActiveUniformFunction GetActiveUniform = nullptr;

    // Call only after an OpenGL context has been made current.
    inline void Load();
}

#include <stdexcept>
#include <string>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")

namespace GL
{

    template<typename T>
    void LoadFunction(T& function, const char* name)
    {
        function = reinterpret_cast<T>(glfwGetProcAddress(name));
        if (!function)
        {
            throw std::runtime_error(std::string("Cannot load OpenGL function: ") + name);
        }
    }

    inline void Load()
    {
        LoadFunction(GenFramebuffers, "glGenFramebuffers");
        LoadFunction(DeleteFramebuffers, "glDeleteFramebuffers");
        LoadFunction(BindFramebuffer, "glBindFramebuffer");
        LoadFunction(FramebufferTexture2D, "glFramebufferTexture2D");
        LoadFunction(CheckFramebufferStatus, "glCheckFramebufferStatus");
        LoadFunction(ActiveTexture, "glActiveTexture");
        LoadFunction(GenerateMipmap, "glGenerateMipmap");
        LoadFunction(Uniform1i, "glUniform1i");
        LoadFunction(Uniform1f, "glUniform1f");
        LoadFunction(Uniform2f, "glUniform2f");
        LoadFunction(Uniform3f, "glUniform3f");
        LoadFunction(Uniform4f, "glUniform4f");
        LoadFunction(GetActiveUniform, "glGetActiveUniform");
        LoadFunction(CreateShader, "glCreateShader");
        LoadFunction(ShaderSource, "glShaderSource");
        LoadFunction(CompileShader, "glCompileShader");
        LoadFunction(GetShaderiv, "glGetShaderiv");
        LoadFunction(GetShaderInfoLog, "glGetShaderInfoLog");
        LoadFunction(DeleteShader, "glDeleteShader");
        LoadFunction(CreateProgram, "glCreateProgram");
        LoadFunction(AttachShader, "glAttachShader");
        LoadFunction(LinkProgram, "glLinkProgram");
        LoadFunction(GetProgramiv, "glGetProgramiv");
        LoadFunction(GetProgramInfoLog, "glGetProgramInfoLog");
        LoadFunction(UseProgram, "glUseProgram");
        LoadFunction(DeleteProgram, "glDeleteProgram");
        LoadFunction(GenVertexArrays, "glGenVertexArrays");
        LoadFunction(BindVertexArray, "glBindVertexArray");
        LoadFunction(DeleteVertexArrays, "glDeleteVertexArrays");
        LoadFunction(GenBuffers, "glGenBuffers");
        LoadFunction(BindBuffer, "glBindBuffer");
        LoadFunction(BufferData, "glBufferData");
        LoadFunction(DeleteBuffers, "glDeleteBuffers");
        LoadFunction(VertexAttribPointer, "glVertexAttribPointer");
        LoadFunction(EnableVertexAttribArray, "glEnableVertexAttribArray");
        LoadFunction(GetUniformLocation, "glGetUniformLocation");
        LoadFunction(UniformMatrix4fv, "glUniformMatrix4fv");
    }
}




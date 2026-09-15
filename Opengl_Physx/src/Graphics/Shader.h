#pragma once
#include "OpenGL.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <map>
#include <set>
#include <initializer_list>

// Shader 独占各 Pass 的 OpenGL Program，必须先于 Window 销毁。
class Shader
{
public:
    explicit Shader(const std::filesystem::path& file, std::initializer_list<std::string> passNames = {})
    {
        const auto path = Resolve(file);
        const std::string source = ReadFile(path);
        std::vector<std::string> names;
        if (passNames.size() == 0) names.push_back("Default");
        else for (const auto& name : passNames) names.push_back(Normalize(name));
        std::set<std::string> unique;
        for (const auto& name : names)
            if (!unique.insert(name).second) throw std::invalid_argument("Duplicate shader pass: " + name);
        try
        {
            for (const auto& name : names)
            {
                std::string define = name == "Default" ? "" : "#define " + name + "\n";
                // #version 必须在最前面；#line 让驱动日志行号对应原始文件。
                std::string vertex = "#version 330 core\n#define VERTEX_SHADER\n" + define + "#line 1\n" + source;
                std::string fragment = "#version 330 core\n#define FRAGMENT_SHADER\n" + define + "#line 1\n" + source;
                GLuint id = Build(vertex, fragment, PathText(path) + " [" + name + "]");
                try { passes.emplace(name, id); }
                catch (...) { GL::DeleteProgram(id); throw; }
            }
            program = passes.at(names.front());
            SearchSamplers();
        }
        catch (...) { Release(); throw; }
    }

    ~Shader() { Release(); }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    GLuint GetProgram() const { return program; }

    void Use() const { GL::UseProgram(program); }
    void UsePass(const std::string& name)
    {
        auto found = passes.find(Normalize(name));
        if (found == passes.end()) throw std::invalid_argument("Unknown shader pass: " + name);
        program = found->second;
        Use();
    }

    const std::map<std::string, GLuint>& GetPasses() const { return passes; }
    const std::vector<std::string>& GetSampler2DNames() const { return samplerNames; }

    GLint GetUniformLocation(const char* name) const
    {
        auto& cache = locations[program];
        auto found = cache.find(name);
        if (found != cache.end()) return found->second;
        GLint location = GL::GetUniformLocation(program, name);
        cache.emplace(name, location);
        // -1 表示当前 Pass 中不存在或被优化掉，OpenGL 会忽略对此位置的上传。
        return location;
    }

    void SetBool(const char* name, bool value) const { SetInt(name, value ? 1 : 0); }
    void SetInt(const char* name, int value) const { Use(); GL::Uniform1i(GetUniformLocation(name), value); }
    void SetFloat(const char* name, float value) const { Use(); GL::Uniform1f(GetUniformLocation(name), value); }
    void SetVector2(const char* name, glm::vec2 value) const { Use(); GL::Uniform2f(GetUniformLocation(name), value.x, value.y); }
    void SetVector3(const char* name, glm::vec3 value) const { Use(); GL::Uniform3f(GetUniformLocation(name), value.x, value.y, value.z); }
    void SetVector4(const char* name, glm::vec4 value) const { Use(); GL::Uniform4f(GetUniformLocation(name), value.x, value.y, value.z, value.w); }
    void SetMatrix4(const char* name, const glm::mat4& matrix) const { SetMatrix4(GetUniformLocation(name), matrix); }
    // 数字 location 只能用于获取它的那个 Pass；切换 Pass 后请重新获取或使用名称版本。
    void SetMatrix4(GLint location, const glm::mat4& matrix) const { Use(); GL::UniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix)); }

private:
    GLuint program = 0;
    std::map<std::string, GLuint> passes;
    mutable std::map<GLuint, std::map<std::string, GLint>> locations;
    std::vector<std::string> samplerNames;
    static GLuint Compile(GLenum type, const char* source);
    static std::filesystem::path Resolve(const std::filesystem::path& file);
    static std::string ReadFile(const std::filesystem::path& file);
    static std::string PathText(const std::filesystem::path& file)
    {
        auto value = file.u8string();
        return std::string(value.begin(), value.end());
    }

    static std::string Normalize(std::string name)
    {
        auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) throw std::invalid_argument("Shader pass cannot be empty.");
        name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
        for (char& c : name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (name == "DEFAULT") return "Default";
        if (name.rfind("PASS_", 0) != 0) name = "PASS_" + name;
        if (name.size() == 5) throw std::invalid_argument("Shader pass must have a name after PASS_.");
        for (char c : name)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                throw std::invalid_argument("Shader pass may only contain ASCII letters, digits and underscores.");
        return name;
    }

    static GLuint Build(const std::string& vs, const std::string& fs, const std::string& label)
    {
        GLuint vertex = 0, fragment = 0, id = 0;
        try
        {
            try { vertex = Compile(GL::VertexShader, vs.c_str()); }
            catch (const std::exception& e) { throw std::runtime_error(label + " [vertex]\n" + e.what()); }
            try { fragment = Compile(GL::FragmentShader, fs.c_str()); }
            catch (const std::exception& e) { throw std::runtime_error(label + " [fragment]\n" + e.what()); }
            id = GL::CreateProgram();
            if (!id) throw std::runtime_error("Cannot create shader program: " + label);
            GL::AttachShader(id, vertex);
            GL::AttachShader(id, fragment);
            GL::LinkProgram(id);
            GLint success = GL_FALSE;
            GL::GetProgramiv(id, GL::LinkStatus, &success);
            if (!success)
            {
                char log[4096] = {};
                GL::GetProgramInfoLog(id, sizeof(log), nullptr, log);
                throw std::runtime_error(label + " linking failed:\n" + log);
            }
        }
        catch (...)
        {
            if (vertex) GL::DeleteShader(vertex);
            if (fragment) GL::DeleteShader(fragment);
            if (id) GL::DeleteProgram(id);
            throw;
        }
        GL::DeleteShader(vertex);
        GL::DeleteShader(fragment);
        return id;
    }

    void SearchSamplers()
    {
        constexpr GLenum activeUniforms = 0x8B86, maxNameLength = 0x8B87, sampler2D = 0x8B5E;
        std::set<std::string> names;
        for (const auto& pass : passes)
        {
            GLint count = 0, capacity = 0;
            GL::GetProgramiv(pass.second, activeUniforms, &count);
            GL::GetProgramiv(pass.second, maxNameLength, &capacity);
            if (count == 0 || capacity <= 0) continue;
            std::vector<char> buffer(static_cast<std::size_t>(capacity));
            for (GLint i = 0; i < count; ++i)
            {
                GLsizei length = 0;
                GLint size = 0;
                GLenum type = 0;
                GL::GetActiveUniform(pass.second, static_cast<GLuint>(i), capacity, &length, &size, &type, buffer.data());
                if (type == sampler2D) names.emplace(buffer.data(), static_cast<std::size_t>(length));
            }
        }
        samplerNames.assign(names.begin(), names.end());
    }

    void Release()
    {
        for (const auto& pass : passes) GL::DeleteProgram(pass.second);
        passes.clear();
        program = 0;
    }
};
inline GLuint Shader::Compile(GLenum type, const char* source)
{
    GLuint shader = GL::CreateShader(type);
    if (!shader)
    {
        throw std::runtime_error("Cannot create shader.");
    }

    GL::ShaderSource(shader, 1, &source, nullptr);
    GL::CompileShader(shader);

    GLint success = GL_FALSE;
    GL::GetShaderiv(shader, GL::CompileStatus, &success);
    if (success == GL_FALSE)
    {
        char log[4096] = {};
        GL::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        GL::DeleteShader(shader);
        throw std::runtime_error(std::string("Shader compilation failed:\n") + log);
    }
    return shader;
}

inline std::filesystem::path Shader::Resolve(const std::filesystem::path& file)
{
    if (file.is_absolute()) return file;
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Cannot locate executable directory.");
        if (length < buffer.size())
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() / file;
        if (buffer.size() >= 32768) throw std::runtime_error("Executable path is too long.");
        buffer.resize(buffer.size() * 2);
    }
}

inline std::string Shader::ReadFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open GLSL file: " + PathText(file));
    auto size = stream.tellg();
    if (size <= 0 || size > 4 * 1024 * 1024) throw std::runtime_error("GLSL file is empty or exceeds 4 MB: " + PathText(file));
    std::string source(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::runtime_error("Cannot read GLSL file: " + PathText(file));
    // 支持编辑器保存的 UTF-8 BOM，确保 #version 是第一条指令。
    if (source.size() >= 3 && source.compare(0, 3, "\xEF\xBB\xBF") == 0) source.erase(0, 3);
    if (source.find('\0') != std::string::npos) throw std::runtime_error("Save GLSL as UTF-8, not UTF-16: " + PathText(file));
    return source;
}




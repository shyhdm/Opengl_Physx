#pragma once
#include "Shader.h"
#include <limits>
#include <algorithm>

// Render-only neighborhood smoothing and volume-normalized covariance axes.
// Never writes the simulation buffer; isolated particles retain sphere geometry.
class LiquidReconstruction {
public:
    LiquidReconstruction() {
        GL::LoadFunction(dispatch_, "glDispatchCompute"); GL::LoadFunction(barrier_, "glMemoryBarrier");
        GL::LoadFunction(bindBase_, "glBindBufferBase"); GL::LoadFunction(getIndexed_, "glGetIntegeri_v");
        GL::LoadFunction(uniformUInt_, "glUniform1ui");
        try {
            for (int i=0;i<3;++i) {
                programs_[i]=BuildCompute(i);
                const char* names[]={"particleCount","hashSize","spacing"};
                for (int j=0;j<3;++j)locations_[i][j]=GL::GetUniformLocation(programs_[i],names[j]);
            }
            GL::GenBuffers(3,buffers_);
        } catch (...) { Release(); throw; }
    }
    ~LiquidReconstruction() { Release(); }
    LiquidReconstruction(const LiquidReconstruction&)=delete;
    LiquidReconstruction& operator=(const LiquidReconstruction&)=delete;
    void Invalidate() { valid_=false; }
    GLuint Prepare(GLuint source,unsigned count,float spacing,unsigned long long revision) {
        if(valid_ && source==source_ && count==count_ && spacing==spacing_ && revision==revision_)return buffers_[2];
        StorageState state(*this);
        unsigned hashSize=1024;
        while(hashSize<std::min(count*2u,1u<<21))hashSize*=2;
        if(hashSize!=hashSize_) { Allocate(0,size_t(hashSize)*sizeof(int));hashSize_=hashSize; }
        if(count>capacity_) { Allocate(1,size_t(count)*sizeof(int));Allocate(2,size_t(count)*80);capacity_=count; }
        bindBase_(0x90D2,0,source);
        for(unsigned i=0;i<3;++i)bindBase_(0x90D2,i+1,buffers_[i]);
        for(unsigned i=0;i<3;++i) {
            GL::UseProgram(programs_[i]);uniformUInt_(locations_[i][0],count);
            uniformUInt_(locations_[i][1],hashSize_);GL::Uniform1f(locations_[i][2],spacing);
            dispatch_(((i==0?hashSize_:count)+127)/128,1,1);barrier_(0x2000);
        }
        barrier_(0x2000|0x0001);
        source_=source;count_=count;spacing_=spacing;revision_=revision;valid_=true;
        return buffers_[2];
    }
private:
    using Dispatch=void(APIENTRY*)(GLuint,GLuint,GLuint);
    using Barrier=void(APIENTRY*)(GLbitfield);
    using BindBase=void(APIENTRY*)(GLenum,GLuint,GLuint);
    using GetIndexed=void(APIENTRY*)(GLenum,GLuint,GLint*);
    using UniformUInt=void(APIENTRY*)(GLint,GLuint);
    struct StorageState {
        LiquidReconstruction& owner;GLint generic,program,bindings[4];
        explicit StorageState(LiquidReconstruction& o):owner(o) {
            glGetIntegerv(0x8B8D,&program);glGetIntegerv(0x90D3,&generic);
            for(unsigned i=0;i<4;++i)o.getIndexed_(0x90D3,i,&bindings[i]);
        }
        ~StorageState() {
            for(unsigned i=0;i<4;++i)owner.bindBase_(0x90D2,i,bindings[i]);
            GL::BindBuffer(0x90D2,generic);GL::UseProgram(program);
        }
    };
    void Allocate(unsigned i,size_t bytes) {GL::BindBuffer(0x90D2,buffers_[i]);GL::BufferData(0x90D2,bytes,nullptr,0x88E8);}
    static GLuint BuildCompute(int stage) {
        std::vector<wchar_t> path(32768); DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())); if (!length || length >= path.size())throw std::runtime_error("Cannot locate surface reconstruction shader");
        auto file = std::filesystem::path(std::wstring(path.data(), length)).parent_path() / "Assets/Shaders/liquid_reconstruct.comp"; std::ifstream input(file, std::ios::binary); if (!input)throw std::runtime_error("Missing Assets/Shaders/liquid_reconstruct.comp");
        std::string source((std::istreambuf_iterator<char>(input)), {}); if (source.compare(0, 3, "\xef\xbb\xbf") == 0)source.erase(0, 3); source.insert(source.find('\n') + 1, "#define KERNEL_STAGE " + std::to_string(stage) + "\n");
        GLuint shader = GL::CreateShader(0x91B9), program = 0; try { const char* text = source.c_str(); GL::ShaderSource(shader, 1, &text, nullptr); GL::CompileShader(shader); GLint ok = 0; GL::GetShaderiv(shader, GL::CompileStatus, &ok); if (!ok) { char log[8192]{}; GL::GetShaderInfoLog(shader, sizeof(log), nullptr, log); throw std::runtime_error(std::string("Surface reconstruction compute: ") + log); }program = GL::CreateProgram(); GL::AttachShader(program, shader); GL::LinkProgram(program); GL::GetProgramiv(program, GL::LinkStatus, &ok); if (!ok) { char log[8192]{}; GL::GetProgramInfoLog(program, sizeof(log), nullptr, log); throw std::runtime_error(std::string("Surface reconstruction link: ") + log); }GL::DeleteShader(shader); return program; }
        catch (...) { GL::DeleteShader(shader); if (program)GL::DeleteProgram(program); throw; }
    }
    void Release() {for(auto p:programs_)if(p)GL::DeleteProgram(p);GL::DeleteBuffers(3,buffers_);}
    GLuint programs_[3]{},buffers_[3]{},source_=0;
    GLint locations_[3][3]{};
    unsigned capacity_=0,hashSize_=0,count_=0;float spacing_=0;
    unsigned long long revision_=0;bool valid_=false;
    Dispatch dispatch_=nullptr;Barrier barrier_=nullptr;BindBase bindBase_=nullptr;
    GetIndexed getIndexed_=nullptr;UniformUInt uniformUInt_=nullptr;
};

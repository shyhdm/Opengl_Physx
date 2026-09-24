#pragma once
#include "Shader.h"
// Current-frame occlusion only: max depth preserves every hole in the occluders.
class SandOcclusion {
public:
    SandOcclusion(){
        GL::LoadFunction(dispatch_,"glDispatchCompute");GL::LoadFunction(barrier_,"glMemoryBarrier");
        GL::LoadFunction(bindImage_,"glBindImageTexture");GL::LoadFunction(getIndexed_,"glGetIntegeri_v");
        program_=Build();glGenTextures(1,&texture_);
    }
    ~SandOcclusion(){glDeleteTextures(1,&texture_);if(program_)GL::DeleteProgram(program_);}
    SandOcclusion(const SandOcclusion&)=delete;
    SandOcclusion& operator=(const SandOcclusion&)=delete;
    GLuint BuildDepth(GLuint source,int width,int height){
        GLint image[6],program;const GLenum names[]={0x8F3A,0x8F3B,0x8F3C,0x8F3D,0x8F3E,0x906E};
        for(int i=0;i<6;++i)getIndexed_(names[i],0,&image[i]);glGetIntegerv(0x8B8D,&program);
        GL::ActiveTexture(0x84C0+11);glBindTexture(GL_TEXTURE_2D,texture_);
        int w=1,h=1;while(w<width)w*=2;while(h<height)h*=2;
        if(w!=width_||h!=height_){
            levels_=0;
            for(int x=w,y=h;;x=std::max(1,x/2),y=std::max(1,y/2)){
                glTexImage2D(GL_TEXTURE_2D,levels_++,0x822E,x,y,0,0x1903,GL_FLOAT,nullptr);if(x==1&&y==1)break;
            }
            glTexParameteri(GL_TEXTURE_2D,0x813D,levels_-1);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,0x2700);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,0x812F);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,0x812F);
            width_=w;height_=h;
        }
        GL::UseProgram(program_);GL::Uniform1i(GL::GetUniformLocation(program_,"sourceDepth"),11);
        GL::Uniform2f(GL::GetUniformLocation(program_,"sourceSize"),float(width),float(height));
        for(int level=0;level<levels_;++level){
            glBindTexture(GL_TEXTURE_2D,level==0?source:texture_);
            GL::Uniform1i(GL::GetUniformLocation(program_,"sourceLevel"),level-1);
            bindImage_(0,texture_,level,GL_FALSE,0,0x88B9,0x822E);
            dispatch_((w+7)/8,(h+7)/8,1);barrier_(0x0020|0x0008);
            w=std::max(1,w/2);h=std::max(1,h/2);
        }
        bindImage_(0,image[0],image[1],GLboolean(image[2]),image[3],image[4],image[5]);GL::UseProgram(program);
        glBindTexture(GL_TEXTURE_2D,texture_);return texture_;
    }
private:
    static GLuint Build(){
        std::vector<wchar_t> path(32768);DWORD length=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
        if(!length||length>=path.size())throw std::runtime_error("Cannot locate sand occlusion shader");
        auto file=std::filesystem::path(std::wstring(path.data(),length)).parent_path()/"Assets/Shaders/sand_occlusion.comp";
        std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Missing Assets/Shaders/sand_occlusion.comp");
        std::string source((std::istreambuf_iterator<char>(input)),{});if(source.compare(0,3,"\xef\xbb\xbf")==0)source.erase(0,3);
        GLuint shader=GL::CreateShader(0x91B9),program=0;
        try {
            const char* text=source.c_str();GL::ShaderSource(shader,1,&text,nullptr);GL::CompileShader(shader);
            GLint ok=0;GL::GetShaderiv(shader,GL::CompileStatus,&ok);char log[8192]{};
            if(!ok){GL::GetShaderInfoLog(shader,sizeof(log),nullptr,log);throw std::runtime_error(std::string("Sand occlusion compile: ")+log);}
            program=GL::CreateProgram();GL::AttachShader(program,shader);GL::LinkProgram(program);GL::GetProgramiv(program,GL::LinkStatus,&ok);
            if(!ok){GL::GetProgramInfoLog(program,sizeof(log),nullptr,log);throw std::runtime_error(std::string("Sand occlusion link: ")+log);}
            GL::DeleteShader(shader);return program;
        }catch(...){GL::DeleteShader(shader);if(program)GL::DeleteProgram(program);throw;}
    }
    GLuint program_{},texture_{};int width_{},height_{},levels_{};
    void(APIENTRY* dispatch_)(GLuint,GLuint,GLuint)=nullptr;
    void(APIENTRY* barrier_)(GLbitfield)=nullptr;
    void(APIENTRY* bindImage_)(GLuint,GLuint,GLint,GLboolean,GLint,GLenum,GLenum)=nullptr;
    void(APIENTRY* getIndexed_)(GLenum,GLuint,GLint*)=nullptr;
};

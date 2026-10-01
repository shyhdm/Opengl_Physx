#pragma once
#include "Shader.h"
#include "Camera.h"
#include "SceneLight.h"
#include <memory>

// Shared by the visible sky, liquid surface and spray. GPU objects are released
// with their scene owners while the OpenGL context is still alive.
class SkyEnvironment {
public:
    static std::shared_ptr<SkyEnvironment> Shared() {
        static std::weak_ptr<SkyEnvironment> weak;
        auto result=weak.lock();
        if(!result){result=std::shared_ptr<SkyEnvironment>(new SkyEnvironment);weak=result;}
        return result;
    }
    ~SkyEnvironment(){glDeleteTextures(1,&cube_);GL::DeleteFramebuffers(1,&fbo_);GL::DeleteVertexArrays(1,&vao_);}
    SkyEnvironment(const SkyEnvironment&)=delete;
    SkyEnvironment& operator=(const SkyEnvironment&)=delete;
    GLuint Texture(){Prepare();return cube_;}
    void Draw(const Camera& camera,int width,int height){
        if(width<=0||height<=0)return;
        Prepare();State state;
        glViewport(0,0,width,height);Setup();glEnable(0x884F);
        shader_.UsePass("SKY");shader_.SetMatrix4("inverseProjection",glm::inverse(camera.GetProjectionMatrix(float(width)/height)));
        shader_.SetMatrix4("inverseView",glm::inverse(camera.GetViewMatrix()));
        GL::ActiveTexture(0x84C0);glBindTexture(0x8513,cube_);shader_.SetInt("environmentMap",0);
        GL::BindVertexArray(vao_);glDrawArrays(GL_TRIANGLES,0,3);
    }
private:
    struct State {
        GLint draw,read,program,vao,active,cube,viewport[4];
        GLboolean depth,blend,cull,scissor,srgb,seamless,depthWrite,colors[4];
        State(){
            glGetIntegerv(0x8CA6,&draw);glGetIntegerv(0x8CAA,&read);glGetIntegerv(0x8B8D,&program);glGetIntegerv(0x85B5,&vao);
            glGetIntegerv(0x84E0,&active);GL::ActiveTexture(0x84C0);glGetIntegerv(0x8514,&cube);glGetIntegerv(GL_VIEWPORT,viewport);
            depth=glIsEnabled(GL_DEPTH_TEST);blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);scissor=glIsEnabled(GL_SCISSOR_TEST);srgb=glIsEnabled(0x8DB9);seamless=glIsEnabled(0x884F);
            glGetBooleanv(GL_DEPTH_WRITEMASK,&depthWrite);glGetBooleanv(GL_COLOR_WRITEMASK,colors);
        }
        static void Restore(GLenum e,bool v){if(v)glEnable(e);else glDisable(e);}
        ~State(){
            GL::BindFramebuffer(0x8CA9,draw);GL::BindFramebuffer(0x8CA8,read);GL::UseProgram(program);GL::BindVertexArray(vao);
            GL::ActiveTexture(0x84C0);glBindTexture(0x8513,cube);GL::ActiveTexture(active);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
            Restore(GL_DEPTH_TEST,depth);Restore(GL_BLEND,blend);Restore(GL_CULL_FACE,cull);Restore(GL_SCISSOR_TEST,scissor);Restore(0x8DB9,srgb);Restore(0x884F,seamless);
            glDepthMask(depthWrite);glColorMask(colors[0],colors[1],colors[2],colors[3]);
        }
    };
    SkyEnvironment():shader_("Assets/Shaders/sky.glsl",{"BAKE","SKY"}){
        State state;glGenTextures(1,&cube_);GL::GenFramebuffers(1,&fbo_);GL::GenVertexArrays(1,&vao_);
        GL::ActiveTexture(0x84C0);glBindTexture(0x8513,cube_);
        for(int face=0;face<6;++face)glTexImage2D(0x8515+face,0,0x881A,Size,Size,0,GL_RGBA,GL_FLOAT,nullptr);
        glTexParameteri(0x8513,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(0x8513,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        for(auto axis:{GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T,0x8072})glTexParameteri(0x8513,axis,0x812F);
    }
    static void Setup(){glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);glDisable(0x8DB9);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);}
    void Prepare(){
        auto light=SceneLight::Direction();if(ready_ && light==light_)return;
        State state;Setup();GL::BindFramebuffer(0x8D40,fbo_);glViewport(0,0,Size,Size);GL::BindVertexArray(vao_);
        shader_.UsePass("BAKE");shader_.SetVector3("sunDirection",light);
        for(int face=0;face<6;++face){
            GL::FramebufferTexture2D(0x8D40,0x8CE0,0x8515+face,cube_,0);glDrawBuffer(0x8CE0);
            if(GL::CheckFramebufferStatus(0x8D40)!=0x8CD5)throw std::runtime_error("Cannot allocate sky framebuffer");
            shader_.SetInt("face",face);glDrawArrays(GL_TRIANGLES,0,3);
        }
        light_=light;ready_=true;
    }
    static constexpr int Size=256;
    Shader shader_;GLuint cube_=0,fbo_=0,vao_=0;bool ready_=false;glm::vec3 light_{};
};

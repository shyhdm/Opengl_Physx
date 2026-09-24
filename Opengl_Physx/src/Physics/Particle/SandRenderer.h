#pragma once
#include "Shader.h"
#include "Camera.h"
#include <algorithm>
#include <cmath>

// Real-time adaptation of multiscale granular appearance, not an offline
// multiple-scattering solver or a full reproduction of the cited papers.
class SandRenderer {
public:
    struct Parameters {
        glm::vec3 color{ 173.f/255.f,125.f/255.f,69.f/255.f };
        float roughness=.15f, sparkle=4.f, mineralFraction=.20f;
        float microScale=6.f, occlusion=1.f;
        float sunAzimuth=-73.f, sunElevation=59.f;
    };
    static Parameters Clamp(Parameters p) {
        Parameters fallback;
        auto bound=[](float x,float a,float b,float f){return std::isfinite(x)?std::clamp(x,a,b):f;};
        for(int i=0;i<4;++i)p.color[i]=bound(p.color[i],0,1,fallback.color[i]);
        p.roughness=bound(p.roughness,.15f,.8f,fallback.roughness);
        p.sparkle=bound(p.sparkle,0,4,fallback.sparkle);
        p.mineralFraction=bound(p.mineralFraction,0,1,fallback.mineralFraction);
        p.microScale=bound(p.microScale,2,20,fallback.microScale);
        p.occlusion=bound(p.occlusion,0,2,fallback.occlusion);
        p.sunAzimuth=bound(p.sunAzimuth,-180,180,fallback.sunAzimuth);
        p.sunElevation=bound(p.sunElevation,5,85,fallback.sunElevation);
        return p;
    }
    SandRenderer():shader_("Assets/Shaders/sand_granular.glsl",{"DEPTH","SHADE"}) {
        GL::LoadFunction(drawBuffers_,"glDrawBuffers");
        GL::LoadFunction(divisor_,"glVertexAttribDivisor");
        GL::LoadFunction(integerPointer_,"glVertexAttribIPointer");
        GL::LoadFunction(instanced_,"glDrawArraysInstanced");
        GL::GenVertexArrays(1,&vao_); GL::GenFramebuffers(1,&fbo_);glGenTextures(4,textures_);
    }
    ~SandRenderer(){GL::DeleteVertexArrays(1,&vao_);GL::DeleteFramebuffers(1,&fbo_);glDeleteTextures(4,textures_);}
    SandRenderer(const SandRenderer&)=delete;
    SandRenderer& operator=(const SandRenderer&)=delete;
    void Draw(GLuint buffer,size_t idOffset,unsigned count,float radius,const Camera& camera,int width,int height,Parameters p) {
        if(!count||width<=0||height<=0)return;
        State state;
        p=Clamp(p);
        GL::BindFramebuffer(0x8D40,fbo_);
        if(width!=width_||height!=height_){
            for(int i=0;i<4;++i){
                GL::ActiveTexture(0x84C0+12+i);glBindTexture(GL_TEXTURE_2D,textures_[i]);
                glTexImage2D(GL_TEXTURE_2D,0,i==0?0x8CAC:(i==1?0x881A:(i==2?0x8236:0x8814)),width,height,0,i==0?GL_DEPTH_COMPONENT:(i==2?0x8D94:GL_RGBA),i==2?GL_UNSIGNED_INT:GL_FLOAT,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,0x812F);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,0x812F);
                GL::FramebufferTexture2D(0x8D40,i==0?0x8D00:0x8CE0+i-1,GL_TEXTURE_2D,textures_[i],0);
            }
            const GLenum targets[]={0x8CE0,0x8CE1,0x8CE2};drawBuffers_(3,targets);glReadBuffer(0x8CE0);
            if(GL::CheckFramebufferStatus(0x8D40)!=0x8CD5)throw std::runtime_error("Sand render target allocation failed");
            width_=width;height_=height;
        }
        // Avoid read/write feedback during the geometry pass.
        for(int i=0;i<4;++i){GL::ActiveTexture(0x84C0+12+i);glBindTexture(GL_TEXTURE_2D,0);}
        GL::BindVertexArray(vao_);GL::BindBuffer(GL::ArrayBuffer,buffer);
        GL::VertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,4*sizeof(float),nullptr);GL::EnableVertexAttribArray(0);divisor_(0,1);
        integerPointer_(2,1,GL_UNSIGNED_INT,sizeof(unsigned),reinterpret_cast<const void*>(idOffset));GL::EnableVertexAttribArray(2);divisor_(2,1);
        auto view=camera.GetViewMatrix();auto projection=camera.GetProjectionMatrix(float(width)/height);
        auto common=[&](const char* pass){
            shader_.UsePass(pass);shader_.SetMatrix4("view",view);shader_.SetMatrix4("projection",projection);
            shader_.SetMatrix4("inverseProjection",glm::inverse(projection));shader_.SetFloat("radius",radius);
            shader_.SetVector2("resolution",glm::vec2(width,height));
        };
        glViewport(0,0,width,height);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);glDisable(0x8DB9);
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glClearDepth(1);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        common("DEPTH");shader_.SetFloat("microScale",p.microScale);shader_.SetVector2("viewportOrigin",glm::vec2(0));instanced_(GL_TRIANGLE_STRIP,0,4,count);
        GL::BindFramebuffer(0x8CA8,state.readFbo);GL::BindFramebuffer(0x8CA9,state.drawFbo);
        glViewport(state.viewport[0],state.viewport[1],state.viewport[2],state.viewport[3]);State::Enable(GL_SCISSOR_TEST,state.scissor);
        State::Enable(0x8DB9,state.srgb);
        common("SHADE");shader_.SetVector2("viewportOrigin",glm::vec2(state.viewport[0],state.viewport[1]));
        for(int i=0;i<4;++i){GL::ActiveTexture(0x84C0+12+i);glBindTexture(GL_TEXTURE_2D,textures_[i]);}
        shader_.SetInt("grainDepth",12);shader_.SetInt("grainNormals",13);shader_.SetInt("grainIds",14);shader_.SetInt("grainHits",15);
        shader_.SetVector3("sandColor",p.color);shader_.SetFloat("roughness",p.roughness);shader_.SetFloat("sparkleStrength",p.sparkle);
        shader_.SetFloat("mineralFraction",p.mineralFraction);shader_.SetFloat("microScale",p.microScale);shader_.SetFloat("occlusionStrength",p.occlusion);
        float az=glm::radians(p.sunAzimuth),el=glm::radians(p.sunElevation);
        shader_.SetVector3("sunDirection",glm::vec3(std::sin(az)*std::cos(el),std::sin(el),std::cos(az)*std::cos(el)));
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
private:
    struct State {
        GLint readFbo{},drawFbo{},active{},textures[4]{},viewport[4]{},depthFunc{},program{},vao{},buffer{};
        GLboolean depthMask{},depthTest{},scissor{},blend{},cull{},srgb{},colorMask[4]{};GLdouble clearDepth{};GLfloat clearColor[4]{};
        State(){
            glGetIntegerv(0x8CAA,&readFbo);glGetIntegerv(0x8CA6,&drawFbo);glGetIntegerv(0x84E0,&active);
            for(int i=0;i<4;++i){GL::ActiveTexture(0x84C0+12+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&textures[i]);}
            glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_DEPTH_FUNC,&depthFunc);glGetIntegerv(0x8B8D,&program);
            glGetIntegerv(0x85B5,&vao);glGetIntegerv(0x8894,&buffer);glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask);
            glGetBooleanv(GL_COLOR_WRITEMASK,colorMask);glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clearDepth);glGetFloatv(GL_COLOR_CLEAR_VALUE,clearColor);
            depthTest=glIsEnabled(GL_DEPTH_TEST);scissor=glIsEnabled(GL_SCISSOR_TEST);blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);srgb=glIsEnabled(0x8DB9);
        }
        static void Enable(GLenum cap,GLboolean enabled){if(enabled)glEnable(cap);else glDisable(cap);}
        ~State(){
            GL::BindFramebuffer(0x8CA8,readFbo);GL::BindFramebuffer(0x8CA9,drawFbo);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
            glDepthFunc(depthFunc);glDepthMask(depthMask);glClearDepth(clearDepth);glClearColor(clearColor[0],clearColor[1],clearColor[2],clearColor[3]);
            glColorMask(colorMask[0],colorMask[1],colorMask[2],colorMask[3]);Enable(GL_DEPTH_TEST,depthTest);Enable(GL_SCISSOR_TEST,scissor);Enable(GL_BLEND,blend);Enable(GL_CULL_FACE,cull);Enable(0x8DB9,srgb);
            for(int i=0;i<4;++i){GL::ActiveTexture(0x84C0+12+i);glBindTexture(GL_TEXTURE_2D,textures[i]);}GL::ActiveTexture(active);
            GL::BindVertexArray(vao);GL::BindBuffer(GL::ArrayBuffer,buffer);GL::UseProgram(program);
        }
    };
    Shader shader_;GLuint vao_{},fbo_{},textures_[4]{};int width_{},height_{};
    void(APIENTRY* drawBuffers_)(GLsizei,const GLenum*)=nullptr;
    void(APIENTRY* divisor_)(GLuint,GLuint)=nullptr;
    void(APIENTRY* integerPointer_)(GLuint,GLint,GLenum,GLsizei,const void*)=nullptr;
    void(APIENTRY* instanced_)(GLenum,GLint,GLsizei,GLsizei)=nullptr;
};

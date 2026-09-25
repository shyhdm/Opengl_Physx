#pragma once
#include "FlowSimulation.h"
#include "Camera.h"
#include "Shader.h"
#include <array>
#include <cstring>

class FlowGLInterop
{
public:
    explicit FlowGLInterop(FlowContext& flow) : flow_(flow), shader_("Assets/Shaders/flow_present.glsl")
    {
        try
        {
            const char* extensions[] = { "GL_EXT_memory_object", "GL_EXT_memory_object_win32",
                "GL_EXT_semaphore", "GL_EXT_semaphore_win32", "GL_ARB_compute_shader", "GL_ARB_shader_storage_buffer_object" };
            for (auto name : extensions)
                if (!glfwExtensionSupported(name)) throw std::runtime_error(std::string("Required Flow interop extension: ") + name);
            GL::LoadFunction(createMemory_, "glCreateMemoryObjectsEXT");
            GL::LoadFunction(deleteMemory_, "glDeleteMemoryObjectsEXT");
            GL::LoadFunction(importMemory_, "glImportMemoryWin32HandleEXT");
            GL::LoadFunction(bufferStorage_, "glBufferStorageMemEXT");
            GL::LoadFunction(genSemaphores_, "glGenSemaphoresEXT");
            GL::LoadFunction(deleteSemaphores_, "glDeleteSemaphoresEXT");
            GL::LoadFunction(importSemaphore_, "glImportSemaphoreWin32HandleEXT");
            GL::LoadFunction(signal_, "glSignalSemaphoreEXT");
            GL::LoadFunction(wait_, "glWaitSemaphoreEXT");
            void(APIENTRY * getUUID)(GLenum, GLubyte*) = nullptr;
            GL::LoadFunction(getUUID, "glGetUnsignedBytevEXT");
            std::array<GLubyte, 16> uuid{};
            getUUID(0x9597, uuid.data());
            const auto device = flow_.PhysicalDevice();
            if (std::memcmp(uuid.data(), device.deviceUUID, 16) != 0)
                throw std::runtime_error("OpenGL and Flow Vulkan must use the same GPU");
            GL::LoadFunction(dispatch_, "glDispatchCompute");
            GL::LoadFunction(barrier_, "glMemoryBarrier");
            GL::LoadFunction(bindBase_, "glBindBufferBase");
            GL::LoadFunction(texBuffer_, "glTexBuffer");
            BuildPackProgram();
            GL::GenVertexArrays(1, &vao_);
            CheckGL();
        }
        catch (...) { Release(); throw; }
    }
    ~FlowGLInterop() { Release(); }
    FlowGLInterop(const FlowGLInterop&) = delete;
    FlowGLInterop& operator=(const FlowGLInterop&) = delete;

    // Keep shader programs; discard resolution-dependent interop allocations.
    void ReleaseIdleResources() noexcept
    {
        const bool hadResources = width_ || height_ || !spareBuffers_.empty();
        ReleaseSlots();
        for (auto& buffer : spareBuffers_) DestroyBuffer(buffer);
        spareBuffers_.clear();
        if (hadResources) flow_.CollectUnusedResources();
    }

    void Draw(const Camera& camera, int width, int height, FlowSimulation* simulation)
    {
        if (width <= 0 || height <= 0) return;
        State state;
        EnsureSize(width, height);
        auto& slot = slots_[nextSlot_];
        auto* context = flow_.Context();
        auto& api = flow_.Interface();
        const GLuint buffers[] = { slot.color.gl, slot.depth.gl, slot.output.gl };
        GL::ActiveTexture(0x84C0);
        glBindTexture(GL_TEXTURE_2D, slot.captureColor);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        GL::ActiveTexture(0x84C1);
        glBindTexture(GL_TEXTURE_2D, slot.captureDepth);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        GL::UseProgram(packProgram_);
        bindBase_(0x90D2, 0, slot.color.gl); bindBase_(0x90D2, 1, slot.depth.gl);
        dispatch_((width + 7) / 8, (height + 7) / 8, 1);
        barrier_(0x2000);
        CheckGL("GPU scene packing");
        signal_(slot.readyGL, 3, buffers, 0, nullptr, nullptr);
        glFlush();

        auto* color = api.registerTextureAsTransient(context, slot.colorTexture);
        auto* depth = api.registerTextureAsTransient(context, slot.depthTexture);
        CopyToTexture(slot.color, color, 16);
        CopyToTexture(slot.depth, depth, 4);
        NvFlowTextureTransient* result = color;
        if (simulation)
        {
            glm::mat4 clip(1.0f);
            clip[1][1] = -1.0f;
            clip[2][2] = 0.5f; clip[3][2] = 0.5f;
            auto view = camera.GetViewMatrix();
            auto projection = clip * camera.GetProjectionMatrix(float(width) / height);
            NvFlowFloat4x4 flowView{}, flowProjection{};
            static_assert(sizeof(flowView) == sizeof(view));
            std::memcpy(&flowView, &view, sizeof(view));
            std::memcpy(&flowProjection, &projection, sizeof(projection));
            result = simulation->RenderNative(flowView, flowProjection, width, height, depth, color);
        }
        NvFlowPassCopyTextureToBufferParams copy{};
        copy.bufferRowPitch = width * 16; copy.bufferDepthPitch = width * height * 16;
        copy.textureExtent = { static_cast<NvFlowUint>(width), static_cast<NvFlowUint>(height), 1u };
        copy.src = result;
        copy.dst = api.registerBufferAsTransient(context, slot.output.vk);
        api.addPassCopyTextureToBuffer(context, &copy);

        NvFlowUint64 frame = 0;
        if (flow_.Loader().deviceInterface.flush(flow_.DeviceQueue(), &frame, slot.readyVK, slot.doneVK))
            throw std::runtime_error("Flow Vulkan device reset during native rendering");
        wait_(slot.doneGL, 3, buffers, 0, nullptr, nullptr);
        GL::ActiveTexture(0x84C0);
        glBindTexture(0x8C2A, slot.presentTexture);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST); glDisable(0x8DB9);
        glDepthMask(GL_FALSE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, width, height);
        shader_.Use(); shader_.SetInt("nativeColor", 0); shader_.SetInt("imageWidth", width);
        GL::BindVertexArray(vao_); glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
        CheckGL();
        nextSlot_ = (nextSlot_ + 1) % slots_.size();
    }

private:
    static constexpr GLenum PackBuffer = 0x88EB, UnpackBuffer = 0x88EC, Win32Handle = 0x9587;
    struct Buffer { NvFlowBuffer* vk = nullptr; GLuint gl = 0, memory = 0; HANDLE handle = nullptr; size_t capacity = 0; };
    struct Slot
    {
        Buffer color, depth, output;
        NvFlowTexture* colorTexture = nullptr;
        NvFlowTexture* depthTexture = nullptr;
        NvFlowDeviceSemaphore* readyVK = nullptr;
        NvFlowDeviceSemaphore* doneVK = nullptr;
        GLuint readyGL = 0, doneGL = 0, presentTexture = 0, captureColor = 0, captureDepth = 0;
    };
    struct State
    {
        GLint active, texture, texture1, textureBuffer, storage, bases[2], pack, unpack, program, vao, viewport[4];
        void(APIENTRY* getIndexed)(GLenum, GLuint, GLint*) = nullptr;
        void(APIENTRY* bindBase)(GLenum, GLuint, GLuint) = nullptr;
        const std::array<GLenum, 8> pixelNames{ GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT, 0x0D02, 0x0D03, 0x0D04, 0x0CF2, 0x0CF3, 0x0CF4 };
        std::array<GLint, 8> pixel{};
        const std::array<GLenum, 5> caps{ GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, 0x8DB9 };
        std::array<GLboolean, 5> enabled{};
        GLboolean depthMask, colorMask[4];
        State()
        {
            glGetIntegerv(0x84E0, &active); GL::ActiveTexture(0x84C0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGetIntegerv(0x8C2C, &textureBuffer);
            GL::ActiveTexture(0x84C1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture1); GL::ActiveTexture(0x84C0);
            GL::LoadFunction(getIndexed, "glGetIntegeri_v"); GL::LoadFunction(bindBase, "glBindBufferBase");
            glGetIntegerv(0x90D3, &storage);
            getIndexed(0x90D3, 0, &bases[0]); getIndexed(0x90D3, 1, &bases[1]);
            glGetIntegerv(0x88ED, &pack); glGetIntegerv(0x88EF, &unpack);
            glGetIntegerv(0x8B8D, &program); glGetIntegerv(0x85B5, &vao);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask); glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            for (size_t i = 0; i < caps.size(); ++i) enabled[i] = glIsEnabled(caps[i]);
            for (size_t i = 0; i < pixel.size(); ++i) glGetIntegerv(pixelNames[i], &pixel[i]);
        }
        ~State()
        {
            for (size_t i = 0; i < caps.size(); ++i) { if (enabled[i]) glEnable(caps[i]); else glDisable(caps[i]); }
            for (size_t i = 0; i < pixel.size(); ++i) glPixelStorei(pixelNames[i], pixel[i]);
            glDepthMask(depthMask); glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            GL::UseProgram(program); GL::BindVertexArray(vao);
            GL::BindBuffer(PackBuffer, pack); GL::BindBuffer(UnpackBuffer, unpack);
            bindBase(0x90D2, 0, bases[0]); bindBase(0x90D2, 1, bases[1]); GL::BindBuffer(0x90D2, storage);
            GL::ActiveTexture(0x84C1); glBindTexture(GL_TEXTURE_2D, texture1);
            GL::ActiveTexture(0x84C0); glBindTexture(0x8C2A, textureBuffer);
            glBindTexture(GL_TEXTURE_2D, texture); GL::ActiveTexture(active);
        }
    };
    static void CheckGL(const char* stage = "draw")
    {
        auto error = glGetError();
        if (error != GL_NO_ERROR) throw std::runtime_error(std::string("Flow OpenGL interop ") + stage + " error " + std::to_string(error));
    }
    void BuildPackProgram()
    {
        const char* source = R"GLSL(#version 430 core
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0) uniform sampler2D sceneColor;
layout(binding=1) uniform sampler2D sceneDepth;
layout(std430,binding=0) writeonly buffer Color { vec4 color[]; };
layout(std430,binding=1) writeonly buffer Depth { float depth[]; };
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy), size=textureSize(sceneColor,0);
    if(any(greaterThanEqual(p,size))) return;
    int i=p.y*size.x+p.x;
    color[i]=texelFetch(sceneColor,p,0);
    depth[i]=texelFetch(sceneDepth,p,0).r;
}
)GLSL";
        GLuint shader = GL::CreateShader(0x91B9);
        GL::ShaderSource(shader, 1, &source, nullptr); GL::CompileShader(shader);
        GLint ok = 0; GL::GetShaderiv(shader, GL::CompileStatus, &ok);
        if (!ok) { char log[2048]{}; GL::GetShaderInfoLog(shader, 2048, nullptr, log); GL::DeleteShader(shader); throw std::runtime_error(log); }
        packProgram_ = GL::CreateProgram(); GL::AttachShader(packProgram_, shader); GL::LinkProgram(packProgram_);
        GL::DeleteShader(shader); GL::GetProgramiv(packProgram_, GL::LinkStatus, &ok);
        if (!ok) throw std::runtime_error("Flow GPU packing program link failed");
    }
    void MakeBuffer(Buffer& buffer, size_t bytes)
    {
        size_t best = spareBuffers_.size();
        for (size_t i = 0; i < spareBuffers_.size(); ++i)
            if (spareBuffers_[i].capacity >= bytes && (best == spareBuffers_.size() || spareBuffers_[i].capacity < spareBuffers_[best].capacity)) best = i;
        if (best != spareBuffers_.size())
        {
            buffer = spareBuffers_[best]; spareBuffers_.erase(spareBuffers_.begin() + best); return;
        }
        buffer.capacity = 65536;
        while (buffer.capacity < bytes) buffer.capacity *= 2;
        NvFlowBufferDesc desc{};
        desc.usageFlags = eNvFlowBufferUsage_bufferCopySrc | eNvFlowBufferUsage_bufferCopyDst | eNvFlowBufferUsage_rwStructuredBuffer;
        desc.structureStride = 4;
        desc.sizeInBytes = buffer.capacity;
        buffer.vk = flow_.Interface().createBuffer(flow_.Context(), eNvFlowMemoryType_device, &desc);
        if (!buffer.vk) throw std::runtime_error("Flow shared buffer allocation failed");
        HANDLE& handle = buffer.handle; NvFlowUint64 allocationSize = 0;
        auto& device = flow_.Loader().deviceInterface;
        device.getBufferExternalHandle(flow_.Context(), buffer.vk, &handle, sizeof(handle), &allocationSize);
        if (!handle) throw std::runtime_error("Flow buffer export failed");
        createMemory_(1, &buffer.memory); CheckGL("create memory");
        importMemory_(buffer.memory, allocationSize, Win32Handle, handle);
        CheckGL("import memory");
        GL::GenBuffers(1, &buffer.gl); GL::BindBuffer(0x90D2, buffer.gl);
        bufferStorage_(0x90D2, static_cast<std::ptrdiff_t>(buffer.capacity), buffer.memory, 0);
        GL::BindBuffer(PackBuffer, 0);
        CheckGL("buffer storage");
    }
    void MakeSemaphore(NvFlowDeviceSemaphore*& vk, GLuint& gl)
    {
        auto& device = flow_.Loader().deviceInterface;
        vk = device.createSemaphore(flow_.Device());
        if (!vk) throw std::runtime_error("Flow semaphore allocation failed");
        HANDLE handle = nullptr;
        device.getSemaphoreExternalHandle(vk, &handle, sizeof(handle));
        if (!handle) throw std::runtime_error("Flow semaphore export failed");
        genSemaphores_(1, &gl); importSemaphore_(gl, Win32Handle, handle);
        device.closeSemaphoreExternalHandle(vk, &handle, sizeof(handle));
        CheckGL();
    }
    void EnsureSize(int width, int height)
    {
        if (width_ == width && height_ == height) return;
        ReleaseIdleResources();
        width_ = width; height_ = height;
        GL::BindBuffer(UnpackBuffer, 0);
        for (auto& slot : slots_)
        {
            MakeBuffer(slot.color, size_t(width) * height * 16);
            MakeBuffer(slot.depth, size_t(width) * height * 4);
            MakeBuffer(slot.output, size_t(width) * height * 16);
            MakeSemaphore(slot.readyVK, slot.readyGL); MakeSemaphore(slot.doneVK, slot.doneGL);
            NvFlowTextureDesc desc{};
            desc.textureType = eNvFlowTextureType_2d;
            desc.usageFlags = eNvFlowTextureUsage_texture | eNvFlowTextureUsage_rwTexture |
                eNvFlowTextureUsage_textureCopySrc | eNvFlowTextureUsage_textureCopyDst;
            desc.width = width; desc.height = height; desc.depth = 1; desc.mipLevels = 1;
            desc.format = eNvFlowFormat_r32g32b32a32_float;
            slot.colorTexture = flow_.Interface().createTexture(flow_.Context(), &desc);
            desc.format = eNvFlowFormat_r32_float;
            slot.depthTexture = flow_.Interface().createTexture(flow_.Context(), &desc);
            if (!slot.colorTexture || !slot.depthTexture) throw std::runtime_error("Flow interop texture allocation failed");
            glGenTextures(1, &slot.presentTexture); glBindTexture(0x8C2A, slot.presentTexture);
            texBuffer_(0x8C2A, 0x8814, slot.output.gl);
            glGenTextures(1, &slot.captureColor); glBindTexture(GL_TEXTURE_2D, slot.captureColor);
            glTexImage2D(GL_TEXTURE_2D, 0, 0x8814, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenTextures(1, &slot.captureDepth); glBindTexture(GL_TEXTURE_2D, slot.captureDepth);
            glTexImage2D(GL_TEXTURE_2D, 0, 0x8CAC, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        CheckGL();
    }
    void CopyToTexture(Buffer& buffer, NvFlowTextureTransient* texture, unsigned pixelSize)
    {
        NvFlowPassCopyBufferToTextureParams copy{};
        copy.bufferRowPitch = width_ * pixelSize; copy.bufferDepthPitch = width_ * height_ * pixelSize;
        copy.textureExtent = { static_cast<NvFlowUint>(width_), static_cast<NvFlowUint>(height_), 1 };
        copy.src = flow_.Interface().registerBufferAsTransient(flow_.Context(), buffer.vk);
        copy.dst = texture;
        flow_.Interface().addPassCopyBufferToTexture(flow_.Context(), &copy);
    }
    void ReleaseSlots() noexcept
    {
        if (!width_ && !height_) return;
        glFinish();
        flow_.Loader().deviceInterface.waitIdle(flow_.DeviceQueue());
        for (auto& slot : slots_)
        {
            if (slot.readyGL) deleteSemaphores_(1, &slot.readyGL);
            if (slot.doneGL) deleteSemaphores_(1, &slot.doneGL);
            if (slot.readyVK) flow_.Loader().deviceInterface.destroySemaphore(slot.readyVK);
            if (slot.doneVK) flow_.Loader().deviceInterface.destroySemaphore(slot.doneVK);
            if (slot.colorTexture) flow_.Interface().destroyTexture(flow_.Context(), slot.colorTexture);
            if (slot.depthTexture) flow_.Interface().destroyTexture(flow_.Context(), slot.depthTexture);
            if (slot.presentTexture) glDeleteTextures(1, &slot.presentTexture);
            if (slot.captureColor) glDeleteTextures(1, &slot.captureColor);
            if (slot.captureDepth) glDeleteTextures(1, &slot.captureDepth);
        }
        for (auto& slot : slots_)
        {
            for (auto* buffer : { &slot.color, &slot.depth, &slot.output })
                if (buffer->vk || buffer->gl || buffer->memory) spareBuffers_.push_back(*buffer);
            slot = {};
        }
        width_ = height_ = 0; nextSlot_ = 0;
    }
    void Release() noexcept
    {
        ReleaseIdleResources();
        if (packProgram_) GL::DeleteProgram(packProgram_); packProgram_ = 0;
        if (vao_) GL::DeleteVertexArrays(1, &vao_); vao_ = 0;
    }
    void DestroyBuffer(Buffer& buffer) noexcept
    {
        if (buffer.gl) GL::DeleteBuffers(1, &buffer.gl);
        if (buffer.memory) deleteMemory_(1, &buffer.memory);
        if (buffer.handle) flow_.Loader().deviceInterface.closeBufferExternalHandle(flow_.Context(), buffer.vk, &buffer.handle, sizeof(buffer.handle));
        if (buffer.vk) flow_.Interface().destroyBuffer(flow_.Context(), buffer.vk);
        buffer = {};
    }
    std::vector<Buffer> spareBuffers_;
    FlowContext& flow_;
    Shader shader_;
    std::array<Slot, 2> slots_{};
    int width_ = 0, height_ = 0;
    size_t nextSlot_ = 0;
    GLuint vao_ = 0, packProgram_ = 0;
    void(APIENTRY* dispatch_)(GLuint, GLuint, GLuint) = nullptr;
    void(APIENTRY* barrier_)(GLbitfield) = nullptr;
    void(APIENTRY* bindBase_)(GLenum, GLuint, GLuint) = nullptr;
    void(APIENTRY* texBuffer_)(GLenum, GLenum, GLuint) = nullptr;
    void(APIENTRY* createMemory_)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteMemory_)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* importMemory_)(GLuint, unsigned long long, GLenum, void*) = nullptr;
    void(APIENTRY* bufferStorage_)(GLenum, std::ptrdiff_t, GLuint, unsigned long long) = nullptr;
    void(APIENTRY* genSemaphores_)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* deleteSemaphores_)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* importSemaphore_)(GLuint, GLenum, void*) = nullptr;
    void(APIENTRY* signal_)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
    void(APIENTRY* wait_)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = nullptr;
};

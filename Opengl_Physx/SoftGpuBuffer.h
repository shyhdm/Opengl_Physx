#pragma once
#include "Mesh.h"
#include "PhysicsWorld.h"
#include <cudamanager/PxCudaContext.h>
#include <array>

class SoftGpuBuffer
{
public:
    SoftGpuBuffer(physx::PxCudaContextManager& context, Mesh& mesh, const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices)
        : cuda(context), bytes(vertices.size() * sizeof(physx::PxVec4))
    {
        try
        {
            driver = LoadLibraryW(L"nvcuda.dll");
            if (!driver) throw std::runtime_error("Cannot load NVIDIA CUDA driver.");
            reg = Load<Register>("cuGraphicsGLRegisterBuffer");
            unregister = Load<Unregister>("cuGraphicsUnregisterResource");
            map = Load<Map>("cuGraphicsMapResources");
            unmap = Load<Map>("cuGraphicsUnmapResources");
            pointer = Load<Pointer>("cuGraphicsResourceGetMappedPointer_v2");
            texBuffer = reinterpret_cast<TexBuffer>(glfwGetProcAddress("glTexBuffer"));
            if (!texBuffer) throw std::runtime_error("OpenGL texture buffers unavailable.");
            std::vector<physx::PxVec4> positions;
            for (auto& v : vertices) positions.emplace_back(v.x, v.y, v.z, 0.0f);
            Create(0, positions.data(), bytes, 0x8814);
            std::vector<std::vector<std::array<unsigned int, 3>>> faces(vertices.size());
            for (std::size_t i = 0; i < indices.size(); i += 3)
                for (int j = 0; j < 3; ++j) faces[indices[i + j]].push_back({ indices[i],indices[i + 1],indices[i + 2] });
            std::vector<unsigned int> ranges, adjacency;
            for (auto& list : faces)
            {
                ranges.push_back(static_cast<unsigned int>(adjacency.size()));
                ranges.push_back(static_cast<unsigned int>(list.size()));
                for (auto& f : list) adjacency.insert(adjacency.end(), f.begin(), f.end());
            }
            Create(1, ranges.data(), ranges.size() * sizeof(unsigned int), 0x823C);
            Create(2, adjacency.data(), adjacency.size() * sizeof(unsigned int), 0x8236);
            {
                physx::PxScopedCudaLock lock(cuda);
                Check(reg(&resource, buffers[0], 2), "Cannot share OpenGL buffer with PhysX CUDA device.");
            }
            mesh.SetGpuPositions(buffers[0], textures);
        }
        catch (...) { Release(); throw; }
    }

    ~SoftGpuBuffer() { Release(); }
    SoftGpuBuffer(const SoftGpuBuffer&) = delete;
    SoftGpuBuffer& operator=(const SoftGpuBuffer&) = delete;

    struct UpdateRequest
    {
        SoftGpuBuffer* buffer;
        physx::PxVec4* source;
    };
    struct Region { physx::PxVec4* source; std::size_t destinationOffset; std::size_t bytes; };

    void Update(physx::PxVec4* source) { UpdateBatch({ {this,source} }); }

    static void UpdateBatch(const std::vector<UpdateRequest>& requests)
    {
        if (requests.empty()) return;
        auto& first = *requests.front().buffer;
        physx::PxScopedCudaLock lock(first.cuda);
        std::vector<void*> resources;
        for (auto& request : requests)
        {
            if (&request.buffer->cuda != &first.cuda) throw std::invalid_argument("Soft buffers must share a CUDA context.");
            resources.push_back(request.buffer->resource);
        }
        auto count = static_cast<unsigned int>(resources.size());
        Check(first.map(count, resources.data(), nullptr), "Cannot map soft-body graphics buffers.");
        try
        {
            for (auto& request : requests)
            {
                auto& buffer = *request.buffer;
                CUdeviceptr destination = 0; std::size_t size = 0;
                Check(first.pointer(&destination, &size, buffer.resource), "Cannot access soft-body graphics buffer.");
                if (size < buffer.bytes) throw std::runtime_error("Soft-body graphics buffer is too small.");
                Check(first.cuda.getCudaContext()->memcpyDtoDAsync(destination, reinterpret_cast<CUdeviceptr>(request.source), buffer.bytes, nullptr),
                    "Cannot copy soft-body vertices on GPU.");
            }
        }
        catch (...) { first.unmap(count, resources.data(), nullptr); throw; }
        Check(first.unmap(count, resources.data(), nullptr), "Cannot release soft-body graphics buffers.");
    }

    void UpdateRegions(const std::vector<Region>& regions)
    {
        if (regions.empty()) return;
        physx::PxScopedCudaLock lock(cuda);
        Check(map(1, &resource, nullptr), "Cannot map combined soft-body graphics buffer.");
        try
        {
            CUdeviceptr destination = 0; std::size_t size = 0;
            Check(pointer(&destination, &size, resource), "Cannot access combined soft-body graphics buffer.");
            for (const Region& region : regions)
            {
                if (!region.source || region.destinationOffset > size || region.bytes > size - region.destinationOffset) throw std::runtime_error("Combined soft-body graphics buffer is too small.");
                Check(cuda.getCudaContext()->memcpyDtoDAsync(destination + region.destinationOffset, reinterpret_cast<CUdeviceptr>(region.source), region.bytes, nullptr),
                    "Cannot copy soft-body region on GPU.");
            }
        }
        catch (...) { unmap(1, &resource, nullptr); throw; }
        Check(unmap(1, &resource, nullptr), "Cannot release combined soft-body graphics buffer.");
    }

private:
    using Register = int(WINAPI*)(void**, unsigned int, unsigned int);
    using Unregister = int(WINAPI*)(void*);
    using Map = int(WINAPI*)(unsigned int, void**, CUstream);
    using Pointer = int(WINAPI*)(CUdeviceptr*, std::size_t*, void*);
    using TexBuffer = void(APIENTRY*)(GLenum, GLenum, GLuint);
    physx::PxCudaContextManager& cuda;
    std::size_t bytes = 0;
    HMODULE driver = nullptr;
    void* resource = nullptr;
    Register reg = nullptr;
    Unregister unregister = nullptr;
    Map map = nullptr, unmap = nullptr;
    Pointer pointer = nullptr;
    TexBuffer texBuffer = nullptr;
    GLuint buffers[3] = {}, textures[3] = {};

    template<class T> T Load(const char* name)
    {
        auto function = reinterpret_cast<T>(GetProcAddress(driver, name));
        if (!function) throw std::runtime_error("CUDA graphics interop function unavailable.");
        return function;
    }

    static void Check(int result, const char* message)
    {
        if (result != 0) throw std::runtime_error(message);
    }

    void Create(int i, const void* data, std::size_t size, GLenum format)
    {
        GL::GenBuffers(1, &buffers[i]);
        GL::BindBuffer(GL::ArrayBuffer, buffers[i]);
        GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(size), data, GL::StaticDraw);
        glGenTextures(1, &textures[i]);
        glBindTexture(0x8C2A, textures[i]);
        texBuffer(0x8C2A, format, buffers[i]);
        glBindTexture(0x8C2A, 0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }

    void Release()
    {
        if (resource)
        {
            physx::PxScopedCudaLock lock(cuda);
            unregister(resource); resource = nullptr;
        }
        glDeleteTextures(3, textures);
        GL::DeleteBuffers(3, buffers);
        if (driver) { FreeLibrary(driver); driver = nullptr; }
    }
};

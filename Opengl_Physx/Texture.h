#pragma once
#include "OpenGL.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// 使用 Windows 自带 WIC 解码 PNG/JPG，不需要另装图片库。
// 必须在 GL::Load() 后创建，并在窗口之前销毁。
class Texture
{
public:
    explicit Texture(const std::filesystem::path& file)
    {
        auto path = Resolve(file);
        auto nameBytes = path.u8string();
        std::string name(nameBytes.begin(), nameBytes.end());
        try
        {
            Apartment apartment;
            using Microsoft::WRL::ComPtr;
            ComPtr<IWICImagingFactory> factory;
            Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())), "Cannot create image decoder.");
            ComPtr<IWICBitmapDecoder> decoder;
            Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "Cannot open image.");
            ComPtr<IWICBitmapFrameDecode> frame;
            Check(decoder->GetFrame(0, frame.GetAddressOf()), "Cannot read image frame.");
            UINT w = 0, h = 0;
            Check(frame->GetSize(&w, &h), "Cannot read image size.");
            GLint maximum = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
            if (!w || !h || w > static_cast<UINT>(maximum) || h > static_cast<UINT>(maximum) || static_cast<std::uint64_t>(w) * h * 4 > 256ull * 1024 * 1024)
                throw std::runtime_error("Texture dimensions are unsupported or image exceeds 256 MB decoded.");
            ComPtr<IWICFormatConverter> converter;
            Check(factory->CreateFormatConverter(converter.GetAddressOf()), "Cannot create pixel converter.");
            Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Cannot convert image to RGBA.");
            UINT stride = w * 4;
            std::vector<unsigned char> pixels(static_cast<std::size_t>(stride) * h);
            Check(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()), "Cannot read image pixels.");
            // WIC 从上向下存行；翻转后，UV 的 v=0 对应图片底部。
            for (UINT y = 0; y < h / 2; ++y)
            {
                auto top = pixels.begin() + static_cast<std::size_t>(y) * stride;
                auto bottom = pixels.begin() + static_cast<std::size_t>(h - 1 - y) * stride;
                std::swap_ranges(top, top + stride, bottom);
            }
            width = static_cast<int>(w); height = static_cast<int>(h);
            Upload(pixels);
        }
        catch (const std::exception& e) { throw std::runtime_error(name + "\n" + e.what()); }
    }

    ~Texture() { if (id) glDeleteTextures(1, &id); }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void Bind() const
    {
        // 本阶段底色贴图固定使用纹理单元0。
        GL::ActiveTexture(0x84C0);
        glBindTexture(GL_TEXTURE_2D, id);
    }

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    GLuint id = 0;
    int width = 0, height = 0;

    struct Apartment
    {
        HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Apartment() { if (FAILED(status) && status != RPC_E_CHANGED_MODE) throw std::runtime_error("Cannot initialize image services."); }
        ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    };

    static void Check(HRESULT status, const char* message)
    {
        if (FAILED(status)) throw std::runtime_error(std::string(message) + " HRESULT=" + std::to_string(static_cast<unsigned long>(status)));
    }

    static std::filesystem::path Resolve(const std::filesystem::path& file)
    {
        if (file.is_absolute()) return file;
        std::vector<wchar_t> path(32768);
        DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) throw std::runtime_error("Cannot locate executable directory.");
        return std::filesystem::path(std::wstring(path.data(), size)).parent_path() / file;
    }

    void Upload(const std::vector<unsigned char>& pixels)
    {
        GLint previous = 0, alignment = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGenTextures(1, &id);
        if (!id) throw std::runtime_error("Cannot allocate texture.");
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        GL::GenerateMipmap(GL_TEXTURE_2D);
        GLenum error = glGetError();
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
        if (error != GL_NO_ERROR)
        {
            glDeleteTextures(1, &id); id = 0;
            throw std::runtime_error("Texture upload failed. OpenGL error=" + std::to_string(error));
        }
    }
};


#pragma once
#include "Window.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/backends/imgui_impl_glfw.h"
#include "ThirdParty/imgui/backends/imgui_impl_opengl3.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>

class ImGuiLayer
{
public:
    explicit ImGuiLayer(Window& window) : window(window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        try
        {
            auto& io = ImGui::GetIO();

            io.IniFilename = nullptr;
            ImGui::StyleColorsDark();
            auto& style = ImGui::GetStyle();
            style.WindowRounding = 8; style.FrameRounding = 4;
            style.WindowPadding = ImVec2(12, 10);
            style.ItemSpacing = ImVec2(8, 7);
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.09f, 0.12f, 0.98f);
            style.Colors[ImGuiCol_Header] = ImVec4(0.13f, 0.23f, 0.30f, 1);
            style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.30f, 0.40f, 1);
            style.FontSizeBase = 16;
            baseStyle = style;
            LoadFont();
            if (!ImGui_ImplGlfw_InitForOpenGL(window.GetGlfwHandle(), true))
                throw std::runtime_error("Cannot initialize ImGui GLFW backend.");
            platformReady = true;
            if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
                throw std::runtime_error("Cannot initialize ImGui OpenGL backend.");
            rendererReady = true;
        }
        catch (...) { Shutdown(); throw; }
    }
    ~ImGuiLayer() { Shutdown(); }
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;
    bool HasChineseFont() const { return chineseFont; }

    void BeginFrame()
    {
        float x = 1, y = 1;
        glfwGetWindowContentScale(window.GetGlfwHandle(), &x, &y);
        float scale = std::clamp(x, 0.75f, 3.0f);
        if (scale != lastScale)
        {
            ImGui::GetStyle() = baseStyle;
            ImGui::GetStyle().ScaleAllSizes(scale);
            ImGui::GetStyle().FontScaleDpi = scale;
            lastScale = scale;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }
    bool CapturesMouse() const { return ImGui::GetIO().WantCaptureMouse; }
    bool CapturesKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
    void Render()
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
private:
    Window& window;
    ImGuiStyle baseStyle;
    float lastScale = 0;
    bool platformReady = false, rendererReady = false, chineseFont = false;
    std::vector<char> fontData;

    void LoadFont()
    {
        wchar_t folder[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(folder, MAX_PATH)) return;
        for (const wchar_t* name : { L"msyh.ttc",L"simhei.ttf" })
        {
            std::filesystem::path path = std::filesystem::path(folder) / L"Fonts" / name;
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) continue;
            auto size = file.tellg();
            if (size <= 0 || size > 64 * 1024 * 1024) continue;
            fontData.resize(static_cast<std::size_t>(size));
            file.seekg(0);
            if (!file.read(fontData.data(), static_cast<std::streamsize>(fontData.size()))) continue;
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            chineseFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(fontData.data(), static_cast<int>(fontData.size()), 16, &config) != nullptr;
            if (chineseFont) return;
        }
    }
    void Shutdown()
    {
        if (rendererReady) ImGui_ImplOpenGL3_Shutdown();
        if (platformReady) ImGui_ImplGlfw_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        rendererReady = platformReady = false;
    }
};





#pragma once

struct GLFWwindow;

// This first version owns one GLFW window and the GLFW lifetime.
// Create and use it on the main thread.
class Window
{
public:
    Window(int width, int height, const char* title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    GLFWwindow* GetGlfwHandle() const { return handle; }
    void RequestClose();
    bool ShouldClose() const;
    bool BeginFrame();
    void Present();
    void GetFramebufferSize(int& width, int& height) const;
    void GetSize(int& width, int& height) const;
    bool IsFocused() const;
    unsigned int GetFocusRevision() const { return focusRevision; }
    bool IsKeyDown(int key) const;
    bool IsMouseButtonDown(int button) const;
    void GetCursorPosition(double& x, double& y) const;
    void SetCursorCaptured(bool captured);

private:
    GLFWwindow* handle = nullptr;
    bool firstFrame = true;
    unsigned int focusRevision = 0;
};

#include "OpenGL.h"
#include <stdexcept>
#include <string>

namespace WindowDetail
{
    inline std::runtime_error GlfwFailure(const char* fallback)
    {
        const char* description = nullptr;
        glfwGetError(&description);
        return std::runtime_error(description ? description : fallback);
    }

    inline void CenterWindow(GLFWwindow* handle, HMONITOR monitor)
    {
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfo(monitor, &info))
        {
            return;
        }

        // Move onto the target monitor before querying decorations.
        glfwSetWindowPos(handle, info.rcWork.left + 32, info.rcWork.top + 32);

        int width = 0, height = 0;
        glfwGetWindowSize(handle, &width, &height);
        int left = 0, top = 0, right = 0, bottom = 0;
        glfwGetWindowFrameSize(handle, &left, &top, &right, &bottom);

        int screenWidth = info.rcWork.right - info.rcWork.left;
        int screenHeight = info.rcWork.bottom - info.rcWork.top;
        int maxWidth = screenWidth - left - right;
        int maxHeight = screenHeight - top - bottom;
        if (maxWidth <= 0 || maxHeight <= 0)
        {
            return;
        }

        if (width > maxWidth)
        {
            width = maxWidth;
        }
        if (height > maxHeight)
        {
            height = maxHeight;
        }

        glfwSetWindowSize(handle, width, height);
        int x = info.rcWork.left + (screenWidth - width - left - right) / 2 + left;
        int y = info.rcWork.top + (screenHeight - height - top - bottom) / 2 + top;
        glfwSetWindowPos(handle, x, y);
    }
}

inline Window::Window(int width, int height, const char* title)
{
    if (!glfwInit())
    {
        throw WindowDetail::GlfwFailure("Cannot initialize GLFW.");
    }

    POINT mouse = {};
    GetCursorPos(&mouse);
    HMONITOR monitor = MonitorFromPoint(mouse, MONITOR_DEFAULTTONEAREST);

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!handle)
    {
        std::runtime_error error = WindowDetail::GlfwFailure("Cannot create window.");
        glfwTerminate();
        throw error;
    }

    glfwSetWindowUserPointer(handle, this);
    glfwSetWindowFocusCallback(handle, [](GLFWwindow* nativeWindow, int)
        {
            auto* window = static_cast<Window*>(glfwGetWindowUserPointer(nativeWindow));
            ++window->focusRevision;
        });
    WindowDetail::CenterWindow(handle, monitor);
    glfwMakeContextCurrent(handle);
    glfwSwapInterval(0);
}

inline Window::~Window()
{
    glfwDestroyWindow(handle);
    glfwTerminate();
}

inline bool Window::ShouldClose() const
{
    return glfwWindowShouldClose(handle) != 0;
}

inline bool Window::BeginFrame()
{
    glfwPollEvents();

    if (ShouldClose())
    {
        return false;
    }
    if (glfwGetWindowAttrib(handle, GLFW_ICONIFIED))
    {
        glfwWaitEvents();
        return false;
    }
    return true;
}

inline void Window::Present()
{
    glfwSwapBuffers(handle);
    if (firstFrame)
    {
        glfwShowWindow(handle);
        firstFrame = false;
    }
}

inline void Window::GetFramebufferSize(int& width, int& height) const
{
    glfwGetFramebufferSize(handle, &width, &height);
}

inline bool Window::IsFocused() const
{
    return glfwGetWindowAttrib(handle, GLFW_FOCUSED) != 0;
}

inline bool Window::IsKeyDown(int key) const
{
    return IsFocused() && glfwGetKey(handle, key) == GLFW_PRESS;
}

inline bool Window::IsMouseButtonDown(int button) const
{
    return IsFocused() && glfwGetMouseButton(handle, button) == GLFW_PRESS;
}

inline void Window::GetCursorPosition(double& x, double& y) const
{
    glfwGetCursorPos(handle, &x, &y);
}

inline void Window::SetCursorCaptured(bool captured)
{
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(handle, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    glfwSetInputMode(handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

inline void Window::GetSize(int& width, int& height) const { glfwGetWindowSize(handle, &width, &height); }

inline void Window::RequestClose() { glfwSetWindowShouldClose(handle, GLFW_TRUE); }



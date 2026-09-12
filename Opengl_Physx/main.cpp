#include "Window.h"
#include "OpenGL.h"
#include "Scene.h"
#include "ImGuiLayer.h"
#include "ImGuiPanel.h"
#include <exception>

int main()
{
    try
    {
        Window window(1280, 720, "OpenGL");
        GL::Load();
        Camera camera;
        camera.position = glm::vec3(7.0f, 5.0f, 10.0f);
        camera.yaw = -125.0f;
        camera.pitch = -18.0f;
        auto scene = std::make_unique<Scene>();
        ImGuiLayer gui(window);
        ImGuiPanel panel(gui.HasChineseFont());
        double lastTime = glfwGetTime();

        while (!window.ShouldClose())
        {
            bool ready = window.BeginFrame();
            double currentTime = glfwGetTime();
            float deltaTime = static_cast<float>(currentTime - lastTime);
            lastTime = currentTime;
            if (!ready) continue;

            gui.BeginFrame();
            panel.Draw(*scene);
            bool mouseBlocked = gui.CapturesMouse();
            camera.Update(window, deltaTime, mouseBlocked);
            scene->HandleInput(window, camera, mouseBlocked);
            if (window.IsKeyDown(GLFW_KEY_ESCAPE)) window.RequestClose();
            scene->Update(deltaTime);

            int width = 0, height = 0;
            window.GetFramebufferSize(width, height);
            scene->Draw(camera, width, height);
            gui.Render();
            window.Present();
            if (scene->GetRequestedMode() >= 0)
            {
                bool useGpu = scene->GetRequestedMode() == 1;
                scene.reset();
                scene = std::make_unique<Scene>(useGpu);
                panel.ResetFps();
                lastTime = glfwGetTime();
            }
        }
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "OpenGL + PhysX Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}

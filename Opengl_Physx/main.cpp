#include "Window.h"
#include "OpenGL.h"
#include "Scene.h"
#include "ImGuiLayer.h"
#include "ImGuiPanel.h"
#include "BlastContext.h"
#include "BlastLibrary.h"
#include "BlastScene.h"
#include "BlastChunkRenderer.h"
#include "DebugOverlay.h"
#include <exception>
#include <memory>
#include <chrono>

int main()
{
    try
    {
        BlastContext blast;
        Window window(1280, 720, "OpenGL + PhysX + Blast");
        GL::Load();
        Camera camera;
        camera.position = glm::vec3(7.0f, 5.0f, 10.0f);
        camera.yaw = -125.0f;
        camera.pitch = -18.0f;
        auto scene = std::make_unique<Scene>();
        auto blastLibrary = std::make_unique<BlastLibrary>();
        auto blastScene = std::make_unique<BlastScene>(blast, *blastLibrary, scene->GetPhysicsWorld());
        auto blastRenderer = std::make_unique<BlastChunkRenderer>(*blastLibrary);
        auto connectBlastSelection = [&]()
            {
                scene->SetExternalRigidHandlers(
                    [&](const physx::PxRigidActor* actor, ModelType& type, bool*& mesh, std::uint64_t& id) {return blastScene->Resolve(actor, type, mesh, id); },
                    [&](std::vector<const physx::PxRigidActor*>& actors, bool all) {blastScene->AppendCollisionActors(actors, all); },
                    [&](OutlineEffect& outline, const Camera& view, int width, int height, const physx::PxRigidActor* actor) {blastRenderer->DrawOutline(outline, *blastScene, view, width, height, actor); blastScene->DrawRuntimeWallOutline(outline, view, width, height, actor); },
                    [&](bool value) {blastScene->SetShowCollisions(value); });
            };
        connectBlastSelection();
        blastScene->SetSceneIndex(scene->GetSceneIndex());
        ImGuiLayer gui(window);
        ImGuiPanel panel(gui.HasChineseFont());
        DebugOverlay debugOverlay;
        double lastTime = glfwGetTime();
        bool previousF1Key = false;
        bool showGui = true;
        float displayedFps = 0.0f;
        double cpuFrameMs = 0.0;
        double nextStatsRefresh = 0.0;

        while (!window.ShouldClose())
        {
            bool ready = window.BeginFrame();
            double currentTime = glfwGetTime();
            float deltaTime = static_cast<float>(currentTime - lastTime);
            lastTime = currentTime;
            if (!ready) continue;
            auto cpuFrameStarted = std::chrono::steady_clock::now();
            gui.BeginFrame();
            bool refreshStats = currentTime >= nextStatsRefresh;
            if (refreshStats)
            {
                displayedFps = ImGui::GetIO().Framerate;
                debugOverlay.Update(*scene, blastScene.get(), displayedFps, cpuFrameMs);
                nextStatsRefresh = currentTime + 0.5;
            }
            bool f1Key = window.IsKeyDown(GLFW_KEY_F1);
            if (f1Key && !previousF1Key) showGui = !showGui;
            previousF1Key = f1Key;
            bool mouseBlocked = showGui && gui.CapturesMouse();
            camera.Update(window, deltaTime, mouseBlocked);
            scene->HandleInput(window, camera, mouseBlocked, [&](ModelType type, glm::vec3 position, glm::vec3 velocity, float scale, float mass)
                {
                    blastScene->Spawn(type, position, velocity, scale, mass);
                });
            if (showGui) panel.Draw(*scene, displayedFps, [&]() {scene->ClearRigidSelection(); blastScene->Clear(); }, [&](ModelType type, glm::vec3 position, float scale, float mass) {blastScene->Spawn(type, position, glm::vec3(0), scale, mass); }, [&]() {blastScene->BuildWall(); }, blastScene.get(), debugOverlay.GetText());
            blastScene->SetSceneIndex(scene->GetSceneIndex());
            if (window.IsKeyDown(GLFW_KEY_ESCAPE)) window.RequestClose();
            blastScene->BeforePhysics();
            scene->Update(deltaTime);
            blastScene->AfterPhysics([&](const physx::PxRigidActor* actor) {scene->ForgetActor(actor); });
            int width = 0, height = 0;
            window.GetFramebufferSize(width, height);
            scene->Draw(camera, width, height, [&](ModelRenderer& renderer, bool shadowPass)
                {
                    blastRenderer->Draw(renderer, *blastScene, shadowPass);
                    blastScene->DrawRuntimeWall(renderer, shadowPass);
                });
            debugOverlay.Draw();
            gui.Render();
            cpuFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuFrameStarted).count();
            window.Present();
            if (scene->GetRequestedMode() >= 0)
            {
                int activeScene = scene->GetSceneIndex();
                bool useGpu = scene->GetRequestedMode() == 1;
                scene->ClearRigidSelection();
                blastScene.reset();
                scene.reset();
                scene = std::make_unique<Scene>(useGpu);
                scene->SetSceneIndex(activeScene);
                blastScene = std::make_unique<BlastScene>(blast, *blastLibrary, scene->GetPhysicsWorld());
                connectBlastSelection();
                blastScene->SetSceneIndex(activeScene);
                lastTime = glfwGetTime();
            }
        }
        scene->ClearRigidSelection();
        blastScene->Clear();
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(error.what());
        return 1;
    }
    return 0;
}

#pragma once
#include "Scene.h"
#include "BlastScene.h"
#include "ThirdParty/imgui/imgui.h"
#include <cstdio>

class DebugOverlay
{
public:
    void Update(const Scene& scene, const BlastScene* blastScene, float fps, double cpuFrameMs)
    {
        uint32_t blastActors = blastScene ? blastScene->GetActorCount() : 0;
        uint32_t chunks = blastScene ? blastScene->GetVisibleChunkCount() : 0;
        uint32_t bonds = blastScene ? blastScene->GetBondCount() : 0;
        double blastMs = blastScene ? blastScene->GetUpdateMs() : 0.0;
        const auto* liquid = scene.GetLiquid();
        char renderMs[32] = "N/A";
        if (liquid && liquid->HasRenderTiming()) std::snprintf(renderMs, sizeof(renderMs), "%.2f ms", liquid->GetRenderGpuMs());
        std::snprintf(waterText, sizeof(waterText), "Particles %u  Water draw GPU %s  Water physics (scene) %.2f ms/step", liquid ? liquid->Count() : 0u, renderMs, scene.GetLiquidPhysicsStepMs());
        std::snprintf(text, sizeof(text), "Scene %d  FPS %.0f  Rigid %zu  Soft %zu  Blast actors %u  Chunks %u  Bonds %u  Steps %u  PhysX %.2f ms  Blast %.2f ms  Soft sync %.2f ms  Upload %.2f ms  Shadow %.2f ms  Main %.2f ms  Draw %.2f ms  CPU frame %.2f ms  %s  F1 UI", scene.GetSceneIndex() + 1, fps, scene.GetBodyCount() + static_cast<size_t>(blastActors), scene.GetSoftBodyCount(), blastActors, chunks, bonds, scene.GetPhysicsSteps(), scene.GetSimulationMs(), blastMs, scene.GetSoftSyncMs(), scene.GetRenderUploadMs(), scene.GetShadowDrawMs(), scene.GetMainDrawMs(), scene.GetDrawSubmitMs(), cpuFrameMs, scene.UsesGpu() ? "GPU" : "CPU");
        std::snprintf(copyText, sizeof(copyText), "%s\n%s", text, waterText);
    }

    void Draw() const
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float scale = ImGui::GetStyle().FontScaleDpi;
        ImVec2 position(viewport->WorkPos.x + 8.0f * scale, viewport->WorkPos.y + 6.0f * scale);
        ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), 12.0f * scale, position, IM_COL32(255, 255, 255, 255), text);
        position.y += 14.0f * scale;
        ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), 12.0f * scale, position, IM_COL32(255, 255, 255, 255), waterText);
    }

    const char* GetText() const { return copyText; }
private:
    char text[512]{};
    char waterText[256]{};
    char copyText[768]{};
};

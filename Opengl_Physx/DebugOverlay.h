#pragma once
#include "Scene.h"
#include "BlastScene.h"
#include "ThirdParty/imgui/imgui.h"
#include <cstdio>

class DebugOverlay
{
public:
    void Draw(const Scene& scene, const BlastScene* blastScene, float fps, bool refresh)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float scale = ImGui::GetStyle().FontScaleDpi;
        if (refresh)
        {
            uint32_t blastActors = blastScene ? blastScene->GetActorCount() : 0;
            uint32_t chunks = blastScene ? blastScene->GetVisibleChunkCount() : 0;
            uint32_t bonds = blastScene ? blastScene->GetBondCount() : 0;
            std::snprintf(text, sizeof(text), "Scene %d  FPS %.0f  Rigid %zu  Soft %zu  Blast actors %u  Chunks %u  Bonds %u  Steps %u  PhysX %.2f ms  Soft sync %.2f ms  Draw %.2f ms  %s  F1 UI", scene.GetSceneIndex() + 1, fps, scene.GetBodyCount() + static_cast<size_t>(blastActors), scene.GetSoftBodyCount(), blastActors, chunks, bonds, scene.GetPhysicsSteps(), scene.GetSimulationMs(), scene.GetSoftSyncMs(), scene.GetDrawSubmitMs(), scene.UsesGpu() ? "GPU" : "CPU");
        }
        ImVec2 position(viewport->WorkPos.x + 8.0f * scale, viewport->WorkPos.y + 6.0f * scale);
        ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), 12.0f * scale, position, IM_COL32(255, 255, 255, 255), text);
    }
private:
    char text[512]{};
};

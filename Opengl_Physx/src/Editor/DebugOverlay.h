#pragma once
#include "Scene.h"
#include "BlastScene.h"
#include "ThirdParty/imgui/imgui.h"
#include "SystemResourceMonitor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

// CPU wall-clock samples. GPU results below are asynchronous renderer queries,
// not components that can be added to these CPU durations.
class DebugOverlay
{
    enum Metric { Frame, Cpu, Update, DrawCpu, Present, Physics, Submit, Fetch, ParticleFetch, Upload, SoftSync, Blast, RenderWait, MetricCount };
    struct Sample { std::array<double, MetricCount> ms{}; unsigned steps = 0; };
    struct Stats { double average = 0, p95 = 0, maximum = 0; size_t count = 0; };
public:
    void Reset()
    {
        samples.clear(); elapsed = refresh = 0; copyText.clear(); sceneIndex = -1;
    }

    void Record(Scene& scene, const BlastScene* blast, double frameMs, double cpuMs,
        double updateMs, double drawMs, double presentMs, int width, int height, double sceneRenderMs = -1)
    {
        if (!std::isfinite(frameMs) || frameMs <= 0) return;
        if (sceneIndex != scene.GetSceneIndex() || paused != scene.IsPaused() || isolated != scene.IsPhysicsTimingIsolated()) Reset();
        sceneIndex = scene.GetSceneIndex(); paused = scene.IsPaused(); isolated = scene.IsPhysicsTimingIsolated(); totalRenderMs = sceneRenderMs;
        auto& world = scene.GetPhysicsWorld();
        unsigned steps = scene.GetPhysicsSteps();
        samples.push_back({ {frameMs, cpuMs, updateMs, drawMs, presentMs,
            scene.GetSimulationMs(), steps ? world.GetLastSubmitMs() : 0,
            steps ? world.GetLastFetchMs() : 0, steps ? world.GetLastParticleFetchMs() : 0,
            scene.GetRenderUploadMs(), scene.GetSoftSyncMs(), blast ? blast->GetUpdateMs() : 0, scene.GetRenderWaitMs()}, steps });
        elapsed += frameMs; refresh += frameMs;
        // Keep a wall-time window, not a fixed number of frames. Hard cap limits
        // memory even when rendering an empty scene at extremely high FPS.
        while (samples.size() > 1 && (elapsed - samples.front().ms[Frame] >= 2000 || samples.size() > 16384))
        {
            elapsed -= samples.front().ms[Frame]; samples.pop_front();
        }
        if (refresh < 500 && !copyText.empty()) return;
        refresh = 0;
        Build(scene, blast, width, height);
    }

    void Draw() const
    {
        const auto* viewport = ImGui::GetMainViewport();
        float scale = ImGui::GetStyle().FontScaleDpi;
        DrawResources(viewport, scale);
        ImVec2 position(viewport->WorkPos.x + 8 * scale, viewport->WorkPos.y + 6 * scale);
        auto* draw = ImGui::GetBackgroundDrawList();
        draw->AddText(ImGui::GetFont(), 12 * scale, position, IM_COL32(255, 255, 255, 255), copyText.c_str());
        if (samples.empty()) return;
        position.y += 14 * scale * static_cast<float>(std::count(copyText.begin(), copyText.end(), '\n') + 1);
        const float w = 360 * scale, h = 48 * scale;
        draw->AddRectFilled(position, ImVec2(position.x + w, position.y + h), IM_COL32(0, 0, 0, 160));
        float budgetY = position.y + h * (1 - 16.667f / 50.f);
        draw->AddLine(ImVec2(position.x, budgetY), ImVec2(position.x + w, budgetY), IM_COL32(100, 160, 100, 180));
        const size_t n = std::min<size_t>(240, samples.size());
        for (size_t i = 1; i < n; ++i)
        {
            const auto& a = samples[samples.size() - n + i - 1];
            const auto& b = samples[samples.size() - n + i];
            draw->AddLine(ImVec2(position.x + w * float(i - 1) / float(n - 1), position.y + h * (1 - float(std::min(a.ms[Frame], 50.0)) / 50)),
                ImVec2(position.x + w * float(i) / float(n - 1), position.y + h * (1 - float(std::min(b.ms[Frame], 50.0)) / 50)),
                b.steps ? IM_COL32(255, 180, 60, 255) : IM_COL32(90, 200, 255, 255));
        }
        draw->AddText(ImGui::GetFont(), 11 * scale, ImVec2(position.x, position.y + h), IM_COL32(255, 255, 255, 255),
            "Last 240 frames | 0-50 ms | green: 16.67 ms | orange: physics");
    }
    const char* GetText() const { return copyText.c_str(); }

private:
    void DrawResources(const ImGuiViewport* viewport, float scale) const
    {
        auto snapshot = resources.Read();
        std::array<std::string, 4> labels;
        auto* font = ImGui::GetFont();
        float size = 15 * scale, gap = 18 * scale, totalWidth = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            labels[i] = SystemResourceMonitor::Format(i, snapshot[i]);
            totalWidth += font->CalcTextSizeA(size, FLT_MAX, 0, labels[i].c_str()).x;
        }
        totalWidth += 3 * gap;
        float available = std::max(1.0f, viewport->WorkSize.x - 16 * scale);
        if (totalWidth > available) { float factor = available / totalWidth; size *= factor; gap *= factor; totalWidth = available; }
        ImVec2 pos(viewport->WorkPos.x + (viewport->WorkSize.x - totalWidth) * .5f, viewport->WorkPos.y + 6 * scale);
        auto* draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled(ImVec2(pos.x - 6 * scale, pos.y - 3 * scale),
            ImVec2(pos.x + totalWidth + 6 * scale, pos.y + size + 3 * scale), IM_COL32(15, 18, 23, 210), 4 * scale);
        for (size_t i = 0; i < 4; ++i)
        {
            ImU32 color = snapshot[i].percent < 0 ? IM_COL32(170, 170, 170, 255) :
                SystemResourceMonitor::High(i, snapshot[i]) ? IM_COL32(255, 75, 75, 255) : IM_COL32(90, 225, 120, 255);
            draw->AddText(font, size, pos, color, labels[i].c_str());
            pos.x += font->CalcTextSizeA(size, FLT_MAX, 0, labels[i].c_str()).x + gap;
        }
    }
    Stats Summarize(Metric metric, int stepFilter = -1, bool perStep = false) const
    {
        std::vector<double> values; values.reserve(samples.size());
        double sum = 0, divisor = 0;
        for (const auto& s : samples)
        {
            if (stepFilter == 1 && !s.steps) continue;
            if (stepFilter == 0 && s.steps) continue;
            if (perStep && !s.steps) continue;
            double value = perStep ? s.ms[metric] / s.steps : s.ms[metric];
            values.push_back(value); sum += s.ms[metric]; divisor += perStep ? s.steps : 1;
        }
        if (values.empty()) return {};
        std::sort(values.begin(), values.end());
        return { sum / divisor, values[static_cast<size_t>(std::ceil(values.size() * .95)) - 1], values.back(), values.size() };
    }
    void AddStats(const char* name, Metric metric, int stepFilter = -1, bool perStep = false)
    {
        auto s = Summarize(metric, stepFilter, perStep);
        char line[256];
        if (s.count) std::snprintf(line, sizeof(line), "%s %.2f / %.2f / %.2f ms", name, s.average, s.p95, s.maximum);
        else std::snprintf(line, sizeof(line), "%s N/A", name);
        copyText += line;
    }
    void Build(const Scene& scene, const BlastScene* blast, int width, int height)
    {
        unsigned totalSteps = 0, over16 = 0, over33 = 0;
        for (const auto& s : samples)
        {
            totalSteps += s.steps; over16 += s.ms[Frame] > 16.667; over33 += s.ms[Frame] > 33.333;
        }
        const auto* water = scene.GetLiquid(); const auto* sand = scene.GetSand();
        char line[512];
        std::snprintf(line, sizeof(line), "Scene %d | %s%s | %dx%d | Window %.2f s (%zu frames) | FPS %.1f\n",
            scene.GetSceneIndex() + 1, scene.UsesGpu() ? "GPU" : "CPU", paused ? " PAUSED" : "", width, height, elapsed / 1000, samples.size(), 1000 * samples.size() / elapsed);
        copyText = line;
        std::snprintf(line, sizeof(line), "Rigid %zu | Soft %zu | Blast %u / Chunks %u / Bonds %u | Water %u / Sand %u / Particle %u\n",
            scene.GetBodyCount(), scene.GetSoftBodyCount(), blast ? blast->GetActorCount() : 0, blast ? blast->GetVisibleChunkCount() : 0,
            blast ? blast->GetBondCount() : 0, water ? water->Count() : 0, sand ? sand->Count() : 0, (water ? water->Count() : 0u) + (sand ? sand->Count() : 0u));
        copyText += line;
        copyText += "CPU wall time: AVG / P95 / MAX (overlapping scopes; do not add)\n";
        AddStats("Frame", Frame);
        std::snprintf(line, sizeof(line), " | >16.67 ms %u | >33.33 ms %u\n", over16, over33); copyText += line;
        AddStats("Frame with step", Frame, 1); copyText += " | "; AddStats("without step", Frame, 0); copyText += '\n';
        AddStats("CPU before Present", Cpu); copyText += " | "; AddStats("Present", Present); copyText += '\n';
        AddStats("Scene update", Update); copyText += " | "; AddStats("Scene+Flow draw CPU", DrawCpu); copyText += '\n';
        AddStats(isolated ? "PhysX scene/step (GL drained)" : "PhysX scene/step (wall, includes waits)", Physics, 1, true);
        const auto physicsStats = Summarize(Physics, 1, true);
        copyText += '\n';
        copyText += "Particle GPU draw (async/smoothed): ";
        double renderTotal = 0;
        bool anyActive = false, allTimed = true;
        unsigned particleIndex = 0;
        for (const auto* p : { water,sand })
        {
            copyText += particleIndex++ == 0 ? "Water " : "Sand ";
            if (p && p->Count()) { anyActive = true; allTimed = allTimed && p->HasRenderTiming(); }
            if (p && p->Count() && p->HasRenderTiming())
            {
                renderTotal += p->GetRenderGpuMs(); std::snprintf(line, sizeof(line), "%.2f ms  ", p->GetRenderGpuMs()); copyText += line;
            }
            else copyText += "N/A  ";
        }
        copyText += "| excludes CUDA/GL upload\n";
        if (physicsStats.count) std::snprintf(line, sizeof(line), "PhysX %.2f ms", physicsStats.average);
        else std::snprintf(line, sizeof(line), "PhysX N/A");
        copyText += line;
        if (totalRenderMs >= 0) std::snprintf(line, sizeof(line), " | Render %.2f ms\n", totalRenderMs);
        else std::snprintf(line, sizeof(line), " | Render N/A\n");
        copyText += line;
        copyText += isolated ? "PhysX isolation ON (profiling changes overlap) | " : "PhysX isolation OFF | ";
        AddStats("Pre-PhysX GL wait/step", RenderWait, 1, true); copyText += '\n';
        if (water && water->HasWaterPassTiming()) {
            std::snprintf(line, sizeof(line), "Water GPU (async EMA): Depth %.3f ms | Thickness %.3f ms | Smooth %.3f ms\n",
                water->WaterPassMs(LiquidSurface::Depth), water->WaterPassMs(LiquidSurface::Thickness), water->WaterPassMs(LiquidSurface::Smooth));
            copyText += line;
            std::snprintf(line, sizeof(line), "Water GPU: Background %.3f ms | Pack %.3f ms | Normals+ViewDepth %.3f ms\n",
                water->WaterPassMs(LiquidSurface::Background), water->WaterPassMs(LiquidSurface::Pack), water->WaterPassMs(LiquidSurface::Normals));
            copyText += line;
            std::snprintf(line, sizeof(line), "Water GPU: Lighting %.3f ms | Composite %.3f ms | Whitewater %.3f ms\n",
                water->WaterPassMs(LiquidSurface::Lighting), water->WaterPassMs(LiquidSurface::Composite), water->WaterPassMs(LiquidSurface::Whitewater));
            copyText += line;
        }
        else copyText += "Water GPU passes: N/A (no water render or awaiting queries)\n";
        if (sand && sand->HasSandPassTiming()) {
            std::snprintf(line, sizeof(line), "Sand GPU (async EMA): Depth %.3f ms | Shade %.3f ms\n", sand->SandDepthMs(), sand->SandShadeMs());
            copyText += line;
        }
        else copyText += "Sand GPU: Depth N/A | Shade N/A\n";
        unsigned timingIndex = 0;
        for (const auto* p : { water,sand }) {
            const char* name = timingIndex++ == 0 ? "Water" : "Sand";
            if (!p || !p->Count()) continue;
            if (p->HasInteropTiming()) {
                std::snprintf(line, sizeof(line), "%s upload (EMA/upload): Map CPU %.3f ms | Unmap CPU %.3f ms | Last upload %.0f ms ago | Copy GPU ",
                    name, p->MapCpuMs(), p->UnmapCpuMs(), p->UploadAgeMs());
                copyText += line;
                if (p->HasCopyTiming()) { std::snprintf(line, sizeof(line), "%.3f ms\n", p->CopyGpuMs()); copyText += line; }
                else copyText += "N/A\n";
            }
            else { copyText += name; copyText += " upload: N/A\n"; }
        }
        std::snprintf(line, sizeof(line), "Steps/s %.1f / target 60 | Sim/wall %.2fx\n", totalSteps * 1000 / elapsed, totalSteps * 1000 / (60 * elapsed)); copyText += line;
        AddStats("Submit/step", Submit, 1, true); copyText += " | "; AddStats("Fetch/step", Fetch, 1, true); copyText += '\n';
        AddStats("Particle fetch/step", ParticleFetch, 1, true); copyText += " (CPU API durations, not GPU kernels)\n";
        AddStats("Soft upload", Upload); copyText += " | "; AddStats("Soft sync", SoftSync); copyText += " | "; AddStats("Blast", Blast); copyText += " | F1 UI";
    }
    SystemResourceMonitor resources;
    std::deque<Sample> samples;
    double elapsed = 0, refresh = 0;
    int sceneIndex = -1;
    bool paused = false, isolated = false;
    double totalRenderMs = -1;
    std::string copyText;
};

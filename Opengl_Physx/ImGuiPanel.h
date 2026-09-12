#pragma once
#include "Scene.h"
#include "ThirdParty/imgui/imgui.h"

class ImGuiPanel
{
public:
    explicit ImGuiPanel(bool chinese = true) : chinese(chinese) {}
    void ResetFps() { fpsElapsed = 0; displayedFps = 0; fpsFrames = 0; }

    void Draw(Scene& scene)
    {
        const auto display = ImGui::GetIO().DisplaySize;
        float scale = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Once);
        float width = std::min(340.0f * scale, std::max(1.0f, display.x - 24));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, std::max(1.0f, display.y - 24)));
        if (!ImGui::Begin(T("调试###Status", "Debug###Status"), nullptr, ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::End();
            return;
        }
        float deltaTime = ImGui::GetIO().DeltaTime;
        if (std::isfinite(deltaTime) && deltaTime > 0)
        {
            fpsElapsed += deltaTime;
            ++fpsFrames;
            if (fpsElapsed >= 0.5)
            {
                displayedFps = static_cast<double>(fpsFrames) / fpsElapsed;
                fpsElapsed = 0;
                fpsFrames = 0;
            }
        }
        if (displayedFps > 0) ImGui::Text("FPS: %.0f", displayedFps);
        else ImGui::TextUnformatted("FPS: --");
        ImGui::SameLine();
        ImGui::TextDisabled(T("刚体 %zu  /  软体 %zu", "Rigid %zu  /  Soft %zu"), scene.GetBodyCount(), scene.GetSoftBodyCount());
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(T("全局", "Global")))
        {
            const char* modelsCN[] = { "方块","平面","球体","圆柱","圆锥","胶囊","圆环" };
            const char* modelsEN[] = { "Box","Plane","Sphere","Cylinder","Cone","Capsule","Torus" };
            const char* typesCN[] = { "刚体","软体" };
            const char* typesEN[] = { "Rigid","Soft" };
            const char* iterations[] = { "4","8","16" };
            const char* densities[] = { "4","6","10" };
            const unsigned int iterationValues[] = { 4,8,16 }, densityValues[] = { 4,6,10 };
            int type = scene.GetSpawnSoft() ? 1 : 0, model = static_cast<int>(scene.GetSpawnModel());
            int iteration = scene.GetSoftIterations() == 4 ? 0 : scene.GetSoftIterations() == 8 ? 1 : 2;
            int density = scene.GetSoftResolution() == 4 ? 0 : scene.GetSoftResolution() == 6 ? 1 : 2;
            bool paused = scene.IsPaused(), wire = scene.GetShowCollisions(), self = scene.GetSoftSelfCollision(), changed = false;
            if (ImGui::BeginTable("##Controls", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1);
                auto row = [&]()
                    {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::SetNextItemWidth(100 * scale);
                    };
                row();
                const char* modes[] = { "CPU","GPU" };
                int mode = scene.UsesGpu() ? 1 : 0;
                if (ImGui::Combo(T("计算", "Compute"), &mode, modes, 2)) scene.RequestGpu(mode == 1);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("切换将恢复标准场景；软体仅支持 GPU", "Switch resets the scene; soft bodies require GPU"));
                row();
                if (ImGui::Combo(T("类型", "Type"), &type, chinese ? typesCN : typesEN, scene.SoftBodiesAvailable() ? 2 : 1)) scene.SetSpawnSoft(type == 1);
                ImGui::TableNextColumn();
                if (ImGui::Checkbox(T("暂停", "Pause"), &paused)) scene.SetPaused(paused);
                row();
                if (ImGui::Combo(T("模型", "Model"), &model, chinese ? modelsCN : modelsEN, static_cast<int>(ModelType::Count)))
                    scene.SetSpawnModel(static_cast<ModelType>(model));
                ImGui::TableNextColumn();
                if (ImGui::Checkbox(T("网格", "Mesh"), &wire)) scene.SetShowCollisions(wire);
                ImGui::BeginDisabled(!scene.SoftBodiesAvailable());
                row();
                changed |= ImGui::Combo(T("迭代", "Iterations"), &iteration, iterations, 3);
                ImGui::TableNextColumn();
                changed |= ImGui::Checkbox(T("自碰撞", "Self hit"), &self);
                row();
                changed |= ImGui::Combo(T("精度", "Density"), &density, densities, 3);
                if (changed) scene.SetSoftSettings(iterationValues[iteration], self, densityValues[density]);
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader(T("场景", "Scene")))
        {
            ImGui::BeginDisabled(scene.GetSpawnSoft() && !scene.SoftBodiesAvailable());
            ImGui::SetNextItemWidth(150 * scale);
            ImGui::InputInt(T("数量", "Count"), &testCount);
            testCount = std::max(testCount, 1);
            if (ImGui::Button(T("标准", "Default"))) scene.Reset();
            ImGui::SameLine();
            if (ImGui::Button(T("分散", "Spread"))) scene.BuildTest(false, testCount);
            ImGui::SameLine();
            if (ImGui::Button(T("堆叠", "Stack"))) scene.BuildTest(true, testCount);
            ImGui::SameLine();
            if (ImGui::Button(T("单列", "Column"))) scene.BuildTest(true, testCount, true);
            ImGui::EndDisabled();
        }

        auto* body = scene.GetSelectedBody();
        auto* material = scene.GetSelectedMaterial();
        auto* soft = scene.GetSelectedSoftBody();
        if ((body || soft) && material && ImGui::CollapsingHeader(T("属性", "Properties")))
        {
            const char* cn[] = { "方块","平面","球体","圆柱","圆锥","胶囊","圆环" };
            const char* en[] = { "Box","Plane","Sphere","Cylinder","Cone","Capsule","Torus" };
            ImGui::TextUnformatted((chinese ? cn : en)[static_cast<int>(body ? body->GetModelType() : soft->GetModelType())]);
            ImGui::PushID(body ? static_cast<void*>(body) : static_cast<void*>(soft));
            ImGui::PushItemWidth(-100.0f * scale);
            if (auto* visible = scene.GetSelectedMeshVisibility())
                ImGui::Checkbox(T("网格", "Show mesh"), visible);
            if (body && ImGui::CollapsingHeader(T("物理", "Physics"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto p = body->GetProperties(); bool changed = false;
                changed |= Number(T("质量 (kg)", "Mass (kg)"), p.mass, 0.1f, 0.01f, 100000);
                changed |= Number(T("静摩擦", "Static friction"), p.staticFriction, 0.01f, 0, 10);
                changed |= Number(T("动摩擦", "Dynamic friction"), p.dynamicFriction, 0.01f, 0, 10);
                changed |= Number(T("弹性", "Restitution"), p.restitution, 0.01f, 0, 1);
                changed |= Number(T("线性阻尼", "Linear damping"), p.linearDamping, 0.01f, 0, 100);
                changed |= Number(T("角阻尼", "Angular damping"), p.angularDamping, 0.01f, 0, 100);
                if (changed) body->SetProperties(p);
            }
            if (soft && ImGui::CollapsingHeader(T("软体", "Soft body"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto& physical = soft->GetPhysicalMaterial();
                float young = physical.getYoungsModulus(), poisson = physical.getPoissons();
                if (Number(T("硬度", "Stiffness"), young, 100, 100, 1000000)) physical.setYoungsModulus(young);
                if (Number(T("泊松比", "Poisson ratio"), poisson, 0.01f, 0, 0.49f)) physical.setPoissons(poisson);
            }
            if (ImGui::CollapsingHeader(T("材质", "Material"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                float color[3] = { material->baseColor.r,material->baseColor.g,material->baseColor.b };
                if (ImGui::ColorEdit3(T("底色", "Base color"), color))
                    for (int i = 0; i < 3; ++i) if (std::isfinite(color[i])) material->baseColor[i] = std::clamp(color[i], 0.0f, 1.0f);
                Number(T("高光", "Specular"), material->specularStrength, 0.01f, 0, 10);
                Number(T("光泽度", "Shininess"), material->shininess, 1, 1, 256);
            }
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
        ImGui::End();
    }
private:
    bool chinese = true;
    double fpsElapsed = 0, displayedFps = 0;
    unsigned int fpsFrames = 0;
    int testCount = 8;
    const char* T(const char* cn, const char* en) const { return chinese ? cn : en; }
    static bool Number(const char* label, float& value, float speed, float low, float high)
    {
        float old = value;
        if (!ImGui::DragFloat(label, &value, speed, low, high, "%.3f", ImGuiSliderFlags_AlwaysClamp)) return false;
        value = std::isfinite(value) ? std::clamp(value, low, high) : old;
        return value != old;
    }
};

#pragma once
#include "Scene.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/imgui_internal.h"
#include <string>

class ImGuiPanel
{
public:
    explicit ImGuiPanel(bool chinese = true) : chinese(chinese) {}

    void Draw(Scene& scene, float fps, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float)>& spawnDestructible = {})
    {
        const auto display = ImGui::GetIO().DisplaySize;
        float scale = ImGui::GetStyle().FontScaleDpi;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float topOffset = 24.0f * scale;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topOffset), ImGuiCond_Always);
        float width = std::min(340.0f * scale, std::max(1.0f, display.x));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, std::max(1.0f, display.y - topOffset)));
        if (!ImGui::Begin(T("调试###Status", "Debug###Status"), nullptr, ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::End();
            return;
        }
        if (sceneVersion != scene.GetVersion()) ResetProperties(scene.GetVersion());
        ImGui::Text("FPS: %.0f", fps);
        const char* scenesCN[] = { "场景 1","场景 2" };
        const char* scenesEN[] = { "Scene 1","Scene 2" };
        int activeScene = scene.GetSceneIndex();
        ImGui::SetNextItemWidth(210.0f * scale);
        if (ImGui::Combo(T("场景", "Scene"), &activeScene, chinese ? scenesCN : scenesEN, 2))
        {
            if (clearDestructibles) clearDestructibles();
            scene.SetSceneIndex(activeScene);
        }
        if (ImGui::CollapsingHeader(T("全局", "Global")))
        {
            const char* iterations[] = { "4","8","16" };
            const char* densities[] = { "4","6","10" };
            const unsigned int iterationValues[] = { 4,8,16 }, densityValues[] = { 4,6,10 };
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
                ImGui::TableNextColumn();
                if (ImGui::Checkbox(T("暂停", "Pause"), &paused)) scene.SetPaused(paused);
                row();
                ImGui::BeginDisabled(!scene.SoftBodiesAvailable());
                changed |= ImGui::Combo(T("迭代", "Iterations"), &iteration, iterations, 3);
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                if (ImGui::Checkbox(T("网格", "Mesh"), &wire)) scene.SetShowCollisions(wire);
                row();
                ImGui::BeginDisabled(!scene.SoftBodiesAvailable());
                changed |= ImGui::Combo(T("精度", "Density"), &density, densities, 3);
                ImGui::TableNextColumn();
                changed |= ImGui::Checkbox(T("自碰撞", "Self hit"), &self);
                if (changed) scene.SetSoftSettings(iterationValues[iteration], self, densityValues[density]);
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            if (ImGui::TreeNode(T("发射", "Launch")))
            {
                const char* modelsCN[] = { "方块","平面","球体","圆柱","圆锥","胶囊","圆环" };
                const char* modelsEN[] = { "Box","Plane","Sphere","Cylinder","Cone","Capsule","Torus" };
                const char* typesCN[] = { "刚体","软体","碎裂刚体" };
                const char* typesEN[] = { "Rigid","Soft","Destructible" };
                int type = scene.GetSpawnType(), model = static_cast<int>(scene.GetSpawnModel());
                float speed = scene.GetLaunchSpeed(), size = scene.GetLaunchScale();
                ImGui::PushItemWidth(160.0f * scale);
                if (ImGui::Combo(T("类型", "Type"), &type, chinese ? typesCN : typesEN, 3)) scene.SetSpawnType(type);
                if (ImGui::Combo(T("模型", "Model"), &model, chinese ? modelsCN : modelsEN, static_cast<int>(ModelType::Count))) scene.SetSpawnModel(static_cast<ModelType>(model));
                bool launchChanged = Number(T("速度", "Speed"), speed, 0.25f, 0.0f, 100.0f);
                launchChanged |= Number(T("大小", "Size"), size, 0.05f, 0.1f, 10.0f);
                if (launchChanged) scene.SetLaunchSettings(speed, size);
                ImGui::PopItemWidth();
                ImGui::TreePop();
            }
        }

        int sceneAction = -1;
        if (scene.GetSceneIndex() == 0 && ImGui::CollapsingHeader(T("测试", "Test")))
        {
            ImGui::BeginDisabled(scene.GetSpawnSoft() && !scene.SoftBodiesAvailable());
            ImGui::SetNextItemWidth(150 * scale);
            ImGui::InputInt(T("数量", "Count"), &testCount);
            testCount = std::max(testCount, 1);
            if (ImGui::Button(T("标准", "Default"))) sceneAction = 0;
            ImGui::SameLine();
            if (ImGui::Button(T("分散", "Spread"))) sceneAction = 1;
            ImGui::SameLine();
            if (ImGui::Button(T("堆叠", "Stack"))) sceneAction = 2;
            ImGui::SameLine();
            if (ImGui::Button(T("单列", "Column"))) sceneAction = 3;
            ImGui::EndDisabled();
        }
        if (scene.GetSceneIndex() == 1 && ImGui::CollapsingHeader(T("测试", "Test"), ImGuiTreeNodeFlags_DefaultOpen))
        {
        }

        if (sceneAction >= 0)
        {
            if (sceneAction == 0)
            {
                if (clearDestructibles) clearDestructibles();
                scene.Reset();
            }
            else scene.BuildTest(sceneAction >= 2, testCount, sceneAction == 3, clearDestructibles, spawnDestructible);
            ResetProperties(scene.GetVersion());
            ImGui::End();
            return;
        }

        const auto selected = scene.GetSelection();
        if (objectId != selected.id)
        {
            ClearEditor();
            objectId = selected.id;
        }
        auto* savedState = ImGui::GetStateStorage();
        ImGui::SetStateStorage(&properties);
        auto* body = selected.body;
        auto* material = selected.material;
        auto* soft = selected.soft;
        auto* actor = selected.actor;
        if ((body || soft || actor) && ImGui::CollapsingHeader(T("属性", "Properties")))
        {
            const char* cn[] = { "方块","平面","球体","圆柱","圆锥","胶囊","圆环" };
            const char* en[] = { "Box","Plane","Sphere","Cylinder","Cone","Capsule","Torus" };
            ImGui::TextUnformatted((chinese ? cn : en)[static_cast<int>(selected.type)]);
            auto key = std::to_string(selected.id);
            ImGui::PushID(key.c_str());
            ImGui::PushItemWidth(-100.0f * scale);
            if (auto* visible = selected.mesh)
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
            if (selected.external && actor && ImGui::CollapsingHeader(T("物理", "Physics"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                float mass = actor->getMass(), linearDamping = actor->getLinearDamping(), angularDamping = actor->getAngularDamping();
                bool changed = Number(T("质量 (kg)", "Mass (kg)"), mass, 0.1f, 0.01f, 100000);
                changed |= Number(T("线性阻尼", "Linear damping"), linearDamping, 0.01f, 0, 100);
                changed |= Number(T("角阻尼", "Angular damping"), angularDamping, 0.01f, 0, 100);
                if (changed)
                {
                    float ratio = mass / actor->getMass();
                    actor->setMassSpaceInertiaTensor(actor->getMassSpaceInertiaTensor() * ratio);
                    actor->setMass(mass);
                    actor->setLinearDamping(linearDamping);
                    actor->setAngularDamping(angularDamping);
                    actor->wakeUp();
                }
            }
            if (soft && ImGui::CollapsingHeader(T("软体", "Soft body"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto& physical = soft->GetPhysicalMaterial();
                float young = soft->GetBaseStiffness(), poisson = physical.getPoissons();
                bool changed = Number(T("硬度", "Stiffness"), young, 0.005f, 0.001f, 10);
                changed |= Number(T("泊松比", "Poisson ratio"), poisson, 0.01f, 0, 0.49f);
                if (changed) soft->SetMaterial(young, poisson);
            }
            if (material && ImGui::CollapsingHeader(T("材质", "Material"), ImGuiTreeNodeFlags_DefaultOpen))
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
        ImGui::SetStateStorage(savedState);
        ImGui::End();
    }
private:
    static void ClearEditor()
    {
        ImGui::ClearActiveID();
        auto& context = *ImGui::GetCurrentContext();
        context.TempInputId = 0;
        context.InputTextState.ID = 0;
        context.InputTextState.ClearFreeMemory();
        context.InputTextDeactivatedState.ClearFreeMemory();
        context.DragCurrentAccum = 0;
        context.DragCurrentAccumDirty = false;
        context.ColorEditSavedID = 0;
        ImGui::ClosePopupsOverWindow(ImGui::GetCurrentWindow(), false);
    }

    void ResetProperties(std::uint64_t version)
    {
        ClearEditor();
        properties.Clear();
        sceneVersion = version;
        objectId = 0;
    }

    ImGuiStorage properties;
    std::uint64_t sceneVersion = 0, objectId = 0;
    bool chinese = true;
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

#pragma once
#include "Scene.h"
#include "BlastScene.h"
#include "FlowSimulation.h"
#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/imgui_internal.h"
#include <string>

class ImGuiPanel
{
public:
    explicit ImGuiPanel(bool chinese = true) : chinese(chinese) {}

    void Draw(Scene& scene, float fps, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float, float)>& spawnDestructible = {}, const std::function<void()>& buildWall = {}, BlastScene* blastScene = nullptr, const char* debugText = nullptr, FlowSimulation* flowSimulation = nullptr)
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
        ImGui::SameLine();
        if (ImGui::Button(T("复制调试信息", "Copy debug info")) && debugText) ImGui::SetClipboardText(debugText);
        const char* scenesCN[] = { "场景 1","场景 2","场景 3" };
        const char* scenesEN[] = { "Scene 1","Scene 2","Scene 3" };
        int activeScene = scene.GetSceneIndex();
        ImGui::SetNextItemWidth(210.0f * scale);
        if (ImGui::Combo(T("场景", "Scene"), &activeScene, chinese ? scenesCN : scenesEN, 3))
        {
            if (clearDestructibles) clearDestructibles();
            scene.SetSceneIndex(activeScene);
        }
        if (ImGui::CollapsingHeader(T("全局", "Global"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* modelsCN[] = { "方块","平面","球体","圆柱","圆锥","胶囊","圆环" };
            const char* modelsEN[] = { "Box","Plane","Sphere","Cylinder","Cone","Capsule","Torus" };
            const char* typesCN[] = { "刚体","软体","碎裂刚体" };
            const char* typesEN[] = { "Rigid","Soft","Destructible" };
            int type = scene.GetSpawnType(), model = static_cast<int>(scene.GetSpawnModel());
            ImGui::PushItemWidth(160.0f * scale);
            if (ImGui::Combo(T("类型", "Type"), &type, chinese ? typesCN : typesEN, 3)) scene.SetSpawnType(type);
            if (ImGui::Combo(T("模型", "Model"), &model, chinese ? modelsCN : modelsEN, static_cast<int>(ModelType::Count))) scene.SetSpawnModel(static_cast<ModelType>(model));
            float testSize = scene.GetTestScale();
            bool testSizeChanged = Number(T("测试大小", "Test size"), testSize, 0.05f, 0.1f, 10.0f);
            if (testSizeChanged) scene.SetTestScale(testSize);
            ImGui::PopItemWidth();
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
                float speed = scene.GetLaunchSpeed(), size = scene.GetLaunchScale(), mass = scene.GetLaunchMass();
                ImGui::PushItemWidth(160.0f * scale);
                bool launchChanged = Number(T("速度", "Speed"), speed, 0.25f, 0.0f, 100.0f);
                launchChanged |= Number(T("发射大小", "Launch size"), size, 0.05f, 0.1f, 10.0f);
                launchChanged |= Number(T("质量 (kg)", "Mass (kg)"), mass, 0.05f, 0.01f, 10000.0f);
                if (launchChanged) scene.SetLaunchSettings(speed, size, mass);
                ImGui::PopItemWidth();
                ImGui::TreePop();
            }
        }

        if (scene.GetSceneIndex() == 2 && ImGui::CollapsingHeader(T("测试", "Test"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(!flowSimulation);
            auto button = [&](const char* label, bool smoke)
                {
                    const bool selected = flowSimulation && flowSimulation->IsSmoke() == smoke;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(label, ImVec2(72.0f * scale, 0)) && flowSimulation) flowSimulation->SetSmoke(smoke);
                    if (selected) ImGui::PopStyleColor();
                };
            ImGui::PushID("FlowTestButtons");
            button(T("火焰", "Fire"), false);
            ImGui::SameLine();
            button(T("烟雾", "Smoke"), true);
            ImGui::PopID();
            ImGui::EndDisabled();
        }

        int sceneAction = -1;
        if (scene.GetSceneIndex() == 0 && ImGui::CollapsingHeader(T("测试", "Test"), ImGuiTreeNodeFlags_DefaultOpen))
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
        int scene2Action = -1;
        if (scene.GetSceneIndex() == 1 && ImGui::CollapsingHeader(T("测试", "Test"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SetNextItemWidth(150 * scale);
            ImGui::InputInt(T("数量", "Count"), &testCount);
            testCount = std::clamp(testCount, 1, 500);
            if (ImGui::Button(T("墙体", "Wall"))) scene2Action = 0;
            ImGui::SameLine();
            if (ImGui::Button(T("墙体滚球", "Wall ball"))) scene2Action = 4;
            if (ImGui::Button(T("平摊", "Flat"))) scene2Action = 2;
            ImGui::SameLine();
            if (ImGui::Button(T("金字塔", "Pyramid"))) scene2Action = 1;
            ImGui::SameLine();
            if (ImGui::Button(T("5000正方体", "5000 cubes"))) scene2Action = 3;
            ImGui::SetNextItemWidth(150 * scale);
            Number(T("滚球大小", "Ball size"), wallBallSize, 0.10f, 0.5f, 12.0f);
            ImGui::SetNextItemWidth(150 * scale);
            Number(T("滚球质量 (kg)", "Ball mass (kg)"), wallBallMass, 1.0f, 0.01f, 100000.0f);
            if (blastScene)
            {
                auto fracture = blastScene->GetWallSettings();
                int fragments = static_cast<int>(fracture.localFragments);
                ImGui::SetNextItemWidth(150 * scale);
                bool changed = Number(T("破坏冲量", "Breaking impulse"), fracture.breakingImpulse, 0.01f, 0.01f, 20.0f);
                ImGui::SetNextItemWidth(150 * scale);
                changed |= Number(T("破坏范围", "Damage radius"), fracture.damageRadius, 0.05f, 0.1f, 2.0f);
                ImGui::SetNextItemWidth(150 * scale);
                changed |= Number(T("连锁范围", "Chain radius"), fracture.chainRadius, 0.05f, 1.0f, 3.0f);
                ImGui::SetNextItemWidth(150 * scale);
                if (ImGui::DragInt(T("核心碎块", "Core fragments"), &fragments, 1.0f, 8, 48, "%d", ImGuiSliderFlags_AlwaysClamp)) changed = true;
                fracture.localFragments = static_cast<unsigned int>(std::clamp(fragments, 8, 48));
                if (changed) blastScene->SetWallSettings(fracture);
            }
        }

        if (scene.GetSceneIndex() == 2 && flowSimulation &&
            ImGui::CollapsingHeader(flowSimulation->IsSmoke() ? T("Flow 烟雾###FlowControls", "Flow Smoke###FlowControls") : T("Flow 火焰###FlowControls", "Flow Fire###FlowControls"), ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& fire = flowSimulation->GetSettings();
            const char* displayModes[] = { T("渲染", "Render"), T("体素", "Voxels") };
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::Combo(T("显示模式", "Display mode"), &fire.displayMode, displayModes, 2);
            ImGui::Checkbox(T("持续发射", "Continuous emission"), &fire.emitting);
            if (flowSimulation->IsSmoke())
            {
                auto position = scene.GetSmokeFloorPosition();
                ImGui::SetNextItemWidth(210.0f * scale);
                if (ImGui::DragFloat3(T("地板位置", "Floor position"), &position.x, 0.05f, -50.0f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
                    scene.SetSmokeFloorPosition(position);
            }
            ImGui::SetNextItemWidth(210.0f * scale);
            ImGui::DragFloat3(T("发射器位置", "Emitter position"), fire.position, 0.05f, -20.0f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("发射器半径", "Emitter radius"), fire.radius, 0.02f, 0.1f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("上升速度", "Upward velocity"), fire.upwardVelocity, 0.05f, 0.0f, 30.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::BeginDisabled(flowSimulation->IsSmoke());
            Number(T("温度", "Temperature"), fire.temperature, 0.05f, 0.0f, 10.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("燃料", "Fuel"), fire.fuel, 0.02f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::EndDisabled();
            Number(T("烟雾", "Smoke"), fire.smoke, 0.02f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::BeginDisabled(flowSimulation->IsSmoke());
            Number(T("冷却速度", "Cooling rate"), fire.coolingRate, 0.02f, 0.0f, 10.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("点火温度", "Ignition temperature"), fire.ignitionTemperature, 0.01f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("燃烧速率", "Burn rate"), fire.burnRate, 0.05f, 0.0f, 20.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("温度浮力", "Temperature buoyancy"), fire.temperatureBuoyancy, 0.02f, 0.0f, 10.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::EndDisabled();
            Number(T("涡量强度", "Vorticity strength"), fire.vorticityStrength, 0.01f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("速度阻尼", "Velocity damping"), fire.velocityDamping, 0.005f, 0.0f, 0.99f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("烟雾消散", "Smoke dissipation"), fire.smokeDissipation, 0.01f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::BeginDisabled(flowSimulation->IsSmoke());
            Number(T("燃烧产烟", "Smoke per burn"), fire.smokePerBurn, 0.05f, 0.0f, 10.0f);
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("体素大小", "Cell size"), fire.cellSize, 0.01f, 0.08f, 1.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(T("体积浓度", "Volume density"), fire.renderDensity, 0.05f, 0.1f, 10.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            Number(flowSimulation->IsSmoke() ? T("烟雾亮度", "Smoke brightness") : T("火焰亮度", "Fire brightness"), fire.fireBrightness, 0.05f, 0.0f, 20.0f);
            ImGui::SetNextItemWidth(150.0f * scale);
            ImGui::DragInt(T("渲染步数", "Ray steps"), &fire.raySteps, 1.0f, 32, 256, "%d", ImGuiSliderFlags_AlwaysClamp);

            ImGui::Separator();
            if (flowSimulation->IsSmoke() && ImGui::CollapsingHeader(T("烟雾色表", "Smoke colors")))
            {
                ImGui::SetNextItemWidth(150.0f * scale);
                Number(T("密度上限", "Density maximum"), fire.smokeColorDensity, 0.02f, 0.01f, 10.0f);
                ImGui::SetNextItemWidth(150.0f * scale);
                ImGui::BeginDisabled(fire.displayMode == 1);
                Number(T("不透明度", "Opacity"), fire.smokeOpacity, 0.02f, 0.0f, 5.0f);
                ImGui::EndDisabled();
                const char* labels[] = { T("稀薄颜色", "Thin color"),T("中等颜色", "Medium color"),T("浓密颜色", "Dense color") };
                for (int i = 0; i < 3; ++i)
                {
                    ImGui::SetNextItemWidth(150.0f * scale);
                    ImGui::ColorEdit3(labels[i], fire.smokeColors[i].data(), ImGuiColorEditFlags_Float);
                }
                if (ImGui::Button(T("恢复烟雾色表", "Reset smoke colors")))
                {
                    const auto defaults = flowSimulation->DefaultSettings();
                    fire.smokeColors = defaults.smokeColors;
                    fire.smokeColorDensity = defaults.smokeColorDensity;
                    fire.smokeOpacity = defaults.smokeOpacity;
                }
            }
            if (!flowSimulation->IsSmoke() && ImGui::CollapsingHeader(T("温度色表", "Temperature colors")))
            {
                ImGui::PushItemWidth(150.0f * scale);
                Number(T("色表温度上限", "Color temperature max"), fire.colormapMaxTemperature, 0.01f, 0.01f, 10.0f);
                for (int i = 0; i < 6; ++i)
                {
                    ImGui::PushID(i);
                    ImGui::Text(T("节点 %d", "Point %d"), i + 1);
                    const float minimum = i ? fire.colormapPositions[i - 1] + 0.001f : 0.0f;
                    const float maximum = i < 5 ? fire.colormapPositions[i + 1] - 0.001f : 1.0f;
                    Number(T("温度位置", "Temperature position"), fire.colormapPositions[i], 0.005f, minimum, maximum);
                    ImGui::ColorEdit4(T("颜色", "Color"), fire.colormapColors[i].data(),
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
                    Number(T("发光强度", "Emission strength"), fire.colormapIntensities[i], 0.05f, 0.0f, 100.0f);
                    ImGui::PopID();
                }
                if (ImGui::Button(T("恢复色表", "Reset colors")))
                {
                    const FlowSimulation::Settings defaults;
                    fire.colormapMaxTemperature = defaults.colormapMaxTemperature;
                    fire.colormapPositions = defaults.colormapPositions;
                    fire.colormapColors = defaults.colormapColors;
                    fire.colormapIntensities = defaults.colormapIntensities;
                }
                ImGui::PopItemWidth();
            }
            ImGui::Separator();

            if (ImGui::Button(flowSimulation->IsSmoke() ? T("重置烟雾", "Reset smoke") : T("重置火焰", "Reset fire")))
            {
                flowSimulation->Reset();
            }
            ImGui::SameLine();
            if (ImGui::Button(T("恢复默认", "Defaults")))
            {
                fire = flowSimulation->DefaultSettings();
                flowSimulation->Reset();
            }
            ImGui::Text(T("Flow 提交帧: %llu", "Flow submitted frame: %llu"),
                static_cast<unsigned long long>(flowSimulation->LastSubmittedFrame()));
        }
        else if (scene.GetSceneIndex() == 2 && !flowSimulation)
        {
            ImGui::TextUnformatted(T("Flow 正在初始化，请稍候...", "Flow is initializing, please wait..."));
        }

        if (scene2Action >= 0)
        {
            scene.Reset();
            if (scene2Action == 0) { if (buildWall) buildWall(); }
            else if (scene2Action == 4)
            {
                if (buildWall) buildWall();
                RigidBody* ball = scene.BuildWallRollingTest(wallBallSize, wallBallMass);
                if (blastScene && ball)
                {
                    constexpr float pi = 3.14159265358979323846f;
                    float radius = wallBallSize * 0.5f;
                    float volume = 4.0f * pi * radius * radius * radius / 3.0f;
                    blastScene->PrepareTrackedProjectile(ball->GetActor(), glm::vec3(0.0f, 0.35f + radius, -2.5f), radius, wallBallMass, volume);
                }
            }
            else if (scene2Action == 1) scene.BuildPyramidTest(testCount, clearDestructibles, spawnDestructible);
            else if (scene2Action == 2) scene.BuildFlatTest(testCount, clearDestructibles, spawnDestructible);
            else scene.BuildCubeStack5000(clearDestructibles);
            ResetProperties(scene.GetVersion());
            ImGui::End();
            return;
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
    float wallBallSize = 6.0f;
    float wallBallMass = 100.0f;
    const char* T(const char* cn, const char* en) const { return chinese ? cn : en; }
    static bool Number(const char* label, float& value, float speed, float low, float high)
    {
        float old = value;
        if (!ImGui::DragFloat(label, &value, speed, low, high, "%.3f", ImGuiSliderFlags_AlwaysClamp)) return false;
        value = std::isfinite(value) ? std::clamp(value, low, high) : old;
        return value != old;
    }
};
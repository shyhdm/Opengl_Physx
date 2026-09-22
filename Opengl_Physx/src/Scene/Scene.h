#pragma once
#include "ModelLibrary.h"
#include "ModelRenderer.h"
#include "Transform.h"
#include "RigidBody.h"
#include "MousePicker.h"
#include "OutlineEffect.h"
#include "CollisionDebugRenderer.h"
#include "SoftBody.h"
#include <memory>
#include <array>
#include <vector>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/gtc/matrix_inverse.hpp>

// Scene 需要在 GL::Load() 之后创建，并在 Window 之前销毁。
class Scene
{
public:
    explicit Scene(bool useGpu = true) : world(useGpu)
    {
        // 可见平面位于 y=0，与现有静态地面碰撞体顶面重合。
        ground.position = glm::vec3(0.0f);
        groundMaterial.baseColor = glm::vec3(1.0f);
        groundMaterial.baseTexture = std::make_shared<Texture>("Assets/Textures/checker.png");
        for (int i = 0; i < static_cast<int>(ModelType::Count); ++i)
        {
            ModelType type = static_cast<ModelType>(i);
            models.Get(type);
            rigidRenderSources[static_cast<std::size_t>(type)] = ModelBuilder::Create(type);
        }
        Reset();
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    void Reset()
    {
        ClearSelection();
        softBodies.clear();
        bodies.clear();
        smokeFloorEnabled = false;
        smokeFloorId = 0;
        smokeFloorPositionInitialized = false;
        smokeFloorPosition = glm::vec3(0, 3.32f, 0);
        paused = false;
        if (sceneIndex == 1)
        {
            ground.scale = glm::vec3(60.0f, 1.0f, 60.0f);
            groundMaterial.textureTiling = glm::vec2(30.0f);
            SetGroundHalfExtent(30.0f);
            return;
        }
        if (sceneIndex == 2)
        {
            ground.scale = glm::vec3(30.0f, 1.0f, 30.0f);
            groundMaterial.textureTiling = glm::vec2(15.0f);
            SetGroundHalfExtent(15.0f);
            return;
        }
        ground.scale = glm::vec3(20.0f, 1.0f, 20.0f);
        groundMaterial.textureTiling = glm::vec2(10.0f);
        SetGroundHalfExtent(10.0f);
        // 六个方块组成三层小金字塔，再放一个高处下落的方块。
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3 - row; ++column)
                AddBox(glm::vec3((column - (2 - row) * 0.5f) * 1.05f, 0.5f + row * 1.02f, 0.0f));
        AddBox(glm::vec3(3.0f, 5.0f, 0.0f));
        for (int i = 0; i < static_cast<int>(ModelType::Count); ++i)
            AddBody(static_cast<ModelType>(i), glm::vec3(-6.0f + 2.0f * i, 3.0f, -4.0f), glm::vec3(0), glm::vec3(1.3f));
    }

    void AddBox(glm::vec3 position, glm::vec3 velocity = glm::vec3(0.0f))
    {
        AddBody(ModelType::Box, position, velocity);
    }

    void AddBody(ModelType type, glm::vec3 position, glm::vec3 velocity = glm::vec3(0), glm::vec3 scale = glm::vec3(1), float mass = 0.0f)
    {
        AddBody(type, position, velocity, scale, DefaultMaterial(type), mass);
    }

    void AddBody(ModelType type, glm::vec3 position, glm::vec3 velocity, glm::vec3 scale, const Material& material, float mass = 0.0f)
    {
        auto body = std::make_unique<RigidBody>(world, type, position, scale);
        if (body->IsDynamic())
        {
            if (std::isfinite(mass) && mass > 0.0f) { auto properties = body->GetProperties(); properties.mass = std::max(mass, minimumLaunchMass); body->SetProperties(properties); }
            body->SetVelocity(velocity);
        }
        bodies.push_back({ std::move(body),material,showCollisions,++nextObject });
    }

    void AddStaticBox(glm::vec3 position, glm::vec3 scale, glm::vec3 rotationDegrees, const Material& material)
    {
        auto body = std::make_unique<RigidBody>(world, ModelType::Box, position, scale, 10.0f, true, rotationDegrees);
        bodies.push_back({ std::move(body),material,showCollisions,++nextObject });
    }
    glm::vec3 GetSmokeFloorPosition() const { return smokeFloorPosition; }
    void SetSmokeFloorPosition(glm::vec3 position)
    {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
        smokeFloorPosition = position;
        smokeFloorPositionInitialized = true;
        for (auto& object : bodies)
            if (object.id == smokeFloorId)
            {
                auto* actor = const_cast<physx::PxRigidActor*>(object.body->GetActor());
                auto pose = actor->getGlobalPose();
                pose.p = physx::PxVec3(position.x, position.y, position.z);
                actor->setGlobalPose(pose);
                rigidBatchRevision = std::numeric_limits<unsigned long long>::max();
                break;
            }
    }
    void SetSmokeFloor(bool enabled, glm::vec3 emitterPosition)
    {
        enabled = enabled && sceneIndex == 2;
        if (smokeFloorEnabled == enabled) return;
        auto found = std::find_if(bodies.begin(), bodies.end(), [&](const auto& object) { return object.id == smokeFloorId; });
        if (found != bodies.end())
        {
            if (GetSelectedBody() == found->body.get()) ClearSelection();
            bodies.erase(found);
        }
        smokeFloorId = 0;
        if (enabled)
        {
            Material material;
            material.baseColor = glm::vec3(0.45f, 0.24f, 0.10f);
            if (!smokeFloorPositionInitialized)
            {
                smokeFloorPosition = emitterPosition + glm::vec3(0, 3.0f, 0);
                smokeFloorPositionInitialized = true;
            }
            AddStaticBox(smokeFloorPosition, glm::vec3(6.0f, 0.3f, 6.0f), glm::vec3(0), material);
            smokeFloorId = bodies.back().id;
        }
        smokeFloorEnabled = enabled;
    }
    void Shoot(const Camera& camera)
    {
        glm::vec3 forward = camera.GetForward();
        glm::vec3 position = FindLaunchPosition(camera.position, forward, GetLaunchRadius());
        if (spawnType == 1)
        {
            if (!world.GetCuda()) return;
            AddSoftBody(selectedType, position, forward * launchSpeed, launchScale);
        }
        else AddBody(selectedType, position, forward * launchSpeed, glm::vec3(launchScale), launchMass);
    }

    void AddSoftBody(ModelType type, glm::vec3 position, glm::vec3 velocity = glm::vec3(0), float scale = 1)
    {
        auto body = std::make_unique<SoftBody>(world, softModels, type, position, velocity, scale, softResolution);
        body->SetSimulationSettings(softIterations, softSelfCollision);
        softBodies.push_back({ std::move(body),DefaultMaterial(type),showCollisions,++nextObject });
    }

    void HandleInput(Window& window, const Camera& camera, bool mouseBlocked = false,
        const std::function<void(ModelType, glm::vec3, glm::vec3, float, float)>& destructibleShot = {},
        const std::function<void(glm::vec3, glm::vec3, float, float, float)>& predictiveShot = {})
    {
        unsigned int revision = window.GetFocusRevision();
        bool leftDown = window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
        bool middleDown = window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE);
        if (revision != focusRevision || !window.IsFocused())
        {
            focusRevision = revision;
            ReleaseDrag();
            leftWasDown = middleWasDown = true;
            firing = false;
            return;
        }
        if (mouseBlocked)
        {
            ReleaseDrag();
            leftWasDown = middleWasDown = true;
            firing = false;
            return;
        }
        double now = glfwGetTime();
        if (middleDown)
        {
            if (now >= nextShot && (!middleWasDown || firing))
            {
                glm::vec3 forward = camera.GetForward();
                float radius = GetLaunchRadius();
                glm::vec3 position = FindLaunchPosition(camera.position, forward, radius);
                glm::vec3 velocity = forward * launchSpeed;
                if (spawnType == 2 && destructibleShot) destructibleShot(selectedType, position, velocity, launchScale, launchMass);
                else Shoot(camera);
                if (predictiveShot)
                {
                    constexpr float pi = 3.14159265358979323846f;
                    float volume = 4.0f * pi * radius * radius * radius / 3.0f;
                    predictiveShot(position, velocity, radius, launchMass, volume);
                }
                firing = true;
                nextShot = now + std::clamp((radius * 2.0f + 0.25f) / std::max(launchSpeed, 1.0f), 0.15f, 0.2f);
            }
        }
        else firing = false;
        middleWasDown = middleDown;

        // 右键相机捕获鼠标时终止拖动；松开左键后才能再次选取。
        if (window.IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT))
        {
            ReleaseDrag();
            leftWasDown = true;
            return;
        }
        double x = 0, y = 0;
        int width = 0, height = 0;
        window.GetCursorPosition(x, y);
        window.GetSize(width, height); // GLFW鼠标坐标使用窗口尺寸，避免高DPI比例错误。
        glm::vec3 direction(0);
        bool valid = MousePicker::MakeRay(camera, x, y, width, height, direction);
        if (!leftDown || !valid) ReleaseDrag();
        if (leftDown && !leftWasDown && valid) BeginDrag(camera.position, direction);
        if (leftDown && valid) MoveDrag(camera.position, direction);
        leftWasDown = leftDown;
    }
    void Update(float deltaTime)
    {
        frameSimulationMs = 0; frameSteps = 0; frameSyncMs = 0;
        if (!paused)
        {
            world.Update(deltaTime, [this](float step) {if (selectedSoft) selectedSoft->UpdateDrag(step); });
            frameSimulationMs = world.GetLastSimulationMs(); frameSteps = world.GetLastSteps();
        }
        SyncSoftBodies();
        bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [this](const auto& body)
            {
                glm::vec3 position = body.body->GetPosition();
                bool remove = !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || position.y < -30.0f || glm::dot(position, position) > 100.0f * 100.0f;
                if (remove) picker.Forget(body.body->GetActor());
                return remove;
            }), bodies.end());
    }

    void Draw(const Camera& camera, int width, int height, const std::function<void(ModelRenderer&, bool)>& externalDraw = {})
    {
        if (width <= 0 || height <= 0) return;
        auto uploadStarted = std::chrono::steady_clock::now();
        SyncSoftBodies();
        UpdateRigidRenderBatches();
        frameUploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStarted).count();
        auto drawStarted = std::chrono::steady_clock::now();
        auto shadowStarted = drawStarted;
        renderer.BeginShadowPass();
        renderer.DrawShadow(models.Get(ModelType::Plane), ground.GetMatrix());
        for (const auto& batch : rigidRenderBatches) renderer.DrawShadow(*batch.mesh, glm::mat4(1.0f));
        for (const auto& batch : softRenderBatches) renderer.DrawShadow(*batch.mesh, glm::mat4(1));
        if (externalDraw) externalDraw(renderer, true);
        renderer.EndShadowPass();
        frameShadowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowStarted).count();
        auto mainStarted = std::chrono::steady_clock::now();
        glDepthMask(GL_TRUE);
        glClearColor(0.10f, 0.16f, 0.24f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        renderer.BeginDraw(camera, width, height);
        renderer.DrawMesh(models.Get(ModelType::Plane), ground.GetMatrix(), groundMaterial);
        for (const auto& batch : rigidRenderBatches) renderer.DrawMesh(*batch.mesh, glm::mat4(1.0f), batch.material);
        for (const auto& batch : softRenderBatches) renderer.DrawMesh(*batch.mesh, glm::mat4(1), batch.material);
        if (externalDraw) externalDraw(renderer, false);
        std::vector<const physx::PxRigidActor*> visibleCollisions;
        for (const auto& object : bodies) if (object.showMesh) visibleCollisions.push_back(object.body->GetActor());
        if (externalCollisions) externalCollisions(visibleCollisions, showCollisions);
        if (showCollisions || !visibleCollisions.empty()) collisionDebug.Draw(world, camera, width, height, &visibleCollisions, showCollisions);
        for (const auto& object : softBodies)
            if (object.showMesh) collisionDebug.DrawSoft(object.body->GetMesh(), camera, width, height);
        if (selectedSoft) outline.Draw(selectedSoft->GetMesh(), camera, width, height, glm::mat4(1));
        for (const auto& body : bodies)
            if (picker.GetSelected() == body.body->GetActor())
                outline.Draw(models.Get(body.body->GetModelType()), camera, width, height, body.body->GetMatrix());
        if (externalOutline && picker.GetSelected()) externalOutline(outline, camera, width, height, picker.GetSelected());
        frameMainDrawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mainStarted).count();
        frameDrawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawStarted).count();
    }
    bool Select(glm::vec3 origin, glm::vec3 direction)
    {
        bool hit = BeginSelection(origin, direction);
        ReleaseDrag();
        return hit;
    }
    struct Selection
    {
        RigidBody* body = nullptr;
        SoftBody* soft = nullptr;
        Material* material = nullptr;
        bool* mesh = nullptr;
        std::uint64_t id = 0;
        physx::PxRigidDynamic* actor = nullptr;
        ModelType type = ModelType::Box;
        bool external = false;
    };

    Selection GetSelection()
    {
        if (selectedSoft)
        {
            for (auto& object : softBodies)
                if (object.body.get() == selectedSoft)
                    return { nullptr,object.body.get(),&object.material,&object.showMesh,object.id,nullptr,object.body->GetModelType(),false };
            return {};
        }
        auto* actor = picker.GetSelected();
        if (actor)
            for (auto& object : bodies)
                if (object.body->GetActor() == actor)
                    return { object.body.get(),nullptr,&object.material,&object.showMesh,object.id,const_cast<physx::PxRigidDynamic*>(actor->is<physx::PxRigidDynamic>()),object.body->GetModelType(),false };
        if (actor && externalSelection)
        {
            ModelType type = ModelType::Box;
            bool* mesh = nullptr;
            std::uint64_t id = 0;
            if (externalSelection(actor, type, mesh, id)) return { nullptr,nullptr,nullptr,mesh,id,const_cast<physx::PxRigidDynamic*>(actor->is<physx::PxRigidDynamic>()),type,true };
        }
        return {};
    }

    void SetExternalRigidHandlers(
        std::function<bool(const physx::PxRigidActor*, ModelType&, bool*&, std::uint64_t&)> selection,
        std::function<void(std::vector<const physx::PxRigidActor*>&, bool)> collisions,
        std::function<void(OutlineEffect&, const Camera&, int, int, const physx::PxRigidActor*)> drawOutline,
        std::function<void(bool)> setMeshes)
    {
        externalSelection = std::move(selection);
        externalCollisions = std::move(collisions);
        externalOutline = std::move(drawOutline);
        externalSetMeshes = std::move(setMeshes);
    }

    void ForgetActor(const physx::PxRigidActor* actor) { picker.Forget(actor); }
    void ClearRigidSelection() { picker.Clear(); }

    std::uint64_t GetVersion() const { return version; }

    RigidBody* GetSelectedBody() { return GetSelection().body; }
    SoftBody* GetSelectedSoftBody() { return GetSelection().soft; }
    bool GetSpawnSoft() const { return spawnType == 1; }
    void SetSpawnSoft(bool value) { spawnType = value && SoftBodiesAvailable() ? 1 : 0; }
    int GetSpawnType() const { return spawnType; }
    void SetSpawnType(int value)
    {
        if (value < 0 || value > 2) throw std::invalid_argument("Invalid spawn type.");
        spawnType = value == 1 && !SoftBodiesAvailable() ? 0 : value;
    }
    float GetLaunchSpeed() const { return launchSpeed; }
    float GetLaunchScale() const { return launchScale; }
    float GetTestScale() const { return testScale; }
    float GetLaunchMass() const { return launchMass; }
    void SetLaunchSettings(float speed, float scale, float mass)
    {
        if (std::isfinite(speed)) launchSpeed = std::clamp(speed, 0.0f, 100.0f);
        if (std::isfinite(scale)) launchScale = std::clamp(scale, 0.1f, 10.0f);
        if (std::isfinite(mass)) launchMass = std::clamp(mass, minimumLaunchMass, 10000.0f);
    }
    void SetTestScale(float scale)
    {
        if (std::isfinite(scale)) testScale = std::clamp(scale, 0.1f, 10.0f);
    }
    bool UsesGpu() const { return world.GetCuda() != nullptr; }
    PhysicsWorld& GetPhysicsWorld() { return world; }
    void RequestGpu(bool value) { requestedMode = value ? 1 : 0; }
    int GetRequestedMode() const { return requestedMode; }
    int GetSceneIndex() const { return sceneIndex; }
    void SetSceneIndex(int value)
    {
        if (value < 0 || value > 2) throw std::invalid_argument("Invalid scene index.");
        if (sceneIndex == value) return;
        sceneIndex = value;
        Reset();
    }
    bool SoftBodiesAvailable() const { return world.GetCuda() != nullptr; }
    std::vector<physx::PxRigidActor*> GetSoftFlowColliders()
    {
        std::vector<physx::PxRigidActor*> result;
        result.reserve(softBodies.size());
        for (auto& object : softBodies) result.push_back(object.body->GetFlowCollider());
        return result;
    }
    std::size_t GetSoftBodyCount() const { return softBodies.size(); }
    Material* GetSelectedMaterial() { return GetSelection().material; }
    void DeleteSelected()
    {
        if (selectedSoft)
        {
            auto* target = selectedSoft; selectedSoft = nullptr;
            softBodies.erase(std::remove_if(softBodies.begin(), softBodies.end(), [target](const auto& object) {return object.body.get() == target; }), softBodies.end());
            return;
        }
        auto* actor = picker.GetSelected();
        picker.Clear();
        bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [actor](const auto& object) {return object.body->GetActor() == actor; }), bodies.end());
    }
    void SetPaused(bool value) { paused = value; ReleaseDrag(); world.ClearAccumulator(); }
    ModelType GetSpawnModel() const { return selectedType; }
    void SetSpawnModel(ModelType type)
    {
        if (static_cast<int>(type) < 0 || type >= ModelType::Count) throw std::invalid_argument("Invalid model type.");
        selectedType = type;
    }
    bool GetShowCollisions() const { return showCollisions; }

    void SetShowCollisions(bool value)
    {
        showCollisions = value;
        for (auto& object : bodies) object.showMesh = value;
        for (auto& object : softBodies) object.showMesh = value;
        if (externalSetMeshes) externalSetMeshes(value);
    }

    bool* GetSelectedMeshVisibility() { return GetSelection().mesh; }

    bool BeginDrag(glm::vec3 origin, glm::vec3 direction)
    {
        bool hit = BeginSelection(origin, direction);
        if (paused) ReleaseDrag();
        return hit;
    }

    void MoveDrag(glm::vec3 origin, glm::vec3 direction)
    {
        picker.Move(origin, direction);
        if (selectedSoft && selectedSoft->IsDragging()) selectedSoft->MoveDrag(origin, direction);
    }

    void ReleaseDrag()
    {
        picker.ReleaseDrag();
        if (selectedSoft) selectedSoft->StopDrag();
    }
    bool IsPaused() const { return paused; }
    void SingleStep() { SetPaused(true); world.SingleStep(); }
    std::size_t GetBodyCount() const { return bodies.size() + 1; }

    unsigned int GetSoftIterations() const { return softIterations; }
    bool GetSoftSelfCollision() const { return softSelfCollision; }
    unsigned int GetSoftResolution() const { return softResolution; }
    void SetSoftSettings(unsigned int iterations, bool selfCollision, unsigned int resolution)
    {
        softIterations = std::clamp(iterations, 4u, 16u);
        softSelfCollision = selfCollision;
        softResolution = std::clamp(resolution, 4u, 12u);
        for (auto& object : softBodies) object.body->SetSimulationSettings(softIterations, softSelfCollision);
    }
    std::size_t GetSoftTetrahedronCount() const
    {
        std::size_t count = 0;
        for (const auto& object : softBodies) count += object.body->GetSimulationTetrahedronCount();
        return count;
    }
    double GetSimulationMs() const { return frameSimulationMs; }
    double GetSoftSyncMs() const { return frameSyncMs; }
    double GetRenderUploadMs() const { return frameUploadMs; }
    double GetShadowDrawMs() const { return frameShadowMs; }
    double GetMainDrawMs() const { return frameMainDrawMs; }
    double GetDrawSubmitMs() const { return frameDrawMs; }
    unsigned int GetPhysicsSteps() const { return frameSteps; }

    void BuildSoftTest(bool stacked, int count, bool singleColumn = false)
    {
        BuildTest(stacked, count, singleColumn);
    }

    void BuildPyramidTest(int count, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float, float)>& spawnDestructible = {})
    {
        auto bounds = physx::PxBounds3::empty();
        if (spawnType == 1)
        {
            if (!SoftBodiesAvailable()) return;
            auto* cooked = softModels.Get(selectedType, softResolution)->getCollisionMesh();
            for (physx::PxU32 i = 0; i < cooked->getNbVertices(); ++i) bounds.include(cooked->getVertices()[i]);
        }
        else
        {
            auto model = ModelBuilder::Create(selectedType);
            for (const auto& vertex : model.vertices) bounds.include(physx::PxVec3(vertex.x, vertex.y, vertex.z));
        }
        bounds.minimum *= testScale;
        bounds.maximum *= testScale;
        auto size = bounds.maximum - bounds.minimum;
        ClearSelection();
        softBodies.clear(); bodies.clear(); world.ClearAccumulator();
        if (clearDestructibles) clearDestructibles();
        count = std::clamp(count, 1, 500);
        int rows = 1;
        while (rows * (rows + 1) / 2 < count) ++rows;
        int remaining = count;
        float stepX = size.x + 0.08f, stepY = size.y + 0.08f;
        for (int row = 0; row < rows && remaining > 0; ++row)
        {
            int rowCount = std::min(rows - row, remaining);
            float y = 0.3f - bounds.minimum.y + static_cast<float>(row) * stepY;
            float startX = -0.5f * static_cast<float>(rowCount - 1) * stepX;
            for (int column = 0; column < rowCount; ++column)
            {
                glm::vec3 position(startX + static_cast<float>(column) * stepX, y, 0.0f);
                if (spawnType == 1) AddSoftBody(selectedType, position, glm::vec3(0), testScale);
                else if (spawnType == 2 && spawnDestructible) spawnDestructible(selectedType, position, testScale, testDestructibleMass);
                else AddBody(selectedType, position, glm::vec3(0), glm::vec3(testScale));
            }
            remaining -= rowCount;
        }
        ++version;
    }

    void BuildFlatTest(int count, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float, float)>& spawnDestructible = {})
    {
        auto bounds = physx::PxBounds3::empty();
        if (spawnType == 1)
        {
            if (!SoftBodiesAvailable()) return;
            auto* cooked = softModels.Get(selectedType, softResolution)->getCollisionMesh();
            for (physx::PxU32 i = 0; i < cooked->getNbVertices(); ++i) bounds.include(cooked->getVertices()[i]);
        }
        else
        {
            auto model = ModelBuilder::Create(selectedType);
            for (const auto& vertex : model.vertices) bounds.include(physx::PxVec3(vertex.x, vertex.y, vertex.z));
            if (selectedType == ModelType::Plane) bounds.minimum.y = -0.04f;
        }
        bounds.minimum *= testScale;
        bounds.maximum *= testScale;
        auto size = bounds.maximum - bounds.minimum;
        ClearSelection();
        softBodies.clear(); bodies.clear(); world.ClearAccumulator();
        if (clearDestructibles) clearDestructibles();
        count = std::clamp(count, 1, 500);
        int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
        int rows = (count + columns - 1) / columns;
        float stepX = std::max(size.x, 0.05f) + 0.12f;
        float stepZ = std::max(size.z, 0.05f) + 0.12f;
        float y = 0.3f - bounds.minimum.y;
        for (int index = 0; index < count; ++index)
        {
            int row = index / columns, column = index % columns;
            int rowCount = std::min(columns, count - row * columns);
            float x = (static_cast<float>(column) - static_cast<float>(rowCount - 1) * 0.5f) * stepX;
            float z = (static_cast<float>(row) - static_cast<float>(rows - 1) * 0.5f) * stepZ;
            glm::vec3 position(x, y, z);
            if (spawnType == 1) AddSoftBody(selectedType, position, glm::vec3(0), testScale);
            else if (spawnType == 2 && spawnDestructible) spawnDestructible(selectedType, position, testScale, testDestructibleMass);
            else AddBody(selectedType, position, glm::vec3(0), glm::vec3(testScale));
        }
        paused = false;
        ++version;
    }

    void BuildCubeStack5000(const std::function<void()>& clearDestructibles = {})
    {
        ModelData model = ModelBuilder::Create(ModelType::Box);
        auto bounds = physx::PxBounds3::empty();
        for (const auto& vertex : model.vertices) bounds.include(physx::PxVec3(vertex.x, vertex.y, vertex.z));
        bounds.minimum *= testScale;
        bounds.maximum *= testScale;
        auto size = bounds.maximum - bounds.minimum;
        ClearSelection();
        softBodies.clear(); bodies.clear(); world.ClearAccumulator();
        if (clearDestructibles) clearDestructibles();
        constexpr int count = 5000;
        constexpr int side = 17;
        constexpr int layerCapacity = side * side;
        constexpr int fullCubeCount = side * side * side;
        constexpr int capCount = count - fullCubeCount;
        constexpr int capColumns = 10;
        constexpr int capRows = (capCount + capColumns - 1) / capColumns;
        float stepX = size.x + 0.015f, stepY = size.y + 0.015f, stepZ = size.z + 0.015f;
        for (int index = 0; index < count; ++index)
        {
            int layer = index / layerCapacity, withinLayer = index % layerCapacity;
            float gridX = 0.0f, gridZ = 0.0f;
            if (index < fullCubeCount)
            {
                gridX = static_cast<float>(withinLayer % side) - static_cast<float>(side - 1) * 0.5f;
                gridZ = static_cast<float>(withinLayer / side) - static_cast<float>(side - 1) * 0.5f;
            }
            else
            {
                int capIndex = index - fullCubeCount;
                int capRow = capIndex / capColumns, capColumn = capIndex % capColumns;
                int rowCount = std::min(capColumns, capCount - capRow * capColumns);
                gridX = static_cast<float>(capColumn) - static_cast<float>(rowCount - 1) * 0.5f;
                gridZ = static_cast<float>(capRow) - static_cast<float>(capRows - 1) * 0.5f;
            }
            float x = gridX * stepX;
            float y = 0.02f - bounds.minimum.y + static_cast<float>(layer) * stepY;
            float z = gridZ * stepZ;
            AddBody(ModelType::Box, glm::vec3(x, y, z), glm::vec3(0), glm::vec3(testScale));
        }
        paused = false;
        ++version;
    }

    // 场景 2：在可碎裂墙体背面生成一条宽弧形坡道，并从高端释放重球。
    // 墙体位于 z=-2，球从更小的 Z 坐标处沿 +Z 方向滚动并撞击墙体背面。
    RigidBody* BuildWallRollingTest(float ballScale = 6.0f, float ballMass = 100.0f)
    {
        if (!std::isfinite(ballScale)) ballScale = 6.0f;
        if (!std::isfinite(ballMass)) ballMass = 100.0f;
        ballScale = std::clamp(ballScale, 0.5f, 12.0f);
        ballMass = std::clamp(ballMass, minimumLaunchMass, 100000.0f);
        ClearSelection();
        softBodies.clear();
        bodies.clear();
        world.ClearAccumulator();

        Material rampMaterial;
        rampMaterial.baseColor = glm::vec3(0.32f, 0.36f, 0.42f);
        rampMaterial.specularStrength = 0.18f;
        rampMaterial.shininess = 24.0f;

        constexpr int segmentCount = 48;
        constexpr float nearZ = -3.30f;
        constexpr float farZ = -42.0f;
        constexpr float baseHeight = 0.35f;
        constexpr float rise = 32.0f;
        constexpr float curvePower = 2.20f;
        constexpr float rampWidth = 12.0f;
        constexpr float rampThickness = 0.65f;

        auto surfaceHeight = [=](float t)
            {
                return baseHeight + rise * std::pow(std::clamp(t, 0.0f, 1.0f), curvePower);
            };

        for (int segment = 0; segment < segmentCount; ++segment)
        {
            float t0 = static_cast<float>(segment) / static_cast<float>(segmentCount);
            float t1 = static_cast<float>(segment + 1) / static_cast<float>(segmentCount);
            float z0 = nearZ + (farZ - nearZ) * t0;
            float z1 = nearZ + (farZ - nearZ) * t1;
            float y0 = surfaceHeight(t0);
            float y1 = surfaceHeight(t1);
            float dz = z1 - z0;
            float dy = y1 - y0;
            // 局部 +Z 轴朝向墙体；局部 +Y 因而始终是坡道朝上的法线。
            float angle = glm::degrees(std::atan2(dy, -dz));
            float radians = glm::radians(angle);
            glm::vec3 topNormal(0.0f, std::cos(radians), std::sin(radians));
            glm::vec3 position(0.0f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f);
            position -= topNormal * (rampThickness * 0.5f);
            float length = std::sqrt(dz * dz + dy * dy) + 0.10f;
            AddStaticBox(position, glm::vec3(rampWidth, rampThickness, length), glm::vec3(angle, 0.0f, 0.0f), rampMaterial);
        }

        // 高端挡板既贴合参考图，也避免重球从坡道背面滑落。
        float highY = surfaceHeight(1.0f);
        AddStaticBox(glm::vec3(0.0f, highY + 3.50f, farZ - 0.325f),
            glm::vec3(rampWidth, 7.0f, 0.65f), glm::vec3(0.0f), rampMaterial);

        float ballRadius = ballScale * 0.5f;
        constexpr float ballT = 0.88f;
        float ballSurfaceZ = nearZ + (farZ - nearZ) * ballT;
        float ballSurfaceY = surfaceHeight(ballT);
        float ballSlope = rise * curvePower * std::pow(ballT, curvePower - 1.0f) / (nearZ - farZ);
        float ballAngle = std::atan(ballSlope);
        glm::vec3 ballNormal(0.0f, std::cos(ballAngle), std::sin(ballAngle));
        glm::vec3 ballPosition = glm::vec3(0.0f, ballSurfaceY, ballSurfaceZ) + ballNormal * (ballRadius + 0.18f);
        Material ballMaterial;
        ballMaterial.baseColor = glm::vec3(0.82f, 0.16f, 0.08f);
        ballMaterial.specularStrength = 0.70f;
        ballMaterial.shininess = 96.0f;
        AddBody(ModelType::Sphere, ballPosition, glm::vec3(0.0f, 0.0f, 0.75f),
            glm::vec3(ballScale), ballMaterial, ballMass);
        RigidBody* ball = bodies.back().body.get();
        if (ball)
        {
            auto properties = ball->GetProperties();
            properties.staticFriction = 0.90f;
            properties.dynamicFriction = 0.65f;
            properties.restitution = 0.05f;
            properties.angularDamping = 0.05f;
            ball->SetProperties(properties);
        }

        paused = false;
        ++version;
        return ball;
    }

    void BuildTest(bool stacked, int count, bool singleColumn = false, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float, float)>& spawnDestructible = {})
    {
        auto bounds = physx::PxBounds3::empty();
        if (spawnType == 1)
        {
            if (!SoftBodiesAvailable()) return;
            auto* cooked = softModels.Get(selectedType, softResolution)->getCollisionMesh();
            for (physx::PxU32 i = 0; i < cooked->getNbVertices(); ++i) bounds.include(cooked->getVertices()[i]);
        }
        else
        {
            auto model = ModelBuilder::Create(selectedType);
            for (const auto& vertex : model.vertices) bounds.include(physx::PxVec3(vertex.x, vertex.y, vertex.z));
            if (selectedType == ModelType::Plane) bounds.minimum.y = -0.04f;
        }
        bounds.minimum *= testScale;
        bounds.maximum *= testScale;
        auto size = bounds.maximum - bounds.minimum;
        ClearSelection();
        softBodies.clear(); bodies.clear(); world.ClearAccumulator();
        if (clearDestructibles) clearDestructibles();
        count = std::max(count, 1);
        int x = 0, z = 0, dx = 1, dz = 0, length = 1, steps = 0, turns = 0;
        for (int i = 0; i < count; ++i)
        {
            int layer = singleColumn ? i : stacked ? i % 4 : 0;
            glm::vec3 position(x * (size.x + 0.7f), 0.3f - bounds.minimum.y + layer * (size.y + 0.08f), z * (size.z + 0.7f));
            if (spawnType == 1) AddSoftBody(selectedType, position, glm::vec3(0), testScale);
            else if (spawnType == 2 && spawnDestructible) spawnDestructible(selectedType, position, testScale, testDestructibleMass);
            else AddBody(selectedType, position, glm::vec3(0), glm::vec3(testScale));
            if (!singleColumn && (!stacked || i % 4 == 3))
            {
                x += dx; z += dz;
                if (++steps == length)
                {
                    steps = 0;
                    int previous = dx; dx = -dz; dz = previous;
                    if (++turns % 2 == 0) ++length;
                }
            }
        }
        paused = false;
    }

private:
    float GetLaunchRadius() const
    {
        ModelData model = ModelBuilder::Create(selectedType);
        float radiusSquared = 0.0f;
        for (const Vertex& vertex : model.vertices) radiusSquared = std::max(radiusSquared, vertex.x * vertex.x + vertex.y * vertex.y + vertex.z * vertex.z);
        return std::sqrt(radiusSquared) * launchScale;
    }

    glm::vec3 FindLaunchPosition(glm::vec3 origin, glm::vec3 direction, float radius) const
    {
        return origin + direction * (radius + 0.25f);
    }

    void ClearSelection()
    {
        ReleaseDrag();
        picker.Clear();
        selectedSoft = nullptr;
        leftWasDown = middleWasDown = true;
        firing = false;
        world.ClearAccumulator();
        version = ++nextVersion;
    }

    bool BeginSelection(glm::vec3 origin, glm::vec3 direction)
    {
        ReleaseDrag();
        selectedSoft = nullptr;
        picker.Clear();
        float rayLength = glm::length(direction);
        if (!std::isfinite(rayLength) || rayLength < 0.000001f) return false;
        direction /= rayLength;
        float distance = 100.0f;
        physx::PxRaycastBuffer hit;
        if (world.GetScene().raycast(physx::PxVec3(origin.x, origin.y, origin.z), physx::PxVec3(direction.x, direction.y, direction.z), distance, hit) && hit.hasBlock)
            distance = hit.block.distance;
        for (auto& object : softBodies) if (object.body->Raycast(origin, direction, distance)) selectedSoft = object.body.get();
        if (selectedSoft)
        {
            picker.Clear();
            if (!paused) selectedSoft->BeginDrag(origin, direction, distance);
            return true;
        }
        return picker.Begin(world, origin, direction);
    }
    void SyncSoftBodies()
    {
        auto started = std::chrono::steady_clock::now();
        std::vector<SoftBody*> updates;
        updates.reserve(softBodies.size());
        for (auto& object : softBodies) if (object.showMesh || object.body.get() == selectedSoft) updates.push_back(object.body.get());
        SoftBody::SyncBatch(updates);
        softBodies.erase(std::remove_if(softBodies.begin(), softBodies.end(), [this](const auto& object) {
            auto p = object.body->GetPosition();
            bool remove = !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.y < -30 || glm::dot(p, p)>10000;
            if (remove && selectedSoft == object.body.get()) selectedSoft = nullptr;
            return remove;
            }), softBodies.end());
        UpdateSoftRenderBatches();
        frameSyncMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    struct SoftObject
    {
        std::unique_ptr<SoftBody> body;
        Material material;
        bool showMesh = false;
        std::uint64_t id = 0;
    };
    struct SceneObject
    {
        std::unique_ptr<RigidBody> body;
        Material material;
        bool showMesh = false;
        std::uint64_t id = 0;
    };

    struct RigidRenderBatch
    {
        Material material;
        std::vector<std::size_t> objects;
        std::vector<Vertex> vertices;
        std::unique_ptr<Mesh> mesh;
    };
    struct SoftRenderBatch
    {
        Material material;
        std::vector<std::size_t> objects;
        std::vector<std::size_t> byteOffsets;
        std::vector<Vertex> vertices;
        std::unique_ptr<Mesh> mesh;
        std::unique_ptr<SoftGpuBuffer> gpu;
    };

    static bool SameMaterial(const Material& a, const Material& b)
    {
        return a.baseColor == b.baseColor && a.specularStrength == b.specularStrength && a.shininess == b.shininess &&
            a.baseTexture.get() == b.baseTexture.get() && a.textureTiling == b.textureTiling;
    }

    static void MixHash(std::uint64_t& hash, std::uint64_t value)
    {
        hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    }

    std::uint64_t GetRigidBatchSignature() const
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        MixHash(hash, bodies.size());
        for (const SceneObject& object : bodies)
        {
            MixHash(hash, object.id);
            MixHash(hash, static_cast<std::uint64_t>(object.body->GetModelType()));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.x));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.y));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.z));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.specularStrength));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.shininess));
            MixHash(hash, reinterpret_cast<std::uintptr_t>(object.material.baseTexture.get()));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.textureTiling.x));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.textureTiling.y));
        }
        return hash;
    }

    void RebuildRigidRenderBatches(std::uint64_t signature)
    {
        rigidRenderBatches.clear();
        for (std::size_t objectIndex = 0; objectIndex < bodies.size(); ++objectIndex)
        {
            const Material& material = bodies[objectIndex].material;
            auto found = std::find_if(rigidRenderBatches.begin(), rigidRenderBatches.end(), [&](const RigidRenderBatch& batch) { return SameMaterial(batch.material, material); });
            if (found == rigidRenderBatches.end())
            {
                rigidRenderBatches.push_back({ material });
                found = std::prev(rigidRenderBatches.end());
            }
            found->objects.push_back(objectIndex);
        }
        for (RigidRenderBatch& batch : rigidRenderBatches)
        {
            std::size_t vertexCount = 0, indexCount = 0;
            for (std::size_t objectIndex : batch.objects)
            {
                const ModelData& source = rigidRenderSources[static_cast<std::size_t>(bodies[objectIndex].body->GetModelType())];
                vertexCount += source.vertices.size(); indexCount += source.indices.size();
            }
            batch.vertices.reserve(vertexCount);
            std::vector<unsigned int> indices; indices.reserve(indexCount);
            unsigned int baseVertex = 0;
            for (std::size_t objectIndex : batch.objects)
            {
                const ModelData& source = rigidRenderSources[static_cast<std::size_t>(bodies[objectIndex].body->GetModelType())];
                batch.vertices.insert(batch.vertices.end(), source.vertices.begin(), source.vertices.end());
                for (unsigned int index : source.indices) indices.push_back(baseVertex + index);
                baseVertex += static_cast<unsigned int>(source.vertices.size());
            }
            batch.mesh = std::make_unique<Mesh>(batch.vertices, indices);
        }
        rigidBatchSignature = signature;
        rigidBatchRevision = std::numeric_limits<unsigned long long>::max();
    }

    void UpdateRigidRenderBatches()
    {
        std::uint64_t signature = GetRigidBatchSignature();
        if (signature != rigidBatchSignature) RebuildRigidRenderBatches(signature);
        unsigned long long revision = world.GetSimulationRevision();
        if (revision == rigidBatchRevision) return;
        for (RigidRenderBatch& batch : rigidRenderBatches)
        {
            std::size_t output = 0;
            for (std::size_t objectIndex : batch.objects)
            {
                const SceneObject& object = bodies[objectIndex];
                const ModelData& source = rigidRenderSources[static_cast<std::size_t>(object.body->GetModelType())];
                glm::mat4 matrix = object.body->GetMatrix();
                glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(matrix)));
                for (const Vertex& input : source.vertices)
                {
                    Vertex& vertex = batch.vertices[output++];
                    glm::vec4 position = matrix * glm::vec4(input.x, input.y, input.z, 1.0f);
                    glm::vec3 normal = normalMatrix * glm::vec3(input.nx, input.ny, input.nz);
                    float length = glm::length(normal);
                    if (length > 0.000001f) normal /= length; else normal = glm::vec3(0.0f, 1.0f, 0.0f);
                    vertex = input;
                    vertex.x = position.x; vertex.y = position.y; vertex.z = position.z;
                    vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
                }
            }
            batch.mesh->UpdateVertices(batch.vertices);
        }
        rigidBatchRevision = revision;
    }

    std::uint64_t GetSoftBatchSignature() const
    {
        std::uint64_t hash = 0x84222325CBF29CE4ull;
        MixHash(hash, softBodies.size());
        for (const SoftObject& object : softBodies)
        {
            MixHash(hash, object.id);
            MixHash(hash, static_cast<std::uint64_t>(object.body->GetModelType()));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.x));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.y));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.baseColor.z));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.specularStrength));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.shininess));
            MixHash(hash, reinterpret_cast<std::uintptr_t>(object.material.baseTexture.get()));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.textureTiling.x));
            MixHash(hash, std::bit_cast<std::uint32_t>(object.material.textureTiling.y));
        }
        return hash;
    }

    void RebuildSoftRenderBatches(std::uint64_t signature)
    {
        softRenderBatches.clear();
        for (std::size_t objectIndex = 0; objectIndex < softBodies.size(); ++objectIndex)
        {
            const Material& material = softBodies[objectIndex].material;
            auto found = std::find_if(softRenderBatches.begin(), softRenderBatches.end(), [&](const SoftRenderBatch& batch) { return SameMaterial(batch.material, material); });
            if (found == softRenderBatches.end()) { softRenderBatches.push_back({ material }); found = std::prev(softRenderBatches.end()); }
            found->objects.push_back(objectIndex);
        }
        for (SoftRenderBatch& batch : softRenderBatches)
        {
            std::vector<unsigned int> indices;
            unsigned int baseVertex = 0;
            for (std::size_t objectIndex : batch.objects)
            {
                const auto& sourceVertices = softBodies[objectIndex].body->GetRenderVertices();
                const auto& sourceIndices = softBodies[objectIndex].body->GetRenderIndices();
                batch.byteOffsets.push_back(static_cast<std::size_t>(baseVertex) * sizeof(physx::PxVec4));
                batch.vertices.insert(batch.vertices.end(), sourceVertices.begin(), sourceVertices.end());
                for (unsigned int index : sourceIndices) indices.push_back(baseVertex + index);
                baseVertex += static_cast<unsigned int>(sourceVertices.size());
            }
            if (!batch.vertices.empty())
            {
                batch.mesh = std::make_unique<Mesh>(batch.vertices, indices);
                batch.gpu = std::make_unique<SoftGpuBuffer>(*world.GetCuda(), *batch.mesh, batch.vertices, indices);
            }
        }
        softBatchSignature = signature;
        softBatchRevision = std::numeric_limits<unsigned long long>::max();
    }

    void UpdateSoftRenderBatches()
    {
        std::uint64_t signature = GetSoftBatchSignature();
        if (signature != softBatchSignature) RebuildSoftRenderBatches(signature);
        unsigned long long revision = world.GetSimulationRevision();
        if (revision == softBatchRevision) return;
        for (SoftRenderBatch& batch : softRenderBatches)
        {
            std::vector<SoftGpuBuffer::Region> regions;
            regions.reserve(batch.objects.size());
            for (std::size_t i = 0; i < batch.objects.size(); ++i)
            {
                SoftBody& body = *softBodies[batch.objects[i]].body;
                regions.push_back({ body.GetGpuRenderPositions(),batch.byteOffsets[i],body.GetRenderVertices().size() * sizeof(physx::PxVec4) });
            }
            if (batch.gpu) batch.gpu->UpdateRegions(regions);
        }
        softBatchRevision = revision;
    }

    static Material DefaultMaterial(ModelType type)
    {
        switch (type)
        {
        case ModelType::Box: return { {0.75f,0.18f,0.10f},0.25f,32.0f };
        case ModelType::Plane: return { {0.55f,0.60f,0.65f},0.05f,8.0f };
        case ModelType::Sphere: return { {0.12f,0.35f,0.85f},0.65f,96.0f };
        case ModelType::Cylinder: return { {0.16f,0.60f,0.28f},0.20f,24.0f };
        case ModelType::Cone: return { {0.85f,0.55f,0.12f},0.15f,16.0f };
        case ModelType::Capsule: return { {0.55f,0.22f,0.75f},0.45f,64.0f };
        case ModelType::Torus: return { {0.12f,0.65f,0.65f},0.50f,64.0f };
        default: throw std::invalid_argument("Unknown material model.");
        }
    }

    void SetGroundHalfExtent(float halfExtent)
    {
        physx::PxActor* actors[1]{};
        if (world.GetScene().getActors(physx::PxActorTypeFlag::eRIGID_STATIC, actors, 1) != 1) throw std::runtime_error("Cannot find PhysX ground actor.");
        auto* rigidGround = static_cast<physx::PxRigidStatic*>(actors[0]);
        physx::PxShape* shapes[1]{};
        if (rigidGround->getShapes(shapes, 1) != 1) throw std::runtime_error("Cannot find PhysX ground shape.");
        shapes[0]->setGeometry(physx::PxBoxGeometry(halfExtent, 0.5f, halfExtent));
    }

    // 成员按声明的相反顺序销毁，必须让 bodies 先于 world 销毁。
    inline static std::uint64_t nextObject = 0, nextVersion = 0;
    std::uint64_t version = 0;
    PhysicsWorld world;
    SoftMeshLibrary softModels{ world.GetPhysics() };
    ModelLibrary models;
    std::array<ModelData, static_cast<std::size_t>(ModelType::Count)> rigidRenderSources;
    ModelRenderer renderer;
    OutlineEffect outline;
    CollisionDebugRenderer collisionDebug;
    Transform ground;
    Material groundMaterial = { glm::vec3(0.38f),0.03f,8.0f };
    std::vector<SceneObject> bodies;
    std::vector<RigidRenderBatch> rigidRenderBatches;
    std::uint64_t rigidBatchSignature = 0;
    unsigned long long rigidBatchRevision = std::numeric_limits<unsigned long long>::max();
    std::vector<SoftObject> softBodies;
    std::vector<SoftRenderBatch> softRenderBatches;
    std::uint64_t softBatchSignature = 0;
    unsigned long long softBatchRevision = std::numeric_limits<unsigned long long>::max();
    SoftBody* selectedSoft = nullptr;
    unsigned int softIterations = 8, softResolution = 4, frameSteps = 0;
    bool softSelfCollision = false;
    double frameSimulationMs = 0, frameSyncMs = 0, frameUploadMs = 0, frameShadowMs = 0, frameMainDrawMs = 0, frameDrawMs = 0;
    int requestedMode = -1;
    glm::vec3 smokeFloorPosition = glm::vec3(0, 3.32f, 0);
    bool smokeFloorPositionInitialized = false;
    bool smokeFloorEnabled = false;
    std::uint64_t smokeFloorId = 0;
    int sceneIndex = 0;
    int spawnType = 0;
    static constexpr float minimumLaunchMass = 0.01f;
    static constexpr float testDestructibleMass = 1.0f;
    float launchSpeed = 20.0f, launchScale = 1.0f, testScale = 1.0f, launchMass = 1.0f;
    MousePicker picker; // 后声明，先释放关节，再销毁bodies。
    bool paused = false;
    bool showCollisions = false;
    bool firing = false;
    double nextShot = 0;
    bool middleWasDown = false;
    bool leftWasDown = false;
    unsigned int focusRevision = 0;
    ModelType selectedType = ModelType::Box;
    std::function<bool(const physx::PxRigidActor*, ModelType&, bool*&, std::uint64_t&)> externalSelection;
    std::function<void(std::vector<const physx::PxRigidActor*>&, bool)> externalCollisions;
    std::function<void(OutlineEffect&, const Camera&, int, int, const physx::PxRigidActor*)> externalOutline;
    std::function<void(bool)> externalSetMeshes;
};

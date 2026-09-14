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
#include <vector>
#include <algorithm>
#include <cstdint>
#include <functional>

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
        for (int i = 0; i < static_cast<int>(ModelType::Count); ++i) models.Get(static_cast<ModelType>(i));
        Reset();
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    void Reset()
    {
        ClearSelection();
        softBodies.clear();
        bodies.clear();
        paused = false;
        if (sceneIndex == 1)
        {
            ground.scale = glm::vec3(60.0f, 1.0f, 60.0f);
            groundMaterial.textureTiling = glm::vec2(30.0f);
            SetGroundHalfExtent(30.0f);
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

    void AddBody(ModelType type, glm::vec3 position, glm::vec3 velocity = glm::vec3(0), glm::vec3 scale = glm::vec3(1))
    {
        AddBody(type, position, velocity, scale, DefaultMaterial(type));
    }

    void AddBody(ModelType type, glm::vec3 position, glm::vec3 velocity, glm::vec3 scale, const Material& material)
    {
        auto body = std::make_unique<RigidBody>(world, type, position, scale);
        if (body->IsDynamic()) body->SetVelocity(velocity);
        bodies.push_back({ std::move(body),material,showCollisions,++nextObject });
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
        else AddBody(selectedType, position, forward * launchSpeed, glm::vec3(launchScale));
    }

    void AddSoftBody(ModelType type, glm::vec3 position, glm::vec3 velocity = glm::vec3(0), float scale = 1)
    {
        auto body = std::make_unique<SoftBody>(world, softModels, type, position, velocity, scale, softResolution);
        body->SetSimulationSettings(softIterations, softSelfCollision);
        softBodies.push_back({ std::move(body),DefaultMaterial(type),showCollisions,++nextObject });
    }

    void HandleInput(Window& window, const Camera& camera, bool mouseBlocked = false, const std::function<void(ModelType, glm::vec3, glm::vec3, float)>& destructibleShot = {})
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
                if (spawnType == 2 && destructibleShot) destructibleShot(selectedType, position, forward * launchSpeed, launchScale);
                else Shoot(camera);
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
        SyncSoftBodies();
        auto drawStarted = std::chrono::steady_clock::now();
        renderer.BeginShadowPass();
        renderer.DrawShadow(models.Get(ModelType::Plane), ground.GetMatrix());
        for (const auto& body : bodies)
            renderer.DrawShadow(models.Get(body.body->GetModelType()), body.body->GetMatrix());
        for (const auto& object : softBodies) renderer.DrawShadow(object.body->GetMesh(), glm::mat4(1));
        if (externalDraw) externalDraw(renderer, true);
        renderer.EndShadowPass();
        glDepthMask(GL_TRUE);
        glClearColor(0.10f, 0.16f, 0.24f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        renderer.BeginDraw(camera, width, height);
        renderer.DrawMesh(models.Get(ModelType::Plane), ground.GetMatrix(), groundMaterial);
        for (const auto& body : bodies)
            renderer.DrawMesh(models.Get(body.body->GetModelType()), body.body->GetMatrix(), body.material);
        for (const auto& object : softBodies)
            renderer.DrawMesh(object.body->GetMesh(), glm::mat4(1), object.material);
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
    void SetLaunchSettings(float speed, float scale)
    {
        if (std::isfinite(speed)) launchSpeed = std::clamp(speed, 0.0f, 100.0f);
        if (std::isfinite(scale)) launchScale = std::clamp(scale, 0.1f, 10.0f);
    }
    bool UsesGpu() const { return world.GetCuda() != nullptr; }
    PhysicsWorld& GetPhysicsWorld() { return world; }
    void RequestGpu(bool value) { requestedMode = value ? 1 : 0; }
    int GetRequestedMode() const { return requestedMode; }
    int GetSceneIndex() const { return sceneIndex; }
    void SetSceneIndex(int value)
    {
        if (value < 0 || value > 1) throw std::invalid_argument("Invalid scene index.");
        if (sceneIndex == value) return;
        sceneIndex = value;
        Reset();
    }
    bool SoftBodiesAvailable() const { return world.GetCuda() != nullptr; }
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
    double GetDrawSubmitMs() const { return frameDrawMs; }
    unsigned int GetPhysicsSteps() const { return frameSteps; }

    void BuildSoftTest(bool stacked, int count, bool singleColumn = false)
    {
        BuildTest(stacked, count, singleColumn);
    }

    void BuildTest(bool stacked, int count, bool singleColumn = false, const std::function<void()>& clearDestructibles = {}, const std::function<void(ModelType, glm::vec3, float)>& spawnDestructible = {})
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
        bounds.minimum *= launchScale;
        bounds.maximum *= launchScale;
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
            if (spawnType == 1) AddSoftBody(selectedType, position, glm::vec3(0), launchScale);
            else if (spawnType == 2 && spawnDestructible) spawnDestructible(selectedType, position, launchScale);
            else AddBody(selectedType, position, glm::vec3(0), glm::vec3(launchScale));
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
        for (auto& object : softBodies) updates.push_back(object.body.get());
        SoftBody::SyncBatch(updates);
        softBodies.erase(std::remove_if(softBodies.begin(), softBodies.end(), [this](const auto& object) {
            auto p = object.body->GetPosition();
            bool remove = !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.y < -30 || glm::dot(p, p)>10000;
            if (remove && selectedSoft == object.body.get()) selectedSoft = nullptr;
            return remove;
            }), softBodies.end());
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
    ModelRenderer renderer;
    OutlineEffect outline;
    CollisionDebugRenderer collisionDebug;
    Transform ground;
    Material groundMaterial = { glm::vec3(0.38f),0.03f,8.0f };
    std::vector<SceneObject> bodies;
    std::vector<SoftObject> softBodies;
    SoftBody* selectedSoft = nullptr;
    unsigned int softIterations = 8, softResolution = 4, frameSteps = 0;
    bool softSelfCollision = false;
    double frameSimulationMs = 0, frameSyncMs = 0, frameDrawMs = 0;
    int requestedMode = -1;
    int sceneIndex = 0;
    int spawnType = 0;
    float launchSpeed = 20.0f, launchScale = 1.0f;
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

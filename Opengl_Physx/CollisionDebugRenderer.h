#pragma once
#include "PhysicsWorld.h"
#include "Camera.h"
#include "Shader.h"
#include <cstdint>
#include <limits>
#include <vector>
#include <unordered_map>
#include <unordered_set>

class CollisionDebugRenderer
{
public:
    struct Point { float x, y, z, r, g, b; };
    CollisionDebugRenderer() : shader("Assets/Shaders/model.glsl", { "collision" })
    {
        GL::GenVertexArrays(1, &vao);
        GL::GenBuffers(1, &vbo);
        if (!vao || !vbo)
        {
            if (vao) GL::DeleteVertexArrays(1, &vao);
            if (vbo) GL::DeleteBuffers(1, &vbo);
            throw std::runtime_error("Cannot create collision debug buffer.");
        }
        GL::BindVertexArray(vao);
        GL::BindBuffer(GL::ArrayBuffer, vbo);
        GL::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point), nullptr);
        GL::EnableVertexAttribArray(0);
        GL::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(3 * sizeof(float)));
        GL::EnableVertexAttribArray(1);
        GL::BindVertexArray(0);
        GL::BindBuffer(GL::ArrayBuffer, 0);
    }
    ~CollisionDebugRenderer()
    {
        GL::DeleteBuffers(1, &vbo);
        GL::DeleteVertexArrays(1, &vao);
    }
    CollisionDebugRenderer(const CollisionDebugRenderer&) = delete;
    CollisionDebugRenderer& operator=(const CollisionDebugRenderer&) = delete;

    void AppendShape(std::vector<Point>& points, const physx::PxShape& shape, const physx::PxRigidActor& actor, glm::vec3 color)
    {
        using namespace physx;
        PxTransform pose = actor.getGlobalPose() * shape.getLocalPose();
        auto line = [&](PxVec3 a, PxVec3 b) {
            a = pose.transform(a); b = pose.transform(b);
            points.push_back({ a.x,a.y,a.z,color.r,color.g,color.b });
            points.push_back({ b.x,b.y,b.z,color.r,color.g,color.b });
            };
        const auto& geometry = shape.getGeometry();
        constexpr int segments = 32;
        constexpr float pi = 3.14159265358979323846f;
        switch (geometry.getType())
        {
        case PxGeometryType::eBOX:
        {
            auto h = static_cast<const PxBoxGeometry&>(geometry).halfExtents;
            PxVec3 v[8];
            for (int i = 0; i < 8; ++i) v[i] = PxVec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
            for (int i = 0; i < 8; ++i) for (int bit = 1; bit <= 4; bit *= 2) if (!(i & bit)) line(v[i], v[i | bit]);
            break;
        }
        case PxGeometryType::eSPHERE:
        {
            float radius = static_cast<const PxSphereGeometry&>(geometry).radius;
            for (int axis = 0; axis < 3; ++axis) for (int i = 0; i < segments; ++i)
            {
                float a = 2 * pi * i / segments, b = 2 * pi * (i + 1) / segments;
                PxVec3 p(0), q(0);
                p[(axis + 1) % 3] = radius * std::cos(a); p[(axis + 2) % 3] = radius * std::sin(a);
                q[(axis + 1) % 3] = radius * std::cos(b); q[(axis + 2) % 3] = radius * std::sin(b);
                line(p, q);
            }
            break;
        }
        case PxGeometryType::eCAPSULE:
        {
            auto& capsule = static_cast<const PxCapsuleGeometry&>(geometry);
            float r = capsule.radius, h = capsule.halfHeight;
            for (int sign : {-1, 1})
            {
                for (int i = 0; i < segments; ++i)
                {
                    float a = 2 * pi * i / segments, b = 2 * pi * (i + 1) / segments;
                    line(PxVec3(sign * h, r * std::cos(a), r * std::sin(a)), PxVec3(sign * h, r * std::cos(b), r * std::sin(b)));
                }
                for (int axis = 1; axis <= 2; ++axis) for (int i = 0; i < segments / 2; ++i)
                {
                    float a = -pi / 2 + pi * i / (segments / 2), b = -pi / 2 + pi * (i + 1) / (segments / 2);
                    PxVec3 p(sign * (h + r * std::cos(a)), 0, 0), q(sign * (h + r * std::cos(b)), 0, 0);
                    p[axis] = r * std::sin(a); q[axis] = r * std::sin(b); line(p, q);
                }
            }
            for (int i = 0; i < 4; ++i)
            {
                float a = pi * i / 2;
                line(PxVec3(-h, r * std::cos(a), r * std::sin(a)), PxVec3(h, r * std::cos(a), r * std::sin(a)));
            }
            break;
        }
        case PxGeometryType::eCONVEXMESH:
        {
            auto& convex = static_cast<const PxConvexMeshGeometry&>(geometry);
            auto scale = convex.scale.toMat33();
            auto found = convexEdges.find(convex.convexMesh);
            if (found == convexEdges.end())
            {
                CachedConvex cache;
                const auto* vertices = convex.convexMesh->getVertices();
                const auto* indices = convex.convexMesh->getIndexBuffer();
                std::unordered_set<std::uint64_t> unique;
                unique.reserve(static_cast<std::size_t>(convex.convexMesh->getNbPolygons()) * 4);
                for (PxU32 i = 0; i < convex.convexMesh->getNbPolygons(); ++i)
                {
                    PxHullPolygon polygon;
                    if (!convex.convexMesh->getPolygonData(i, polygon)) continue;
                    for (PxU32 j = 0; j < polygon.mNbVerts; ++j)
                    {
                        PxU32 a = indices[polygon.mIndexBase + j], b = indices[polygon.mIndexBase + (j + 1) % polygon.mNbVerts];
                        std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
                        if (unique.insert(key).second) cache.edges.push_back({ vertices[a],vertices[b] });
                    }
                }
                found = convexEdges.emplace(convex.convexMesh, std::move(cache)).first;
            }
            for (const Edge& edge : found->second.edges) line(scale * edge.a, scale * edge.b);
            break;
        }
        default: break;
        }
    }

    void DrawSoft(const Mesh& mesh, const Camera& camera, int width, int height)
    {
        GLint polygon[2] = {}, program = 0, oldVao = 0;
        GLboolean depthWrite = GL_TRUE;
        bool depthTest = glIsEnabled(GL_DEPTH_TEST) != 0;
        glGetIntegerv(GL_POLYGON_MODE, polygon); glGetIntegerv(0x8B8D, &program); glGetIntegerv(0x85B5, &oldVao);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        shader.UsePass("collision");
        shader.SetMatrix4("mvp", camera.GetProjectionMatrix(float(width) / height) * camera.GetViewMatrix());
        shader.SetBool("overrideCollisionColor", true);
        shader.SetVector3("collisionTint", glm::vec3(0.2f, 1.0f, 0.35f));
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        mesh.Draw();
        shader.SetBool("overrideCollisionColor", false);
        glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(polygon[0]));
        glDepthMask(depthWrite); if (depthTest) glEnable(GL_DEPTH_TEST);
        GL::UseProgram(static_cast<GLuint>(program)); GL::BindVertexArray(static_cast<GLuint>(oldVao));
    }

    void Draw(PhysicsWorld& world, const Camera& camera, int width, int height, const std::vector<const physx::PxRigidActor*>* visible = nullptr, bool includeStatic = true)
    {
        if (width <= 0 || height <= 0) return;
        using namespace physx;
        PxActorTypeFlags types = PxActorTypeFlag::eRIGID_STATIC | PxActorTypeFlag::eRIGID_DYNAMIC;
        std::uint64_t visibility = includeStatic ? 0x9E3779B97F4A7C15ull : 0xCBF29CE484222325ull;
        visibility ^= static_cast<std::uint64_t>(world.GetScene().getNbActors(types)) + (visibility << 6) + (visibility >> 2);
        if (visible) for (const PxRigidActor* actor : *visible) visibility ^= reinterpret_cast<std::uintptr_t>(actor) + 0x9E3779B97F4A7C15ull + (visibility << 6) + (visibility >> 2);
        unsigned long long revision = world.GetSimulationRevision();
        bool rebuild = revision != cachedRevision || visibility != cachedVisibility;
        if (rebuild)
        {
            if (visibility != cachedVisibility) convexEdges.clear();
            points.clear();
            auto appendActor = [&](PxRigidActor* actor)
                {
                    if (!actor || (actor->is<PxRigidStatic>() && !includeStatic)) return;
                    shapes.resize(actor->getNbShapes());
                    PxU32 shapeCount = actor->getShapes(shapes.data(), static_cast<PxU32>(shapes.size()));
                    glm::vec3 color = actor->is<PxRigidDynamic>() ? glm::vec3(0.2f, 1.0f, 0.35f) : glm::vec3(0.4f, 0.65f, 1.0f);
                    for (PxU32 j = 0; j < shapeCount; ++j)
                        if (shapes[j]->getFlags().isSet(PxShapeFlag::eSIMULATION_SHAPE)) AppendShape(points, *shapes[j], *actor, color);
                };
            if (visible && !includeStatic)
            {
                for (const PxRigidActor* actor : *visible) appendActor(const_cast<PxRigidActor*>(actor));
            }
            else
            {
                std::unordered_set<const PxRigidActor*> allowed;
                if (visible) { allowed.reserve(visible->size()); allowed.insert(visible->begin(), visible->end()); }
                actors.resize(world.GetScene().getNbActors(types));
                PxU32 count = world.GetScene().getActors(types, actors.data(), static_cast<PxU32>(actors.size()));
                for (PxU32 i = 0; i < count; ++i)
                {
                    auto* actor = actors[i]->is<PxRigidActor>();
                    if (actor->is<PxRigidDynamic>() && visible && !allowed.contains(actor)) continue;
                    appendActor(actor);
                }
            }
            cachedRevision = revision;
            cachedVisibility = visibility;
        }
        if (points.empty()) return;
        GLint oldVao = 0, oldBuffer = 0, oldProgram = 0;
        GLboolean depthWrite = GL_TRUE;
        bool depthTest = glIsEnabled(GL_DEPTH_TEST) != 0;
        GLfloat lineWidth = 1;
        glGetIntegerv(0x85B5, &oldVao); glGetIntegerv(0x8894, &oldBuffer); glGetIntegerv(0x8B8D, &oldProgram);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite); glGetFloatv(GL_LINE_WIDTH, &lineWidth);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glLineWidth(1);
        shader.UsePass("collision");
        shader.SetMatrix4("mvp", camera.GetProjectionMatrix(float(width) / height) * camera.GetViewMatrix());
        GL::BindVertexArray(vao); GL::BindBuffer(GL::ArrayBuffer, vbo);
        if (rebuild) GL::BufferData(GL::ArrayBuffer, static_cast<std::ptrdiff_t>(points.size() * sizeof(Point)), points.data(), 0x88E0);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(points.size()));
        GL::BindVertexArray(static_cast<GLuint>(oldVao)); GL::BindBuffer(GL::ArrayBuffer, static_cast<GLuint>(oldBuffer));
        GL::UseProgram(static_cast<GLuint>(oldProgram));
        glLineWidth(lineWidth); glDepthMask(depthWrite); if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    }
private:
    Shader shader;
    GLuint vao = 0, vbo = 0;
    std::vector<Point> points;
    std::vector<physx::PxActor*> actors;
    std::vector<physx::PxShape*> shapes;
    struct Edge { physx::PxVec3 a, b; };
    struct CachedConvex { std::vector<Edge> edges; };
    std::unordered_map<const physx::PxConvexMesh*, CachedConvex> convexEdges;
    unsigned long long cachedRevision = std::numeric_limits<unsigned long long>::max();
    std::uint64_t cachedVisibility = 0;
};

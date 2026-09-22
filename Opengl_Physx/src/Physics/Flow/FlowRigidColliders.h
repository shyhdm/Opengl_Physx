#pragma once
#include <PxPhysicsAPI.h>
#include <nvflowext/NvFlowExt.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

class FlowRigidColliders
{
public:
    void Reset() { records_.clear(); indices_.clear(); pointers_.clear(); freeSlots_.clear(); }
    void Update(physx::PxScene& scene, float cellSize = 0.1f, const std::vector<physx::PxRigidActor*>& proxies = {})
    {
        using namespace physx;
        for (auto& record : records_) record->seen = false;
        const auto types = PxActorTypeFlag::eRIGID_STATIC | PxActorTypeFlag::eRIGID_DYNAMIC;
        actors_.resize(scene.getNbActors(types));
        scene.getActors(types, actors_.data(), static_cast<PxU32>(actors_.size()));
        actors_.insert(actors_.end(), proxies.begin(), proxies.end());
        for (auto* actor : actors_)
        {
            auto* rigid = actor->is<PxRigidActor>();
            if (!rigid || (actor->getActorFlags() & PxActorFlag::eDISABLE_SIMULATION)) continue;
            shapes_.resize(rigid->getNbShapes());
            rigid->getShapes(shapes_.data(), static_cast<PxU32>(shapes_.size()));
            for (auto* shape : shapes_)
            {
                if (!(shape->getFlags() & PxShapeFlag::eSIMULATION_SHAPE)) continue;
                auto key = std::make_pair(reinterpret_cast<uintptr_t>(rigid), reinterpret_cast<uintptr_t>(shape));
                auto found = indices_.find(key);
                size_t index;
                if (found == indices_.end())
                {
                    if (freeSlots_.empty())
                    {
                        index = records_.size();
                        records_.push_back(std::make_unique<Record>());
                    }
                    else
                    {
                        index = freeSlots_.back(); freeSlots_.pop_back();
                        *records_[index] = Record{};
                    }
                    indices_[key] = index;
                }
                else index = found->second;
                auto& record = *records_[index];
                const bool hadPose = record.active;
                record.seen = true;
                record.planes.clear();
                NvFlowEmitterBoxParams params = NvFlowEmitterBoxParams_default;
                PxTransform pose = rigid->getGlobalPose() * shape->getLocalPose();
                PxMat44 transform(pose);
                const auto& geometry = shape->getGeometry();
                if (!Geometry(geometry, record, params, transform)) { record.active = false; continue; }
                static_assert(sizeof(params.localToWorld) == sizeof(PxMat44));
                std::memcpy(&params.localToWorld, &transform, sizeof(transform));
                if (geometry.getType() == PxGeometryType::eBOX)
                {
                    const float minimumHalfSize = 0.5f * std::clamp(cellSize, 0.08f, 1.0f);
                    params.halfSize.x = std::max(params.halfSize.x, minimumHalfSize);
                    params.halfSize.y = std::max(params.halfSize.y, minimumHalfSize);
                    params.halfSize.z = std::max(params.halfSize.z, minimumHalfSize);
                }
                params.luid = static_cast<NvFlowUint64>(index + 1) * 2;
                params.velocityIsWorldSpace = NV_FLOW_TRUE;
                params.velocity = { 0,0,0 };
                if (!hadPose)
                {
                    if (auto* dynamic = rigid->is<PxRigidDynamic>())
                    {
                        auto center = rigid->getGlobalPose().transform(dynamic->getCMassLocalPose().p);
                        auto velocity = dynamic->getLinearVelocity() + dynamic->getAngularVelocity().cross(pose.p - center);
                        params.velocity = { velocity.x,velocity.y,velocity.z };
                    }
                }
                params.physicsVelocityScale = hadPose ? 1.0f : 0.0f;
                params.isPhysicsCollision = NV_FLOW_TRUE;
                params.allocationScale = 0;
                params.temperature = params.fuel = params.burn = params.smoke = params.divergence = 0;
                params.coupleRateVelocity = 1e6f;
                params.coupleRateDivergence = 0;
                params.coupleRateTemperature = params.coupleRateFuel = params.coupleRateBurn = params.coupleRateSmoke = 1e6f;
                params.multisample = NV_FLOW_TRUE;
                params.clippingPlanes = record.planes.data();
                params.clippingPlaneCount = record.planes.size();
                record.pre = params;
                record.post = params;
                record.post.luid++;
                record.post.applyPostPressure = NV_FLOW_TRUE;
                record.pre.coupleRateTemperature = record.pre.coupleRateFuel = record.pre.coupleRateBurn = record.pre.coupleRateSmoke = 0;
                record.active = true;
            }
        }
        pointers_.clear();
        for (auto& record : records_)
        {
            if (!record->seen) record->active = false;
            record->pre.clippingPlanes = record->post.clippingPlanes = record->planes.data();
            record->pre.clippingPlaneCount = record->post.clippingPlaneCount = record->planes.size();
            record->pre.enabled = record->post.enabled = record->active ? NV_FLOW_TRUE : NV_FLOW_FALSE;
            pointers_.push_back(reinterpret_cast<NvFlowUint8*>(&record->pre));
            pointers_.push_back(reinterpret_cast<NvFlowUint8*>(&record->post));
        }
        for (auto it = indices_.begin(); it != indices_.end();)
            if (!records_[it->second]->seen) { freeSlots_.push_back(it->second); it = indices_.erase(it); }
            else ++it;
    }
    NvFlowUint8** Data() { return pointers_.data(); }
    NvFlowUint64 Count() const { return pointers_.size(); }
private:
    struct Record
    {
        NvFlowEmitterBoxParams pre = NvFlowEmitterBoxParams_default, post = NvFlowEmitterBoxParams_default;
        std::vector<NvFlowFloat4> planes;
        bool active = false, seen = false;
    };
    static bool Geometry(const physx::PxGeometry& geometry, Record& record, NvFlowEmitterBoxParams& params, physx::PxMat44& transform)
    {
        using namespace physx;
        switch (geometry.getType())
        {
        case PxGeometryType::eBOX:
        {
            auto size = static_cast<const PxBoxGeometry&>(geometry).halfExtents;
            params.halfSize = { size.x,size.y,size.z };
            break;
        }
        case PxGeometryType::eSPHERE:
        case PxGeometryType::eCAPSULE:
        {
            const bool capsule = geometry.getType() == PxGeometryType::eCAPSULE;
            const float radius = capsule ? static_cast<const PxCapsuleGeometry&>(geometry).radius : static_cast<const PxSphereGeometry&>(geometry).radius;
            const float height = capsule ? static_cast<const PxCapsuleGeometry&>(geometry).halfHeight : 0;
            params.halfSize = { radius + height,radius,radius };
            for (int ring = 1; ring < 8; ++ring)
            {
                float theta = PxPi * float(ring) / 8;
                for (int segment = 0; segment < 16; ++segment)
                {
                    float phi = PxTwoPi * float(segment) / 16;
                    PxVec3 normal(std::cos(theta), std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi));
                    record.planes.push_back({ normal.x,normal.y,normal.z,-radius - height * std::abs(normal.x) });
                }
            }
            record.planes.push_back({ 1,0,0,-radius - height });
            record.planes.push_back({ -1,0,0,-radius - height });
            break;
        }
        case PxGeometryType::eCONVEXMESH:
        {
            const auto& convex = static_cast<const PxConvexMeshGeometry&>(geometry);
            auto bounds = convex.convexMesh->getLocalBounds();
            auto center = bounds.getCenter(), size = bounds.getExtents();
            params.position = { center.x,center.y,center.z };
            params.halfSize = { size.x,size.y,size.z };
            transform = transform * PxMat44(convex.scale.toMat33(), PxVec3(0));
            for (PxU32 i = 0; i < convex.convexMesh->getNbPolygons(); ++i)
            {
                PxHullPolygon polygon{};
                if (convex.convexMesh->getPolygonData(i, polygon))
                    record.planes.push_back({ polygon.mPlane[0],polygon.mPlane[1],polygon.mPlane[2],polygon.mPlane[3] });
            }
            break;
        }
        default: return false;
        }
        return params.halfSize.x > 0 && params.halfSize.y > 0 && params.halfSize.z > 0;
    }
    std::map<std::pair<uintptr_t, uintptr_t>, size_t> indices_;
    std::vector<std::unique_ptr<Record>> records_;
    std::vector<size_t> freeSlots_;
    std::vector<NvFlowUint8*> pointers_;
    std::vector<physx::PxActor*> actors_;
    std::vector<physx::PxShape*> shapes_;
};

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Physics - PhysicsWorld implementation (module IMPLEMENTATION unit).
//
// ALL Jolt contact lives here: the interface stays JPH-free (API hygiene + GCC's module
// serializer cannot digest Jolt's headers inside an interface unit's global fragment).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h> // AppendBodyTriangles
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h> // CollideShape* (ShapeOverlap)
#include <Jolt/Physics/Collision/ShapeCast.h>    // RShapeCast / ShapeCast* (ShapeCast)
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <atomic>
#include <cmath>
#include <cstring>

module foundation.physics;

import foundation.core;

using namespace foundation::core;

namespace foundation::physics
{
    // ---- layers ----
    // ObjectLayer encoding: (semantic << 8) | designer group. Semantics keep the fixed
    // hard-coded rules (static-static never, trigger sensing); groups add the designer
    // matrix on top.
    namespace layers
    {
        constexpr JPH::ObjectLayer kStatic = 0;
        constexpr JPH::ObjectLayer kTrigger = 3;

        constexpr JPH::BroadPhaseLayer kBpStatic{0};
        constexpr JPH::BroadPhaseLayer kBpMoving{1};
        constexpr JPH::uint kBpCount = 2;

        [[nodiscard]] inline JPH::ObjectLayer From(PhysicsLayer layer, u8 group)
        {
            return static_cast<JPH::ObjectLayer>((static_cast<u32>(layer) << 8) | (group & 0x1Fu));
        }
        [[nodiscard]] inline JPH::ObjectLayer Semantic(JPH::ObjectLayer layer)
        {
            return static_cast<JPH::ObjectLayer>(layer >> 8);
        }
        [[nodiscard]] inline u32 Group(JPH::ObjectLayer layer)
        {
            return static_cast<u32>(layer & 0x1Fu);
        }

        // Semantic rules: statics never pair with statics; triggers sense everything
        // that moves; everything else collides.
        [[nodiscard]] inline bool SemanticCollides(JPH::ObjectLayer a, JPH::ObjectLayer b)
        {
            if (a == kStatic && b == kStatic)
            {
                return false;
            }
            if (a == kTrigger && b == kTrigger)
            {
                return false;
            }
            if ((a == kTrigger && b == kStatic) || (a == kStatic && b == kTrigger))
            {
                return false;
            }
            return true;
        }
    }

    namespace
    {
        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
        {
        public:
            [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override
            {
                return layers::kBpCount;
            }
            [[nodiscard]] JPH::BroadPhaseLayer
            GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
            {
                return layers::Semantic(layer) == layers::kStatic ? layers::kBpStatic
                                                                  : layers::kBpMoving;
            }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override
            {
                return "bp";
            }
#endif
        };

        class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                             JPH::BroadPhaseLayer bp) const override
            {
                if (layers::Semantic(layer) == layers::kStatic)
                {
                    return bp == layers::kBpMoving;
                }
                return true;
            }
        };

        class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
        {
        public:
            u32 groupCollides[kCollisionGroupCount] = {}; // copied from world settings

            [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
            {
                if (!layers::SemanticCollides(layers::Semantic(a), layers::Semantic(b)))
                {
                    return false;
                }
                const u32 groupA = layers::Group(a);
                const u32 groupB = layers::Group(b);
                return (groupCollides[groupA] & (1u << groupB)) != 0 &&
                       (groupCollides[groupB] & (1u << groupA)) != 0;
            }
        };

        // Query-side filter: match bodies whose GROUP bit is in the mask.
        class GroupMaskFilter final : public JPH::ObjectLayerFilter
        {
        public:
            explicit GroupMaskFilter(u32 mask) : m_mask(mask) {}
            [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override
            {
                return (m_mask & (1u << layers::Group(layer))) != 0;
            }

        private:
            u32 m_mask;
        };

        [[nodiscard]] JPH::Vec3 ToJph(Float3 v) { return JPH::Vec3(v.x, v.y, v.z); }
        [[nodiscard]] JPH::Quat ToJph(Quaternion q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
        [[nodiscard]] Float3 FromJph(JPH::Vec3 v) { return Float3{v.GetX(), v.GetY(), v.GetZ()}; }
        [[nodiscard]] Quaternion FromJph(JPH::Quat q)
        {
            return Quaternion{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
        }

        // Run `fn(const JPH::Shape&)` with a STACK-constructed convex query volume
        // (sphere/box/capsule) for ShapeCast / ShapeOverlap - Jolt's transient-shape idiom
        // (SetEmbedded marks the refcount so a query cannot delete a stack object), chosen over
        // heap Refs because these queries run per projectile per frame (auto-aim). Other
        // ShapeKinds are body shapes, not query shapes -> false (the query no-ops safely).
        // Dimensions come straight off the script surface, so non-positive sizes are rejected
        // here (false -> no-hit) instead of tripping Jolt's debug asserts / feeding the
        // broadphase NaNs.
        template <typename Fn> [[nodiscard]] bool WithQueryShape(const QueryShape& q, Fn&& fn)
        {
            switch (q.kind)
            {
            case ShapeKind::Box:
            {
                const f32 minExtent =
                    Min(q.halfExtents.x, Min(q.halfExtents.y, q.halfExtents.z));
                if (!(minExtent > 0.0f))
                {
                    return false;
                }
                // Jolt asserts halfExtent >= convexRadius; shrink the radius for thin boxes.
                const float convexRadius =
                    Min(JPH::cDefaultConvexRadius, minExtent * 0.5f);
                JPH::BoxShape shape(ToJph(q.halfExtents), convexRadius);
                shape.SetEmbedded();
                fn(shape);
                return true;
            }
            case ShapeKind::Sphere:
            {
                if (!(q.radius > 0.0f))
                {
                    return false;
                }
                JPH::SphereShape shape(q.radius);
                shape.SetEmbedded();
                fn(shape);
                return true;
            }
            case ShapeKind::Capsule:
            {
                if (!(q.radius > 0.0f) || !(q.halfHeight > 0.0f))
                {
                    return false;
                }
                JPH::CapsuleShape shape(q.halfHeight, q.radius);
                shape.SetEmbedded();
                fn(shape);
                return true;
            }
            default:
                return false;
            }
        }

        // Process-wide Jolt bring-up (allocator/factory/types), refcounted across worlds and
        // cooks. Under a lock, not a bare counter: parallel cooks (the cook driver builds on job
        // workers) each acquire, and a second caller must not use Jolt before the first has
        // finished registering it - nor a release tear it down under a caller still acquiring.
        Mutex g_joltMutex;
        i32 g_joltUsers = 0;
        void AcquireJolt()
        {
            ScopedLock lock(g_joltMutex);
            if (g_joltUsers++ == 0)
            {
                JPH::RegisterDefaultAllocator();
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }
        }
        void ReleaseJolt()
        {
            ScopedLock lock(g_joltMutex);
            if (--g_joltUsers == 0)
            {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
        }

        // Cooked blobs are Jolt binary shape state; these adapters bridge it to Array<byte>.
        struct BlobOut final : JPH::StreamOut
        {
            Array<byte>& blob;
            explicit BlobOut(Array<byte>& b) : blob(b) {}
            void WriteBytes(const void* data, size_t count) override
            {
                const usize offset = blob.Size();
                blob.Resize(offset + count);
                std::memcpy(blob.Data() + offset, data, count);
            }
            [[nodiscard]] bool IsFailed() const override { return false; }
        };

        struct BlobIn final : JPH::StreamIn
        {
            Span<const byte> blob;
            usize cursor = 0;
            bool failed = false;
            explicit BlobIn(Span<const byte> b) : blob(b) {}
            void ReadBytes(void* out, size_t count) override
            {
                if (cursor + count > blob.Size())
                {
                    failed = true;
                    return;
                }
                std::memcpy(out, blob.Data() + cursor, count);
                cursor += count;
            }
            // istream semantics: EOF only trips when a read runs PAST the end - Jolt
            // checks IsEOF() after a fully-consumed successful restore.
            [[nodiscard]] bool IsEOF() const override { return failed; }
            [[nodiscard]] bool IsFailed() const override { return failed; }
        };

        [[nodiscard]] JPH::Ref<JPH::Shape> RestoreCooked(Span<const byte> blob)
        {
            // Jolt indexes its construct table with the leading subtype byte UNVALIDATED -
            // reject out-of-range values before handing over a corrupt/foreign blob.
            if (blob.IsEmpty() || static_cast<JPH::uint>(blob[0]) >= JPH::NumSubShapeTypes)
            {
                return {};
            }
            BlobIn in(blob);
            JPH::Shape::ShapeResult result = JPH::Shape::sRestoreFromBinaryState(in);
            if (in.IsFailed() || !result.IsValid())
            {
                return {};
            }
            return result.Get();
        }

        // Build a Jolt HeightFieldShape from a square grid of world-Y height floats. Jolt maps
        // sample (x, z) to world = offset + scale * (x, height, z); we centre the footprint on the
        // shape origin (offset.xz = -worldSize/2, scale.xz = worldSize/(n-1), scale.y = 1, height =
        // the world-Y value). Jolt itself rounds the sample count up to its block size and fills the
        // padding with cNoCollisionValue, so we pass the grid straight through - never hand-padded.
        [[nodiscard]] JPH::Ref<JPH::Shape> BuildHeightfield(const ShapeDesc& desc)
        {
            const u32 n = desc.heightSampleCount;
            if (n < 2 || desc.heightSamples.Size() < static_cast<usize>(n) * n)
            {
                return {};
            }
            const f32 sx = desc.heightWorldSize.x / static_cast<f32>(n - 1);
            const f32 sz = desc.heightWorldSize.y / static_cast<f32>(n - 1);
            JPH::HeightFieldShapeSettings settings(
                desc.heightSamples.Data(),
                JPH::Vec3(-desc.heightWorldSize.x * 0.5f, 0.0f, -desc.heightWorldSize.y * 0.5f),
                JPH::Vec3(sx, 1.0f, sz), n);
            JPH::Shape::ShapeResult result = settings.Create();
            return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>{};
        }

        [[nodiscard]] JPH::Ref<JPH::Shape> BuildOne(const ShapeDesc& desc, f32 density)
        {
            JPH::Ref<JPH::Shape> shape;
            switch (desc.kind)
            {
            case ShapeKind::Box:
            {
                // Jolt asserts halfExtent >= convexRadius: a thin box (an imported model's
                // placeholder body box is 0.01) gets a smaller radius, as a query box does.
                const f32 minExtent =
                    Min(desc.halfExtents.x, Min(desc.halfExtents.y, desc.halfExtents.z));
                if (!(minExtent > 0.0f))
                {
                    return nullptr;
                }
                shape = new JPH::BoxShape(ToJph(desc.halfExtents),
                                          Min(JPH::cDefaultConvexRadius, minExtent * 0.5f));
                break;
            }
            case ShapeKind::Sphere:
                shape = new JPH::SphereShape(desc.radius);
                break;
            case ShapeKind::Capsule:
                shape = new JPH::CapsuleShape(desc.halfHeight, desc.radius);
                break;
            case ShapeKind::Cooked:
                shape = RestoreCooked(desc.cooked);
                break;
            case ShapeKind::Plane:
                shape = new JPH::PlaneShape(
                    JPH::Plane(ToJph(desc.planeNormal).Normalized(), desc.planeDistance), nullptr,
                    desc.planeHalfExtent);
                break;
            case ShapeKind::Heightfield:
                shape = BuildHeightfield(desc);
                break;
            }
            // Density drives CalculateMassAndInertia (kg/m^3); only convex shapes carry it.
            if (shape != nullptr && shape->GetType() == JPH::EShapeType::Convex && density > 0.0f)
            {
                static_cast<JPH::ConvexShape*>(shape.GetPtr())->SetDensity(density);
            }
            if (shape != nullptr &&
                (desc.scale.x != 1.0f || desc.scale.y != 1.0f || desc.scale.z != 1.0f))
            {
                shape = new JPH::ScaledShape(shape, ToJph(desc.scale));
            }
            return shape;
        }

        [[nodiscard]] JPH::Ref<JPH::Shape> BuildShape(const BodyDesc& desc)
        {
            if (desc.shapes.IsEmpty())
            {
                return nullptr;
            }
            // One shape at the origin with no local rotation is used AS IS; any placement (offset
            // OR rotation) goes through a compound, which is the only thing that applies it.
            const Quaternion& lone = desc.shapes[0].localRotation;
            const bool loneRotated = lone.x != 0.0f || lone.y != 0.0f || lone.z != 0.0f;
            if (desc.shapes.Size() == 1 && desc.shapes[0].localPosition.x == 0.0f &&
                desc.shapes[0].localPosition.y == 0.0f && desc.shapes[0].localPosition.z == 0.0f &&
                !loneRotated)
            {
                return BuildOne(desc.shapes[0], desc.density);
            }
            JPH::StaticCompoundShapeSettings compound;
            for (const ShapeDesc& child : desc.shapes)
            {
                JPH::Ref<JPH::Shape> shape = BuildOne(child, desc.density);
                if (shape == nullptr)
                {
                    return nullptr;
                }
                compound.AddShape(ToJph(child.localPosition), ToJph(child.localRotation), shape);
            }
            JPH::Shape::ShapeResult result = compound.Create();
            return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>{};
        }

        struct ContactBuffer final : JPH::ContactListener
        {
            Mutex mutex;
            Array<ContactEvent> events;
            // Set after the PhysicsSystem is constructed: End events (OnContactRemoved) only
            // carry body IDs, so we resolve their user words through the body interface.
            JPH::PhysicsSystem* system = nullptr;

            void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override
            {
                ContactEvent e;
                e.kind =
                    a.IsSensor() || b.IsSensor() ? ContactKind::TriggerEnter : ContactKind::Begin;
                e.bodyA = BodyId{a.GetID().GetIndexAndSequenceNumber()};
                e.bodyB = BodyId{b.GetID().GetIndexAndSequenceNumber()};
                e.userA = a.GetUserData();
                e.userB = b.GetUserData();
                e.normal = FromJph(manifold.mWorldSpaceNormal);
                if (manifold.mRelativeContactPointsOn1.size() > 0)
                {
                    e.point = FromJph(manifold.GetWorldSpaceContactPointOn1(0));
                }
                // Approach speed, NOT a solver impulse (see ContactEvent::speed): Jolt does
                // not expose the true impulse cleanly here, so we report the magnitude of the
                // relative velocity along the contact normal - a physically-meaningful measure
                // of how hard the two bodies met.
                const JPH::Vec3 relative = a.GetLinearVelocity() - b.GetLinearVelocity();
                e.speed = std::fabs(relative.Dot(manifold.mWorldSpaceNormal));
                ScopedLock lock(mutex);
                events.PushBack(e);
            }
            void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
            {
                ContactEvent e;
                e.kind = ContactKind::End;
                e.bodyA = BodyId{pair.GetBody1ID().GetIndexAndSequenceNumber()};
                e.bodyB = BodyId{pair.GetBody2ID().GetIndexAndSequenceNumber()};
                // Resolve the user words by body ID if the body still exists (the callback can
                // fire for an already-destroyed body - leave that side 0 so the subsystem skips
                // it). NoLock interface: contact callbacks run with bodies already locked.
                if (system != nullptr)
                {
                    const JPH::BodyLockInterface& bodies = system->GetBodyLockInterfaceNoLock();
                    {
                        JPH::BodyLockRead lock(bodies, pair.GetBody1ID());
                        if (lock.Succeeded())
                        {
                            e.userA = lock.GetBody().GetUserData();
                        }
                    }
                    {
                        JPH::BodyLockRead lock(bodies, pair.GetBody2ID());
                        if (lock.Succeeded())
                        {
                            e.userB = lock.GetBody().GetUserData();
                        }
                    }
                }
                ScopedLock lock(mutex);
                events.PushBack(e);
            }
        };
    }

    bool CookConvexHull(Span<const Float3> points, Array<byte>& outBlob, f32 hullTolerance)
    {
        if (points.Size() < 4)
        {
            return false;
        }
        AcquireJolt(); // Factory/type registry must exist for shape construction
        JPH::Array<JPH::Vec3> hull;
        hull.reserve(points.Size());
        for (const Float3& p : points)
        {
            hull.push_back(ToJph(p));
        }
        JPH::ConvexHullShapeSettings settings(hull);
        settings.mHullTolerance = hullTolerance;
        const JPH::Shape::ShapeResult result = settings.Create();
        bool ok = false;
        if (result.IsValid())
        {
            BlobOut out(outBlob);
            result.Get()->SaveBinaryState(out);
            ok = true;
        }
        ReleaseJolt();
        return ok;
    }

    bool CookTriangleMesh(Span<const Float3> positions, Span<const u32> indices,
                          Span<const u32> triangleMaterialSlots, Array<byte>& outBlob)
    {
        if (positions.IsEmpty() || indices.IsEmpty() || indices.Size() % 3 != 0)
        {
            return false;
        }
        const usize triangleCount = indices.Size() / 3;
        if (!triangleMaterialSlots.IsEmpty() && triangleMaterialSlots.Size() != triangleCount)
        {
            return false;
        }
        AcquireJolt();
        JPH::VertexList vertices;
        vertices.reserve(positions.Size());
        for (const Float3& p : positions)
        {
            vertices.push_back(JPH::Float3(p.x, p.y, p.z));
        }
        JPH::IndexedTriangleList triangles;
        triangles.reserve(triangleCount);
        for (usize t = 0; t < triangleCount; ++t)
        {
            // Material slot rides in the per-triangle USER DATA (RayHit::surface); Jolt's
            // own material index stays 0 (we don't use JPH::PhysicsMaterial).
            triangles.push_back(JPH::IndexedTriangle(
                indices[t * 3 + 0], indices[t * 3 + 1], indices[t * 3 + 2], 0,
                triangleMaterialSlots.IsEmpty() ? 0 : triangleMaterialSlots[t]));
        }
        JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
        settings.mPerTriangleUserData = true;
        const JPH::Shape::ShapeResult result = settings.Create();
        bool ok = false;
        if (result.IsValid())
        {
            BlobOut out(outBlob);
            result.Get()->SaveBinaryState(out);
            ok = true;
        }
        ReleaseJolt();
        return ok;
    }

    bool ExtractShapeTriangles(Span<const byte> blob, Array<Float3>& outTriangles)
    {
        AcquireJolt();
        JPH::Ref<JPH::Shape> shape = RestoreCooked(blob);
        bool ok = false;
        if (shape != nullptr)
        {
            JPH::Shape::GetTrianglesContext context;
            shape->GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::Vec3::sZero(),
                                     JPH::Quat::sIdentity(), JPH::Vec3::sOne());
            JPH::Float3 buffer[3 * JPH::Shape::cGetTrianglesMinTrianglesRequested];
            for (;;)
            {
                const int count = shape->GetTrianglesNext(
                    context, JPH::Shape::cGetTrianglesMinTrianglesRequested, buffer);
                if (count <= 0)
                {
                    break;
                }
                for (int v = 0; v < count * 3; ++v)
                {
                    outTriangles.PushBack(Float3{buffer[v].x, buffer[v].y, buffer[v].z});
                }
            }
            ok = !outTriangles.IsEmpty();
        }
        ReleaseJolt();
        return ok;
    }

    usize AppendBodyTriangles(Span<const BodyDesc> bodies, const AABB& bounds,
                              Array<Float3>& outTriangles)
    {
        usize failed = 0;
        AcquireJolt();
        {
            const JPH::AABox box(ToJph(bounds.min), ToJph(bounds.max));
            JPH::Float3 buffer[3 * JPH::Shape::cGetTrianglesMinTrianglesRequested];
            for (const BodyDesc& body : bodies)
            {
                JPH::Ref<JPH::Shape> shape = BuildShape(body);
                if (shape == nullptr)
                {
                    ++failed;
                    continue;
                }
                // A compound only yields triangles through its leaves, so every shape goes
                // through CollectTransformedShapes first (a leaf collects itself). The body's
                // origin is its centre of mass, as CreateBody places it.
                const JPH::Quat rotation = ToJph(body.rotation);
                const JPH::TransformedShape whole(
                    JPH::RVec3(ToJph(body.position)) + rotation * shape->GetCenterOfMass(),
                    rotation, shape, JPH::BodyID());
                JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
                whole.CollectTransformedShapes(box, leaves);
                for (const JPH::TransformedShape& leaf : leaves.mHits)
                {
                    JPH::Shape::GetTrianglesContext context;
                    leaf.GetTrianglesStart(context, box, JPH::RVec3::sZero());
                    for (;;)
                    {
                        const int count = leaf.GetTrianglesNext(
                            context, JPH::Shape::cGetTrianglesMinTrianglesRequested, buffer);
                        if (count <= 0)
                        {
                            break;
                        }
                        // The box only culls inside meshes and heightfields; a convex shape or a
                        // plane gives all of itself, so each triangle is tested here.
                        for (int t = 0; t < count; ++t)
                        {
                            const JPH::Float3* corner = &buffer[3 * t];
                            AABB extent = AABB::Empty();
                            for (int v = 0; v < 3; ++v)
                            {
                                extent.Expand(Float3{corner[v].x, corner[v].y, corner[v].z});
                            }
                            if (!extent.Intersects(bounds))
                            {
                                continue;
                            }
                            for (int v = 0; v < 3; ++v)
                            {
                                outTriangles.PushBack(Float3{corner[v].x, corner[v].y, corner[v].z});
                            }
                        }
                    }
                }
            }
        }
        ReleaseJolt();
        return failed;
    }

    struct PhysicsWorld::Impl
    {
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhase objectVsBroadPhase;
        ObjectPairFilter pairFilter;
        ContactBuffer contacts;
        UniquePtr<JPH::TempAllocatorImpl> tempAllocator;
        UniquePtr<JPH::JobSystemThreadPool> jobSystem;
        UniquePtr<JPH::PhysicsSystem> system;
        // JointId = slot index; null joint = freed. The two body IDs ride the slot so
        // DestroyJoint can wake the SURVIVING bodies BY ID through the body manager - the
        // constraint's raw Body pointers dangle if a connected body was destroyed first
        // (heap-use-after-free found by ASAN 2026-08-19: entity-active reconcile destroyed a
        // deactivated target's body, then the joint teardown read the corpse via GetBody1/2).
        struct JointSlot
        {
            JPH::Ref<JPH::Constraint> joint;
            JPH::BodyID bodyA;
            JPH::BodyID bodyB; // invalid = world attachment
        };
        Array<JointSlot> joints;
        Array<JPH::Ref<JPH::CharacterVirtual>> characters; // CharacterId = slot; null = freed
        Array<Float2> characterSteps;                      // (stepUp, stepDown) per slot
    };

    PhysicsWorld::PhysicsWorld(core::IAllocator& allocator, const PhysicsWorldSettings& settings)
    {
        AcquireJolt();
        m_impl = MakeUnique<Impl>(allocator);
        m_impl->tempAllocator =
            MakeUnique<JPH::TempAllocatorImpl>(allocator, 10 * 1024 * 1024);
        m_impl->jobSystem = MakeUnique<JPH::JobSystemThreadPool>(
            allocator, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            static_cast<int>(JPH::thread::hardware_concurrency()) - 1);
        m_impl->system = MakeUnique<JPH::PhysicsSystem>(allocator);
        for (u32 i = 0; i < kCollisionGroupCount; ++i)
        {
            m_impl->pairFilter.groupCollides[i] = settings.groupCollides[i];
        }
        m_impl->system->Init(settings.maxBodies, 0, settings.maxBodyPairs,
                             settings.maxContactConstraints, m_impl->broadPhaseLayers,
                             m_impl->objectVsBroadPhase, m_impl->pairFilter);
        m_impl->system->SetGravity(ToJph(settings.gravity));
        m_impl->contacts.system = m_impl->system.Get(); // End events resolve user words by ID
        m_impl->system->SetContactListener(&m_impl->contacts);
    }

    PhysicsWorld::~PhysicsWorld()
    {
        m_impl = nullptr;
        ReleaseJolt();
    }

    void PhysicsWorld::SetGravity(Float3 gravity) { m_impl->system->SetGravity(ToJph(gravity)); }
    Float3 PhysicsWorld::Gravity() const { return FromJph(m_impl->system->GetGravity()); }

    BodyId PhysicsWorld::CreateBody(const BodyDesc& desc)
    {
        JPH::Ref<JPH::Shape> shape = BuildShape(desc);
        if (shape == nullptr)
        {
            return BodyId{};
        }

        const PhysicsLayer layer = desc.isTrigger ? PhysicsLayer::Trigger : desc.layer;
        const u8 group = static_cast<u8>(desc.group & 0x1Fu);
        MotionKind motionKind = desc.motion;
        if (motionKind != MotionKind::Static && shape->MustBeStatic())
        {
            // Jolt's own rule (Shape::MustBeStatic: mesh, heightfield, plane, and any compound
            // or decorated shape holding one): such a shape derives no mass, and Jolt sets mass
            // properties for EVERY non-static body - dynamic or kinematic - so it asserts
            // "Invalid mass" at creation (the editor's Simulate crash, 2026-09-21; a release
            // build instead gets a body with no inertia). A data error, not a crash: the body
            // simulates as static, and the scene system names the entity.
            LOG_ERROR(u8"Physics",
                      u8"{} body over a mesh/plane/heightfield shape: such a shape can only be "
                      u8"static (a moving body needs a convex shape: box, sphere, capsule, convex "
                      u8"hull) - simulated as STATIC",
                      motionKind == MotionKind::Kinematic ? u8"kinematic" : u8"dynamic");
            motionKind = MotionKind::Static;
        }
        const JPH::EMotionType motion = motionKind == MotionKind::Static ? JPH::EMotionType::Static
                                        : motionKind == MotionKind::Kinematic
                                            ? JPH::EMotionType::Kinematic
                                            : JPH::EMotionType::Dynamic;
        JPH::BodyCreationSettings settings(shape, ToJph(desc.position), ToJph(desc.rotation),
                                           motion, layers::From(layer, group));
        settings.mFriction = desc.friction;
        settings.mRestitution = desc.restitution;
        settings.mLinearDamping = desc.linearDamping;
        settings.mAngularDamping = desc.angularDamping;
        settings.mIsSensor = desc.isTrigger;
        settings.mUserData = desc.userData;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateMassAndInertia;
        if (motionKind == MotionKind::Dynamic)
        {
            if (desc.continuousCollision)
            {
                settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            }
            // A convex shape degenerate to zero volume (a hull cooked from a flat quad - Jolt
            // refuses a zero scale at shape creation, so the flat hull is the reachable case)
            // derives no mass and would trip the same "Invalid mass" assert. It still collides
            // correctly, so it gets the mass and inertia of a solid box filling its local bounds
            // (a centimetre thick at least), density-derived or scaled to the explicit mass, and
            // keeps simulating; the warning points at the data.
            const JPH::MassProperties derived = shape->GetMassProperties();
            const bool shapeDerivesMass = derived.mMass > 0.0f && std::isfinite(derived.mMass);
            if (!shapeDerivesMass)
            {
                JPH::Vec3 size = shape->GetLocalBounds().GetSize();
                size = JPH::Vec3::sMax(size, JPH::Vec3::sReplicate(0.01f)); // never a zero side
                JPH::MassProperties box;
                box.SetMassAndInertiaOfSolidBox(size, desc.density > 0.0f ? desc.density : 1000.0f);
                if (desc.massOverride > 0.0f)
                {
                    box.ScaleToMass(desc.massOverride);
                }
                settings.mOverrideMassProperties =
                    JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride = box;
                LOG_WARNING(u8"Physics",
                            u8"dynamic body: its convex shape derives no mass (zero volume); "
                            u8"using its bounds as a solid box ({} kg)",
                            box.mMass);
            }
            else if (desc.massOverride > 0.0f)
            {
                // Inertia stays density-derived; only the scalar mass is overridden.
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = desc.massOverride;
            }
        }

        JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
        const JPH::BodyID id = bodies.CreateAndAddBody(
            settings, motionKind == MotionKind::Static ? JPH::EActivation::DontActivate
                                                       : JPH::EActivation::Activate);
        return id.IsInvalid() ? BodyId{} : BodyId{id.GetIndexAndSequenceNumber()};
    }

    void PhysicsWorld::DestroyBody(BodyId id)
    {
        if (!id.IsValid())
        {
            return;
        }
        JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
        const JPH::BodyID jolt(id.value);
        bodies.RemoveBody(jolt);
        bodies.DestroyBody(jolt);
    }

    usize PhysicsWorld::BodyCount() const { return m_impl->system->GetNumBodies(); }

    f32 PhysicsWorld::BodyMass(BodyId id) const
    {
        if (!id.IsValid())
        {
            return 0.0f;
        }
        const JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), JPH::BodyID(id.value));
        if (!lock.Succeeded())
        {
            return 0.0f;
        }
        const JPH::Body& body = lock.GetBody();
        if (!body.IsDynamic())
        {
            return 0.0f;
        }
        const f32 inverseMass = body.GetMotionProperties()->GetInverseMass();
        return inverseMass > 0.0f ? 1.0f / inverseMass : 0.0f;
    }

    void PhysicsWorld::Step(f32 deltaTime, i32 collisionSteps)
    {
        m_impl->system->Update(deltaTime, collisionSteps, m_impl->tempAllocator.Get(),
                               m_impl->jobSystem.Get());
    }

    void PhysicsWorld::GetBodyTransform(BodyId id, Float3& outPosition,
                                        Quaternion& outRotation) const
    {
        const JPH::BodyID jolt(id.value);
        JPH::RVec3 position;
        JPH::Quat rotation;
        m_impl->system->GetBodyInterface().GetPositionAndRotation(jolt, position, rotation);
        outPosition = FromJph(position);
        outRotation = FromJph(rotation);
    }

    void PhysicsWorld::SetBodyTransform(BodyId id, Float3 position, Quaternion rotation)
    {
        m_impl->system->GetBodyInterface().SetPositionAndRotation(
            JPH::BodyID(id.value), ToJph(position), ToJph(rotation), JPH::EActivation::Activate);
    }

    void PhysicsWorld::MoveKinematic(BodyId id, Float3 position, Quaternion rotation, f32 deltaTime)
    {
        m_impl->system->GetBodyInterface().MoveKinematic(JPH::BodyID(id.value), ToJph(position),
                                                         ToJph(rotation), deltaTime);
    }

    void PhysicsWorld::SetLinearVelocity(BodyId id, Float3 velocity)
    {
        m_impl->system->GetBodyInterface().SetLinearVelocity(JPH::BodyID(id.value),
                                                             ToJph(velocity));
    }

    Float3 PhysicsWorld::LinearVelocity(BodyId id) const
    {
        return FromJph(m_impl->system->GetBodyInterface().GetLinearVelocity(JPH::BodyID(id.value)));
    }

    void PhysicsWorld::AddImpulse(BodyId id, Float3 impulse)
    {
        m_impl->system->GetBodyInterface().AddImpulse(JPH::BodyID(id.value), ToJph(impulse));
    }

    void PhysicsWorld::AddForce(BodyId id, Float3 force)
    {
        m_impl->system->GetBodyInterface().AddForce(JPH::BodyID(id.value), ToJph(force),
                                                    JPH::EActivation::Activate);
    }

    bool PhysicsWorld::IsActive(BodyId id) const
    {
        return m_impl->system->GetBodyInterface().IsActive(JPH::BodyID(id.value));
    }

    u64 PhysicsWorld::UserData(BodyId id) const
    {
        return m_impl->system->GetBodyInterface().GetUserData(JPH::BodyID(id.value));
    }

    bool PhysicsWorld::RayCast(Float3 from, Float3 direction, f32 maxDistance, RayHit& out,
                               u32 groupMask) const
    {
        const JPH::RRayCast ray{ToJph(from), ToJph(direction) * maxDistance};
        JPH::RayCastResult hit;
        const GroupMaskFilter filter(groupMask);
        if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, filter))
        {
            return false;
        }
        out.body = BodyId{hit.mBodyID.GetIndexAndSequenceNumber()};
        out.fraction = hit.mFraction;
        out.position = Float3{from.x + direction.x * maxDistance * hit.mFraction,
                              from.y + direction.y * maxDistance * hit.mFraction,
                              from.z + direction.z * maxDistance * hit.mFraction};
        out.userData = UserData(out.body);
        out.surface = 0;
        JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            out.normal = FromJph(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2,
                                                                 ray.GetPointOnRay(hit.mFraction)));
            JPH::SubShapeID remainder;
            const JPH::Shape* leaf = body.GetShape()->GetLeafShape(hit.mSubShapeID2, remainder);
            if (leaf != nullptr && leaf->GetSubType() == JPH::EShapeSubType::Mesh)
            {
                out.surface =
                    static_cast<const JPH::MeshShape*>(leaf)->GetTriangleUserData(remainder);
            }
        }
        return true;
    }

    void PhysicsWorld::QueryPoint(Float3 point, Array<BodyId>& out, u32 groupMask) const
    {
        out.Clear(); // fill semantics, like ShapeOverlap: a reused array never mixes queries
        JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
        const GroupMaskFilter filter(groupMask);
        m_impl->system->GetNarrowPhaseQuery().CollidePoint(ToJph(point), collector, {}, filter);
        for (const JPH::CollidePointResult& result : collector.mHits)
        {
            out.PushBack(BodyId{result.mBodyID.GetIndexAndSequenceNumber()});
        }
    }

    void PhysicsWorld::ShapeOverlap(const QueryShape& shape, Float3 position, Quaternion rotation,
                                    Array<BodyId>& out, u32 groupMask) const
    {
        out.Clear(); // fill semantics: a reused array never mixes results across queries
        (void)WithQueryShape(
            shape,
            [&](const JPH::Shape& js)
            {
                const JPH::RMat44 com =
                    JPH::RMat44::sRotationTranslation(ToJph(rotation), ToJph(position));
                const JPH::CollideShapeSettings settings;
                JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
                const GroupMaskFilter filter(groupMask);
                m_impl->system->GetNarrowPhaseQuery().CollideShape(
                    &js, JPH::Vec3::sReplicate(1.0f), com, settings, JPH::RVec3::sZero(), collector,
                    {}, filter);
                // A compound/multi-shape body can report several sub-shape hits; each body ONCE.
                for (const JPH::CollideShapeResult& r : collector.mHits)
                {
                    const BodyId id{r.mBodyID2.GetIndexAndSequenceNumber()};
                    bool seen = false;
                    for (const BodyId& existing : out)
                    {
                        if (existing == id)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        out.PushBack(id);
                    }
                }
            });
    }

    bool PhysicsWorld::ShapeCast(const QueryShape& shape, Float3 from, Quaternion rotation,
                                 Float3 direction, f32 maxDistance, RayHit& out, u32 groupMask) const
    {
        bool hitAny = false;
        (void)WithQueryShape(
            shape,
            [&](const JPH::Shape& js)
            {
                const JPH::RMat44 start =
                    JPH::RMat44::sRotationTranslation(ToJph(rotation), ToJph(from));
                const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
                    &js, JPH::Vec3::sReplicate(1.0f), start, ToJph(direction) * maxDistance);
                const JPH::ShapeCastSettings settings;
                JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
                const GroupMaskFilter filter(groupMask);
                m_impl->system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(),
                                                                collector, {}, filter);
                if (!collector.HadHit())
                {
                    return;
                }
                const JPH::ShapeCastResult& hit = collector.mHit;
                out.body = BodyId{hit.mBodyID2.GetIndexAndSequenceNumber()};
                out.fraction = hit.mFraction;
                out.position = FromJph(hit.mContactPointOn2);
                // mPenetrationAxis points from the query shape INTO the hit body; the outward
                // surface normal is its negation. Guard the (rare) zero-penetration touch.
                out.normal = hit.mPenetrationAxis.IsNearZero()
                                 ? Float3{0, 0, 0}
                                 : FromJph(-hit.mPenetrationAxis.Normalized());
                out.userData = UserData(out.body);
                out.surface = 0;
                hitAny = true;
            });
        return hitAny;
    }

    void PhysicsWorld::DrainContacts(Array<ContactEvent>& out)
    {
        ScopedLock lock(m_impl->contacts.mutex);
        for (ContactEvent& e : m_impl->contacts.events)
        {
            out.PushBack(e);
        }
        m_impl->contacts.events.Clear();
    }

    // ---- character controllers ----

    CharacterId PhysicsWorld::CreateCharacter(const CharacterDesc& desc)
    {
        JPH::CharacterVirtualSettings settings;
        settings.mShape = new JPH::CapsuleShape(desc.capsuleHalfHeight, desc.capsuleRadius);
        // Support only on the bottom sphere: standing on a ledge edge at waist height
        // must not count as grounded.
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc.capsuleHalfHeight);
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(desc.maxSlopeDegrees);
        settings.mMass = desc.mass;
        settings.mMaxStrength = desc.maxStrength;
        JPH::Ref<JPH::CharacterVirtual> character =
            new JPH::CharacterVirtual(&settings, ToJph(desc.position), JPH::Quat::sIdentity(),
                                      desc.userData, m_impl->system.Get());
        // Stash the step settings per character (uniform across the world would also do,
        // but they're authored per character).
        character->SetUserData(desc.userData);

        for (usize i = 0; i < m_impl->characters.Size(); ++i)
        {
            if (m_impl->characters[i] == nullptr)
            {
                m_impl->characters[i] = character;
                m_impl->characterSteps[i] = Float2{desc.stepUp, desc.stepDown};
                return CharacterId{static_cast<u32>(i)};
            }
        }
        m_impl->characters.PushBack(character);
        m_impl->characterSteps.PushBack(Float2{desc.stepUp, desc.stepDown});
        return CharacterId{static_cast<u32>(m_impl->characters.Size() - 1)};
    }

    void PhysicsWorld::DestroyCharacter(CharacterId id)
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size())
        {
            return;
        }
        m_impl->characters[id.value] = nullptr;
    }

    void PhysicsWorld::SetCharacterVelocity(CharacterId id, Float3 velocity)
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return;
        }
        m_impl->characters[id.value]->SetLinearVelocity(ToJph(velocity));
    }

    Float3 PhysicsWorld::CharacterVelocity(CharacterId id) const
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return Float3{0, 0, 0};
        }
        return FromJph(m_impl->characters[id.value]->GetLinearVelocity());
    }

    void PhysicsWorld::SetCharacterStrength(CharacterId id, f32 strength)
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return;
        }
        m_impl->characters[id.value]->SetMaxStrength(strength);
    }

    void PhysicsWorld::UpdateCharacter(CharacterId id, f32 deltaTime)
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return;
        }
        JPH::CharacterVirtual& character = *m_impl->characters[id.value];
        const Float2 steps = m_impl->characterSteps[id.value];
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3(0.0f, steps.x, 0.0f);
        update.mStickToFloorStepDown = JPH::Vec3(0.0f, -steps.y, 0.0f);
        character.ExtendedUpdate(
            deltaTime, m_impl->system->GetGravity(), update,
            m_impl->system->GetDefaultBroadPhaseLayerFilter(layers::From(PhysicsLayer::Dynamic, 0)),
            m_impl->system->GetDefaultLayerFilter(layers::From(PhysicsLayer::Dynamic, 0)), {}, {},
            *m_impl->tempAllocator);
    }

    Float3 PhysicsWorld::CharacterPosition(CharacterId id) const
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return Float3{0, 0, 0};
        }
        return FromJph(m_impl->characters[id.value]->GetPosition());
    }

    void PhysicsWorld::SetCharacterPosition(CharacterId id, Float3 position)
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return;
        }
        m_impl->characters[id.value]->SetPosition(ToJph(position));
    }

    Float3 PhysicsWorld::CharacterGroundNormal(CharacterId id) const
    {
        const Float3 up{0.0f, 1.0f, 0.0f};
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return up;
        }
        const JPH::CharacterVirtual& character = *m_impl->characters[id.value];
        if (character.GetGroundState() == JPH::CharacterBase::EGroundState::InAir)
        {
            return up;
        }
        const JPH::Vec3 normal = character.GetGroundNormal();
        // Jolt leaves a zero normal when nothing supports the character.
        return normal.LengthSq() > 1.0e-6f ? FromJph(normal.Normalized()) : up;
    }

    CharacterGround PhysicsWorld::GetCharacterGround(CharacterId id) const
    {
        if (!id.IsValid() || id.value >= m_impl->characters.Size() ||
            m_impl->characters[id.value] == nullptr)
        {
            return CharacterGround::InAir;
        }
        switch (m_impl->characters[id.value]->GetGroundState())
        {
        case JPH::CharacterBase::EGroundState::OnGround:
            return CharacterGround::OnGround;
        case JPH::CharacterBase::EGroundState::OnSteepGround:
            return CharacterGround::OnSteepGround;
        case JPH::CharacterBase::EGroundState::NotSupported:
            return CharacterGround::NotSupported;
        case JPH::CharacterBase::EGroundState::InAir:
            break;
        }
        return CharacterGround::InAir;
    }

    // ---- joints ----

    JointId PhysicsWorld::CreateJoint(const JointDesc& desc)
    {
        if (!desc.bodyA.IsValid())
        {
            return JointId{};
        }
        // Main-thread creation outside Step (same threading contract as CreateBody).
        const JPH::BodyLockInterfaceNoLock& bodies = m_impl->system->GetBodyLockInterfaceNoLock();
        JPH::Body* a = bodies.TryGetBody(JPH::BodyID(desc.bodyA.value));
        JPH::Body* b = desc.bodyB.IsValid() ? bodies.TryGetBody(JPH::BodyID(desc.bodyB.value))
                                            : &JPH::Body::sFixedToWorld;
        if (a == nullptr || b == nullptr)
        {
            return JointId{};
        }

        const JPH::RVec3 anchor = ToJph(desc.anchor);
        const JPH::Vec3 axis = ToJph(desc.axis).NormalizedOr(JPH::Vec3::sAxisY());
        const bool limited = desc.limitMin <= desc.limitMax;
        JPH::Ref<JPH::Constraint> joint;
        switch (desc.kind)
        {
        case JointKind::Fixed:
        {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = true;
            joint = settings.Create(*a, *b);
            break;
        }
        case JointKind::Point:
        {
            JPH::PointConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = anchor;
            settings.mPoint2 = anchor;
            joint = settings.Create(*a, *b);
            break;
        }
        case JointKind::Hinge:
        {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = anchor;
            settings.mHingeAxis1 = settings.mHingeAxis2 = axis;
            settings.mNormalAxis1 = settings.mNormalAxis2 = axis.GetNormalizedPerpendicular();
            if (limited)
            {
                settings.mLimitsMin = desc.limitMin;
                settings.mLimitsMax = desc.limitMax;
            }
            settings.mMotorSettings.SetTorqueLimit(desc.motorLimit);
            joint = settings.Create(*a, *b);
            if (desc.motorEnabled)
            {
                auto* hinge = static_cast<JPH::HingeConstraint*>(joint.GetPtr());
                hinge->SetTargetAngularVelocity(desc.motorTargetVelocity);
                hinge->SetMotorState(JPH::EMotorState::Velocity);
            }
            break;
        }
        case JointKind::Slider:
        {
            JPH::SliderConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = true;
            settings.SetSliderAxis(axis);
            if (limited)
            {
                settings.mLimitsMin = desc.limitMin;
                settings.mLimitsMax = desc.limitMax;
            }
            settings.mMotorSettings.SetForceLimit(desc.motorLimit);
            joint = settings.Create(*a, *b);
            if (desc.motorEnabled)
            {
                auto* slider = static_cast<JPH::SliderConstraint*>(joint.GetPtr());
                slider->SetTargetVelocity(desc.motorTargetVelocity);
                slider->SetMotorState(JPH::EMotorState::Velocity);
            }
            break;
        }
        case JointKind::Distance:
        {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            // Rope semantics: from each body's center (world-attached: from `anchor`).
            settings.mPoint1 = a->GetCenterOfMassPosition();
            settings.mPoint2 = desc.bodyB.IsValid() ? b->GetCenterOfMassPosition() : anchor;
            settings.mMinDistance = desc.minDistance;
            settings.mMaxDistance = desc.maxDistance;
            joint = settings.Create(*a, *b);
            break;
        }
        }
        if (joint == nullptr)
        {
            return JointId{};
        }
        m_impl->system->AddConstraint(joint.GetPtr());

        const JPH::BodyID idA(desc.bodyA.value);
        const JPH::BodyID idB = desc.bodyB.IsValid() ? JPH::BodyID(desc.bodyB.value) : JPH::BodyID();
        for (usize i = 0; i < m_impl->joints.Size(); ++i)
        {
            if (m_impl->joints[i].joint == nullptr)
            {
                m_impl->joints[i] = Impl::JointSlot{joint, idA, idB};
                return JointId{static_cast<u32>(i)};
            }
        }
        m_impl->joints.PushBack(Impl::JointSlot{joint, idA, idB});
        return JointId{static_cast<u32>(m_impl->joints.Size() - 1)};
    }

    void PhysicsWorld::DestroyJoint(JointId id)
    {
        if (!id.IsValid() || id.value >= m_impl->joints.Size() ||
            m_impl->joints[id.value].joint == nullptr)
        {
            return;
        }
        Impl::JointSlot& slot = m_impl->joints[id.value];
        // Wake the connected bodies: a body held asleep by the joint must respond to gravity
        // again once released (RemoveConstraint alone leaves it sleeping). BY ID through the
        // body manager - NEVER via the constraint's Body pointers, which dangle if a connected
        // body was destroyed before the joint (the ASAN-found UAF; IsAdded rejects dead IDs,
        // GetMotionType goes through the manager, both safe on any ID).
        JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
        for (const JPH::BodyID bodyId : {slot.bodyA, slot.bodyB})
        {
            if (!bodyId.IsInvalid() && bodies.IsAdded(bodyId) &&
                bodies.GetMotionType(bodyId) != JPH::EMotionType::Static)
            {
                bodies.ActivateBody(bodyId);
            }
        }
        m_impl->system->RemoveConstraint(slot.joint.GetPtr());
        slot = Impl::JointSlot{};
    }

    void PhysicsWorld::SetJointMotor(JointId id, bool enabled, f32 targetVelocity)
    {
        if (!id.IsValid() || id.value >= m_impl->joints.Size())
        {
            return;
        }
        JPH::Constraint* joint = m_impl->joints[id.value].joint.GetPtr();
        if (joint == nullptr)
        {
            return;
        }
        if (joint->GetSubType() == JPH::EConstraintSubType::Hinge)
        {
            auto* hinge = static_cast<JPH::HingeConstraint*>(joint);
            hinge->SetTargetAngularVelocity(targetVelocity);
            hinge->SetMotorState(enabled ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
        }
        else if (joint->GetSubType() == JPH::EConstraintSubType::Slider)
        {
            auto* slider = static_cast<JPH::SliderConstraint*>(joint);
            slider->SetTargetVelocity(targetVelocity);
            slider->SetMotorState(enabled ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
        }
        else
        {
            return; // no motor on this joint kind
        }
        // A motor with real drive must keep its bodies AWAKE: a body that slept while the target was 0
        // (or before the motor was enabled) would otherwise ignore the constraint, so a live edit to
        // motorTargetVelocity does nothing. Only force-wake when actually driving, so a settled/zero
        // motor can still sleep. (Hinge/Slider are TwoBodyConstraints; no RTTI - cast off GetSubType.)
        // BY ID through the body manager (the slot's IDs), never via the constraint's Body
        // pointers: the subsystem syncs motors EVERY fixed step, and those pointers dangle once a
        // connected body was destroyed before the joint (the same use-after-free DestroyJoint
        // guards against).
        if (enabled && targetVelocity != 0.0f)
        {
            const Impl::JointSlot& slot = m_impl->joints[id.value];
            JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
            for (const JPH::BodyID bodyId : {slot.bodyA, slot.bodyB})
            {
                if (!bodyId.IsInvalid() && bodies.IsAdded(bodyId) &&
                    bodies.GetMotionType(bodyId) != JPH::EMotionType::Static)
                {
                    bodies.ActivateBody(bodyId);
                }
            }
        }
    }
}

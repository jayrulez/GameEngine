// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles:modules - the initializer and behavior module base classes, the concrete
// modules, the CPU simulator and the runtime type-id registry.
//
// An initializer runs once per spawned particle; a behavior runs every frame over all live
// particles. Each declares the streams it needs (allocated lazily). Velocity integration and aging
// are a fixed final step on ParticleSystem, not a module, and the render type is an enum, not a
// module. A GPU simulator would sit behind the same interfaces and BehaviorSupport.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT, RTTI_DEFINE_OBJECT

export module foundation.particles:modules;

import foundation.core;
import :types;
import :streams;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::particles
{
    // ---- Base classes ------------------------------------------------------------------------

    // Modules are reflected ISerializable objects: reflection gives the type-id, polymorphic
    // reconstruction (Serializables().Create), and the Serialize(ISerializer&) hook, the same
    // machinery cooked resources use. The cook writes each module's reflected type tag, then Serialize.
    class ParticleInitializer : public ISerializable
    {
        RTTI_OBJECT(ParticleInitializer, ISerializable)
    public:
        [[nodiscard]] virtual BehaviorSupport Support() const noexcept = 0;
        virtual void DeclareStreams(ParticleStreamContainer& streams) = 0;
        virtual void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) = 0;
        // Hook: the system pushes its transform state before a spawn burst so emitter-aware
        // initializers (Position, Velocity) can offset or inherit. Default no-op (avoids an RTTI cast).
        virtual void SetEmitterState(Float3 position, Float3 velocity) noexcept
        {
            (void)position;
            (void)velocity;
        }
        void Serialize(ISerializer& ar) override
        {
            (void)ar;
        } // paramless default; modules with params override
    };

    class ParticleBehavior : public ISerializable
    {
        RTTI_OBJECT(ParticleBehavior, ISerializable)
    public:
        [[nodiscard]] virtual BehaviorSupport Support() const noexcept = 0;
        virtual void DeclareStreams(ParticleStreamContainer& streams) = 0;
        virtual void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) = 0;
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    class ParticleSimulator
    {
    public:
        virtual ~ParticleSimulator() = default;
        virtual void Simulate(ParticleStreamContainer& streams,
                              const Array<RefPtr<ParticleBehavior>>& behaviors,
                              ParticleUpdateContext& ctx) = 0;
        virtual i32 CompactDead(ParticleStreamContainer& streams) = 0;
    };

    // ---- Initializers ------------------------------------------------------------------------

    class PositionInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(PositionInitializer, ParticleInitializer)
    public:
        EmissionShape shape = EmissionShape::Point();
        Float3 emitterPosition{0.0f, 0.0f, 0.0f}; // set by the system each spawn (hidden)
        bool localSpace = false;

        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "shape", shape);
            core::Serialize(ar, "localSpace", localSpace);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer&) override {} // Position is a core stream
        void SetEmitterState(Float3 position, Float3) noexcept override
        {
            emitterPosition = position;
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            Float3 pos, dir;
            shape.Sample(rng, pos, dir);
            (*streams.Positions())[index] = localSpace ? pos : emitterPosition + pos;
        }
    };

    class VelocityInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(VelocityInitializer, ParticleInitializer)
    public:
        Float3 baseVelocity{0.0f, 1.0f, 0.0f};
        Float3 randomness{0.0f, 0.0f, 0.0f};
        f32 shapeDirectionSpeed = 0.0f;
        f32 velocityInheritance = 0.0f;
        EmissionShape shape = EmissionShape::Point();
        Float3 emitterVelocity{0.0f, 0.0f, 0.0f}; // set by the system each spawn (hidden)

        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "baseVelocity", baseVelocity);
            core::Serialize(ar, "randomness", randomness);
            core::Serialize(ar, "shapeDirectionSpeed", shapeDirectionSpeed);
            core::Serialize(ar, "velocityInheritance", velocityInheritance);
            core::Serialize(ar, "shape", shape);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
            streams.EnsureStream(ParticleStreamId::StartVelocity, StreamElementType::Float3);
        }
        void SetEmitterState(Float3, Float3 velocity) noexcept override
        {
            emitterVelocity = velocity;
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            Float3 pos, dir;
            shape.Sample(rng, pos, dir);
            const Float3 rnd{rng.NextFloat(-randomness.x, randomness.x),
                             rng.NextFloat(-randomness.y, randomness.y),
                             rng.NextFloat(-randomness.z, randomness.z)};
            const Float3 v = baseVelocity + rnd + dir * shapeDirectionSpeed +
                             emitterVelocity * velocityInheritance;
            (*streams.Velocities())[index] = v;
            (*streams.StartVelocities())[index] = v;
        }
    };

    class LifetimeInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(LifetimeInitializer, ParticleInitializer)
    public:
        RangeFloat lifetime{1.0f, 1.0f};
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "lifetime", lifetime); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer&) override {} // Age and Lifetime are core
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            (*streams.Lifetimes())[index] = Max(lifetime.Evaluate(rng.NextFloat()), 0.01f);
            (*streams.Ages())[index] = 0.0f;
        }
    };

    class ColorInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(ColorInitializer, ParticleInitializer)
    public:
        RangeColor color = RangeColor::Constant(Float4{1.0f, 1.0f, 1.0f, 1.0f});
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "color", color); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Color, StreamElementType::Float4);
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            (*streams.Colors())[index] = color.Evaluate(rng.NextFloat());
        }
    };

    class SizeInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(SizeInitializer, ParticleInitializer)
    public:
        RangeFloat2 size = RangeFloat2::Constant(Float2{0.1f, 0.1f});
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "size", size); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Size, StreamElementType::Float2);
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            (*streams.Sizes())[index] = size.Evaluate(rng.NextFloat());
        }
    };

    class RotationInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(RotationInitializer, ParticleInitializer)
    public:
        RangeFloat rotation{0.0f, 6.2831853f};
        RangeFloat rotationSpeed{-2.0f, 2.0f};
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "rotation", rotation);
            core::Serialize(ar, "rotationSpeed", rotationSpeed);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Rotation, StreamElementType::Float);
            streams.EnsureStream(ParticleStreamId::RotationSpeed, StreamElementType::Float);
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            (*streams.Rotations())[index] = rotation.Evaluate(rng.NextFloat());
            (*streams.RotationSpeeds())[index] = rotationSpeed.Evaluate(rng.NextFloat());
        }
    };

    class MeshOrientationInitializer final : public ParticleInitializer
    {
        RTTI_OBJECT(MeshOrientationInitializer, ParticleInitializer)
    public:
        bool randomAxis = true;
        Float3 fixedAxis{0.0f, 1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "randomAxis", randomAxis);
            core::Serialize(ar, "fixedAxis", fixedAxis);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Axis, StreamElementType::Float3);
        }
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            Float3 axis;
            if (randomAxis)
            {
                const f32 z = rng.NextFloat(-1.0f, 1.0f);
                const f32 phi = rng.NextFloat(0.0f, 6.2831853f);
                const f32 r = Sqrt(Max(1.0f - z * z, 0.0f));
                axis = Float3{r * Cos(phi), r * Sin(phi), z};
            }
            else
            {
                axis = (LengthSquared(fixedAxis) > 1e-6f) ? Normalized(fixedAxis) : Float3::UnitY;
            }
            (*streams.Axes())[index] = axis;
        }
    };

    // ---- Behaviors ---------------------------------------------------------------------------

    class GravityBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(GravityBehavior, ParticleBehavior)
    public:
        f32 multiplier = 1.0f;
        Float3 direction{0.0f, -1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "multiplier", multiplier);
            core::Serialize(ar, "direction", direction);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            if (vel == nullptr)
            {
                return;
            }
            const Float3 dv = direction * (9.81f * multiplier * ctx.deltaTime);
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*vel)[i] += dv;
            }
        }
    };

    class DragBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(DragBehavior, ParticleBehavior)
    public:
        f32 drag = 1.0f;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "drag", drag); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            if (vel == nullptr)
            {
                return;
            }
            const f32 factor = Max(1.0f - drag * ctx.deltaTime, 0.0f);
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*vel)[i] *= factor;
            }
        }
    };

    class WindBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(WindBehavior, ParticleBehavior)
    public:
        Float3 force{1.0f, 0.0f, 0.0f};
        f32 turbulence = 0.0f;
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "force", force);
            core::Serialize(ar, "turbulence", turbulence);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            if (vel == nullptr)
            {
                return;
            }
            Random& rng = *ctx.rng;
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 t{rng.NextFloat(-turbulence, turbulence),
                               rng.NextFloat(-turbulence, turbulence),
                               rng.NextFloat(-turbulence, turbulence)};
                (*vel)[i] += (force + t) * ctx.deltaTime;
            }
        }
    };

    class TurbulenceBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(TurbulenceBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        f32 frequency = 1.0f;
        f32 speed = 1.0f;
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "strength", strength);
            core::Serialize(ar, "frequency", frequency);
            core::Serialize(ar, "speed", speed);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::CPUOnly;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            CPUStream<Float3>* pos = streams.Positions();
            if (vel == nullptr || pos == nullptr)
            {
                return;
            }
            const f32 scroll = ctx.totalTime * speed;
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 p = (*pos)[i] * frequency + Float3{scroll, scroll, scroll};
                const Float3 noise{Sin(p.y * 1.7f + p.z), Sin(p.z * 1.3f + p.x),
                                   Sin(p.x * 1.9f + p.y)}; // cheap pseudo-noise
                (*vel)[i] += noise * (strength * ctx.deltaTime);
            }
        }
    };

    class VortexBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(VortexBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        Float3 center{0.0f, 0.0f, 0.0f};
        Float3 axis{0.0f, 1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "strength", strength);
            core::Serialize(ar, "center", center);
            core::Serialize(ar, "axis", axis);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            CPUStream<Float3>* pos = streams.Positions();
            if (vel == nullptr || pos == nullptr)
            {
                return;
            }
            const Float3 a = (LengthSquared(axis) > 1e-6f) ? Normalized(axis) : Float3::UnitY;
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 radial = (*pos)[i] - center;
                const Float3 tangent = Cross(a, radial);
                const f32 dist = Max(Length(radial), 0.1f);
                (*vel)[i] += tangent * (strength * ctx.deltaTime / dist);
            }
        }
    };

    class AttractorBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(AttractorBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        Float3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "strength", strength);
            core::Serialize(ar, "position", position);
            core::Serialize(ar, "radius", radius);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            CPUStream<Float3>* pos = streams.Positions();
            if (vel == nullptr || pos == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 delta = position - (*pos)[i];
                const f32 dist = Length(delta);
                if (dist < 1e-4f)
                {
                    continue;
                }
                f32 s = strength;
                if (radius > 0.0f && dist > radius)
                {
                    s *= radius / dist;
                }
                (*vel)[i] += (delta / dist) * (s * ctx.deltaTime);
            }
        }
    };

    class RadialForceBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(RadialForceBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "strength", strength); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<Float3>* vel = streams.Velocities();
            CPUStream<Float3>* pos = streams.Positions();
            if (vel == nullptr || pos == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 delta = (*pos)[i] - ctx.emitterPosition;
                if (LengthSquared(delta) < 1e-8f)
                {
                    continue;
                }
                (*vel)[i] += Normalized(delta) * (strength * ctx.deltaTime);
            }
        }
    };

    // A world-space collision plane: points with Dot(normal, p) - distance < 0 are behind it.
    struct CollisionPlane
    {
        Float3 normal{0.0f, 1.0f, 0.0f};
        f32 distance = 0.0f;
    };

    // A world-space collision sphere (an analytic obstacle particles bounce off).
    struct CollisionSphere
    {
        Float3 center{0.0f, 0.0f, 0.0f};
        f32 radius = 1.0f;
    };

    // A world-space axis-aligned collision box (center and half-extents).
    struct CollisionBox
    {
        Float3 center{0.0f, 0.0f, 0.0f};
        Float3 halfExtents{0.5f, 0.5f, 0.5f};
    };

    inline void Serialize(ISerializer& ar, CollisionPlane& p)
    {
        core::Serialize(ar, "normal", p.normal);
        core::Serialize(ar, "distance", p.distance);
    }
    inline void Serialize(ISerializer& ar, CollisionSphere& s)
    {
        core::Serialize(ar, "center", s.center);
        core::Serialize(ar, "radius", s.radius);
    }
    inline void Serialize(ISerializer& ar, CollisionBox& b)
    {
        core::Serialize(ar, "center", b.center);
        core::Serialize(ar, "halfExtents", b.halfExtents);
    }

    // Bounces particles off a small set of world planes and spheres (ground, walls, obstacles). On a
    // hit: push to the surface, reflect the normal velocity by `bounce`, damp the tangential velocity
    // by `friction`, and optionally age the particle by
    // `lifetimeLoss` of its remaining life. Runs as a behavior (before integrate), so it corrects last
    // frame's penetration.
    class CollisionBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(CollisionBehavior, ParticleBehavior)
    public:
        static constexpr i32 kMaxPlanes = 4;
        static constexpr i32 kMaxSpheres = 4;
        static constexpr i32 kMaxBoxes = 4;
        CollisionPlane planes[kMaxPlanes]{};
        CollisionSphere spheres[kMaxSpheres]{};
        CollisionBox boxes[kMaxBoxes]{};
        i32 planeCount = 1; // default: the ground plane (y = 0)
        i32 sphereCount = 0;
        i32 boxCount = 0;
        f32 radius = 0.0f;       // particle collision radius (offsets the surface)
        f32 bounce = 0.5f;       // normal restitution (0 = stick, 1 = perfect bounce)
        f32 friction = 0.1f;     // tangential velocity damping on contact [0,1]
        f32 lifetimeLoss = 0.0f; // fraction of remaining life lost per hit [0,1]

        void Serialize(ISerializer& ar) override
        {
            core::Serialize(ar, "planeCount", planeCount);
            core::Serialize(ar, "sphereCount", sphereCount);
            core::Serialize(ar, "boxCount", boxCount);
            for (i32 i = 0; i < Clamp(planeCount, 0, kMaxPlanes); ++i)
            {
                ar.Key("plane");
                ar.BeginObject();
                foundation::particles::Serialize(ar, planes[i]);
                ar.EndObject();
            }
            for (i32 i = 0; i < Clamp(sphereCount, 0, kMaxSpheres); ++i)
            {
                ar.Key("sphere");
                ar.BeginObject();
                foundation::particles::Serialize(ar, spheres[i]);
                ar.EndObject();
            }
            for (i32 i = 0; i < Clamp(boxCount, 0, kMaxBoxes); ++i)
            {
                ar.Key("box");
                ar.BeginObject();
                foundation::particles::Serialize(ar, boxes[i]);
                ar.EndObject();
            }
            core::Serialize(ar, "radius", radius);
            core::Serialize(ar, "bounce", bounce);
            core::Serialize(ar, "friction", friction);
            core::Serialize(ar, "lifetimeLoss", lifetimeLoss);
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext&) override
        {
            CPUStream<Float3>* pos = streams.Positions();
            CPUStream<Float3>* vel = streams.Velocities();
            if (pos == nullptr || vel == nullptr)
            {
                return;
            }
            CPUStream<f32>* ages = streams.Ages();
            CPUStream<f32>* lifes = streams.Lifetimes();
            const i32 np = Min(planeCount, kMaxPlanes);
            const i32 ns = Min(sphereCount, kMaxSpheres);
            const i32 nb = Min(boxCount, kMaxBoxes);
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                for (i32 pl = 0; pl < np; ++pl)
                {
                    const CollisionPlane& plane = planes[pl];
                    const f32 pen = Dot(plane.normal, (*pos)[i]) - plane.distance - radius;
                    if (pen < 0.0f)
                    {
                        Resolve((*pos)[i], (*vel)[i], plane.normal, pen, ages, lifes, i);
                    }
                }
                for (i32 sp = 0; sp < ns; ++sp)
                {
                    const CollisionSphere& s = spheres[sp];
                    const Float3 d = (*pos)[i] - s.center;
                    const f32 dist = Length(d);
                    const f32 pen = dist - s.radius - radius;
                    if (pen < 0.0f && dist > 1e-4f)
                    {
                        Resolve((*pos)[i], (*vel)[i], d / dist, pen, ages, lifes, i);
                    }
                }
                for (i32 bx = 0; bx < nb; ++bx)
                {
                    const CollisionBox& b = boxes[bx];
                    const Float3 d = (*pos)[i] - b.center;
                    const Float3 e{b.halfExtents.x + radius, b.halfExtents.y + radius,
                                   b.halfExtents.z + radius};
                    const Float3 ad{Abs(d.x), Abs(d.y), Abs(d.z)};
                    if (ad.x >= e.x || ad.y >= e.y || ad.z >= e.z)
                    {
                        continue;
                    } // outside the box
                    // Inside: exit along the axis of least penetration.
                    const f32 px = e.x - ad.x, py = e.y - ad.y, pz = e.z - ad.z;
                    Float3 normal;
                    f32 depth;
                    if (px <= py && px <= pz)
                    {
                        normal = Float3{d.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
                        depth = px;
                    }
                    else if (py <= pz)
                    {
                        normal = Float3{0.0f, d.y < 0.0f ? -1.0f : 1.0f, 0.0f};
                        depth = py;
                    }
                    else
                    {
                        normal = Float3{0.0f, 0.0f, d.z < 0.0f ? -1.0f : 1.0f};
                        depth = pz;
                    }
                    Resolve((*pos)[i], (*vel)[i], normal, -depth, ages, lifes, i);
                }
            }
        }

    private:
        // Push a penetrating particle out along the contact normal and reflect its inbound velocity.
        void Resolve(Float3& p, Float3& v, const Float3& normal, f32 penetration,
                     CPUStream<f32>* ages, CPUStream<f32>* lifes, i32 i) const noexcept
        {
            p -= normal * penetration; // push out to the surface (penetration < 0)
            const f32 vn = Dot(v, normal);
            if (vn >= 0.0f)
            {
                return;
            } // already moving away
            const Float3 vNormal = normal * vn;
            const Float3 vTangent = v - vNormal;
            v = vTangent * (1.0f - friction) - vNormal * bounce;
            if (lifetimeLoss > 0.0f && ages != nullptr && lifes != nullptr)
            {
                (*ages)[i] += ((*lifes)[i] - (*ages)[i]) * lifetimeLoss; // age toward death
            }
        }
    };

    // ---- Over-lifetime behaviors (sample t = GetLifeRatio) -----------------------------------

    class ColorOverLifetimeBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(ColorOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveColor curve;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "curve", curve); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Color, StreamElementType::Float4);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext&) override
        {
            if (!curve.IsActive())
            {
                return;
            }
            CPUStream<Float4>* col = streams.Colors();
            if (col == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*col)[i] = curve.Evaluate(streams.GetLifeRatio(i));
            }
        }
    };

    class AlphaOverLifetimeBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(AlphaOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "curve", curve); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Color, StreamElementType::Float4);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext&) override
        {
            if (!curve.IsActive())
            {
                return;
            }
            CPUStream<Float4>* col = streams.Colors();
            if (col == nullptr)
            {
                return;
            }
            // Set the alpha to the curve's opacity envelope (as ColorOverLifetime sets colour). This runs
            // every frame, so it must not accumulate: `w *= curve` would multiply w by a value below 1 each
            // frame and collapse alpha to about 0 within a fraction of a second, so a fade-in curve would
            // render nothing.
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*col)[i].w = curve.Evaluate(streams.GetLifeRatio(i));
            }
        }
    };

    // A particle's size over its life: `size` sets both axes, or, with `separateAxes`, X while
    // `sizeY` sets Y (each a curve of its own keys). An absolute size, not a multiplier.
    class SizeOverLifetimeBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(SizeOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat size;
        bool separateAxes = false;
        ParticleCurveFloat sizeY;
        void Serialize(ISerializer& ar) override
        {
            // Each curve in an object of its own: a curve writes its keys into the object it is in.
            ar.Key("size");
            ar.BeginObject();
            foundation::particles::Serialize(ar, size);
            ar.EndObject();
            core::Serialize(ar, "separateAxes", separateAxes);
            ar.Key("sizeY");
            ar.BeginObject();
            foundation::particles::Serialize(ar, sizeY);
            ar.EndObject();
        }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Size, StreamElementType::Float2);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext&) override
        {
            if (!size.IsActive())
            {
                return;
            }
            CPUStream<Float2>* sizes = streams.Sizes();
            if (sizes == nullptr)
            {
                return;
            }
            const bool separate = separateAxes && sizeY.IsActive();
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const f32 t = streams.GetLifeRatio(i);
                const f32 x = size.Evaluate(t);
                (*sizes)[i] = Float2{x, separate ? sizeY.Evaluate(t) : x};
            }
        }
    };

    class RotationOverLifetimeBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(RotationOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "curve", curve); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Rotation, StreamElementType::Float);
            streams.EnsureStream(ParticleStreamId::RotationSpeed, StreamElementType::Float);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext& ctx) override
        {
            CPUStream<f32>* rot = streams.Rotations();
            CPUStream<f32>* spd = streams.RotationSpeeds();
            if (rot == nullptr || spd == nullptr)
            {
                return;
            }
            const bool active = curve.IsActive();
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const f32 scale = active ? curve.Evaluate(streams.GetLifeRatio(i)) : 1.0f;
                (*rot)[i] += (*spd)[i] * scale * ctx.deltaTime;
            }
        }
    };

    class SpeedOverLifetimeBehavior final : public ParticleBehavior
    {
        RTTI_OBJECT(SpeedOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { core::Serialize(ar, "curve", curve); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer& streams) override
        {
            streams.EnsureStream(ParticleStreamId::Velocity, StreamElementType::Float3);
            streams.EnsureStream(ParticleStreamId::StartVelocity, StreamElementType::Float3);
        }
        void Update(ParticleStreamContainer& streams, ParticleUpdateContext&) override
        {
            if (!curve.IsActive())
            {
                return;
            }
            CPUStream<Float3>* vel = streams.Velocities();
            CPUStream<Float3>* start = streams.StartVelocities();
            if (vel == nullptr || start == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                const Float3 v = (*vel)[i];
                const f32 len = Length(v);
                if (len < 1e-6f)
                {
                    continue;
                }
                const f32 target = Length((*start)[i]) * curve.Evaluate(streams.GetLifeRatio(i));
                (*vel)[i] = (v / len) * target;
            }
        }
    };

    // ---- CPU simulator -----------------------------------------------------------------------

    class CPUSimulator final : public ParticleSimulator
    {
    public:
        void Simulate(ParticleStreamContainer& streams,
                      const Array<RefPtr<ParticleBehavior>>& behaviors,
                      ParticleUpdateContext& ctx) override
        {
            for (usize i = 0; i < behaviors.Size(); ++i)
            {
                behaviors[i]->Update(streams, ctx);
            }
        }
        i32 CompactDead(ParticleStreamContainer& streams) override { return streams.CompactDead(); }
    };

    // ---- Reflection registration -------------------------------------------------------------
    // Modules are reflected ISerializable types. The cooked resource writes each module's reflected
    // type tag and reconstructs it through Serializables().Create(typeId) and ISerializable::Serialize,
    // the same machinery other cooked resources (TextureResource, say) use.

    RTTI_DEFINE_OBJECT(ParticleInitializer, "rtti::particles")
    RTTI_DEFINE_OBJECT(ParticleBehavior, "rtti::particles")
    // Every concrete module's StaticType() is defined with its reflected properties in
    // ParticleModulesImpl.cpp (REFLECT_MEMBERS bodies stay out of module interfaces). The
    // curve-driven OverLifetime behaviors reflect their curves through the BoundedArray container
    // primitive (count-bound key arrays).

    // Registers the reflected range leaf value types (module reflection bodies self-build via
    // REFLECT_MEMBERS). Defined in ParticleModulesImpl.cpp; idempotent.
    void RegisterParticleModuleReflection();

    // Reflects the effect graph value types (ParticleEmitter, ParticleSystem, ParticleEffect, the
    // EmissionMode enum and the Array<UniquePtr<ParticleSystem>> container), so tooling and scripts can
    // traverse a whole effect: its systems, their modules, and their ranges and curves. Defined in
    // ParticleEffectReflectionImpl.cpp (the :effect types are not visible from :modules, so this is a
    // forward declaration resolved at link time within foundation.particles); idempotent.
    void RegisterParticleEffectReflection();

    // Register every module type (the hierarchy) and a construct-by-type-id entry for the concrete ones.
    // Call once at startup before loading cooked particle resources.
    void RegisterParticleModules()
    {
        RegisterParticleModuleReflection(); // the range value types the module properties reference
        GlobalTypeRegistry().Register(ParticleInitializer::StaticType());
        GlobalTypeRegistry().Register(ParticleBehavior::StaticType());
#define PARTICLE_REG(T)                                                                   \
    do                                                                                             \
    {                                                                                              \
        GlobalTypeRegistry().Register(T::StaticType());                                            \
        RegisterSerializable<T>();                                                                 \
    } while (0)
        PARTICLE_REG(PositionInitializer);
        PARTICLE_REG(VelocityInitializer);
        PARTICLE_REG(LifetimeInitializer);
        PARTICLE_REG(ColorInitializer);
        PARTICLE_REG(SizeInitializer);
        PARTICLE_REG(RotationInitializer);
        PARTICLE_REG(MeshOrientationInitializer);
        PARTICLE_REG(GravityBehavior);
        PARTICLE_REG(DragBehavior);
        PARTICLE_REG(WindBehavior);
        PARTICLE_REG(TurbulenceBehavior);
        PARTICLE_REG(VortexBehavior);
        PARTICLE_REG(AttractorBehavior);
        PARTICLE_REG(RadialForceBehavior);
        PARTICLE_REG(CollisionBehavior);
        PARTICLE_REG(ColorOverLifetimeBehavior);
        PARTICLE_REG(AlphaOverLifetimeBehavior);
        PARTICLE_REG(SizeOverLifetimeBehavior);
        PARTICLE_REG(RotationOverLifetimeBehavior);
        PARTICLE_REG(SpeedOverLifetimeBehavior);
#undef PARTICLE_REG
        RegisterParticleEffectReflection(); // effect, system and emitter value types and the systems container
    }
}

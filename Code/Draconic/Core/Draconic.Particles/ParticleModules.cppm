// draconic.particles:modules - the initializer/behavior module taxonomy + the concrete
// modules + the CPU simulator + the runtime type-id registry. Ported from Sedulous.Particles
// (ParticleInitializer.bf, ParticleBehavior.bf, ParticleSimulator.bf, CPUSimulator.bf,
// Initializers/*, Behaviors/*, ParticleTypeRegistry.bf).
//
// An INITIALIZER runs once per spawned particle; a BEHAVIOR runs every frame over all live
// particles. Each declares the streams it needs (lazy allocation). Velocity integration + aging
// are a hardcoded final step on ParticleSystem (not a module). Render "type" is an enum, not a
// module. The GPU simulator is deferred (Phase 6) behind the same interfaces + BehaviorSupport.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h" // DRACONIC_OBJECT / DRACONIC_DEFINE_OBJECT

export module draconic.particles:modules;

import draconic.foundation;
import :types;
import :streams;

using namespace draconic::foundation;

export namespace draconic::particles
{
    // ---- Base classes ------------------------------------------------------------------------

    // Modules are reflected ISerializable objects: reflection gives the type-id, polymorphic
    // reconstruction (Serializables().Create), and the Serialize(ISerializer&) hook - the same
    // machinery cooked resources use. The cook writes each module's reflected type tag + Serialize.
    class ParticleInitializer : public ISerializable
    {
        DRACONIC_OBJECT(ParticleInitializer, ISerializable)
    public:
        [[nodiscard]] virtual BehaviorSupport Support() const noexcept = 0;
        virtual void DeclareStreams(ParticleStreamContainer& streams) = 0;
        virtual void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) = 0;
        // Hook: the system pushes its transform state before a spawn burst so emitter-aware
        // initializers (Position/Velocity) can offset/inherit. Default no-op (avoids an RTTI cast).
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
        DRACONIC_OBJECT(ParticleBehavior, ISerializable)
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
        DRACONIC_OBJECT(PositionInitializer, ParticleInitializer)
    public:
        EmissionShape shape = EmissionShape::Point();
        Float3 emitterPosition{0.0f, 0.0f, 0.0f}; // set by the system each spawn (hidden)
        bool localSpace = false;

        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "shape", shape);
            foundation::Serialize(ar, "localSpace", localSpace);
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
        DRACONIC_OBJECT(VelocityInitializer, ParticleInitializer)
    public:
        Float3 baseVelocity{0.0f, 1.0f, 0.0f};
        Float3 randomness{0.0f, 0.0f, 0.0f};
        f32 shapeDirectionSpeed = 0.0f;
        f32 velocityInheritance = 0.0f;
        EmissionShape shape = EmissionShape::Point();
        Float3 emitterVelocity{0.0f, 0.0f, 0.0f}; // set by the system each spawn (hidden)

        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "baseVelocity", baseVelocity);
            foundation::Serialize(ar, "randomness", randomness);
            foundation::Serialize(ar, "shapeDirectionSpeed", shapeDirectionSpeed);
            foundation::Serialize(ar, "velocityInheritance", velocityInheritance);
            foundation::Serialize(ar, "shape", shape);
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
        DRACONIC_OBJECT(LifetimeInitializer, ParticleInitializer)
    public:
        RangeFloat lifetime{1.0f, 1.0f};
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "lifetime", lifetime); }
        [[nodiscard]] BehaviorSupport Support() const noexcept override
        {
            return BehaviorSupport::Both;
        }
        void DeclareStreams(ParticleStreamContainer&) override {} // Age/Lifetime are core
        void Initialize(ParticleStreamContainer& streams, i32 index, Random& rng) override
        {
            (*streams.Lifetimes())[index] = Max(lifetime.Evaluate(rng.NextFloat()), 0.01f);
            (*streams.Ages())[index] = 0.0f;
        }
    };

    class ColorInitializer final : public ParticleInitializer
    {
        DRACONIC_OBJECT(ColorInitializer, ParticleInitializer)
    public:
        RangeColor color = RangeColor::Constant(Float4{1.0f, 1.0f, 1.0f, 1.0f});
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "color", color); }
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
        DRACONIC_OBJECT(SizeInitializer, ParticleInitializer)
    public:
        RangeFloat2 size = RangeFloat2::Constant(Float2{0.1f, 0.1f});
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "size", size); }
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
        DRACONIC_OBJECT(RotationInitializer, ParticleInitializer)
    public:
        RangeFloat rotation{0.0f, 6.2831853f};
        RangeFloat rotationSpeed{-2.0f, 2.0f};
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "rotation", rotation);
            foundation::Serialize(ar, "rotationSpeed", rotationSpeed);
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
        DRACONIC_OBJECT(MeshOrientationInitializer, ParticleInitializer)
    public:
        bool randomAxis = true;
        Float3 fixedAxis{0.0f, 1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "randomAxis", randomAxis);
            foundation::Serialize(ar, "fixedAxis", fixedAxis);
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
        DRACONIC_OBJECT(GravityBehavior, ParticleBehavior)
    public:
        f32 multiplier = 1.0f;
        Float3 direction{0.0f, -1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "multiplier", multiplier);
            foundation::Serialize(ar, "direction", direction);
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
        DRACONIC_OBJECT(DragBehavior, ParticleBehavior)
    public:
        f32 drag = 1.0f;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "drag", drag); }
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
        DRACONIC_OBJECT(WindBehavior, ParticleBehavior)
    public:
        Float3 force{1.0f, 0.0f, 0.0f};
        f32 turbulence = 0.0f;
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "force", force);
            foundation::Serialize(ar, "turbulence", turbulence);
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
        DRACONIC_OBJECT(TurbulenceBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        f32 frequency = 1.0f;
        f32 speed = 1.0f;
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "strength", strength);
            foundation::Serialize(ar, "frequency", frequency);
            foundation::Serialize(ar, "speed", speed);
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
        DRACONIC_OBJECT(VortexBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        Float3 center{0.0f, 0.0f, 0.0f};
        Float3 axis{0.0f, 1.0f, 0.0f};
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "strength", strength);
            foundation::Serialize(ar, "center", center);
            foundation::Serialize(ar, "axis", axis);
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
        DRACONIC_OBJECT(AttractorBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        Float3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        void Serialize(ISerializer& ar) override
        {
            foundation::Serialize(ar, "strength", strength);
            foundation::Serialize(ar, "position", position);
            foundation::Serialize(ar, "radius", radius);
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
        DRACONIC_OBJECT(RadialForceBehavior, ParticleBehavior)
    public:
        f32 strength = 1.0f;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "strength", strength); }
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

    // A world-space axis-aligned collision box (center + half-extents).
    struct CollisionBox
    {
        Float3 center{0.0f, 0.0f, 0.0f};
        Float3 halfExtents{0.5f, 0.5f, 0.5f};
    };

    inline void Serialize(ISerializer& ar, CollisionPlane& p)
    {
        foundation::Serialize(ar, "normal", p.normal);
        foundation::Serialize(ar, "distance", p.distance);
    }
    inline void Serialize(ISerializer& ar, CollisionSphere& s)
    {
        foundation::Serialize(ar, "center", s.center);
        foundation::Serialize(ar, "radius", s.radius);
    }
    inline void Serialize(ISerializer& ar, CollisionBox& b)
    {
        foundation::Serialize(ar, "center", b.center);
        foundation::Serialize(ar, "halfExtents", b.halfExtents);
    }

    // Bounces particles off a small set of world planes + spheres (ground, walls, obstacles). Beyond
    // Sedulous, which had no collision at all. On a hit: push to the surface, reflect the normal velocity
    // by `bounce`, damp the tangential velocity by `friction`, and optionally age the particle by
    // `lifetimeLoss` of its remaining life. Runs as a behavior (before integrate), so it corrects last
    // frame's penetration.
    class CollisionBehavior final : public ParticleBehavior
    {
        DRACONIC_OBJECT(CollisionBehavior, ParticleBehavior)
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
            foundation::Serialize(ar, "planeCount", planeCount);
            foundation::Serialize(ar, "sphereCount", sphereCount);
            foundation::Serialize(ar, "boxCount", boxCount);
            for (i32 i = 0; i < Clamp(planeCount, 0, kMaxPlanes); ++i)
            {
                ar.Key("plane");
                ar.BeginObject();
                draconic::particles::Serialize(ar, planes[i]);
                ar.EndObject();
            }
            for (i32 i = 0; i < Clamp(sphereCount, 0, kMaxSpheres); ++i)
            {
                ar.Key("sphere");
                ar.BeginObject();
                draconic::particles::Serialize(ar, spheres[i]);
                ar.EndObject();
            }
            for (i32 i = 0; i < Clamp(boxCount, 0, kMaxBoxes); ++i)
            {
                ar.Key("box");
                ar.BeginObject();
                draconic::particles::Serialize(ar, boxes[i]);
                ar.EndObject();
            }
            foundation::Serialize(ar, "radius", radius);
            foundation::Serialize(ar, "bounce", bounce);
            foundation::Serialize(ar, "friction", friction);
            foundation::Serialize(ar, "lifetimeLoss", lifetimeLoss);
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
        DRACONIC_OBJECT(ColorOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveColor curve;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "curve", curve); }
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
        DRACONIC_OBJECT(AlphaOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "curve", curve); }
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
            // SET the alpha to the curve's opacity envelope (like ColorOverLifetime sets colour). This runs
            // every frame, so it must NOT accumulate: `w *= curve` would multiply w by a sub-1 value each
            // frame and collapse alpha to ~0 in a fraction of a second (only masked when the curve holds at
            // 1.0 early, e.g. FadeOut) - which made fade-IN curves like smoke's render as nothing.
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*col)[i].w = curve.Evaluate(streams.GetLifeRatio(i));
            }
        }
    };

    class SizeOverLifetimeBehavior final : public ParticleBehavior
    {
        DRACONIC_OBJECT(SizeOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat2 curve;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "curve", curve); }
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
            if (!curve.IsActive())
            {
                return;
            }
            CPUStream<Float2>* size = streams.Sizes();
            if (size == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < streams.aliveCount; ++i)
            {
                (*size)[i] = curve.Evaluate(streams.GetLifeRatio(i));
            }
        }
    };

    class RotationOverLifetimeBehavior final : public ParticleBehavior
    {
        DRACONIC_OBJECT(RotationOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "curve", curve); }
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
        DRACONIC_OBJECT(SpeedOverLifetimeBehavior, ParticleBehavior)
    public:
        ParticleCurveFloat curve;
        void Serialize(ISerializer& ar) override { foundation::Serialize(ar, "curve", curve); }
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
    // type tag and reconstructs it via Serializables().Create(typeId) + ISerializable::Serialize -
    // the same machinery cooked resources (e.g. TextureResource) use; no hand-rolled string registry.

    DRACONIC_DEFINE_OBJECT(ParticleInitializer, "draconic::particles")
    DRACONIC_DEFINE_OBJECT(ParticleBehavior, "draconic::particles")
    // Every concrete module's StaticType() is defined WITH its reflected properties in
    // ParticleModulesImpl.cpp (GCC module hygiene: DRACONIC_REFLECT out of interfaces). The
    // curve-driven OverLifetime behaviors reflect their curves via the BoundedArray container
    // primitive (count-bound key arrays). All 20 concrete module types are reflected.

    // Registers the reflected range leaf value types (module reflection bodies self-build via
    // DRACONIC_REFLECT). Defined in ParticleModulesImpl.cpp; idempotent.
    void RegisterParticleModuleReflection();

    // Reflects the effect graph value types (ParticleEmitter / ParticleSystem / ParticleEffect + the
    // EmissionMode enum + the Array<UniquePtr<ParticleSystem>> container), so tooling/scripting can
    // traverse a whole effect: effect -> systems -> modules -> ranges/curves. Defined in
    // ParticleEffectReflectionImpl.cpp (the :effect types are not visible from :modules, so this is a
    // forward declaration resolved at link time within draconic.particles); idempotent.
    void RegisterParticleEffectReflection();

    // Register every module type (hierarchy) + a construct-by-type-id entry for the concrete ones.
    // Call once at startup before loading cooked particle resources.
    void RegisterParticleModules()
    {
        RegisterParticleModuleReflection(); // the range value types the module properties reference
        GlobalTypeRegistry().Register(ParticleInitializer::StaticType());
        GlobalTypeRegistry().Register(ParticleBehavior::StaticType());
#define DRACONIC_PARTICLE_REG(T)                                                                   \
    do                                                                                             \
    {                                                                                              \
        GlobalTypeRegistry().Register(T::StaticType());                                            \
        RegisterSerializable<T>();                                                                 \
    } while (0)
        DRACONIC_PARTICLE_REG(PositionInitializer);
        DRACONIC_PARTICLE_REG(VelocityInitializer);
        DRACONIC_PARTICLE_REG(LifetimeInitializer);
        DRACONIC_PARTICLE_REG(ColorInitializer);
        DRACONIC_PARTICLE_REG(SizeInitializer);
        DRACONIC_PARTICLE_REG(RotationInitializer);
        DRACONIC_PARTICLE_REG(MeshOrientationInitializer);
        DRACONIC_PARTICLE_REG(GravityBehavior);
        DRACONIC_PARTICLE_REG(DragBehavior);
        DRACONIC_PARTICLE_REG(WindBehavior);
        DRACONIC_PARTICLE_REG(TurbulenceBehavior);
        DRACONIC_PARTICLE_REG(VortexBehavior);
        DRACONIC_PARTICLE_REG(AttractorBehavior);
        DRACONIC_PARTICLE_REG(RadialForceBehavior);
        DRACONIC_PARTICLE_REG(CollisionBehavior);
        DRACONIC_PARTICLE_REG(ColorOverLifetimeBehavior);
        DRACONIC_PARTICLE_REG(AlphaOverLifetimeBehavior);
        DRACONIC_PARTICLE_REG(SizeOverLifetimeBehavior);
        DRACONIC_PARTICLE_REG(RotationOverLifetimeBehavior);
        DRACONIC_PARTICLE_REG(SpeedOverLifetimeBehavior);
#undef DRACONIC_PARTICLE_REG
        RegisterParticleEffectReflection(); // effect/system/emitter value types + systems container
    }
}

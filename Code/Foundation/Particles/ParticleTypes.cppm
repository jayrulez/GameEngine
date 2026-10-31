// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles:types - the particle value primitives: pure value types over Core math.
// Fields are camelCase; methods are PascalCase.

module;
#include "Core/Prelude.h"

export module foundation.particles:types;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::particles
{
    // ---- Simulation, space and render mode enums ---------------------------------------------

    // Where a system's particle update runs. There is no GPU simulator, so all systems run on the
    // CPU; Auto resolves to GPU only when every behavior supports it and the system is large.
    enum class SimulationMode : u8
    {
        CPU,
        GPU,
        Auto
    };

    // Whether a behavior can run on CPU, GPU, or either. Drives Auto and GPU resolution.
    enum class BehaviorSupport : u8
    {
        CPUOnly,
        GPUOnly,
        Both
    };

    // Simulation coordinate space: World (particles independent of the emitter after spawn) or
    // Local (particles follow the emitter transform).
    enum class ParticleSpace : u8
    {
        World,
        Local
    };

    // Blend mode for a system's draw. Consumed by the render layer.
    enum class ParticleBlendMode : u8
    {
        Alpha,
        Additive,
        Premultiplied,
        Multiply
    };

    // How a system's particles are rendered: a tag the render extractor branches on, not a
    // polymorphic module. Light contributes a point light per particle to the clustered-forward
    // light list, and also draws the billboard glow.
    enum class ParticleRenderMode : u8
    {
        Billboard,
        StretchedBillboard,
        HorizontalBillboard,
        VerticalBillboard,
        Mesh,
        Trail,
        Light
    };

    // ---- Range values ----------------------------------------------------------------------
    // A min/max pair sampled by a single shared t in [0,1): a diagonal lerp from Min to Max, not an
    // independent sample per component.

    struct RangeFloat
    {
        f32 min = 0.0f;
        f32 max = 0.0f;
        constexpr RangeFloat() noexcept = default;
        constexpr explicit RangeFloat(f32 value) noexcept : min(value), max(value) {}
        constexpr RangeFloat(f32 mn, f32 mx) noexcept : min(mn), max(mx) {}
        [[nodiscard]] constexpr bool IsConstant() const noexcept { return min == max; }
        [[nodiscard]] constexpr f32 Evaluate(f32 t) const noexcept { return min + (max - min) * t; }
        [[nodiscard]] static constexpr RangeFloat Constant(f32 v) noexcept { return RangeFloat(v); }
        [[nodiscard]] static constexpr RangeFloat Range(f32 mn, f32 mx) noexcept
        {
            return RangeFloat(mn, mx);
        }
    };

    struct RangeFloat2
    {
        Float2 min{0.0f, 0.0f};
        Float2 max{0.0f, 0.0f};
        constexpr RangeFloat2() noexcept = default;
        constexpr explicit RangeFloat2(Float2 value) noexcept : min(value), max(value) {}
        constexpr RangeFloat2(Float2 mn, Float2 mx) noexcept : min(mn), max(mx) {}
        [[nodiscard]] constexpr Float2 Evaluate(f32 t) const noexcept
        {
            return min + (max - min) * t;
        }
        [[nodiscard]] static constexpr RangeFloat2 Constant(Float2 v) noexcept
        {
            return RangeFloat2(v);
        }
    };

    struct RangeColor
    {
        Float4 min{1.0f, 1.0f, 1.0f, 1.0f};
        Float4 max{1.0f, 1.0f, 1.0f, 1.0f};
        constexpr RangeColor() noexcept = default;
        constexpr explicit RangeColor(Float4 value) noexcept : min(value), max(value) {}
        constexpr RangeColor(Float4 mn, Float4 mx) noexcept : min(mn), max(mx) {}
        [[nodiscard]] constexpr Float4 Evaluate(f32 t) const noexcept
        {
            return min + (max - min) * t;
        }
        [[nodiscard]] static constexpr RangeColor Constant(Float4 v) noexcept
        {
            return RangeColor(v);
        }
    };

    // ---- Curves --------------------------------------------------------------------------------
    // Fixed 8-key curves sampled by normalized lifetime, of Core's types and evaluated by Core's
    // functions, so an editor drawing with the same functions shows what plays. A float curve's keys are CurveKeys (constant, linear or cubic,
    // a tangent mode each); a colour curve is a gradient, its colour stops authored sRGB (decoded to
    // linear only when the particle is drawn) and its alpha stops apart. IsActive() gates the
    // OverLifetime behaviors (an empty curve is a no-op). Values, not containers: reflected as
    // count-bound arrays.

    inline constexpr i32 kMaxCurveKeys = 8;

    struct ParticleCurveFloat
    {
        CurveKey keys[kMaxCurveKeys]{};
        i32 keyCount = 0;

        [[nodiscard]] constexpr bool IsActive() const noexcept { return keyCount > 0; }

        [[nodiscard]] f32 Evaluate(f32 t) const noexcept
        {
            return EvaluateCurve(Span<const CurveKey>(keys, static_cast<usize>(Clamp(keyCount, 0, kMaxCurveKeys))), t);
        }

        // Sorted insert (false once full at the fixed 8-key cap).
        bool AddKey(const CurveKey& key) noexcept
        {
            if (keyCount >= kMaxCurveKeys)
            {
                return false;
            }
            i32 idx = keyCount;
            while (idx > 0 && keys[idx - 1].time > key.time)
            {
                keys[idx] = keys[idx - 1];
                --idx;
            }
            keys[idx] = key;
            ++keyCount;
            return true;
        }
        // A cubic key by default, its tangents as given (flat by default, so a run of them eases from
        // key to key); another interpolation when asked.
        bool AddKey(f32 time, f32 value, f32 tangentIn = 0.0f, f32 tangentOut = 0.0f,
                    CurveKeyInterpolation interpolation = CurveKeyInterpolation::Cubic,
                    CurveTangentMode mode = CurveTangentMode::Broken) noexcept
        {
            return AddKey(CurveKey{time, value, tangentIn, tangentOut, interpolation, mode});
        }

        [[nodiscard]] static ParticleCurveFloat Constant(f32 value) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, value);
            return c;
        }
        // Straight from a to b.
        [[nodiscard]] static ParticleCurveFloat Linear(f32 a, f32 b) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, a, 0.0f, 0.0f, CurveKeyInterpolation::Linear);
            c.AddKey(1.0f, b, 0.0f, 0.0f, CurveKeyInterpolation::Linear);
            return c;
        }
        // From a to b, easing out of a and into b (flat at both ends: a smoothstep).
        [[nodiscard]] static ParticleCurveFloat Ease(f32 a, f32 b) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, a, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(1.0f, b, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            return c;
        }
        // Slow out of a, fast into b.
        [[nodiscard]] static ParticleCurveFloat EaseIn(f32 a, f32 b) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, a, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(1.0f, b, 2.0f * (b - a), 0.0f);
            return c;
        }
        // Fast out of a, slow into b.
        [[nodiscard]] static ParticleCurveFloat EaseOut(f32 a, f32 b) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, a, 0.0f, 2.0f * (b - a));
            c.AddKey(1.0f, b, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            return c;
        }
        // Holds `value`, then eases to 0 over [fadeStart, 1].
        [[nodiscard]] static ParticleCurveFloat FadeOut(f32 value, f32 fadeStart = 0.75f) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, value, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(fadeStart, value, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(1.0f, 0.0f, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            return c;
        }
        // Eases from 0 up to `peakValue` at `peakTime` and back down to 0.
        [[nodiscard]] static ParticleCurveFloat PeakAt(f32 peakValue, f32 peakTime = 0.3f) noexcept
        {
            ParticleCurveFloat c;
            c.AddKey(0.0f, 0.0f, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(peakTime, peakValue, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            c.AddKey(1.0f, 0.0f, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Flat);
            return c;
        }
    };

    struct ParticleCurveColor
    {
        GradientColorStop colors[kMaxCurveKeys]{};
        i32 colorCount = 0;
        GradientAlphaStop alphas[kMaxCurveKeys]{};
        i32 alphaCount = 0;
        GradientInterpolation interpolation = GradientInterpolation::Srgb;

        [[nodiscard]] constexpr bool IsActive() const noexcept { return colorCount > 0 || alphaCount > 0; }

        // The sRGB colour and alpha at `t` (white, opaque, where it has no stops).
        [[nodiscard]] Float4 Evaluate(f32 t) const noexcept
        {
            const Color c = EvaluateGradient(Span<const GradientColorStop>(colors, static_cast<usize>(Clamp(colorCount, 0, kMaxCurveKeys))),
                                             Span<const GradientAlphaStop>(alphas, static_cast<usize>(Clamp(alphaCount, 0, kMaxCurveKeys))),
                                             interpolation, t);
            return Float4{c.r, c.g, c.b, c.a};
        }

        // Sorted inserts (false once full).
        bool AddColor(f32 time, Color color) noexcept { return Insert(colors, colorCount, GradientColorStop{time, color}); }
        bool AddAlpha(f32 time, f32 alpha) noexcept { return Insert(alphas, alphaCount, GradientAlphaStop{time, alpha}); }
        // A colour stop and an alpha stop at one time, from an sRGB rgba.
        bool AddKey(f32 time, Float4 color) noexcept
        {
            const bool c = AddColor(time, Color{color.x, color.y, color.z, 1.0f});
            const bool a = AddAlpha(time, color.w);
            return c && a;
        }

        [[nodiscard]] static ParticleCurveColor Constant(Float4 color) noexcept
        {
            ParticleCurveColor c;
            c.AddKey(0.0f, color);
            return c;
        }
        [[nodiscard]] static ParticleCurveColor Linear(Float4 a, Float4 b) noexcept
        {
            ParticleCurveColor c;
            c.AddKey(0.0f, a);
            c.AddKey(1.0f, b);
            return c;
        }
        // Constant RGB with alpha faded to 0 over [fadeStart, 1].
        [[nodiscard]] static ParticleCurveColor FadeAlpha(Float4 color, f32 fadeStart = 0.75f) noexcept
        {
            ParticleCurveColor c;
            c.AddColor(0.0f, Color{color.x, color.y, color.z, 1.0f});
            c.AddAlpha(0.0f, color.w);
            c.AddAlpha(fadeStart, color.w);
            c.AddAlpha(1.0f, 0.0f);
            return c;
        }

    private:
        template <typename Stop>
        static bool Insert(Stop (&stops)[kMaxCurveKeys], i32& count, const Stop& stop) noexcept
        {
            if (count >= kMaxCurveKeys)
            {
                return false;
            }
            i32 idx = count;
            while (idx > 0 && stops[idx - 1].time > stop.time)
            {
                stops[idx] = stops[idx - 1];
                --idx;
            }
            stops[idx] = stop;
            ++count;
            return true;
        }
    };

    // ---- Emission shape --------------------------------------------------------------------
    // A tagged spawn volume: Sample() returns a spawn-local position and an outward direction.

    enum class EmissionShapeType : u8
    {
        Point,
        Sphere,
        Hemisphere,
        Box,
        Cone,
        Ring,
        Circle,
        Edge
    };

    struct EmissionShape
    {
        EmissionShapeType type = EmissionShapeType::Point;
        f32 radius = 1.0f; // Sphere, Hemisphere, Cone, Ring, Circle; Edge: half-length along X
        Float3 extents{1.0f, 1.0f, 1.0f}; // Box half-extents
        f32 angle = 0.7853982f;           // Cone half-angle (radians)
        f32 arc = 1.0f;             // fraction [0,1] of the full azimuth (Sphere, Cone, Ring, Circle)
        bool emitFromShell = false; // Sphere, Ring, Circle: surface or volume

        // Returns a position (spawn-local, before the emitter offset) and a normalized direction.
        void Sample(Random& rng, Float3& outPosition, Float3& outDirection) const noexcept
        {
            const f32 tau = 6.2831853f * Clamp(arc, 0.0f, 1.0f);
            switch (type)
            {
            case EmissionShapeType::Sphere:
            case EmissionShapeType::Hemisphere:
            {
                const f32 cosMin = (type == EmissionShapeType::Hemisphere) ? 0.0f : -1.0f;
                const f32 z = rng.NextFloat(cosMin, 1.0f);
                const f32 phi = rng.NextFloat(0.0f, tau);
                const f32 r = Sqrt(1.0f - z * z);
                const Float3 dir{r * Cos(phi), r * Sin(phi), z};
                const f32 dist = emitFromShell
                                     ? radius
                                     : radius * Pow(rng.NextFloat(), 1.0f / 3.0f); // volume-uniform
                outPosition = dir * dist;
                outDirection = dir;
                break;
            }
            case EmissionShapeType::Box:
            {
                outPosition = Float3{rng.NextFloat(-extents.x, extents.x),
                                     rng.NextFloat(-extents.y, extents.y),
                                     rng.NextFloat(-extents.z, extents.z)};
                outDirection =
                    (LengthSquared(outPosition) > 1e-6f) ? Normalized(outPosition) : Float3::UnitY;
                break;
            }
            case EmissionShapeType::Cone:
            {
                const f32 phi = rng.NextFloat(0.0f, tau);
                const f32 rr = radius * Sqrt(rng.NextFloat());
                outPosition = Float3{rr * Cos(phi), 0.0f, rr * Sin(phi)};
                const f32 spread = Sin(angle);
                outDirection = Normalized(Float3{spread * Cos(phi), Cos(angle), spread * Sin(phi)});
                break;
            }
            case EmissionShapeType::Ring:
            {
                const f32 phi = rng.NextFloat(0.0f, tau);
                outPosition = Float3{radius * Cos(phi), 0.0f, radius * Sin(phi)};
                outDirection =
                    (LengthSquared(outPosition) > 1e-6f) ? Normalized(outPosition) : Float3::UnitY;
                break;
            }
            case EmissionShapeType::Circle: // filled flat disc on the XZ plane
            {
                const f32 phi = rng.NextFloat(0.0f, tau);
                const f32 rr =
                    emitFromShell ? radius : radius * Sqrt(rng.NextFloat()); // area-uniform
                outPosition = Float3{rr * Cos(phi), 0.0f, rr * Sin(phi)};
                outDirection =
                    (LengthSquared(outPosition) > 1e-6f) ? Normalized(outPosition) : Float3::UnitY;
                break;
            }
            case EmissionShapeType::Edge: // line segment along X, spawns fire upward
            {
                outPosition = Float3{rng.NextFloat(-radius, radius), 0.0f, 0.0f};
                outDirection = Float3::UnitY;
                break;
            }
            case EmissionShapeType::Point:
            default:
                outPosition = Float3::Zero;
                outDirection = Float3::UnitY;
                break;
            }
        }

        [[nodiscard]] static EmissionShape Point() noexcept { return EmissionShape{}; }
        [[nodiscard]] static EmissionShape Sphere(f32 radius, bool shell = false) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Sphere;
            s.radius = radius;
            s.emitFromShell = shell;
            return s;
        }
        [[nodiscard]] static EmissionShape Box(Float3 extents) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Box;
            s.extents = extents;
            return s;
        }
        [[nodiscard]] static EmissionShape Cone(f32 radius, f32 angle) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Cone;
            s.radius = radius;
            s.angle = angle;
            return s;
        }
        [[nodiscard]] static EmissionShape Circle(f32 radius, bool shell = false) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Circle;
            s.radius = radius;
            s.emitFromShell = shell;
            return s;
        }
        [[nodiscard]] static EmissionShape Ring(f32 radius) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Ring;
            s.radius = radius;
            return s;
        }
        [[nodiscard]] static EmissionShape Edge(f32 halfLength) noexcept
        {
            EmissionShape s;
            s.type = EmissionShapeType::Edge;
            s.radius = halfLength;
            return s;
        }
    };

    // ---- Flipbook (texture-sheet animation) ------------------------------------------------
    // Animates a particle's UV rect across a grid of sub-images in its texture atlas. Per system.
    struct FlipbookSettings
    {
        bool enabled = false;
        i32 columns = 1;
        i32 rows = 1;
        f32 fps = 0.0f;           // frames per second when overLifetime is false (time-based looping)
        bool overLifetime = true; // true: sweep the whole sheet once across the particle's lifetime
        i32 startFrame = 0;

        [[nodiscard]] constexpr i32 FrameCount() const noexcept { return columns * rows; }
        [[nodiscard]] constexpr bool IsActive() const noexcept
        {
            return enabled && FrameCount() > 1;
        }

        // The UV sub-rect (min.xy, size.zw) for a particle at the given life ratio or age.
        [[nodiscard]] Float4 FrameUV(f32 lifeRatio, f32 age) const noexcept
        {
            const i32 count = FrameCount();
            i32 frame = overLifetime ? static_cast<i32>(lifeRatio * static_cast<f32>(count))
                                     : static_cast<i32>(age * fps);
            frame = startFrame + frame;
            frame = ((frame % count) + count) % count; // wrap (safe for negatives)
            const i32 col = frame % columns;
            const i32 row = frame / columns;
            const f32 sx = 1.0f / static_cast<f32>(columns);
            const f32 sy = 1.0f / static_cast<f32>(rows);
            return Float4{static_cast<f32>(col) * sx, static_cast<f32>(row) * sy, sx, sy};
        }
    };

    // ---- Trails and ribbons ------------------------------------------------------------------
    // A Trail-render-mode system records each particle's position history into a ring buffer; the
    // render extractor expands those points into a camera-facing ribbon. Settings are per-system.

    struct TrailSettings
    {
        bool enabled = false;
        i32 maxPoints = 16;            // ring-buffer length per particle
        f32 recordInterval = 0.033f;   // min seconds between recorded points
        f32 lifetime = 1.0f;           // a point fades out over this many seconds
        f32 widthStart = 0.15f;        // ribbon half-width at the head (newest)
        f32 widthEnd = 0.0f;           // ribbon half-width at the tail (oldest)
        f32 minVertexDistance = 0.05f; // also record when the particle moves at least this far
        bool useParticleColor = true;  // tint by the particle's color, else trailColor
        Float4 trailColor{1.0f, 1.0f, 1.0f, 1.0f};

        [[nodiscard]] constexpr bool IsActive() const noexcept { return enabled && maxPoints >= 2; }
        [[nodiscard]] static TrailSettings Default() noexcept { return TrailSettings{}; }
    };

    // One recorded point along a particle's trail (a ring-buffer entry).
    struct TrailPoint
    {
        Float3 position{0.0f, 0.0f, 0.0f};
        f32 width = 0.0f;
        Float4 color{1.0f, 1.0f, 1.0f, 1.0f};
        f32 recordTime = 0.0f; // system time when recorded (for age-based fade)
    };

    // Per-particle trail ring-buffer bookkeeping.
    struct ParticleTrailState
    {
        i32 head = 0;  // index of the newest point (writes advance here)
        i32 count = 0; // valid points in the ring
        f32 lastRecordTime = 0.0f;
        Float3 lastPosition{0.0f, 0.0f, 0.0f};
        void Clear() noexcept
        {
            head = 0;
            count = 0;
            lastRecordTime = 0.0f;
            lastPosition = Float3::Zero;
        }
    };

    // ---- Sub-emitter events ------------------------------------------------------------------

    enum class ParticleEventType : u8
    {
        OnBirth,
        OnDeath
    };

    struct ParticleEvent
    {
        Float3 position{0.0f, 0.0f, 0.0f};
        Float3 velocity{0.0f, 0.0f, 0.0f};
        Float4 color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct SubEmitterLink
    {
        ParticleEventType trigger = ParticleEventType::OnDeath;
        i32 childSystemIndex = -1;
        i32 spawnCount = 1;
        f32 probability = 1.0f;
        bool inheritPosition = true;
        bool inheritVelocity = false;
        f32 velocityInheritFactor = 0.5f;
        bool inheritColor = false;

        [[nodiscard]] static SubEmitterLink Default() noexcept { return SubEmitterLink{}; }
    };

    // ---- Per-frame update context ------------------------------------------------------------
    // Passed by reference to every behavior each frame. Rng is borrowed (owned by the system).

    struct ParticleUpdateContext
    {
        f32 totalTime = 0.0f;
        f32 deltaTime = 0.0f;
        Float3 emitterPosition{0.0f, 0.0f, 0.0f};
        Random* rng = nullptr;
    };

    // ---- Serialization (foundation.core :serialize) --------------------------------------------
    // Bidirectional Serialize(ISerializer&, T&) overloads for the authoring value types. Found by ADL
    // from core's Serialize(ar, "key", value). Fields are written one by one, not as a blob, so they
    // read in the XML asset and stay portable in the binary cook. Curves write keyCount then the active keys only.
    using foundation::core::ISerializer;
    inline void Serialize(ISerializer& ar, RangeFloat& v)
    {
        core::Serialize(ar, "min", v.min);
        core::Serialize(ar, "max", v.max);
    }
    inline void Serialize(ISerializer& ar, RangeFloat2& v)
    {
        core::Serialize(ar, "min", v.min);
        core::Serialize(ar, "max", v.max);
    }
    inline void Serialize(ISerializer& ar, RangeColor& v)
    {
        core::Serialize(ar, "min", v.min);
        core::Serialize(ar, "max", v.max);
    }

    // A curve writes its fields into the object it is in; a float key names its interpolation and
    // tangent mode, a colour curve its interpolation and its colour and alpha stops.
    inline void Serialize(ISerializer& ar, ParticleCurveFloat& c)
    {
        core::Serialize(ar, "keyCount", c.keyCount);
        const i32 n = Clamp(c.keyCount, 0, kMaxCurveKeys);
        for (i32 i = 0; i < n; ++i)
        {
            CurveKey& key = c.keys[i];
            ar.Key("key");
            ar.BeginObject();
            core::Serialize(ar, "t", key.time);
            core::Serialize(ar, "v", key.value);
            core::Serialize(ar, "in", key.tangentIn);
            core::Serialize(ar, "out", key.tangentOut);
            core::Serialize(ar, "interp", key.interpolation);
            core::Serialize(ar, "mode", key.tangentMode);
            ar.EndObject();
        }
    }
    inline void Serialize(ISerializer& ar, ParticleCurveColor& c)
    {
        core::Serialize(ar, "interpolation", c.interpolation);
        core::Serialize(ar, "colorCount", c.colorCount);
        for (i32 i = 0; i < Clamp(c.colorCount, 0, kMaxCurveKeys); ++i)
        {
            ar.Key("color");
            ar.BeginObject();
            core::Serialize(ar, "t", c.colors[i].time);
            core::Serialize(ar, "c", c.colors[i].color);
            ar.EndObject();
        }
        core::Serialize(ar, "alphaCount", c.alphaCount);
        for (i32 i = 0; i < Clamp(c.alphaCount, 0, kMaxCurveKeys); ++i)
        {
            ar.Key("alpha");
            ar.BeginObject();
            core::Serialize(ar, "t", c.alphas[i].time);
            core::Serialize(ar, "a", c.alphas[i].alpha);
            ar.EndObject();
        }
    }
    inline void Serialize(ISerializer& ar, EmissionShape& s)
    {
        core::Serialize(ar, "type", s.type);
        core::Serialize(ar, "radius", s.radius);
        core::Serialize(ar, "extents", s.extents);
        core::Serialize(ar, "angle", s.angle);
        core::Serialize(ar, "arc", s.arc);
        core::Serialize(ar, "shell", s.emitFromShell);
    }
    inline void Serialize(ISerializer& ar, TrailSettings& t)
    {
        core::Serialize(ar, "enabled", t.enabled);
        core::Serialize(ar, "maxPoints", t.maxPoints);
        core::Serialize(ar, "recordInterval", t.recordInterval);
        core::Serialize(ar, "lifetime", t.lifetime);
        core::Serialize(ar, "widthStart", t.widthStart);
        core::Serialize(ar, "widthEnd", t.widthEnd);
        core::Serialize(ar, "minVertexDistance", t.minVertexDistance);
        core::Serialize(ar, "useParticleColor", t.useParticleColor);
        core::Serialize(ar, "trailColor", t.trailColor);
    }
    inline void Serialize(ISerializer& ar, FlipbookSettings& f)
    {
        core::Serialize(ar, "enabled", f.enabled);
        core::Serialize(ar, "columns", f.columns);
        core::Serialize(ar, "rows", f.rows);
        core::Serialize(ar, "fps", f.fps);
        core::Serialize(ar, "overLifetime", f.overLifetime);
        core::Serialize(ar, "startFrame", f.startFrame);
    }
    inline void Serialize(ISerializer& ar, SubEmitterLink& l)
    {
        core::Serialize(ar, "trigger", l.trigger);
        core::Serialize(ar, "child", l.childSystemIndex);
        core::Serialize(ar, "spawnCount", l.spawnCount);
        core::Serialize(ar, "probability", l.probability);
        core::Serialize(ar, "inheritPosition", l.inheritPosition);
        core::Serialize(ar, "inheritVelocity", l.inheritVelocity);
        core::Serialize(ar, "velInheritFactor", l.velocityInheritFactor);
        core::Serialize(ar, "inheritColor", l.inheritColor);
    }
}

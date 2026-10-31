// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :curve partition
// A general scalar keyframe curve: an ordered set of {time, value, tangents, interpolation} keys
// sampled at an arbitrary time. EvaluateCurve is the engine's one cubic Hermite over keys: Curve
// samples with it, the particles' fixed curves do, and an editor draws with it, so what is edited is
// what plays. A key's tangent mode says how its tangents are kept (ApplyTangentModes sets them), so
// an editor and an importer agree. Float3/Color tracks hold one Curve per component; Quat tracks
// slerp quaternion keys directly and do not use this (never per-component Euler curves).

module;
#include "Core/Prelude.h"

export module foundation.core:curve;

import :base;
import :math;
import :array;
import :span;

export namespace foundation::core
{
    // How the segment leaving a key is interpolated toward the next key.
    enum class CurveKeyInterpolation : u8
    {
        Constant, // hold this key's value until the next key (step)
        Linear,   // straight lerp to the next key
        Cubic,    // cubic Hermite using tangentOut (this key) + tangentIn (next key)
    };

    // How a key's tangents are kept (ApplyTangentModes).
    enum class CurveTangentMode : u8
    {
        Broken, // in and out as set, each its own (what a key read from older data keeps)
        Auto,   // both the slope between the keys either side (one side at an end)
        Smooth, // in follows out: one slope through the key
        Flat,   // both zero
    };

    // One keyframe. `tangentIn`/`tangentOut` are slopes (value units per time unit); they matter only
    // for Cubic segments (in = arriving at this key from the previous segment; out = leaving toward
    // the next). Constant/Linear ignore them.
    struct CurveKey
    {
        f32 time = 0.0f;
        f32 value = 0.0f;
        f32 tangentIn = 0.0f;
        f32 tangentOut = 0.0f;
        CurveKeyInterpolation interpolation = CurveKeyInterpolation::Linear;
        CurveTangentMode tangentMode = CurveTangentMode::Broken;
    };

    // Cubic Hermite basis: p0 at t=0, p1 at t=1, m0/m1 the endpoint tangents (already scaled).
    [[nodiscard]] constexpr f32 HermiteBasis(f32 p0, f32 m0, f32 p1, f32 m1, f32 t) noexcept
    {
        const f32 t2 = t * t;
        const f32 t3 = t2 * t;
        return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * m1;
    }

    // Samples keys sorted by time at `time`. None -> 0; one -> its value; outside the keys -> the
    // nearest end value (clamped). Between two keys, the left key's interpolation drives the segment;
    // coincident keys jump to the later value.
    [[nodiscard]] inline f32 EvaluateCurve(Span<const CurveKey> keys, f32 time) noexcept
    {
        const usize count = keys.Size();
        if (count == 0)
            return 0.0f;
        if (count == 1 || time <= keys[0].time)
            return keys[0].value;
        if (time >= keys[count - 1].time)
            return keys[count - 1].value;
        usize i = 0; // the segment [i, i+1] holding `time` (a linear scan: key counts are small)
        while (i + 1 < count && keys[i + 1].time <= time)
            ++i;
        const CurveKey& a = keys[i];
        const CurveKey& b = keys[i + 1];
        const f32 segment = b.time - a.time;
        if (segment <= 1e-6f)
            return b.value;
        const f32 localT = (time - a.time) / segment;
        switch (a.interpolation)
        {
        case CurveKeyInterpolation::Constant:
            return a.value;
        case CurveKeyInterpolation::Linear:
            return a.value + (b.value - a.value) * localT;
        case CurveKeyInterpolation::Cubic:
            // Tangents are slopes, scaled by the segment's length to the [0,1] basis.
            return HermiteBasis(a.value, a.tangentOut * segment, b.value, b.tangentIn * segment, localT);
        }
        return a.value;
    }

    // Sets each key's tangents by its mode: Auto from its neighbours, Smooth in from out, Flat to
    // zero; Broken keys are left as they are. Keys sorted by time.
    inline void ApplyTangentModes(Span<CurveKey> keys) noexcept
    {
        const usize count = keys.Size();
        for (usize i = 0; i < count; ++i)
        {
            CurveKey& key = keys[i];
            switch (key.tangentMode)
            {
            case CurveTangentMode::Broken:
                break;
            case CurveTangentMode::Flat:
                key.tangentIn = key.tangentOut = 0.0f;
                break;
            case CurveTangentMode::Smooth:
                key.tangentIn = key.tangentOut;
                break;
            case CurveTangentMode::Auto:
            {
                const CurveKey& before = keys[i > 0 ? i - 1 : i];
                const CurveKey& after = keys[i + 1 < count ? i + 1 : i];
                const f32 span = after.time - before.time;
                key.tangentIn = key.tangentOut = span > 1e-6f ? (after.value - before.value) / span : 0.0f;
                break;
            }
            }
        }
    }

    // An ordered scalar curve. Keys are kept sorted by time (AddKey inserts in order); Evaluate
    // clamps to the end values outside [firstTime, lastTime] (loop and ping-pong wrapping is the
    // clip's job, not the curve's). Cheap value type: copyable, no hidden state.
    class Curve
    {
    public:
        [[nodiscard]] usize KeyCount() const noexcept { return m_keys.Size(); }
        [[nodiscard]] const Array<CurveKey>& Keys() const noexcept { return m_keys; }
        [[nodiscard]] Array<CurveKey>& Keys() noexcept { return m_keys; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_keys.IsEmpty(); }

        // The curve's time span: the last key's time (0 when empty). The first key is expected at
        // t=0 for a normalized clip, but that is a convention, not enforced.
        [[nodiscard]] f32 Duration() const noexcept
        {
            return m_keys.IsEmpty() ? 0.0f : m_keys[m_keys.Size() - 1].time;
        }

        // Insert a key, keeping the array sorted by time (stable for equal times: the new key lands
        // after existing keys of the same time).
        void AddKey(const CurveKey& key)
        {
            usize i = m_keys.Size();
            while (i > 0 && m_keys[i - 1].time > key.time)
            {
                --i;
            }
            m_keys.Insert(i, key);
        }

        void Clear() noexcept { m_keys.Clear(); }

        // Sample the curve at `time` (EvaluateCurve over its keys).
        [[nodiscard]] f32 Evaluate(f32 time) const noexcept { return EvaluateCurve(m_keys.AsSpan(), time); }

        // Sets its keys' tangents by their modes.
        void ApplyTangentModes() noexcept { foundation::core::ApplyTangentModes(m_keys.AsSpan()); }

    private:
        Array<CurveKey> m_keys;
    };
}

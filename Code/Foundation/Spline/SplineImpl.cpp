// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SplineCurve implementation (see Spline.cppm for the model).

module;
#include "Core/Prelude.h"

module foundation.spline;

import foundation.core;

using namespace foundation::core;

namespace foundation::spline
{
    namespace
    {
        // De Casteljau evaluation of one cubic segment.
        [[nodiscard]] Float3 EvalCubic(const Float3& b0, const Float3& b1, const Float3& b2,
                                       const Float3& b3, f32 u)
        {
            const f32 v = 1.0f - u;
            return b0 * (v * v * v) + b1 * (3.0f * v * v * u) + b2 * (3.0f * v * u * u) +
                   b3 * (u * u * u);
        }

        [[nodiscard]] Float3 EvalCubicDerivative(const Float3& b0, const Float3& b1,
                                                 const Float3& b2, const Float3& b3, f32 u)
        {
            const f32 v = 1.0f - u;
            return (b1 - b0) * (3.0f * v * v) + (b2 - b1) * (6.0f * v * u) +
                   (b3 - b2) * (3.0f * u * u);
        }
    }

    void SplineCurve::SegmentControls(u32 index, Float3& b0, Float3& b1, Float3& b2,
                                      Float3& b3) const
    {
        const usize n = points.Size();
        const usize i0 = index % n;
        const usize i1 = (index + 1) % n;
        const SplinePoint& a = points[i0];
        const SplinePoint& b = points[i1];
        b0 = a.position;
        b1 = a.position + a.outHandle;
        b2 = b.position + b.inHandle;
        b3 = b.position;
    }

    void SplineCurve::UpdateAutoHandles()
    {
        const usize n = points.Size();
        if (n < 2)
        {
            return;
        }
        for (usize i = 0; i < n; ++i)
        {
            SplinePoint& point = points[i];
            if (point.mode != SplineHandleMode::Auto)
            {
                continue;
            }
            // Catmull-Rom rule: the tangent is the chord between the neighbors; each handle
            // is a sixth of it (matches the standard Bezier<->Catmull-Rom conversion). Open
            // endpoints fall back to the single adjacent chord.
            const bool hasPrev = closed || i > 0;
            const bool hasNext = closed || i + 1 < n;
            const Float3 prev = hasPrev ? points[(i + n - 1) % n].position : point.position;
            const Float3 next = hasNext ? points[(i + 1) % n].position : point.position;
            const Float3 tangent = (next - prev) * (1.0f / 6.0f);
            point.outHandle = hasNext ? tangent : Float3{};
            point.inHandle = hasPrev ? tangent * -1.0f : Float3{};
        }
    }

    Float3 SplineCurve::Evaluate(f32 t) const
    {
        const u32 segments = SegmentCount();
        if (segments == 0)
        {
            return points.IsEmpty() ? Float3{} : points[0].position;
        }
        if (closed)
        {
            t = t - Floor(t / static_cast<f32>(segments)) * static_cast<f32>(segments);
        }
        t = Clamp(t, 0.0f, static_cast<f32>(segments));
        u32 index = static_cast<u32>(t);
        if (index >= segments)
        {
            index = segments - 1; // t == MaxT lands on the last segment's end
        }
        Float3 b0, b1, b2, b3;
        SegmentControls(index, b0, b1, b2, b3);
        return EvalCubic(b0, b1, b2, b3, t - static_cast<f32>(index));
    }

    Float3 SplineCurve::Tangent(f32 t) const
    {
        const u32 segments = SegmentCount();
        if (segments == 0)
        {
            return Float3{};
        }
        if (closed)
        {
            t = t - Floor(t / static_cast<f32>(segments)) * static_cast<f32>(segments);
        }
        t = Clamp(t, 0.0f, static_cast<f32>(segments));
        u32 index = static_cast<u32>(t);
        if (index >= segments)
        {
            index = segments - 1;
        }
        Float3 b0, b1, b2, b3;
        SegmentControls(index, b0, b1, b2, b3);
        Float3 derivative = EvalCubicDerivative(b0, b1, b2, b3, t - static_cast<f32>(index));
        const f32 length = foundation::core::Length(derivative);
        if (length < 0.000001f)
        {
            // A zero-handle knot (or a degenerate segment): nudge off the exact parameter.
            derivative = EvalCubicDerivative(b0, b1, b2, b3,
                                             Clamp(t - static_cast<f32>(index) + 0.001f, 0.0f, 1.0f));
            const f32 retry = foundation::core::Length(derivative);
            return retry < 0.000001f ? Float3{} : derivative * (1.0f / retry);
        }
        return derivative * (1.0f / length);
    }

    void SplineCurve::RebuildArcLength()
    {
        m_arcLength.Clear();
        m_totalLength = 0.0f;
        const u32 segments = SegmentCount();
        if (segments == 0)
        {
            return;
        }
        // One cumulative entry per table sample: segment * kSamplesPerSegment chords + the
        // leading zero.
        m_arcLength.Reserve(static_cast<usize>(segments) * kSamplesPerSegment + 1);
        m_arcLength.PushBack(0.0f);
        Float3 previous = Evaluate(0.0f);
        for (u32 s = 0; s < segments; ++s)
        {
            for (u32 i = 1; i <= kSamplesPerSegment; ++i)
            {
                const f32 t = static_cast<f32>(s) +
                              static_cast<f32>(i) / static_cast<f32>(kSamplesPerSegment);
                const Float3 position = Evaluate(t);
                m_totalLength += foundation::core::Length(position - previous);
                m_arcLength.PushBack(m_totalLength);
                previous = position;
            }
        }
    }

    f32 SplineCurve::DistanceToT(f32 distance) const
    {
        const u32 segments = SegmentCount();
        if (segments == 0 || m_arcLength.Size() < 2 || m_totalLength <= 0.0f)
        {
            return 0.0f;
        }
        distance = Clamp(distance, 0.0f, m_totalLength);
        // Binary search the cumulative table, then lerp inside the chord.
        usize lo = 0;
        usize hi = m_arcLength.Size() - 1;
        while (lo + 1 < hi)
        {
            const usize mid = (lo + hi) / 2;
            if (m_arcLength[mid] <= distance)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }
        const f32 span = m_arcLength[hi] - m_arcLength[lo];
        const f32 within = span > 0.000001f ? (distance - m_arcLength[lo]) / span : 0.0f;
        const f32 step = 1.0f / static_cast<f32>(kSamplesPerSegment);
        return (static_cast<f32>(lo) + within) * step;
    }

    f32 SplineCurve::TToDistance(f32 t) const
    {
        const u32 segments = SegmentCount();
        if (segments == 0 || m_arcLength.Size() < 2 || m_totalLength <= 0.0f)
        {
            return 0.0f;
        }
        // The table holds kSamplesPerSegment chords per segment: t's sample index, then a lerp
        // inside the chord, as DistanceToT reads it the other way.
        const f32 sample = Clamp(t, 0.0f, static_cast<f32>(segments)) *
                           static_cast<f32>(kSamplesPerSegment);
        const usize last = m_arcLength.Size() - 1;
        usize lo = static_cast<usize>(sample);
        if (lo >= last)
        {
            return m_totalLength;
        }
        const f32 within = sample - static_cast<f32>(lo);
        return m_arcLength[lo] + (m_arcLength[lo + 1] - m_arcLength[lo]) * within;
    }

    SplineSample SplineCurve::ClosestPoint(Float3 target) const
    {
        SplineSample best;
        const u32 segments = SegmentCount();
        if (segments == 0)
        {
            best.position = points.IsEmpty() ? Float3{} : points[0].position;
            return best;
        }
        // Coarse pass over the arc-length sampling density, then a ternary refine around the
        // winner (robust without derivatives of the distance function).
        const u32 coarse = segments * kSamplesPerSegment;
        f32 bestT = 0.0f;
        f32 bestDistSq = kFloatMax;
        for (u32 i = 0; i <= coarse; ++i)
        {
            const f32 t = MaxT() * static_cast<f32>(i) / static_cast<f32>(coarse);
            const Float3 p = Evaluate(t);
            const f32 d = LengthSquared(p - target);
            if (d < bestDistSq)
            {
                bestDistSq = d;
                bestT = t;
            }
        }
        const f32 window = MaxT() / static_cast<f32>(coarse);
        f32 lo = Max(bestT - window, 0.0f);
        f32 hi = Min(bestT + window, MaxT());
        for (u32 i = 0; i < 24; ++i)
        {
            const f32 m1 = lo + (hi - lo) / 3.0f;
            const f32 m2 = hi - (hi - lo) / 3.0f;
            if (LengthSquared(Evaluate(m1) - target) < LengthSquared(Evaluate(m2) - target))
            {
                hi = m2;
            }
            else
            {
                lo = m1;
            }
        }
        best.t = (lo + hi) * 0.5f;
        best.position = Evaluate(best.t);
        return best;
    }
}

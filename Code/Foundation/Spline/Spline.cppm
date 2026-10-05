// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Spline - `foundation.spline`: authorable 3D spline math over Core. A LEAF
/// under Core (the Foundation::Lod precedent - domain math never lands in core itself).
///
/// The curve is CUBIC BEZIER NATIVE: every point carries an in/out handle (relative
/// offsets) plus a mode - Auto derives Catmull-Rom-style handles from the neighbors (drop
/// points, get a smooth curve through them), Smooth keeps user handles collinear, Broken
/// frees them (sharp corners, road-grade arcs). Handles are STORED, never derived at eval:
/// UpdateAutoHandles() resolves Auto points after an edit, so evaluation and serialization
/// are deterministic. Arc length rides a per-segment lookup table (RebuildArcLength after
/// edits) for even spacing and distance-parameterized queries; closest-point is a coarse
/// sample + Newton refine. Open and closed loops.

module;
#include "Core/Prelude.h"

export module foundation.spline;

import foundation.core;

using namespace foundation::core;

export namespace foundation::spline
{
    enum class SplineHandleMode : u8
    {
        Auto,   // handles derived from neighbors (Catmull-Rom rule); recomputed on edit
        Smooth, // user handles, kept collinear by the editor (curve stays C1)
        Broken, // independent handles (corners)
    };

    struct SplinePoint
    {
        Float3 position{};
        Float3 inHandle{};  // RELATIVE to position; toward the previous point
        Float3 outHandle{}; // RELATIVE to position; toward the next point
        SplineHandleMode mode = SplineHandleMode::Auto;
    };

    inline void Serialize(ISerializer& ar, SplinePoint& p)
    {
        foundation::core::Serialize(ar, "position", p.position);
        foundation::core::Serialize(ar, "in", p.inHandle);
        foundation::core::Serialize(ar, "out", p.outHandle);
        u8 mode = static_cast<u8>(p.mode);
        foundation::core::Serialize(ar, "mode", mode);
        p.mode = static_cast<SplineHandleMode>(mode);
    }

    /// A sampled position + parameter pair (closest-point / distance queries).
    struct SplineSample
    {
        Float3 position{};
        f32 t = 0.0f; // global parameter: segment index + local [0,1)
    };

    class SplineCurve
    {
    public:
        Array<SplinePoint> points;
        bool closed = false;

        /// Segments: open = points-1, closed = points (the wrap segment).
        [[nodiscard]] u32 SegmentCount() const noexcept
        {
            const usize n = points.Size();
            if (n < 2)
            {
                return 0;
            }
            return static_cast<u32>(closed ? n : n - 1);
        }

        /// Max global parameter (t in [0, SegmentCount()]).
        [[nodiscard]] f32 MaxT() const noexcept { return static_cast<f32>(SegmentCount()); }

        /// Recompute the handles of every Auto point from its neighbors (call after any
        /// point edit). Smooth/Broken handles are untouched.
        void UpdateAutoHandles();

        /// Position at global t (clamped open / wrapped closed).
        [[nodiscard]] Float3 Evaluate(f32 t) const;

        /// Unit tangent at global t (zero vector on a degenerate segment).
        [[nodiscard]] Float3 Tangent(f32 t) const;

        /// Rebuild the arc-length table (call after any point edit; kSamplesPerSegment
        /// chord samples per segment).
        void RebuildArcLength();

        /// Total length per the last RebuildArcLength (0 until built).
        [[nodiscard]] f32 Length() const noexcept { return m_totalLength; }

        /// Global t at `distance` along the curve (clamped; needs RebuildArcLength).
        [[nodiscard]] f32 DistanceToT(f32 distance) const;

        /// Distance along the curve at global t, DistanceToT's inverse (clamped; needs
        /// RebuildArcLength): how far a closest point is down a course, which t, spread over the
        /// segments however long each is, does not say.
        [[nodiscard]] f32 TToDistance(f32 t) const;

        /// Position at `distance` along the curve (even spacing; needs RebuildArcLength).
        [[nodiscard]] Float3 EvaluateAtDistance(f32 distance) const
        {
            return Evaluate(DistanceToT(distance));
        }

        /// Closest point on the curve to `target` (coarse table sample + refinement;
        /// needs RebuildArcLength for the coarse table).
        [[nodiscard]] SplineSample ClosestPoint(Float3 target) const;

        static constexpr u32 kSamplesPerSegment = 16;

    private:
        // Bezier control points of segment `index` (b0..b3).
        void SegmentControls(u32 index, Float3& b0, Float3& b1, Float3& b2, Float3& b3) const;

        Array<f32> m_arcLength; // cumulative length at each table sample
        f32 m_totalLength = 0.0f;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SplineCurve math: knot interpolation, auto handles, arc length, distance param,
// closest point, closed loops - all headless.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.spline;

using namespace foundation::core;
using namespace foundation::spline;

namespace
{
    [[nodiscard]] SplineCurve Line(Float3 a, Float3 b)
    {
        SplineCurve curve;
        curve.points.PushBack(SplinePoint{a});
        curve.points.PushBack(SplinePoint{b});
        curve.UpdateAutoHandles();
        curve.RebuildArcLength();
        return curve;
    }

    [[nodiscard]] bool Near(Float3 a, Float3 b, f32 eps = 0.001f)
    {
        return Length(a - b) < eps;
    }
}

TEST_CASE("spline: the curve passes through every knot")
{
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{4, 2, 0}});
    curve.points.PushBack(SplinePoint{Float3{8, 0, 3}});
    curve.UpdateAutoHandles();

    CHECK(Near(curve.Evaluate(0.0f), Float3{0, 0, 0}));
    CHECK(Near(curve.Evaluate(1.0f), Float3{4, 2, 0}));
    CHECK(Near(curve.Evaluate(2.0f), Float3{8, 0, 3}));
    CHECK(Near(curve.Evaluate(curve.MaxT()), Float3{8, 0, 3})); // clamped end
}

TEST_CASE("spline: a two-point auto curve is the straight segment")
{
    SplineCurve curve = Line(Float3{0, 0, 0}, Float3{10, 0, 0});
    CHECK(Near(curve.Evaluate(0.5f), Float3{5, 0, 0}));
    CHECK(Near(curve.Tangent(0.5f), Float3{1, 0, 0}));
    CHECK(curve.Length() == doctest::Approx(10.0f).epsilon(0.001));
    CHECK(Near(curve.EvaluateAtDistance(2.5f), Float3{2.5f, 0, 0}, 0.01f));
}

TEST_CASE("spline: auto handles follow the neighbor chord")
{
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{5, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    curve.UpdateAutoHandles();
    // Middle point: chord (10,0,0)-(0,0,0) = (10,0,0); handles are a sixth of it.
    CHECK(Near(curve.points[1].outHandle, Float3{10.0f / 6.0f, 0, 0}));
    CHECK(Near(curve.points[1].inHandle, Float3{-10.0f / 6.0f, 0, 0}));
    // Collinear points -> the whole curve stays on the line.
    curve.RebuildArcLength();
    CHECK(curve.Length() == doctest::Approx(10.0f).epsilon(0.001));
}

TEST_CASE("spline: explicit broken handles bend the segment and survive UpdateAutoHandles")
{
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    curve.points[0].mode = SplineHandleMode::Broken;
    curve.points[0].outHandle = Float3{0, 6, 0}; // pull the start upward
    curve.UpdateAutoHandles();
    CHECK(Near(curve.points[0].outHandle, Float3{0, 6, 0})); // untouched
    CHECK(curve.Evaluate(0.25f).y > 0.5f);                   // the bend is real
}

TEST_CASE("spline: closed loop wraps evaluation and adds the wrap segment")
{
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{10, 0, 10}});
    curve.points.PushBack(SplinePoint{Float3{0, 0, 10}});
    curve.closed = true;
    curve.UpdateAutoHandles();
    curve.RebuildArcLength();

    CHECK(curve.SegmentCount() == 4u);
    CHECK(Near(curve.Evaluate(4.0f), curve.Evaluate(0.0f))); // wraps
    CHECK(Near(curve.Evaluate(-1.0f), curve.Evaluate(3.0f)));
    CHECK(curve.Length() > 39.0f); // >= the square's perimeter (bulges outward slightly)
}

TEST_CASE("spline: closest point lands on the nearest segment")
{
    SplineCurve curve = Line(Float3{0, 0, 0}, Float3{10, 0, 0});
    const SplineSample sample = curve.ClosestPoint(Float3{3, 5, 0});
    CHECK(Near(sample.position, Float3{3, 0, 0}, 0.01f));
    // The Bezier parameter is not linear in distance, so pin self-consistency, not a t value.
    CHECK(Near(curve.Evaluate(sample.t), sample.position, 0.001f));
}

TEST_CASE("spline: TToDistance inverts DistanceToT, on segments of different lengths")
{
    // A short segment then a long one: t's share of the segments (0.5 at the knot) is far from
    // the share of the length, which is what a course needs.
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{2, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{20, 0, 0}});
    curve.UpdateAutoHandles();
    curve.RebuildArcLength();

    CHECK(curve.TToDistance(0.0f) == doctest::Approx(0.0f));
    CHECK(curve.TToDistance(curve.MaxT()) == doctest::Approx(curve.Length()));
    // The middle knot: the first segment's own arc (auto handles bow it past the 2 m chord),
    // measured here by fine chords, independent of the table.
    f32 firstArc = 0.0f;
    for (u32 i = 1; i <= 1000; ++i)
    {
        firstArc += Length(curve.Evaluate(static_cast<f32>(i) / 1000.0f) -
                           curve.Evaluate(static_cast<f32>(i - 1) / 1000.0f));
    }
    CHECK(curve.TToDistance(1.0f) == doctest::Approx(firstArc).epsilon(0.01));
    CHECK(curve.TToDistance(1.0f) < 0.5f * curve.Length()); // not t's half of the segments
    const f32 distances[] = {0.5f, 1.9f, 3.0f, 7.5f, 12.0f, 19.5f};
    for (f32 d : distances)
    {
        CHECK(curve.TToDistance(curve.DistanceToT(d)) == doctest::Approx(d).epsilon(0.001));
    }
    // Out of range clamps; an empty curve answers 0.
    CHECK(curve.TToDistance(-1.0f) == doctest::Approx(0.0f));
    CHECK(curve.TToDistance(9.0f) == doctest::Approx(curve.Length()));
    CHECK(SplineCurve{}.TToDistance(0.5f) == 0.0f);
}

TEST_CASE("spline: distance parameterization spaces samples evenly on a curved path")
{
    SplineCurve curve;
    curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    curve.points.PushBack(SplinePoint{Float3{5, 4, 0}});
    curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    curve.UpdateAutoHandles();
    curve.RebuildArcLength();

    const f32 step = curve.Length() / 8.0f;
    Float3 previous = curve.EvaluateAtDistance(0.0f);
    for (u32 i = 1; i <= 8; ++i)
    {
        const Float3 position = curve.EvaluateAtDistance(step * static_cast<f32>(i));
        const f32 spacing = Length(position - previous);
        CHECK(spacing == doctest::Approx(step).epsilon(0.08)); // chord-table tolerance
        previous = position;
    }
}

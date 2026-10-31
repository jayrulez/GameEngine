// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Curve (foundation.core:curve), the scalar keyframe sampler: exact key hits, per-segment
// interpolation (Constant/Linear/Cubic), end clamping, key ordering, and the Hermite tangent math.

#include <doctest/doctest.h>

import foundation.core;

using namespace foundation::core;

namespace
{
    CurveKey Key(f32 t, f32 v, CurveKeyInterpolation interp = CurveKeyInterpolation::Linear, f32 tin = 0.0f,
                 f32 tout = 0.0f)
    {
        CurveKey k;
        k.time = t;
        k.value = v;
        k.interpolation = interp;
        k.tangentIn = tin;
        k.tangentOut = tout;
        return k;
    }
}

TEST_CASE("curve: empty + single key")
{
    Curve empty;
    CHECK(empty.IsEmpty());
    CHECK(empty.Evaluate(0.0f) == 0.0f);
    CHECK(empty.Duration() == 0.0f);

    Curve one;
    one.AddKey(Key(0.5f, 7.0f));
    CHECK(one.KeyCount() == 1u);
    CHECK(one.Evaluate(0.0f) == 7.0f);   // clamped before
    CHECK(one.Evaluate(0.5f) == 7.0f);   // at
    CHECK(one.Evaluate(10.0f) == 7.0f);  // clamped after
    CHECK(one.Duration() == 0.5f);
}

TEST_CASE("curve: linear interpolation + exact key hits + end clamp")
{
    Curve c;
    c.AddKey(Key(0.0f, 0.0f));
    c.AddKey(Key(1.0f, 10.0f));
    c.AddKey(Key(2.0f, 20.0f));

    // Exact key hits.
    CHECK(c.Evaluate(0.0f) == doctest::Approx(0.0f));
    CHECK(c.Evaluate(1.0f) == doctest::Approx(10.0f));
    CHECK(c.Evaluate(2.0f) == doctest::Approx(20.0f));
    // Between keys.
    CHECK(c.Evaluate(0.5f) == doctest::Approx(5.0f));
    CHECK(c.Evaluate(1.25f) == doctest::Approx(12.5f));
    // Clamp outside the range.
    CHECK(c.Evaluate(-1.0f) == doctest::Approx(0.0f));
    CHECK(c.Evaluate(5.0f) == doctest::Approx(20.0f));
    CHECK(c.Duration() == doctest::Approx(2.0f));
}

TEST_CASE("curve: constant (step) interpolation holds the left value")
{
    Curve c;
    c.AddKey(Key(0.0f, 1.0f, CurveKeyInterpolation::Constant));
    c.AddKey(Key(1.0f, 9.0f, CurveKeyInterpolation::Constant));
    // The left key's mode drives the segment: hold 1.0 across [0,1), jump at the key.
    CHECK(c.Evaluate(0.0f) == doctest::Approx(1.0f));
    CHECK(c.Evaluate(0.5f) == doctest::Approx(1.0f));
    CHECK(c.Evaluate(0.999f) == doctest::Approx(1.0f));
    CHECK(c.Evaluate(1.0f) == doctest::Approx(9.0f));
}

TEST_CASE("curve: cubic Hermite - endpoints exact, flat tangents ease (smoothstep)")
{
    // Zero tangents on both ends over [0,1] with values 0..1 reduce Hermite to smoothstep
    // (3t^2 - 2t^3): endpoints exact, midpoint 0.5, symmetric.
    Curve c;
    c.AddKey(Key(0.0f, 0.0f, CurveKeyInterpolation::Cubic, 0.0f, 0.0f));
    c.AddKey(Key(1.0f, 1.0f, CurveKeyInterpolation::Cubic, 0.0f, 0.0f));
    CHECK(c.Evaluate(0.0f) == doctest::Approx(0.0f));
    CHECK(c.Evaluate(1.0f) == doctest::Approx(1.0f));
    CHECK(c.Evaluate(0.5f) == doctest::Approx(0.5f));               // smoothstep(0.5) = 0.5
    CHECK(c.Evaluate(0.25f) == doctest::Approx(0.15625f));          // 3*.0625 - 2*.015625
    // Symmetry: f(t) + f(1-t) == 1 for smoothstep.
    CHECK(c.Evaluate(0.25f) + c.Evaluate(0.75f) == doctest::Approx(1.0f));
}

TEST_CASE("curve: cubic with matched linear tangents equals the straight line")
{
    // With the tangent equal to the slope (rise over run) on both ends of a value-0..10,
    // time-0..2 segment, the cubic collapses to the straight line through the endpoints.
    Curve c;
    const f32 slope = 5.0f; // (10-0)/(2-0)
    c.AddKey(Key(0.0f, 0.0f, CurveKeyInterpolation::Cubic, slope, slope));
    c.AddKey(Key(2.0f, 10.0f, CurveKeyInterpolation::Cubic, slope, slope));
    CHECK(c.Evaluate(0.5f) == doctest::Approx(2.5f));
    CHECK(c.Evaluate(1.0f) == doctest::Approx(5.0f));
    CHECK(c.Evaluate(1.5f) == doctest::Approx(7.5f));
}

TEST_CASE("curve: AddKey keeps keys sorted regardless of insertion order")
{
    Curve c;
    c.AddKey(Key(2.0f, 20.0f));
    c.AddKey(Key(0.0f, 0.0f));
    c.AddKey(Key(1.0f, 10.0f));
    REQUIRE(c.KeyCount() == 3u);
    CHECK(c.Keys()[0].time == doctest::Approx(0.0f));
    CHECK(c.Keys()[1].time == doctest::Approx(1.0f));
    CHECK(c.Keys()[2].time == doctest::Approx(2.0f));
    // And sampling still works after out-of-order inserts.
    CHECK(c.Evaluate(0.5f) == doctest::Approx(5.0f));
    CHECK(c.Evaluate(1.5f) == doctest::Approx(15.0f));
}

TEST_CASE("curve: coincident keys jump to the later value")
{
    Curve c;
    c.AddKey(Key(0.0f, 0.0f));
    c.AddKey(Key(1.0f, 5.0f));
    c.AddKey(Key(1.0f, 50.0f)); // same time: a hard step
    CHECK(c.Evaluate(0.5f) == doctest::Approx(2.5f));
    // At the coincident pair the segment is degenerate, so the later value wins.
    CHECK(c.Evaluate(1.0f) == doctest::Approx(50.0f));
}

TEST_CASE("curve: EvaluateCurve over a span of keys is the curve's own sampling")
{
    const CurveKey keys[] = {Key(0.0f, 0.0f, CurveKeyInterpolation::Cubic), Key(1.0f, 2.0f, CurveKeyInterpolation::Constant), Key(2.0f, 4.0f)};
    Curve curve;
    for (const CurveKey& k : keys)
        curve.AddKey(k);
    for (f32 t = -0.5f; t <= 2.5f; t += 0.25f)
        CHECK(EvaluateCurve(Span<const CurveKey>(keys, 3), t) == doctest::Approx(curve.Evaluate(t)));
    CHECK(EvaluateCurve(Span<const CurveKey>{}, 1.0f) == doctest::Approx(0.0f));
    CHECK(HermiteBasis(1.0f, 0.0f, 3.0f, 0.0f, 0.5f) == doctest::Approx(2.0f));
}

TEST_CASE("curve: tangent modes set the tangents: auto from the neighbours, smooth in from out, flat zero, broken kept")
{
    CurveKey keys[] = {Key(0.0f, 0.0f, CurveKeyInterpolation::Cubic), Key(1.0f, 1.0f, CurveKeyInterpolation::Cubic),
                       Key(3.0f, 5.0f, CurveKeyInterpolation::Cubic), Key(4.0f, 5.0f, CurveKeyInterpolation::Cubic)};
    for (CurveKey& k : keys)
        k.tangentMode = CurveTangentMode::Auto;
    keys[2].tangentMode = CurveTangentMode::Smooth;
    keys[2].tangentIn = 9.0f;
    keys[2].tangentOut = 0.5f;
    keys[3].tangentMode = CurveTangentMode::Broken;
    keys[3].tangentIn = 7.0f;
    keys[3].tangentOut = -7.0f;
    ApplyTangentModes(Span<CurveKey>(keys, 4));
    CHECK(keys[0].tangentOut == doctest::Approx(1.0f)); // an end: the slope to its one neighbour
    CHECK(keys[1].tangentIn == doctest::Approx(5.0f / 3.0f)); // (5 - 0) / (3 - 0)
    CHECK(keys[1].tangentOut == doctest::Approx(5.0f / 3.0f));
    CHECK(keys[2].tangentIn == doctest::Approx(0.5f)); // smooth: in follows out
    CHECK(keys[3].tangentIn == doctest::Approx(7.0f)); // broken: as set
    CHECK(keys[3].tangentOut == doctest::Approx(-7.0f));

    Curve curve;
    curve.AddKey(Key(0.0f, 0.0f, CurveKeyInterpolation::Cubic, 3.0f, 3.0f));
    curve.Keys()[0].tangentMode = CurveTangentMode::Flat;
    curve.ApplyTangentModes();
    CHECK(curve.Keys()[0].tangentOut == doctest::Approx(0.0f));
    CHECK(CurveKey{}.tangentMode == CurveTangentMode::Broken); // a key read from older data keeps its tangents
}

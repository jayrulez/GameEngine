// Ported from Sedulous.VG.Tests/CurveUtilsTests.bf. Mirrors the Sedulous
// assertions (Test.Assert -> CHECK; Float2 -> Float2; List -> Array; .X/.Y -> .x/.y).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("curveutils: flatten quadratic straight line -> few points")
{
    Array<Float2> output;
    CurveUtils::FlattenQuadratic(Float2{0, 0}, Float2{5, 0}, Float2{10, 0}, 0.25f, output);
    CHECK(output.Size() >= 2u);
    CHECK(output.Size() <= 4u);
}

TEST_CASE("curveutils: flatten cubic curved -> many points")
{
    Array<Float2> output;
    CurveUtils::FlattenCubic(Float2{0, 0}, Float2{0, 10}, Float2{10, 10}, Float2{10, 0}, 0.25f,
                             output);
    CHECK(output.Size() > 4u);
}

TEST_CASE("curveutils: quadratic point at endpoints")
{
    const Float2 p0{0, 0}, p1{5, 10}, p2{10, 0};
    const Float2 start = CurveUtils::QuadraticPointAt(p0, p1, p2, 0.0f);
    const Float2 end = CurveUtils::QuadraticPointAt(p0, p1, p2, 1.0f);
    CHECK(Abs(start.x - p0.x) < 0.001f);
    CHECK(Abs(start.y - p0.y) < 0.001f);
    CHECK(Abs(end.x - p2.x) < 0.001f);
    CHECK(Abs(end.y - p2.y) < 0.001f);
}

TEST_CASE("curveutils: cubic point at endpoints")
{
    const Float2 p0{0, 0}, p1{0, 10}, p2{10, 10}, p3{10, 0};
    const Float2 start = CurveUtils::CubicPointAt(p0, p1, p2, p3, 0.0f);
    const Float2 end = CurveUtils::CubicPointAt(p0, p1, p2, p3, 1.0f);
    CHECK(Abs(start.x - p0.x) < 0.001f);
    CHECK(Abs(start.y - p0.y) < 0.001f);
    CHECK(Abs(end.x - p3.x) < 0.001f);
    CHECK(Abs(end.y - p3.y) < 0.001f);
}

TEST_CASE("curveutils: arc to cubics quarter circle")
{
    Array<Float2> controlPoints;
    CurveUtils::ArcToCubics(Float2{100, 0}, 100, 100, 0, false, true, Float2{0, 100},
                            controlPoints);
    CHECK(controlPoints.Size() >= 3u);
    CHECK(controlPoints.Size() % 3 == 0u);
}

TEST_CASE("curveutils: quadratic length straight line")
{
    const f32 len = CurveUtils::QuadraticLength(Float2{0, 0}, Float2{5, 0}, Float2{10, 0});
    CHECK(Abs(len - 10.0f) < 0.1f);
}

TEST_CASE("curveutils: cubic length straight line")
{
    const f32 len =
        CurveUtils::CubicLength(Float2{0, 0}, Float2{3.33f, 0}, Float2{6.66f, 0}, Float2{10, 0});
    CHECK(Abs(len - 10.0f) < 0.1f);
}

TEST_CASE("curveutils: quadratic tangent at endpoints")
{
    const Float2 tangent =
        CurveUtils::QuadraticTangentAt(Float2{0, 0}, Float2{5, 0}, Float2{10, 0}, 0.5f);
    CHECK(Abs(tangent.x - 1.0f) < 0.01f);
    CHECK(Abs(tangent.y) < 0.01f);
}

// Ported from Sedulous.VG.Tests/TriangulatorTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("triangulator: convex polygon -> correct triangle count")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(points, 4), FillRule::EvenOdd, indices);
    CHECK(indices.Size() == 6u);
}

TEST_CASE("triangulator: concave produces valid mesh")
{
    Float2 points[6] = {{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}};
    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(points, 6), FillRule::EvenOdd, indices);
    CHECK(indices.Size() == 12u);
    for (usize i = 0; i < indices.Size(); ++i)
        CHECK(indices[i] < 6u);
}

TEST_CASE("triangulator: polygon area CCW positive")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(Triangulator::PolygonArea(Span<const Float2>(points, 4)) > 0.0f);
}

TEST_CASE("triangulator: polygon area CW negative")
{
    Float2 points[4] = {{0, 0}, {0, 10}, {10, 10}, {10, 0}};
    CHECK(Triangulator::PolygonArea(Span<const Float2>(points, 4)) < 0.0f);
}

TEST_CASE("triangulator: polygon area correct value")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(Abs(Triangulator::PolygonArea(Span<const Float2>(points, 4)) - 100.0f) < 0.01f);
}

TEST_CASE("triangulator: point in triangle inside")
{
    CHECK(Triangulator::PointInTriangle(Float2{5, 5}, Float2{0, 0}, Float2{10, 0}, Float2{5, 10}));
}

TEST_CASE("triangulator: point in triangle outside")
{
    CHECK_FALSE(
        Triangulator::PointInTriangle(Float2{15, 5}, Float2{0, 0}, Float2{10, 0}, Float2{5, 10}));
}

TEST_CASE("triangulator: pentagon -> correct triangles")
{
    Float2 points[5];
    for (i32 i = 0; i < 5; ++i)
    {
        const f32 angle = kTwoPi * static_cast<f32>(i) / 5.0f - kHalfPi;
        points[static_cast<usize>(i)] = Float2{Cos(angle) * 10.0f, Sin(angle) * 10.0f};
    }
    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(points, 5), FillRule::EvenOdd, indices);
    CHECK(indices.Size() == 9u);
}

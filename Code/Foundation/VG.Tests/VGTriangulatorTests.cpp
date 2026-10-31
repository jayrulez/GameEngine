// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Triangulator: ear clipping, signed area, holes and the fan fallback.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

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

namespace
{
    // The signed area the emitted triangles cover. Equal to the polygon's own area only when
    // the triangles neither overlap nor leave gaps; a triangle count never shows either.
    f32 TriangleArea(Span<const Float2> points, const Array<u32>& indices)
    {
        f32 area = 0.0f;
        for (usize i = 0; i + 2 < indices.Size(); i += 3)
        {
            const Float2 a = points[indices[i]];
            const Float2 b = points[indices[i + 1]];
            const Float2 c = points[indices[i + 2]];
            area += 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
        }
        return area;
    }
}

TEST_CASE("triangulator: the emitted triangles cover exactly the polygon's area (no overlap, no gap)")
{
    // An L-shape: 10x10 minus the 5x5 notch = 75.
    Float2 lShape[6] = {{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}};
    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(lShape, 6), FillRule::EvenOdd, indices);
    CHECK(Abs(TriangleArea(Span<const Float2>(lShape, 6), indices)) == doctest::Approx(75.0f));

    // A 20x20 square with a 10x10 hole = 300, through the hole-merging path.
    Float2 outer[4] = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    Float2 hole[4] = {{5, 5}, {5, 15}, {15, 15}, {15, 5}};
    const Span<const Float2> holes[1] = {Span<const Float2>(hole, 4)};
    Array<u32> holed;
    Array<Float2> merged;
    Triangulator::TriangulateWithHoles(Span<const Float2>(outer, 4),
                                       Span<const Span<const Float2>>(holes, 1), FillRule::EvenOdd,
                                       holed, merged);
    REQUIRE(!merged.IsEmpty());
    CHECK(Abs(TriangleArea(Span<const Float2>(merged.Data(), merged.Size()), holed)) ==
          doctest::Approx(300.0f).epsilon(1e-3));
}

TEST_CASE("triangulator: a self-intersecting bowtie falls back to a bounded fan (no hang, no drop)")
{
    // No valid ear decomposition exists; after a full pass finds no ear the fan fallback
    // fires: every index in range, the shape not dropped, and the call returns.
    Float2 bowtie[4] = {{0, 0}, {10, 10}, {10, 0}, {0, 10}};
    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(bowtie, 4), FillRule::EvenOdd, indices);
    REQUIRE(!indices.IsEmpty());
    CHECK(indices.Size() % 3u == 0u);
    for (const u32 index : indices)
    {
        CHECK(index < 4u);
    }
}

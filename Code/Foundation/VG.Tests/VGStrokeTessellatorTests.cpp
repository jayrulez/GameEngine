// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// StrokeTessellator: strips, caps and the miter limit.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("stroketess: solid line produces a quad strip")
{
    Float2 points[2] = {{0, 0}, {10, 0}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    CHECK(vertices.Size() >= 4u);
    CHECK(indices.Size() >= 6u);
}

TEST_CASE("stroketess: closed path has no caps")
{
    Float2 points[3] = {{0, 0}, {10, 0}, {5, 10}};

    Array<VGVertex> verticesClosed;
    Array<u32> indicesClosed;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), true, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, verticesClosed,
                                  indicesClosed);

    Array<VGVertex> verticesOpen;
    Array<u32> indicesOpen;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 3), false,
                                  StrokeStyle(2.0f, VGLineCap::Square, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, verticesOpen,
                                  indicesOpen);

    CHECK(
        (verticesOpen.Size() > verticesClosed.Size() || indicesOpen.Size() > indicesClosed.Size()));
}

TEST_CASE("stroketess: round cap adds more vertices")
{
    Float2 points[2] = {{0, 0}, {10, 0}};

    Array<VGVertex> vertsButt;
    Array<u32> idxButt;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false,
                                  StrokeStyle(4.0f, VGLineCap::Butt, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertsButt, idxButt);

    Array<VGVertex> vertsRound;
    Array<u32> idxRound;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 2), false,
                                  StrokeStyle(4.0f, VGLineCap::Round, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertsRound, idxRound);

    CHECK(vertsRound.Size() > vertsButt.Size());
}

TEST_CASE("stroketess: polyline multiple segments")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 4), false, StrokeStyle(2.0f),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    CHECK(vertices.Size() >= 8u);
    CHECK(indices.Size() >= 18u);
}

TEST_CASE("stroketess: a hairpin's miter is bounded by the limit (no vertex escapes)")
{
    // As the corner sharpens the miter length goes to infinity; the miter limit must bevel it.
    // A 100-unit line that turns back on itself: nothing may land past 200 units.
    Float2 hairpin[3] = {{0, 0}, {100, 0}, {0, 0.5f}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(hairpin, 3), false,
                                  StrokeStyle(4.0f, VGLineCap::Butt, VGLineJoin::Miter),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    REQUIRE(!vertices.IsEmpty());
    for (const VGVertex& v : vertices)
    {
        CHECK(Abs(v.position.x) < 200.0f);
        CHECK(Abs(v.position.y) < 200.0f);
    }
}

TEST_CASE("stroketess: a zero-length edge has no direction and produces no NaN")
{
    Float2 points[4] = {{0, 0}, {10, 0}, {10, 0}, {20, 5}};
    Array<VGVertex> vertices;
    Array<u32> indices;
    StrokeTessellator::Tessellate(Span<const Float2>(points, 4), false,
                                  StrokeStyle(2.0f, VGLineCap::Round, VGLineJoin::Round),
                                  Span<const f32>{}, false, Color::White, vertices, indices);
    REQUIRE(!vertices.IsEmpty());
    for (const VGVertex& v : vertices)
    {
        CHECK(v.position.x == v.position.x); // a NaN is the one value unequal to itself
        CHECK(v.position.y == v.position.y);
    }
    for (const u32 index : indices)
    {
        CHECK(index < vertices.Size());
    }
}

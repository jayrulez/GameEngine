// Ported from Sedulous.VG.Tests/StrokeTessellatorTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

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

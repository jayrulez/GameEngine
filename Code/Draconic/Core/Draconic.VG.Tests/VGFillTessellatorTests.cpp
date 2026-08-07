// Ported from Sedulous.VG.Tests/FillTessellatorTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

namespace
{
    Path Square()
    {
        PathBuilder b;
        b.MoveTo(0, 0);
        b.LineTo(10, 0);
        b.LineTo(10, 10);
        b.LineTo(0, 10);
        b.Close();
        return b.ToPath();
    }
}

TEST_CASE("filltess: rectangle -> two triangles")
{
    const Path path = Square();
    Array<VGVertex> vertices;
    Array<u32> indices;
    FillTessellator::Tessellate(path, FillRule::EvenOdd, Color::White, false, vertices, indices);
    CHECK(vertices.Size() == 4u);
    CHECK(indices.Size() == 6u);
}

TEST_CASE("filltess: circle creates triangles")
{
    PathBuilder b;
    ShapeBuilder::BuildCircle(Float2{50, 50}, 25, b);
    const Path path = b.ToPath();

    Array<VGVertex> vertices;
    Array<u32> indices;
    FillTessellator::Tessellate(path, FillRule::EvenOdd, Color::Red, false, vertices, indices);
    CHECK(vertices.Size() > 8u);
    CHECK(indices.Size() > 8u);
    CHECK(indices.Size() % 3 == 0u);
}

TEST_CASE("filltess: AA has more vertices")
{
    const Path path = Square();

    Array<VGVertex> vertsNoAA;
    Array<u32> indicesNoAA;
    FillTessellator::Tessellate(path, FillRule::EvenOdd, Color::White, false, vertsNoAA,
                                indicesNoAA);

    Array<VGVertex> vertsAA;
    Array<u32> indicesAA;
    FillTessellator::Tessellate(path, FillRule::EvenOdd, Color::White, true, vertsAA, indicesAA);

    CHECK(vertsAA.Size() > vertsNoAA.Size());
    CHECK(indicesAA.Size() > indicesNoAA.Size());
}

TEST_CASE("filltess: non-AA vertex coverage is one")
{
    const Path path = Square();
    Array<VGVertex> vertices;
    Array<u32> indices;
    FillTessellator::Tessellate(path, FillRule::EvenOdd, Color::White, false, vertices, indices);
    for (usize i = 0; i < vertices.Size(); ++i)
        CHECK(vertices[i].coverage == 1.0f);
}

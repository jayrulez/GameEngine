// VG tessellation: triangulation winding, fill mesh (with/without AA), stroke mesh.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("vg.triangulator: convex quad fans into two triangles")
{
    Float2 quad[] = {Float2{0, 0}, Float2{10, 0}, Float2{10, 10}, Float2{0, 10}};
    CHECK(Triangulator::PolygonArea(Span<const Float2>(quad, 4)) == doctest::Approx(100.0f));

    Array<u32> indices;
    Triangulator::Triangulate(Span<const Float2>(quad, 4), FillRule::NonZero, indices, 0);
    CHECK(indices.Size() == 6u); // 2 triangles
    for (usize i = 0; i < indices.Size(); ++i)
        CHECK(indices[i] < 4u);
}

TEST_CASE("vg.fill: solid square (no AA) -> 4 verts, 2 tris")
{
    PathBuilder b;
    b.MoveTo(0, 0);
    b.LineTo(10, 0);
    b.LineTo(10, 10);
    b.LineTo(0, 10);
    b.Close();
    const Path path = b.ToPath();

    Array<VGVertex> verts;
    Array<u32> idx;
    FillTessellator::Tessellate(path, FillRule::NonZero, Color::Red, /*antiAlias*/ false, verts,
                                idx);
    CHECK(verts.Size() == 4u);
    CHECK(idx.Size() == 6u);
    for (usize i = 0; i < verts.Size(); ++i)
        CHECK(verts[i].color == Color::Red);
}

TEST_CASE("vg.fill: AA adds an inner+outer fringe ring")
{
    PathBuilder b;
    b.MoveTo(0, 0);
    b.LineTo(10, 0);
    b.LineTo(10, 10);
    b.LineTo(0, 10);
    b.Close();
    const Path path = b.ToPath();

    Array<VGVertex> verts;
    Array<u32> idx;
    FillTessellator::Tessellate(path, FillRule::NonZero, Color::Red, /*antiAlias*/ true, verts,
                                idx);
    // Inner ring (4) + outer ring (4); fringe quads + inner fan.
    CHECK(verts.Size() == 8u);
    CHECK(idx.Size() > 6u);
    // Outer ring vertices are the transparent fringe (coverage 0).
    bool sawTransparent = false;
    for (usize i = 0; i < verts.Size(); ++i)
        if (verts[i].coverage == 0.0f && verts[i].color.a == 0.0f)
            sawTransparent = true;
    CHECK(sawTransparent);
}

TEST_CASE("vg.fill: gradient fill interpolates per-vertex color (no AA)")
{
    PathBuilder b;
    b.MoveTo(0, 0);
    b.LineTo(10, 0);
    b.LineTo(10, 10);
    b.LineTo(0, 10);
    b.Close();
    const Path path = b.ToPath();

    VGLinearGradientFill grad(Float2{0, 0}, Float2{10, 0});
    grad.AddStop(0.0f, Color::Black);
    grad.AddStop(1.0f, Color::White);

    Array<VGVertex> verts;
    Array<u32> idx;
    FillTessellator::TessellateWithFill(path, FillRule::NonZero, grad, false, verts, idx);
    REQUIRE(verts.Size() == 4u);
    // Vertices differ in color across the gradient axis.
    bool varied = false;
    for (usize i = 1; i < verts.Size(); ++i)
        if (!(verts[i].color == verts[0].color))
            varied = true;
    CHECK(varied);
}

TEST_CASE("vg.stroke: open polyline (no AA) produces a quad strip")
{
    Float2 line[] = {Float2{0, 0}, Float2{10, 0}, Float2{20, 0}};
    StrokeStyle style(2.0f);

    Array<VGVertex> verts;
    Array<u32> idx;
    StrokeTessellator::Tessellate(Span<const Float2>(line, 3), /*closed*/ false, style,
                                  Span<const f32>{},
                                  /*antiAlias*/ false, Color::Blue, verts, idx);
    CHECK(verts.Size() == 6u); // 2 per point
    CHECK(idx.Size() == 12u);  // 2 segments * 2 tris * 3
}

TEST_CASE("vg.stroke: AA produces 4 rings")
{
    Float2 line[] = {Float2{0, 0}, Float2{10, 0}};
    StrokeStyle style(2.0f);

    Array<VGVertex> verts;
    Array<u32> idx;
    StrokeTessellator::Tessellate(Span<const Float2>(line, 2), false, style, Span<const f32>{},
                                  /*antiAlias*/ true, Color::Blue, verts, idx);
    CHECK(verts.Size() == 8u); // 4 rings * 2 points
    CHECK(idx.Size() > 0u);
}

// VG geometry: PathBuilder/Path queries, flattening, shapes, dashing.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("vg.path: builder + bounds + length on a unit square")
{
    PathBuilder b;
    b.MoveTo(0.0f, 0.0f);
    b.LineTo(10.0f, 0.0f);
    b.LineTo(10.0f, 10.0f);
    b.LineTo(0.0f, 10.0f);
    b.Close();

    const Path path = b.ToPath();
    CHECK(path.SubPathCount() == 1u);
    CHECK(path.CommandCount() == 5u);

    const Rectangle bounds = path.GetBounds();
    CHECK(bounds.x == doctest::Approx(0.0f));
    CHECK(bounds.width == doctest::Approx(10.0f));
    CHECK(bounds.height == doctest::Approx(10.0f));

    // Perimeter of the three explicit edges (Close is implicit, no length).
    CHECK(path.GetLength() == doctest::Approx(30.0f));
}

TEST_CASE("vg.path: contains via fill rules")
{
    PathBuilder b;
    b.MoveTo(0.0f, 0.0f);
    b.LineTo(10.0f, 0.0f);
    b.LineTo(10.0f, 10.0f);
    b.LineTo(0.0f, 10.0f);
    b.Close();
    const Path path = b.ToPath();

    CHECK(path.Contains(Float2{5.0f, 5.0f}, FillRule::NonZero));
    CHECK(path.Contains(Float2{5.0f, 5.0f}, FillRule::EvenOdd));
    CHECK_FALSE(path.Contains(Float2{20.0f, 5.0f}, FillRule::NonZero));
    CHECK_FALSE(path.Contains(Float2{-1.0f, 5.0f}, FillRule::EvenOdd));
}

TEST_CASE("vg.path: point/tangent at distance along a line")
{
    PathBuilder b;
    b.MoveTo(0.0f, 0.0f);
    b.LineTo(10.0f, 0.0f);
    const Path path = b.ToPath();

    CHECK(NearlyEqual(path.GetPointAtDistance(4.0f), Float2{4.0f, 0.0f}));
    CHECK(NearlyEqual(path.GetTangentAtDistance(4.0f), Float2{1.0f, 0.0f}));
}

TEST_CASE("vg.shapes: rounded rect flattens to a closed loop")
{
    PathBuilder b;
    ShapeBuilder::BuildRoundedRect(Rectangle{0.0f, 0.0f, 20.0f, 20.0f}, CornerRadii(4.0f), b);
    const Path path = b.ToPath();

    Array<FlattenedSubPath> subPaths;
    PathFlattener::Flatten(path, 0.25f, subPaths);
    REQUIRE(subPaths.Size() == 1u);
    CHECK(subPaths[0].isClosed);
    CHECK(subPaths[0].points.Size() > 4u); // corners add curve points

    const Rectangle bounds = path.GetBounds();
    CHECK(bounds.width == doctest::Approx(20.0f));
    CHECK(bounds.height == doctest::Approx(20.0f));
}

TEST_CASE("vg.shapes: circle bounds")
{
    PathBuilder b;
    ShapeBuilder::BuildCircle(Float2{50.0f, 50.0f}, 10.0f, b);
    const Path path = b.ToPath();
    const Rectangle bounds = path.GetBounds();
    CHECK(bounds.Center().x == doctest::Approx(50.0f));
    CHECK(bounds.Center().y == doctest::Approx(50.0f));
    CHECK(bounds.width == doctest::Approx(20.0f));
}

TEST_CASE("vg.dash: splits a line into dashes")
{
    Float2 line[] = {Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f}};
    f32 pattern[] = {2.0f, 2.0f}; // 2 on, 2 off
    Array<Array<Float2>> dashes;
    DashGenerator::GenerateDashes(Span<const Float2>(line, 2), false, Span<const f32>(pattern, 2),
                                  0.0f, dashes);

    // 10 units / 4 per cycle => dashes at [0,2],[4,6],[8,10] = 3 segments.
    CHECK(dashes.Size() == 3u);
    for (usize i = 0; i < dashes.Size(); ++i)
        CHECK(dashes[i].Size() >= 2u);
}

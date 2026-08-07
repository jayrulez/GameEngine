// Ported from Sedulous.VG.Tests/PathTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("path: GetBounds correct rect")
{
    PathBuilder builder;
    builder.MoveTo(10, 20);
    builder.LineTo(30, 40);
    builder.LineTo(5, 50);
    builder.Close();

    const Path path = builder.ToPath();
    const Rectangle bounds = path.GetBounds();
    CHECK(Abs(bounds.x - 5.0f) < 0.01f);
    CHECK(Abs(bounds.y - 20.0f) < 0.01f);
    CHECK(Abs(bounds.width - 25.0f) < 0.01f);
    CHECK(Abs(bounds.height - 30.0f) < 0.01f);
}

TEST_CASE("path: contains inside")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    builder.LineTo(10, 10);
    builder.LineTo(0, 10);
    builder.Close();
    const Path path = builder.ToPath();
    CHECK(path.Contains(Float2{5, 5}, FillRule::EvenOdd));
}

TEST_CASE("path: contains outside")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    builder.LineTo(10, 10);
    builder.LineTo(0, 10);
    builder.Close();
    const Path path = builder.ToPath();
    CHECK_FALSE(path.Contains(Float2{15, 5}, FillRule::EvenOdd));
    CHECK_FALSE(path.Contains(Float2{5, 15}, FillRule::EvenOdd));
    CHECK_FALSE(path.Contains(Float2{-5, 5}, FillRule::EvenOdd));
}

TEST_CASE("path: SubPathCount multiple moves")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 10);
    builder.MoveTo(20, 20);
    builder.LineTo(30, 30);
    builder.MoveTo(40, 40);
    builder.LineTo(50, 50);
    const Path path = builder.ToPath();
    CHECK(path.SubPathCount() == 3u);
}

TEST_CASE("path: GetLength straight line")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    const Path path = builder.ToPath();
    CHECK(Abs(path.GetLength() - 10.0f) < 0.01f);
}

TEST_CASE("path: GetPointAtDistance midpoint")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    const Path path = builder.ToPath();
    const Float2 pt = path.GetPointAtDistance(5.0f);
    CHECK(Abs(pt.x - 5.0f) < 0.01f);
    CHECK(Abs(pt.y) < 0.01f);
}

TEST_CASE("path: iterator yields correct segments")
{
    PathBuilder builder;
    builder.MoveTo(0, 0);
    builder.LineTo(10, 0);
    builder.Close();
    const Path path = builder.ToPath();

    PathIterator iter = path.GetIterator();
    PathSegment seg;
    REQUIRE(iter.GetNext(seg));
    CHECK(seg.command == PathCommand::MoveTo);
    REQUIRE(iter.GetNext(seg));
    CHECK(seg.command == PathCommand::LineTo);
    REQUIRE(iter.GetNext(seg));
    CHECK(seg.command == PathCommand::Close);
    CHECK_FALSE(iter.GetNext(seg));
}

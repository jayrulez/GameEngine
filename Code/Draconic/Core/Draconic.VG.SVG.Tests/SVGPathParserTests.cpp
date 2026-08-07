// Ported from Sedulous.VG.Tests/SVGPathParserTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.vg.svg;

using namespace draconic::foundation;
using namespace draconic::vg;
using namespace draconic::vg::svg;

TEST_CASE("svg.path: MoveTo absolute")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 10 20", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 1u);
    CHECK(path.Commands()[0] == PathCommand::MoveTo);
    CHECK(Abs(path.Points()[0].x - 10.0f) < 0.01f);
    CHECK(Abs(path.Points()[0].y - 20.0f) < 0.01f);
}

TEST_CASE("svg.path: MoveTo relative")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 10 20 m 5 5", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 2u);
    CHECK(Abs(path.Points()[1].x - 15.0f) < 0.01f);
    CHECK(Abs(path.Points()[1].y - 25.0f) < 0.01f);
}

TEST_CASE("svg.path: LineTo all variants")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 L 10 0 H 20 V 10 l 5 5 h 5 v 5", builder).IsOk());
    CHECK(builder.ToPath().CommandCount() == 7u);
}

TEST_CASE("svg.path: cubic absolute")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 C 10 20 30 40 50 0", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 2u);
    CHECK(path.Commands()[1] == PathCommand::CubicTo);
}

TEST_CASE("svg.path: cubic relative")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 10 10 c 5 10 15 10 20 0", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.Commands()[1] == PathCommand::CubicTo);
    CHECK(Abs(path.Points()[3].x - 30.0f) < 0.01f); // (10+20)
}

TEST_CASE("svg.path: shorthand cubic reflection")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 C 10 20 30 20 40 0 S 70 -20 80 0", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 3u);
    CHECK(path.Commands()[2] == PathCommand::CubicTo);
}

TEST_CASE("svg.path: quad absolute")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 Q 25 50 50 0", builder).IsOk());
    CHECK(builder.ToPath().Commands()[1] == PathCommand::QuadTo);
}

TEST_CASE("svg.path: quad relative")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 10 10 q 15 30 30 0", builder).IsOk());
    CHECK(builder.ToPath().Commands()[1] == PathCommand::QuadTo);
}

TEST_CASE("svg.path: shorthand quad reflection")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 Q 25 50 50 0 T 100 0", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 3u);
    CHECK(path.Commands()[2] == PathCommand::QuadTo);
}

TEST_CASE("svg.path: arc")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 10 80 A 25 25 0 0 1 50 80", builder).IsOk());
    CHECK(builder.ToPath().CommandCount() >= 2u);
}

TEST_CASE("svg.path: close")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 L 10 0 L 10 10 Z", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.Commands()[path.CommandCount() - 1] == PathCommand::Close);
}

TEST_CASE("svg.path: multiple subpaths")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 L 10 0 Z M 20 0 L 30 0 Z", builder).IsOk());
    CHECK(builder.ToPath().SubPathCount() == 2u);
}

TEST_CASE("svg.path: implicit LineTo after MoveTo")
{
    PathBuilder builder;
    REQUIRE(SVGPathParser::Parse(u8"M 0 0 10 10 20 0", builder).IsOk());
    const Path path = builder.ToPath();
    CHECK(path.CommandCount() == 3u);
    CHECK(path.Commands()[1] == PathCommand::LineTo);
    CHECK(path.Commands()[2] == PathCommand::LineTo);
}

TEST_CASE("svg.path: real-world icon path")
{
    PathBuilder builder;
    const Status r =
        SVGPathParser::Parse(u8"M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 "
                             u8"3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 "
                             u8"5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z",
                             builder);
    REQUIRE(r.IsOk());
    CHECK(builder.ToPath().CommandCount() > 5u);
}

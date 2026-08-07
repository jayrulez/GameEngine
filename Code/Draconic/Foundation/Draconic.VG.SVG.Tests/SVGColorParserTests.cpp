// Ported from Sedulous.VG.Tests/SVGColorParserTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg.svg;

using namespace draconic::foundation;
using namespace draconic::vg::svg;

TEST_CASE("svg.color: hex6")
{
    const Result<Color> r = SVGColorParser::Parse(u8"#ff0000");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 255);
    CHECK(ToColor32(r.Value()).g == 0);
    CHECK(ToColor32(r.Value()).b == 0);
}

TEST_CASE("svg.color: hex3")
{
    const Result<Color> r = SVGColorParser::Parse(u8"#f00");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 255);
    CHECK(ToColor32(r.Value()).g == 0);
    CHECK(ToColor32(r.Value()).b == 0);
}

TEST_CASE("svg.color: named red")
{
    const Result<Color> r = SVGColorParser::Parse(u8"red");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 255);
    CHECK(ToColor32(r.Value()).g == 0);
    CHECK(ToColor32(r.Value()).b == 0);
}

TEST_CASE("svg.color: named blue")
{
    const Result<Color> r = SVGColorParser::Parse(u8"blue");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 0);
    CHECK(ToColor32(r.Value()).g == 0);
    CHECK(ToColor32(r.Value()).b == 255);
}

TEST_CASE("svg.color: rgb() function")
{
    const Result<Color> r = SVGColorParser::Parse(u8"rgb(128, 64, 32)");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 128);
    CHECK(ToColor32(r.Value()).g == 64);
    CHECK(ToColor32(r.Value()).b == 32);
}

TEST_CASE("svg.color: hex mixed case")
{
    const Result<Color> r = SVGColorParser::Parse(u8"#FfAa00");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).r == 255);
    CHECK(ToColor32(r.Value()).g == 170);
    CHECK(ToColor32(r.Value()).b == 0);
}

TEST_CASE("svg.color: none is transparent")
{
    const Result<Color> r = SVGColorParser::Parse(u8"none");
    REQUIRE(r.HasValue());
    CHECK(ToColor32(r.Value()).a == 0);
}

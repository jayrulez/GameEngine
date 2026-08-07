// Ported from Sedulous.VG.Tests/SVGTransformParserTests.bf.
// (M11->m[0][0], M12->m[0][1], M21->m[1][0], M22->m[1][1], M41->m[3][0], M42->m[3][1].)
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg.svg;

using namespace draconic::foundation;
using namespace draconic::vg::svg;

TEST_CASE("svg.transform: translate")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"translate(10, 20)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    CHECK(Abs(m.m[3][0] - 10.0f) < 0.01f);
    CHECK(Abs(m.m[3][1] - 20.0f) < 0.01f);
}

TEST_CASE("svg.transform: scale")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"scale(2, 3)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    CHECK(Abs(m.m[0][0] - 2.0f) < 0.01f);
    CHECK(Abs(m.m[1][1] - 3.0f) < 0.01f);
}

TEST_CASE("svg.transform: scale uniform")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"scale(2)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    CHECK(Abs(m.m[0][0] - 2.0f) < 0.01f);
    CHECK(Abs(m.m[1][1] - 2.0f) < 0.01f);
}

TEST_CASE("svg.transform: rotate 90")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"rotate(90)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    CHECK(Abs(m.m[0][0]) < 0.01f);        // cos(90) ~ 0
    CHECK(Abs(m.m[0][1] - 1.0f) < 0.01f); // sin(90) ~ 1
    CHECK(Abs(m.m[1][0] + 1.0f) < 0.01f);
    CHECK(Abs(m.m[1][1]) < 0.01f);
}

TEST_CASE("svg.transform: combined")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"translate(10, 20) scale(2)");
    CHECK(r.HasValue());
}

TEST_CASE("svg.transform: matrix")
{
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"matrix(1 0 0 1 10 20)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    CHECK(Abs(m.m[0][0] - 1.0f) < 0.01f);
    CHECK(Abs(m.m[3][0] - 10.0f) < 0.01f);
    CHECK(Abs(m.m[3][1] - 20.0f) < 0.01f);
}

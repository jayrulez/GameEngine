// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for SVGTransformParser. SVG matrix entries map to Float4x4 as
// M11->m[0][0], M12->m[0][1], M21->m[1][0], M22->m[1][1], M41->m[3][0], M42->m[3][1].
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg.svg;

using namespace foundation::core;
using namespace foundation::vg::svg;

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

TEST_CASE("svg.transform: rotate(angle, cx, cy) leaves the centre fixed")
{
    // rotate(90, 50, 50): the centre stays put and a point 10 to its right swings to 10 below
    // it (SVG's y-down frame). Three successive pre-multiplications would run the
    // translate-rotate-translate group backwards and put the centre at (-150, 50).
    const Result<Float4x4> r = SVGTransformParser::Parse(u8"rotate(90, 50, 50)");
    REQUIRE(r.HasValue());
    const Float4x4 m = r.Value();
    const Float2 centre = TransformPoint2D(Float2{50.0f, 50.0f}, m);
    CHECK(centre.x == doctest::Approx(50.0f).epsilon(1e-3));
    CHECK(centre.y == doctest::Approx(50.0f).epsilon(1e-3));
    const Float2 right = TransformPoint2D(Float2{60.0f, 50.0f}, m);
    CHECK(right.x == doctest::Approx(50.0f).epsilon(1e-3));
    CHECK(right.y == doctest::Approx(60.0f).epsilon(1e-3));
    // The centred form composes with its neighbours like any other function.
    const Result<Float4x4> combined = SVGTransformParser::Parse(u8"translate(5, 0) rotate(90, 50, 50)");
    REQUIRE(combined.HasValue());
    const Float2 c2 = TransformPoint2D(Float2{50.0f, 50.0f}, combined.Value());
    CHECK(c2.x == doctest::Approx(55.0f).epsilon(1e-3));
    CHECK(c2.y == doctest::Approx(50.0f).epsilon(1e-3));
}


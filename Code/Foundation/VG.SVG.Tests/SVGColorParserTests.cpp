// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for SVGColorParser.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg.svg;

using namespace foundation::core;
using namespace foundation::vg::svg;

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

TEST_CASE("svg.color: the keyword green is the dark one; lime is full green")
{
    const Result<Color> green = SVGColorParser::Parse(u8"green");
    REQUIRE(green.HasValue());
    CHECK(ToColor32(green.Value()).g == 128);
    CHECK(ToColor32(green.Value()).r == 0);
    const Result<Color> lime = SVGColorParser::Parse(u8"lime");
    REQUIRE(lime.HasValue());
    CHECK(ToColor32(lime.Value()).g == 255);
    CHECK(ToColor32(lime.Value()).r == 0);
}


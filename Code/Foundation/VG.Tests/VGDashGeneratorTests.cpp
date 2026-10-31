// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DashGenerator: dash patterns, offsets and closed paths.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("dash: simple pattern correct segment count")
{
    Float2 points[2] = {{0, 0}, {20, 0}};
    f32 pattern[2] = {5, 5};
    Array<Array<Float2>> output;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  0, output);
    CHECK(output.Size() == 2u);
}

TEST_CASE("dash: offset shifts pattern")
{
    Float2 points[2] = {{0, 0}, {20, 0}};
    f32 pattern[2] = {5, 5};

    Array<Array<Float2>> output1;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  0, output1);

    Array<Array<Float2>> output2;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  5.0f, output2);

    CHECK(output2.Size() > 0u);
}

TEST_CASE("dash: closed path wraps")
{
    Float2 points[3] = {{0, 0}, {10, 0}, {5, 10}};
    f32 pattern[2] = {3, 3};
    Array<Array<Float2>> output;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 3), true, Span<const f32>(pattern, 2),
                                  0, output);
    CHECK(output.Size() >= 2u);
}

TEST_CASE("dash: an odd-length pattern repeats to even (SVG rule) - [10] is ten on, ten off")
{
    // A 40-unit line under [10]: dashes at 0-10 and 20-30, gaps between. A one-element pattern
    // read by index parity would stay on index 0 forever and draw a solid line in pieces.
    const Float2 points[2] = {Float2{0.0f, 0.0f}, Float2{40.0f, 0.0f}};
    const f32 pattern[1] = {10.0f};
    Array<Array<Float2>> dashes;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false,
                                  Span<const f32>(pattern, 1), 0.0f, dashes);
    REQUIRE(dashes.Size() == 2u);
    CHECK(dashes[0].Front().x == doctest::Approx(0.0f));
    CHECK(dashes[0].Back().x == doctest::Approx(10.0f));
    CHECK(dashes[1].Front().x == doctest::Approx(20.0f));
    CHECK(dashes[1].Back().x == doctest::Approx(30.0f));

    // Three elements repeat as six: [5, 3, 2] is 5 on, 3 off, 2 on, 5 off, 3 on, 2 off (20).
    const Float2 line[2] = {Float2{0.0f, 0.0f}, Float2{20.0f, 0.0f}};
    const f32 three[3] = {5.0f, 3.0f, 2.0f};
    dashes.Clear();
    DashGenerator::GenerateDashes(Span<const Float2>(line, 2), false, Span<const f32>(three, 3),
                                  0.0f, dashes);
    REQUIRE(dashes.Size() == 3u);
    CHECK(dashes[0].Back().x == doctest::Approx(5.0f));
    CHECK(dashes[1].Front().x == doctest::Approx(8.0f));
    CHECK(dashes[1].Back().x == doctest::Approx(10.0f));
    CHECK(dashes[2].Front().x == doctest::Approx(15.0f));
    CHECK(dashes[2].Back().x == doctest::Approx(18.0f));
}


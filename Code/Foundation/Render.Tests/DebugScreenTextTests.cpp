// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Debug-draw 2D text placement: the right-aligned encoding DrawScreenTextRight stores and
// the pixel x the screen pass resolves it to.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.render;

using namespace foundation::core;
namespace debug = foundation::render::debug;

TEST_CASE("debug-draw: DrawScreenTextRight stores the margin as a negative x and the pass "
          "resolves it against the viewport width")
{
    debug::DebugDraw dd;
    dd.DrawScreenText(12.0f, 12.0f, u8"status", Color{1, 1, 1, 1});
    dd.DrawScreenTextRight(12.0f, 12.0f, u8"60 fps  16.7 ms", Color{1, 1, 1, 1});
    REQUIRE(dd.Commands2D().Size() == 2u);
    const debug::Debug2DCommand& left = dd.Commands2D()[0];
    const debug::Debug2DCommand& right = dd.Commands2D()[1];
    CHECK(left.position.x == 12.0f);
    CHECK(right.position.x < 0.0f); // the encoding: -(margin + 1)
    CHECK(right.textLength == 15);

    const f32 glyph = static_cast<f32>(debug::kCharWidth);
    // A left x passes through; a right x lands so the text ENDS 12 px in from the right edge.
    CHECK(debug::ResolveScreenTextX(left.position.x, left.textLength, glyph, 800) == 12.0f);
    const f32 x = debug::ResolveScreenTextX(right.position.x, right.textLength, glyph, 800);
    CHECK(x == doctest::Approx(800.0f - 12.0f - 15.0f * glyph));
    CHECK(x + 15.0f * glyph == doctest::Approx(788.0f));
    // Scale widens the glyph cell and the resolve follows it.
    const f32 x2 = debug::ResolveScreenTextX(right.position.x, right.textLength, glyph * 2.0f, 800);
    CHECK(x2 == doctest::Approx(800.0f - 12.0f - 15.0f * glyph * 2.0f));
}

TEST_CASE("debug-draw: DrawScreenLine records a line, drawn as a quad its thickness wide")
{
    debug::DebugDraw dd;
    dd.DrawScreenLine(10.0f, 20.0f, 50.0f, 20.0f, Color{1, 0, 0, 1}, 4.0f);
    REQUIRE(dd.Commands2D().Size() == 1u);
    const debug::Debug2DCommand& line = dd.Commands2D()[0];
    CHECK(line.kind == debug::Debug2DKind::Line);
    CHECK(line.position.x == 10.0f);
    CHECK(line.size.x == 50.0f);
    CHECK(line.scale == 4.0f);

    // A horizontal line widens 2 px up and down along its whole length.
    Float2 q[4];
    debug::ScreenLineQuad(Float2{10.0f, 20.0f}, Float2{50.0f, 20.0f}, 4.0f, q);
    CHECK(q[0].x == doctest::Approx(10.0f));
    CHECK(q[1].x == doctest::Approx(50.0f));
    CHECK(Abs(q[0].y - q[3].y) == doctest::Approx(4.0f));
    CHECK(Abs(q[1].y - q[2].y) == doctest::Approx(4.0f));

    // A diagonal one widens across itself: each side's offset is perpendicular to the line.
    debug::ScreenLineQuad(Float2{0.0f, 0.0f}, Float2{30.0f, 40.0f}, 2.0f, q);
    const Float2 along{30.0f, 40.0f};
    const Float2 side{q[0].x - q[3].x, q[0].y - q[3].y};
    CHECK(side.x * along.x + side.y * along.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(Sqrt(side.x * side.x + side.y * side.y) == doctest::Approx(2.0f));

    // A zero-length line still has a width.
    debug::ScreenLineQuad(Float2{5.0f, 5.0f}, Float2{5.0f, 5.0f}, 2.0f, q);
    CHECK(Abs(q[0].y - q[3].y) + Abs(q[0].x - q[3].x) > 0.0f);
}

TEST_CASE("debug-draw: a list carries one shader grid a frame, the last asked for, until cleared")
{
    debug::DebugDraw dd;
    CHECK(dd.GridPlane() == nullptr);
    CHECK_FALSE(dd.HasAnyDraws());
    debug::GridPlaneDesc grid;
    grid.spacing = 1.0f;
    dd.DrawGridPlane(grid);
    grid.spacing = 10.0f;
    dd.DrawGridPlane(grid);
    REQUIRE(dd.GridPlane() != nullptr);
    CHECK(dd.GridPlane()->spacing == 10.0f);
    CHECK(dd.HasAnyDraws());
    dd.Clear();
    CHECK(dd.GridPlane() == nullptr);
    CHECK_FALSE(dd.HasAnyDraws());
}

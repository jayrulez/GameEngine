// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.VG.Tests/VGContextTests.bf. (Sedulous Color32.Yellow is
// inlined; Float4x4 != uses the C++20 rewrite of operator==.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

TEST_CASE("vgcontext: FillRect produces vertices and indices")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{10, 10, 100, 50}, Color::Red);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
    CHECK(batch.CommandCount() > 0u);
}

TEST_CASE("vgcontext: StrokeRect produces output")
{
    VGContext ctx;
    ctx.StrokeRect(Rectangle{10, 10, 100, 50}, Color::Blue, 2.0f);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
}

TEST_CASE("vgcontext: state stack restores transform")
{
    VGContext ctx;
    const Float4x4 identity = ctx.GetTransform();

    ctx.PushState();
    ctx.Translate(100, 200);
    const Float4x4 translated = ctx.GetTransform();
    CHECK(translated != identity);

    ctx.PopState();
    const Float4x4 restored = ctx.GetTransform();
    CHECK(restored == identity);
}

TEST_CASE("vgcontext: clip rect affects commands")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Red);
    ctx.PushClipRect(Rectangle{0, 0, 50, 50});
    ctx.FillRect(Rectangle{5, 5, 10, 10}, Color::Blue);
    ctx.PopClip();

    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.CommandCount() >= 2u);
}

TEST_CASE("vgcontext: opacity applied to vertex color")
{
    VGContext ctx;
    ctx.PushOpacity(0.5f);
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::White);

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() > 0u);
    for (usize i = 0; i < batch.VertexCount(); ++i)
        CHECK(batch.vertices[i].color.a < 0.8f);
}

TEST_CASE("vgcontext: FillCircle produces output")
{
    VGContext ctx;
    ctx.FillCircle(Float2{50, 50}, 25, Color::Green);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 8u);
    CHECK(batch.IndexCount() > 0u);
}

TEST_CASE("vgcontext: rounded rect per-corner radii")
{
    VGContext ctx;
    ctx.FillRoundedRect(Rectangle{0, 0, 100, 100}, CornerRadii(10, 20, 30, 40), Color::White);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 4u);
    CHECK(batch.IndexCount() > 6u);
}

TEST_CASE("vgcontext: clear resets everything")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Red);
    CHECK(ctx.GetBatch().VertexCount() > 0u);

    ctx.Clear();
    CHECK(ctx.GetBatch().VertexCount() == 0u);
}

TEST_CASE("vgcontext: FillStar produces output")
{
    VGContext ctx;
    ctx.FillStar(Float2{50, 50}, 30, 15, 5,
                 ToColor(Color32{255, 255, 0, 255})); // Sedulous Color32.Yellow
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
}

TEST_CASE("vgcontext: IsRectVisible tracks the scissor clip (draw-culling query)")
{
    VGContext ctx;
    // No clip: everything is visible.
    CHECK(ctx.IsRectVisible(Rectangle{10000, 10000, 5, 5}));

    ctx.PushClipRect(Rectangle{0, 0, 100, 100});
    CHECK(ctx.IsRectVisible(Rectangle{50, 50, 10, 10}));    // inside
    CHECK(ctx.IsRectVisible(Rectangle{90, 90, 50, 50}));    // straddles the edge
    CHECK(!ctx.IsRectVisible(Rectangle{200, 200, 10, 10})); // fully outside

    // The query is transform-aware: a translated rect can leave/enter the clip.
    ctx.PushState();
    ctx.Translate(0.0f, 500.0f);
    CHECK(!ctx.IsRectVisible(Rectangle{50, 50, 10, 10})); // now at y=550, clipped out
    ctx.PopState();

    ctx.PopClip();
    CHECK(ctx.IsRectVisible(Rectangle{200, 200, 10, 10})); // clip gone
}

TEST_CASE("vgcontext: IsRectVisible and a clip rect hold under a rotation or a flip")
{
    VGContext ctx;
    ctx.PushClipRect(Rectangle{0, 0, 100, 100});
    // Turned half way round about (50, 50): the box stays where it was, its corners swapped.
    ctx.PushState();
    ctx.Translate(50.0f, 50.0f); // canvas order: the last call applies to points first
    ctx.Rotate(3.14159265f);
    ctx.Translate(-50.0f, -50.0f);
    CHECK(ctx.IsRectVisible(Rectangle{40, 40, 20, 20}));
    CHECK(!ctx.IsRectVisible(Rectangle{-300, 40, 20, 20})); // turned to x = 380
    CHECK(ctx.IsRectVisible(Rectangle{90, 40, 20, 20}));     // turned to straddle the left edge
    ctx.PopState();
    // Flipped: a rect clip pushed inside keeps a positive size.
    ctx.PushState();
    ctx.Translate(100.0f, 0.0f);
    ctx.Scale(-1.0f, 1.0f);
    ctx.PushClipRect(Rectangle{10, 10, 30, 30}); // at x 60 to 90 once flipped
    CHECK(ctx.IsRectVisible(Rectangle{20, 20, 5, 5}));
    CHECK(!ctx.IsRectVisible(Rectangle{70, 20, 5, 5}));
    ctx.PopClip();
    ctx.PopState();
}

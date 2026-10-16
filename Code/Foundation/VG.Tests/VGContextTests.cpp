// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VGContext: batch production, transform/opacity, immediate-mode, images, commands.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
import foundation.vg;
import foundation.fonts;

using namespace foundation::core;
using namespace foundation::vg;
namespace image = foundation::image;

TEST_CASE("vg.context: white texture sits at index 0")
{
    VGContext ctx;
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.textures.Size() >= 1u);
    CHECK(batch.textures[0] != nullptr);
    CHECK(batch.textures[0]->Width() == 1u);
    CHECK(batch.textures[0]->Height() == 1u);
}

TEST_CASE("vg.context: FillRect produces geometry + a solid command")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Red);

    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
    REQUIRE(batch.CommandCount() == 1u);
    CHECK(batch.GetCommand(0).textureIndex == 0); // solid -> white texture
}

TEST_CASE("vg.context: transform is baked into emitted vertices")
{
    VGContext ctx;
    ctx.Translate(100.0f, 50.0f);
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Green);

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() > 0u);
    // Every vertex shifted by the translation (~100,50; allow <1px AA fringe slack).
    bool allShifted = true;
    for (usize i = 0; i < batch.VertexCount(); ++i)
        if (batch.vertices[i].position.x < 99.0f || batch.vertices[i].position.y < 49.0f)
            allShifted = false;
    CHECK(allShifted);
}

TEST_CASE("vg.context: opacity scales vertex alpha")
{
    VGContext ctx;
    ctx.PushOpacity(0.5f);
    ctx.FillRect(Rectangle{0, 0, 10, 10}, ToColor(Color32{255, 255, 255, 255}));
    ctx.PopOpacity();

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() > 0u);
    // Inner (opaque) vertices should now carry ~half alpha.
    bool sawHalfAlpha = false;
    for (usize i = 0; i < batch.VertexCount(); ++i)
        if (batch.vertices[i].color.a > 0.47f && batch.vertices[i].color.a < 0.53f)
            sawHalfAlpha = true;
    CHECK(sawHalfAlpha);
}

TEST_CASE("vg.context: gradient fill bakes + binds a ramp LUT")
{
    VGContext ctx;
    VGLinearGradientFill grad(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
    grad.AddStop(0.0f, Color::Red);
    grad.AddStop(1.0f, Color::Blue);

    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    ctx.FillPath(pb.ToPath(), grad, FillRule::NonZero, /*antiAlias*/ false);

    VGBatch& batch = ctx.GetBatch();
    // A ramp LUT texture was registered beyond the index-0 white passthrough.
    CHECK(batch.textures.Size() >= 2u);
    // Gradient vertices carry white (the LUT supplies color) with non-solid texcoords, so the
    // ramp is sampled per pixel rather than Gouraud-interpolated.
    bool sawGradientVertex = false;
    for (usize i = 0; i < batch.VertexCount(); ++i)
        if (batch.vertices[i].color == Color::White &&
            batch.vertices[i].texCoord.x != VGVertex::SolidUV)
            sawGradientVertex = true;
    CHECK(sawGradientVertex);

    // Clearing frees the per-frame LUT pool and re-seats only the white texture.
    ctx.Clear();
    CHECK(ctx.GetBatch().textures.Size() == 1u);
}

TEST_CASE("vg.context: per-pixel radial gradient emits the radial draw mode + gradient coords")
{
    VGContext ctx;
    ctx.SetPerPixelGradients(true); // host has wired vg_grad_radial/conic
    VGRadialGradientFill grad(Float2{5.0f, 5.0f}, 5.0f);
    grad.AddStop(0.0f, Color::Red);
    grad.AddStop(1.0f, Color::Blue);

    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    ctx.FillPath(pb.ToPath(), grad, FillRule::NonZero, /*antiAlias*/ false);

    VGBatch& batch = ctx.GetBatch();
    // The gradient geometry is routed to the radial per-pixel pipeline.
    bool sawRadialCmd = false;
    for (usize i = 0; i < batch.commands.Size(); ++i)
        if (batch.commands[i].drawMode == VGDrawMode::GradientRadial)
            sawRadialCmd = true;
    CHECK(sawRadialCmd);
    // Vertices carry the gradient-space coordinate (pos-center)/radius, not a [0,1] LUT u; the
    // (0,0) corner maps to (-1,-1), so at least one texcoord is negative.
    bool sawNegativeCoord = false;
    for (usize i = 0; i < batch.VertexCount(); ++i)
        if (batch.vertices[i].texCoord.x < 0.0f)
            sawNegativeCoord = true;
    CHECK(sawNegativeCoord);

    // With the flag off, the same fill stays on the default pipeline (affine LUT approximation).
    VGContext plain;
    plain.FillPath(pb.ToPath(), grad, FillRule::NonZero, /*antiAlias*/ false);
    VGBatch& plainBatch = plain.GetBatch();
    for (usize i = 0; i < plainBatch.commands.Size(); ++i)
        CHECK(plainBatch.commands[i].drawMode == VGDrawMode::Default);
}

TEST_CASE("vg.context: state stack save/restore of transform")
{
    VGContext ctx;
    ctx.Translate(10.0f, 0.0f);
    ctx.PushState();
    ctx.Translate(90.0f, 0.0f);
    CHECK(ctx.GetTransform()(3, 0) == doctest::Approx(100.0f));
    ctx.PopState();
    CHECK(ctx.GetTransform()(3, 0) == doctest::Approx(10.0f));
}

TEST_CASE("vg.context: immediate-mode path fill")
{
    VGContext ctx;
    ctx.BeginPath();
    ctx.MoveTo(0, 0);
    ctx.LineTo(10, 0);
    ctx.LineTo(10, 10);
    ctx.ClosePath();
    ctx.Fill(Color::Blue, FillRule::NonZero, /*antiAlias*/ false);

    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() == 3u);
    CHECK(batch.IndexCount() == 3u);
}

TEST_CASE("vg.context: DrawImage registers the texture and switches command")
{
    VGContext ctx;
    const u8 px[4] = {10, 20, 30, 40};
    image::OwnedImageData tex(1, 1, image::PixelFormat::RGBA8, Span<const u8>(px, 4));

    ctx.FillRect(Rectangle{0, 0, 5, 5}, Color::Red); // solid command (tex 0)
    ctx.DrawImage(&tex, Float2{0, 0});               // textured command (tex 1)

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.textures.Size() == 2u);
    CHECK(batch.textures[1] == &tex);
    REQUIRE(batch.CommandCount() == 2u);
    CHECK(batch.GetCommand(0).textureIndex == 0);
    CHECK(batch.GetCommand(1).textureIndex == 1);
}

TEST_CASE("vg.context: clear resets and re-seeds the white texture")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 5, 5}, Color::Red);
    ctx.Clear();
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() == 0u);
    CHECK(batch.CommandCount() == 0u);
    REQUIRE(batch.textures.Size() == 1u);
    CHECK(batch.textures[0]->Width() == 1u);
}

TEST_CASE("vg.context: identical gradients share ONE cached LUT, stable across frames")
{
    // The LUT cache is content-keyed and PERSISTENT: N fills of the same gradient bake one
    // LUT (not one per FillPath), and the same gradient next frame reuses the same
    // ImageData identity - the stability the renderer's identity-keyed GPU texture cache
    // depends on (a per-frame pool made freed/recycled LUTs cache-hit stale GPU ramps).
    VGContext ctx;
    VGLinearGradientFill grad(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
    grad.AddStop(0.0f, Color::Red);
    grad.AddStop(1.0f, Color::Blue);

    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    const Path path = pb.ToPath();

    ctx.FillPath(path, grad, FillRule::NonZero, false);
    ctx.FillPath(path, grad, FillRule::NonZero, false);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.textures.Size() == 2u); // white + ONE shared LUT, not one per fill
    const foundation::image::ImageData* firstFrameLut = batch.textures[1];

    ctx.Clear();
    CHECK(ctx.GetBatch().evictedTextures.IsEmpty()); // tiny cache: nothing evicted
    ctx.FillPath(path, grad, FillRule::NonZero, false);
    VGBatch& second = ctx.GetBatch();
    REQUIRE(second.textures.Size() == 2u);
    CHECK(second.textures[1] == firstFrameLut); // same identity across frames

    // A DIFFERENT ramp gets its own LUT.
    VGLinearGradientFill other(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
    other.AddStop(0.0f, Color::Green);
    other.AddStop(1.0f, Color::Black);
    ctx.FillPath(path, other, FillRule::NonZero, false);
    CHECK(ctx.GetBatch().textures.Size() == 3u);
}

TEST_CASE("vg.fills: ApplyGradientSpread pad/repeat/reflect mapping")
{
    using foundation::vg::ApplyGradientSpread;
    using foundation::vg::VGGradientSpread;
    // Pad clamps.
    CHECK(ApplyGradientSpread(-0.5f, VGGradientSpread::Pad) == doctest::Approx(0.0f));
    CHECK(ApplyGradientSpread(1.7f, VGGradientSpread::Pad) == doctest::Approx(1.0f));
    // Repeat wraps (fractional part).
    CHECK(ApplyGradientSpread(1.25f, VGGradientSpread::Repeat) == doctest::Approx(0.25f));
    CHECK(ApplyGradientSpread(-0.25f, VGGradientSpread::Repeat) == doctest::Approx(0.75f));
    // Reflect mirrors every other period.
    CHECK(ApplyGradientSpread(0.25f, VGGradientSpread::Reflect) == doctest::Approx(0.25f));
    CHECK(ApplyGradientSpread(1.25f, VGGradientSpread::Reflect) == doctest::Approx(0.75f));
    CHECK(ApplyGradientSpread(2.25f, VGGradientSpread::Reflect) == doctest::Approx(0.25f));
    CHECK(ApplyGradientSpread(-0.25f, VGGradientSpread::Reflect) == doctest::Approx(0.25f));
}

TEST_CASE("vg.context: gradient spread rides the command and cuts the batch")
{
    // Two fills of the SAME ramp with different spreads share one LUT but may not share
    // one command: the spread picks the LUT sampler, which lives in the bind group.
    VGContext ctx;
    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    const Path path = pb.ToPath();

    VGLinearGradientFill pad(Float2{0.0f, 0.0f}, Float2{5.0f, 0.0f});
    pad.AddStop(0.0f, Color::Red);
    pad.AddStop(1.0f, Color::Blue);
    VGLinearGradientFill repeat = pad;
    repeat.spread = foundation::vg::VGGradientSpread::Repeat;

    ctx.FillPath(path, pad, FillRule::NonZero, false);
    ctx.FillPath(path, repeat, FillRule::NonZero, false);
    VGBatch& batch = ctx.GetBatch(); // flushes the open command
    CHECK(batch.textures.Size() == 2u); // white + ONE shared LUT
    REQUIRE(batch.commands.Size() >= 2u);
    const VGCommand& first = batch.commands[batch.commands.Size() - 2];
    const VGCommand& second = batch.commands[batch.commands.Size() - 1];
    CHECK(first.gradientSpread == foundation::vg::VGGradientSpread::Pad);
    CHECK(second.gradientSpread == foundation::vg::VGGradientSpread::Repeat);
    CHECK(first.textureIndex == second.textureIndex); // same LUT, different sampler
}

TEST_CASE("vg.tessellation: non-pad linear gradients emit the RAW parameter")
{
    // Pad compresses to LUT texel centers (clamp sampler); repeat/reflect must emit raw
    // t so the sampler's wrap/mirror applies per pixel - a per-vertex clamp would kill
    // the tiling. A gradient line spanning HALF the shape puts t=2 at the far edge.
    VGLinearGradientFill repeat(Float2{0.0f, 0.0f}, Float2{5.0f, 0.0f});
    repeat.AddStop(0.0f, Color::Red);
    repeat.AddStop(1.0f, Color::Blue);
    repeat.spread = foundation::vg::VGGradientSpread::Repeat;
    const Rectangle bounds{0.0f, 0.0f, 10.0f, 10.0f};
    const Float2 rawFar = FillTessellator::GradientTexCoord(
        foundation::vg::VGGradientTess::LinearLut, repeat, Float2{10.0f, 0.0f}, bounds);
    CHECK(rawFar.x == doctest::Approx(2.0f)); // raw, NOT clamped/compressed

    VGLinearGradientFill pad = repeat;
    pad.spread = foundation::vg::VGGradientSpread::Pad;
    const Float2 padFar = FillTessellator::GradientTexCoord(
        foundation::vg::VGGradientTess::LinearLut, pad, Float2{10.0f, 0.0f}, bounds);
    CHECK(padFar.x == doctest::Approx(255.5f / 256.0f)); // clamped to the last texel center
}

TEST_CASE("vg.context: blend mode rides the command and cuts the batch")
{
    VGContext ctx;
    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    const Path path = pb.ToPath();

    ctx.FillPath(path, Color::Red, FillRule::NonZero, false);
    ctx.SetBlendMode(foundation::vg::VGBlendMode::Additive);
    ctx.FillPath(path, Color::Blue, FillRule::NonZero, false);
    ctx.SetBlendMode(foundation::vg::VGBlendMode::Normal);
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() >= 2u);
    const VGCommand& first = batch.commands[batch.commands.Size() - 2];
    const VGCommand& second = batch.commands[batch.commands.Size() - 1];
    CHECK(first.blendMode == foundation::vg::VGBlendMode::Normal);
    CHECK(second.blendMode == foundation::vg::VGBlendMode::Additive);
}

TEST_CASE("vg.context: PushClipPath emits write+apply, marks draws, PopClipPath clears")
{
    VGContext ctx;
    ctx.SetStencilFills(true);
    PathBuilder clip;
    clip.MoveTo(0, 0);
    clip.LineTo(20, 0);
    clip.LineTo(20, 20);
    clip.LineTo(0, 20);
    clip.Close();
    ctx.PushClipPath(clip.ToPath());

    PathBuilder pb;
    pb.MoveTo(5, 5);
    pb.LineTo(15, 5);
    pb.LineTo(15, 15);
    pb.LineTo(5, 15);
    pb.Close();
    ctx.FillPath(pb.ToPath(), Color::Red, FillRule::NonZero, false);
    ctx.PopClipPath();
    ctx.FillPath(pb.ToPath(), Color::Blue, FillRule::NonZero, false);

    VGBatch& batch = ctx.GetBatch();
    // Expected command stream: StencilWrite (clip winding), ClipApply, the CLIPPED
    // fill, ClipClear, the unclipped fill.
    REQUIRE(batch.commands.Size() == 5u);
    CHECK(batch.commands[0].fillPhase == foundation::vg::VGFillPhase::StencilWrite);
    CHECK(batch.commands[0].clipMode == foundation::vg::VGClipMode::None); // mask writing
    CHECK(batch.commands[1].fillPhase == foundation::vg::VGFillPhase::ClipApply);
    CHECK(batch.commands[2].fillPhase == foundation::vg::VGFillPhase::Direct);
    CHECK(batch.commands[2].clipMode == foundation::vg::VGClipMode::Stencil);
    CHECK(batch.commands[3].fillPhase == foundation::vg::VGFillPhase::ClipClear);
    CHECK(batch.commands[4].clipMode == foundation::vg::VGClipMode::None);
}

TEST_CASE("vg.context: a COMPLEX fill inside a clip keeps both stencil roles")
{
    // A self-intersecting star inside a path clip: the fill's write/cover commands must
    // carry clipMode Stencil (the renderer picks the clip-aware pipelines that confine
    // winding to the mask and RESTORE the clip bit on cover).
    VGContext ctx;
    ctx.SetStencilFills(true);
    PathBuilder clip;
    clip.MoveTo(0, 0);
    clip.LineTo(40, 0);
    clip.LineTo(40, 40);
    clip.LineTo(0, 40);
    clip.Close();
    ctx.PushClipPath(clip.ToPath());

    PathBuilder star;
    star.MoveTo(20, 2);
    star.LineTo(30, 34);
    star.LineTo(4, 14);
    star.LineTo(36, 14);
    star.LineTo(10, 34);
    star.Close();
    ctx.FillPath(star.ToPath(), Color::Green, FillRule::NonZero, false);
    ctx.PopClipPath();

    VGBatch& batch = ctx.GetBatch();
    // clip write + apply, star write + cover, clip clear.
    REQUIRE(batch.commands.Size() == 5u);
    CHECK(batch.commands[2].fillPhase == foundation::vg::VGFillPhase::StencilWrite);
    CHECK(batch.commands[2].clipMode == foundation::vg::VGClipMode::Stencil);
    CHECK(batch.commands[3].fillPhase == foundation::vg::VGFillPhase::StencilCover);
    CHECK(batch.commands[3].clipMode == foundation::vg::VGClipMode::Stencil);
}

TEST_CASE("vg.context: PushClipPath without stencil support degrades to bounds scissor")
{
    VGContext ctx; // stencil fills OFF (default)
    ctx.Translate(10.0f, 0.0f);
    PathBuilder clip;
    clip.MoveTo(0, 0);
    clip.LineTo(20, 0);
    clip.LineTo(20, 20);
    clip.LineTo(0, 20);
    clip.Close();
    ctx.PushClipPath(clip.ToPath());

    PathBuilder pb;
    pb.MoveTo(5, 5);
    pb.LineTo(15, 5);
    pb.LineTo(15, 15);
    pb.LineTo(5, 15);
    pb.Close();
    ctx.FillPath(pb.ToPath(), Color::Red, FillRule::NonZero, false);
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(!batch.commands.IsEmpty());
    const VGCommand& cmd = batch.commands[batch.commands.Size() - 1];
    CHECK(cmd.clipMode == foundation::vg::VGClipMode::Scissor);
    CHECK(cmd.clipRect.x == doctest::Approx(10.0f)); // TRANSFORMED bounds
    CHECK(cmd.clipRect.width == doctest::Approx(20.0f));
}

namespace
{
    Path Square(f32 x0, f32 y0, f32 x1, f32 y1)
    {
        PathBuilder b;
        b.MoveTo(x0, y0);
        b.LineTo(x1, y0);
        b.LineTo(x1, y1);
        b.LineTo(x0, y1);
        b.Close();
        return b.ToPath();
    }

    // The device bounds of a command's vertices.
    Rectangle CommandBounds(const VGBatch& batch, const VGCommand& cmd)
    {
        Float2 mn{3.4e38f, 3.4e38f};
        Float2 mx{-3.4e38f, -3.4e38f};
        for (i32 i = 0; i < cmd.indexCount; ++i)
        {
            const Float2 p = batch.vertices[batch.indices[static_cast<usize>(cmd.startIndex + i)]].position;
            mn = Float2{Min(mn.x, p.x), Min(mn.y, p.y)};
            mx = Float2{Max(mx.x, p.x), Max(mx.y, p.y)};
        }
        return Rectangle{mn.x, mn.y, mx.x - mn.x, mx.y - mn.y};
    }
}

TEST_CASE("vg.context: a clip path inside another intersects it, and popping it restores the outer")
{
    using foundation::vg::VGClipMode;
    using foundation::vg::VGFillPhase;
    VGContext ctx;
    ctx.SetStencilFills(true);
    ctx.PushClipPath(Square(0, 0, 20, 20));
    ctx.PushClipPath(Square(10, 10, 30, 30));
    ctx.FillPath(Square(0, 0, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.PopClipPath();
    ctx.FillPath(Square(0, 0, 30, 30), Color::Blue, FillRule::NonZero, false);
    ctx.PopClipPath();
    ctx.FillPath(Square(0, 0, 30, 30), Color::Green, FillRule::NonZero, false);

    const VGBatch& batch = ctx.GetBatch();
    // outer write + apply; inner write (CLIPPED: only inside the outer mask) + apply over the
    // OUTER's bounds; the red fill; the pop: clear, outer write + apply again; blue; clear; green.
    REQUIRE(batch.commands.Size() == 11u);
    CHECK(batch.commands[0].fillPhase == VGFillPhase::StencilWrite);
    CHECK(batch.commands[0].clipMode == VGClipMode::None);
    CHECK(batch.commands[1].fillPhase == VGFillPhase::ClipApply);
    CHECK(batch.commands[2].fillPhase == VGFillPhase::StencilWrite);
    CHECK(batch.commands[2].clipMode == VGClipMode::Stencil);
    CHECK(batch.commands[3].fillPhase == VGFillPhase::ClipApply);
    const Rectangle apply = CommandBounds(batch, batch.commands[3]);
    CHECK(apply.x == doctest::Approx(0.0f)); // the outer's bounds: zeroes outer-only pixels
    CHECK(apply.width == doctest::Approx(20.0f));
    CHECK(batch.commands[4].clipMode == VGClipMode::Stencil); // red, inside both
    CHECK(batch.commands[5].fillPhase == VGFillPhase::ClipClear);
    CHECK(batch.commands[6].fillPhase == VGFillPhase::StencilWrite);
    CHECK(batch.commands[6].clipMode == VGClipMode::None); // the outer rebuilt
    CHECK(batch.commands[7].fillPhase == VGFillPhase::ClipApply);
    CHECK(batch.commands[8].clipMode == VGClipMode::Stencil); // blue, inside the outer
    CHECK(batch.commands[9].fillPhase == VGFillPhase::ClipClear);
    CHECK(batch.commands[10].clipMode == VGClipMode::None); // green, unclipped
}

TEST_CASE("vg.context: a clip rect inside a clip path applies both")
{
    using foundation::vg::VGClipMode;
    VGContext ctx;
    ctx.SetStencilFills(true);
    ctx.PushClipPath(Square(0, 0, 40, 40));
    ctx.PushClipRect(Rectangle{5, 5, 10, 10});
    ctx.FillPath(Square(0, 0, 40, 40), Color::Red, FillRule::NonZero, false);
    ctx.PopClip();
    ctx.FillPath(Square(0, 0, 40, 40), Color::Blue, FillRule::NonZero, false);
    ctx.PopClipPath();

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() == 5u);
    const VGCommand& red = batch.commands[2];
    CHECK(red.clipMode == VGClipMode::Stencil);
    CHECK(red.scissored);
    CHECK(red.clipRect.x == doctest::Approx(5.0f));
    CHECK(red.clipRect.width == doctest::Approx(10.0f));
    const VGCommand& blue = batch.commands[3];
    CHECK(blue.clipMode == VGClipMode::Stencil);
    CHECK_FALSE(blue.scissored);
}

TEST_CASE("vg.context: without stencil, a nested clip path's scissor pops back to the outer")
{
    using foundation::vg::VGClipMode;
    VGContext ctx; // stencil fills off
    ctx.PushClipPath(Square(0, 0, 20, 20));
    ctx.PushClipPath(Square(10, 10, 30, 30));
    ctx.FillPath(Square(0, 0, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.PopClipPath();
    ctx.FillPath(Square(0, 0, 30, 30), Color::Blue, FillRule::NonZero, false);
    ctx.PopClipPath();
    ctx.FillPath(Square(0, 0, 30, 30), Color::Green, FillRule::NonZero, false);

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() == 3u);
    CHECK(batch.commands[0].clipMode == VGClipMode::Scissor);
    CHECK(batch.commands[0].clipRect.x == doctest::Approx(10.0f)); // the intersection
    CHECK(batch.commands[0].clipRect.width == doctest::Approx(10.0f));
    CHECK(batch.commands[1].clipMode == VGClipMode::Scissor);
    CHECK(batch.commands[1].clipRect.x == doctest::Approx(0.0f)); // the outer again
    CHECK(batch.commands[1].clipRect.width == doctest::Approx(20.0f));
    CHECK(batch.commands[2].clipMode == VGClipMode::None);
}

TEST_CASE("vg.context: an analytic box is six box vertices in a Box command, batched with the next box")
{
    using foundation::vg::VGDrawMode;
    VGContext ctx;
    ctx.SetAnalyticBoxes(true);
    ctx.DrawBox(Rectangle{10, 20, 40, 30}, CornerRadii(6.0f), Color::Red, foundation::vg::BorderWidths(2.0f),
                Color::Blue);
    ctx.DrawBox(Rectangle{60, 20, 10, 10}, CornerRadii(), Color::Green);
    ctx.FillPath(Square(0, 0, 5, 5), Color::White, FillRule::NonZero, false); // other geometry
    ctx.DrawBox(Rectangle{80, 20, 10, 10}, CornerRadii(), Color::Green);

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.boxVertices.Size() == 18u);
    REQUIRE(batch.commands.Size() == 3u);
    CHECK(batch.commands[0].drawMode == VGDrawMode::Box); // the first two boxes, one command
    CHECK(batch.commands[0].startIndex == 0);
    CHECK(batch.commands[0].indexCount == 12);
    CHECK(batch.commands[1].drawMode != VGDrawMode::Box); // the path, in order
    CHECK(batch.commands[2].drawMode == VGDrawMode::Box);
    CHECK(batch.commands[2].startIndex == 12);
    CHECK(batch.commands[2].indexCount == 6);

    // The first box: its centre, size, radii and borders in every vertex, in device units (it
    // snapped: an identity transform keeps whole-pixel edges), and its quad a pixel larger.
    const foundation::vg::VGBoxVertex& v = batch.boxVertices[0];
    CHECK(v.halfSize.x == doctest::Approx(20.0f));
    CHECK(v.halfSize.y == doctest::Approx(15.0f));
    CHECK(v.radii.x == doctest::Approx(6.0f));
    CHECK(v.borders.w == doctest::Approx(2.0f));
    CHECK(v.fill.r == doctest::Approx(1.0f));
    CHECK(v.borderTop.b == doctest::Approx(1.0f)); // one colour: every side's
    CHECK(v.borderLeft.b == doctest::Approx(1.0f));
    Float2 mn{1e9f, 1e9f}, mx{-1e9f, -1e9f};
    for (usize i = 0; i < 6; ++i)
    {
        mn = Float2{Min(mn.x, batch.boxVertices[i].position.x), Min(mn.y, batch.boxVertices[i].position.y)};
        mx = Float2{Max(mx.x, batch.boxVertices[i].position.x), Max(mx.y, batch.boxVertices[i].position.y)};
    }
    CHECK(mn.x == doctest::Approx(9.0f));
    CHECK(mx.x == doctest::Approx(51.0f));
    CHECK(mn.y == doctest::Approx(19.0f));
    CHECK(mx.y == doctest::Approx(51.0f));
}

TEST_CASE("vg.context: a box's radii are clamped to it, and a scaled box snaps in device units")
{
    VGContext ctx;
    ctx.SetAnalyticBoxes(true);
    ctx.Scale(2.0f, 2.0f);
    ctx.DrawBox(Rectangle{1.2f, 1.2f, 10, 6}, CornerRadii(20.0f), Color::Red, foundation::vg::BorderWidths(1.0f),
                Color::Blue);
    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.boxVertices.Size() == 6u);
    const foundation::vg::VGBoxVertex& v = batch.boxVertices[0];
    CHECK(v.halfSize.x == doctest::Approx(10.0f)); // 2.4..22.4 device, snapped to 2..22
    CHECK(v.halfSize.y == doctest::Approx(6.0f));
    CHECK(v.radii.x == doctest::Approx(6.0f));     // clamped to half the short side (3), doubled
    CHECK(v.borders.x == doctest::Approx(2.0f));   // a unit border, two device pixels
}

TEST_CASE("vg.context: a box's borders on a scaled canvas are whole device pixels, at least one")
{
    VGContext ctx;
    ctx.SetAnalyticBoxes(true);
    ctx.Scale(0.48f, 0.48f);
    ctx.DrawBox(Rectangle{100, 100, 200, 100}, CornerRadii(), Color::Red, foundation::vg::BorderWidths(2.0f, 1.0f, 5.0f, 0.0f),
                Color::Blue);
    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.boxVertices.Size() == 6u);
    const foundation::vg::VGBoxVertex& v = batch.boxVertices[0];
    CHECK(v.borders.x == doctest::Approx(1.0f)); // 2 x 0.48 = 0.96: one pixel
    CHECK(v.borders.y == doctest::Approx(1.0f)); // 1 x 0.48: still one, never thinner
    CHECK(v.borders.z == doctest::Approx(2.0f)); // 5 x 0.48 = 2.4: two
    CHECK(v.borders.w == doctest::Approx(0.0f)); // none stays none
}

TEST_CASE("vg.context: without analytic boxes, DrawBox tessellates its fill and border")
{
    VGContext ctx; // analytic boxes off (the default)
    ctx.DrawBox(Rectangle{10, 10, 40, 30}, CornerRadii(6.0f), Color::Red, foundation::vg::BorderWidths(2.0f),
                Color::Blue);
    const VGBatch& batch = ctx.GetBatch();
    CHECK(batch.boxVertices.IsEmpty());
    REQUIRE_FALSE(batch.commands.IsEmpty());
    for (const VGCommand& cmd : batch.commands)
    {
        CHECK(cmd.drawMode != foundation::vg::VGDrawMode::Box);
    }
    CHECK(batch.vertices.Size() > 8u); // the rounded fill and the ring
}

TEST_CASE("vg.context: a layer's draws go into it, and its composite into what is under it")
{
    using foundation::vg::VGDrawMode;
    VGContext ctx;
    ctx.SetLayers(true);
    ctx.FillPath(Square(0, 0, 5, 5), Color::White, FillRule::NonZero, false); // before: the target
    ctx.PushLayer(0.5f);
    ctx.FillPath(Square(10, 10, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.FillPath(Square(20, 20, 40, 40), Color::Blue, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.FillPath(Square(0, 0, 5, 5), Color::White, FillRule::NonZero, false); // after: the target

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.layers.Size() == 1u);
    const foundation::vg::VGLayer& layer = batch.layers[0];
    CHECK(layer.parent == -1);
    CHECK(layer.opacity == doctest::Approx(0.5f));
    // The content's bounds, on whole pixels with a pixel of margin.
    CHECK(layer.bounds.x == doctest::Approx(9.0f));
    CHECK(layer.bounds.width == doctest::Approx(32.0f));
    REQUIRE(batch.commands.Size() == 4u);
    CHECK(batch.commands[0].layer == -1);
    CHECK(batch.commands[1].layer == 0); // red and blue, one command, into the layer
    CHECK(batch.commands[2].drawMode == VGDrawMode::Layer);
    CHECK(batch.commands[2].layer == -1);
    CHECK(batch.commands[2].compositeLayer == 0);
    CHECK(batch.vertices[batch.indices[static_cast<usize>(batch.commands[2].startIndex)]].color.a ==
          doctest::Approx(0.5f)); // the opacity, applied once, at the composite
    CHECK(batch.commands[3].layer == -1);
}

TEST_CASE("vg.context: inside a layer no clip or opacity applies; around it they apply to its composite")
{
    using foundation::vg::VGClipMode;
    VGContext ctx;
    ctx.SetLayers(true);
    ctx.SetStencilFills(true);
    ctx.PushClipPath(Square(0, 0, 50, 50));
    ctx.PushOpacity(0.5f);
    ctx.PushLayer(0.8f);
    ctx.FillPath(Square(10, 10, 20, 20), Color::Red, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.PopOpacity();
    ctx.PopClipPath();

    const VGBatch& batch = ctx.GetBatch();
    const VGCommand* inside = nullptr;
    const VGCommand* composite = nullptr;
    for (const VGCommand& cmd : batch.commands)
    {
        if (cmd.layer == 0)
            inside = &cmd;
        if (cmd.compositeLayer == 0)
            composite = &cmd;
    }
    REQUIRE(inside != nullptr);
    REQUIRE(composite != nullptr);
    CHECK(inside->clipMode == VGClipMode::None);
    CHECK(batch.vertices[batch.indices[static_cast<usize>(inside->startIndex)]].color.a == doctest::Approx(1.0f));
    CHECK(composite->clipMode == VGClipMode::Stencil);
    CHECK(batch.vertices[batch.indices[static_cast<usize>(composite->startIndex)]].color.a ==
          doctest::Approx(0.4f)); // 0.8 of the layer, times the 0.5 around it
}

TEST_CASE("vg.context: a layer inside a layer composites into it")
{
    VGContext ctx;
    ctx.SetLayers(true);
    ctx.PushLayer(0.5f);
    ctx.FillPath(Square(0, 0, 10, 10), Color::Red, FillRule::NonZero, false);
    ctx.PushLayer(0.5f);
    ctx.FillPath(Square(40, 40, 50, 50), Color::Blue, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.PopLayer();
    ctx.PushLayer(1.0f);
    ctx.PopLayer(); // empty: no bounds, no composite

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.layers.Size() == 3u);
    CHECK(batch.layers[1].parent == 0);
    CHECK(batch.layers[0].bounds.width == doctest::Approx(53.0f)); // its fill (0..10) and the inner composite (39..51), padded
    CHECK(batch.layers[2].bounds.width == doctest::Approx(0.0f));
    int composites = 0;
    for (const VGCommand& cmd : batch.commands)
    {
        if (cmd.compositeLayer == 1)
        {
            CHECK(cmd.layer == 0);
            ++composites;
        }
        if (cmd.compositeLayer == 0)
        {
            CHECK(cmd.layer == -1);
            ++composites;
        }
        CHECK(cmd.compositeLayer != 2);
    }
    CHECK(composites == 2);
}

TEST_CASE("vg.context: a blurred layer carries its sigma and three sigmas of margin")
{
    VGContext ctx;
    ctx.SetLayers(true);
    ctx.PushLayer(1.0f, VGBlendMode::Normal, 4.0f);
    ctx.FillPath(Square(10, 10, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.PushLayer(1.0f, VGBlendMode::Normal, 500.0f); // past the widest
    ctx.FillPath(Square(10, 10, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.PopLayer();

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.layers.Size() == 2u);
    CHECK(batch.layers[0].blur == doctest::Approx(4.0f));
    CHECK(batch.layers[0].bounds.x == doctest::Approx(9.0f - 12.0f)); // the pixel margin, then 3 sigma
    CHECK(batch.layers[0].bounds.width == doctest::Approx(22.0f + 24.0f));
    CHECK(batch.layers[1].blur == doctest::Approx(VGContext::MaxLayerBlur));
}

TEST_CASE("vg.context: a backdrop blur moves what was drawn into a layer and shows its blurred copy")
{
    using foundation::vg::VGDrawMode;
    VGContext ctx;
    ctx.SetLayers(true);
    ctx.FillPath(Square(0, 0, 40, 40), Color::Red, FillRule::NonZero, false);
    ctx.PushLayer(0.5f); // drawn before the backdrop: becomes a child of the moved layer
    ctx.FillPath(Square(50, 0, 60, 10), Color::Green, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.DrawBackdropBlur(Rectangle{10, 10, 20, 20}, CornerRadii(4.0f), 3.0f);
    ctx.FillPath(Square(0, 50, 10, 60), Color::Blue, FillRule::NonZero, false); // after: the target

    const VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.layers.Size() == 2u);
    const foundation::vg::VGLayer& behind = batch.layers[1];
    CHECK(behind.parent == -1);
    CHECK(behind.backdropBlur == doctest::Approx(3.0f));
    CHECK(behind.opacity == doctest::Approx(1.0f));
    CHECK(batch.layers[0].parent == 1); // the earlier layer now composites into the moved one
    CHECK(batch.commands[0].layer == 1); // the red fill moved
    int composites = 0;
    int backdrops = 0;
    for (const VGCommand& cmd : batch.commands)
    {
        if (cmd.compositeLayer == 1 && !cmd.backdrop)
        {
            CHECK(cmd.layer == -1);
            CHECK(cmd.clipMode == foundation::vg::VGClipMode::None);
            ++composites;
        }
        if (cmd.backdrop)
        {
            CHECK(cmd.drawMode == VGDrawMode::Layer);
            CHECK(cmd.compositeLayer == 1);
            CHECK(cmd.layer == -1);
            ++backdrops;
        }
    }
    CHECK(composites == 1);
    CHECK(backdrops >= 1);
    CHECK(batch.commands.Back().layer == -1); // the blue fill after it stays in the target
    CHECK(batch.commands.Back().drawMode != VGDrawMode::Layer);
}

TEST_CASE("vg.context: no backdrop blur without layers or inside a clip path")
{
    {
        VGContext ctx;
        ctx.FillPath(Square(0, 0, 40, 40), Color::Red, FillRule::NonZero, false);
        ctx.DrawBackdropBlur(Rectangle{10, 10, 20, 20}, CornerRadii(4.0f), 3.0f);
        CHECK(ctx.GetBatch().layers.IsEmpty());
        CHECK(ctx.GetBatch().commands.Size() == 1u);
    }
    {
        VGContext ctx;
        ctx.SetLayers(true);
        ctx.SetStencilFills(true);
        ctx.FillPath(Square(0, 0, 40, 40), Color::Red, FillRule::NonZero, false);
        ctx.PushClipPath(Square(0, 0, 30, 30));
        ctx.DrawBackdropBlur(Rectangle{10, 10, 20, 20}, CornerRadii(4.0f), 3.0f);
        ctx.PopClipPath();
        CHECK(ctx.GetBatch().layers.IsEmpty());
    }
}

TEST_CASE("vg.context: without layers, a layer is an opacity push")
{
    VGContext ctx; // the host never said its renderer draws layers
    ctx.PushOpacity(0.5f);
    ctx.PushLayer(0.5f);
    ctx.FillPath(Square(10, 10, 30, 30), Color::Red, FillRule::NonZero, false);
    ctx.PopLayer();
    ctx.FillPath(Square(40, 40, 50, 50), Color::Red, FillRule::NonZero, false);
    ctx.PopOpacity();

    const VGBatch& batch = ctx.GetBatch();
    CHECK(batch.layers.IsEmpty());
    REQUIRE(batch.commands.Size() == 1u); // no layer, no composite: one run of fills
    CHECK(batch.commands[0].layer == -1);
    CHECK(batch.vertices[batch.indices[0]].color.a == doctest::Approx(0.25f));
    CHECK(batch.vertices.Back().color.a == doctest::Approx(0.5f)); // the layer's opacity popped
}

TEST_CASE("vg.context: past MaxLayers in a batch, a layer is an opacity push")
{
    VGContext ctx;
    ctx.SetLayers(true);
    for (i32 i = 0; i < VGContext::MaxLayers + 2; ++i)
    {
        ctx.PushLayer(0.5f);
        ctx.FillPath(Square(0, 0, 10, 10), Color::Red, FillRule::NonZero, false);
        ctx.PopLayer();
    }
    const VGBatch& batch = ctx.GetBatch();
    CHECK(batch.layers.Size() == static_cast<usize>(VGContext::MaxLayers));
    CHECK(batch.commands.Back().layer == -1);
    CHECK(batch.vertices.Back().color.a == doctest::Approx(0.5f)); // faded on its own
}

TEST_CASE("vg.context: over-budget LUT cache eviction is announced through the batch")
{
    VGContext ctx;
    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    const Path path = pb.ToPath();

    // Exceed the cache budget with DISTINCT ramps (the animated-gradient shape).
    const usize distinct = VGContext::kMaxGradientLutCacheEntries + 1;
    for (usize i = 0; i < distinct; ++i)
    {
        VGLinearGradientFill grad(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
        const f32 r = static_cast<f32>(i % 256) / 255.0f;
        const f32 g = static_cast<f32>((i / 256) % 256) / 255.0f;
        grad.AddStop(0.0f, Color{r, g, 0.0f, 1.0f});
        grad.AddStop(1.0f, Color::Blue);
        ctx.FillPath(path, grad, FillRule::NonZero, false);
    }

    // The NEXT frame's batch carries the eviction notice for every dropped LUT, and the
    // cache restarts (a fresh gradient bakes again and the batch stays consistent).
    ctx.Clear();
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.evictedTextures.Size() == distinct);
    // The evicted ramps SURVIVE their announcing frame: the images are held one more frame so
    // the keys the renderer releases are not dangling while its frame is in flight.
    for (const image::ImageData* evicted : batch.evictedTextures)
    {
        REQUIRE(evicted != nullptr);
        CHECK(evicted->Width() == 256u);
        CHECK(evicted->Height() == 1u);
        CHECK(evicted->PixelData().Size() == 256u * 4u);
    }

    VGLinearGradientFill grad(Float2{0.0f, 0.0f}, Float2{10.0f, 0.0f});
    grad.AddStop(0.0f, Color::Red);
    grad.AddStop(1.0f, Color::Blue);
    ctx.FillPath(path, grad, FillRule::NonZero, false);
    CHECK(ctx.GetBatch().textures.Size() == 2u);

    // The frame after that: the eviction list is spent, the cache is small again.
    ctx.Clear();
    CHECK(ctx.GetBatch().evictedTextures.IsEmpty());
}

// --- stencil-then-cover emission (SetStencilFills) -------------------------------------

namespace
{
    // A donut: outer CCW square, inner CW square - the canonical hole case the direct
    // tessellator fills SOLID (contours triangulated independently, no subtraction).
    Path MakeDonut()
    {
        PathBuilder pb;
        pb.MoveTo(0, 0);
        pb.LineTo(100, 0);
        pb.LineTo(100, 100);
        pb.LineTo(0, 100);
        pb.Close();
        pb.MoveTo(30, 30);
        pb.LineTo(30, 70);
        pb.LineTo(70, 70);
        pb.LineTo(70, 30);
        pb.Close();
        return pb.ToPath();
    }
}

TEST_CASE("vg.context: stencil fills OFF leaves complex paths on the direct tessellator")
{
    VGContext ctx;
    ctx.FillPath(MakeDonut(), Color::Red, FillRule::NonZero, false);
    for (usize i = 0; i < ctx.GetBatch().commands.Size(); ++i)
    {
        CHECK(ctx.GetBatch().commands[i].fillPhase == VGFillPhase::Direct);
    }
}

TEST_CASE("vg.context: a hole emits stencil write + cover commands")
{
    VGContext ctx;
    ctx.SetStencilFills(true);
    ctx.FillPath(MakeDonut(), Color::Red, FillRule::NonZero, false);

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() == 2u);
    const VGCommand write = batch.commands[0];
    const VGCommand cover = batch.commands[1];

    CHECK(write.fillPhase == VGFillPhase::StencilWrite);
    CHECK(write.fillRule == FillRule::NonZero);
    // Two quads fan into 2 triangles each = 12 indices of winding geometry.
    CHECK(write.indexCount == 12);

    CHECK(cover.fillPhase == VGFillPhase::StencilCover);
    CHECK(cover.indexCount == 6); // the bounding quad
    CHECK(cover.startIndex == write.startIndex + write.indexCount);

    // The cover quad spans the path bounds and carries the fill color.
    const VGVertex& corner = batch.vertices[batch.vertices.Size() - 4];
    CHECK(corner.position.x == doctest::Approx(0.0f));
    CHECK(corner.position.y == doctest::Approx(0.0f));
    const VGVertex& opposite = batch.vertices[batch.vertices.Size() - 2];
    CHECK(opposite.position.x == doctest::Approx(100.0f));
    CHECK(opposite.position.y == doctest::Approx(100.0f));
    CHECK(corner.color.r == doctest::Approx(1.0f));
}

TEST_CASE("vg.context: convex single contours keep the direct fast path with stencil on")
{
    VGContext ctx;
    ctx.SetStencilFills(true);
    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.LineTo(0, 10);
    pb.Close();
    ctx.FillPath(pb.ToPath(), Color::Blue, FillRule::NonZero, false);
    for (usize i = 0; i < ctx.GetBatch().commands.Size(); ++i)
    {
        CHECK(ctx.GetBatch().commands[i].fillPhase == VGFillPhase::Direct);
    }
}

TEST_CASE("vg.context: a self-intersecting star goes through the stencil (both rules)")
{
    // Five-point star drawn edge-to-edge: self-intersecting, turns two revolutions -
    // NonZero fills the core, EvenOdd leaves it open; the direct tessellator gets
    // BOTH wrong, so each must route through the stencil.
    PathBuilder pb;
    pb.MoveTo(50, 0);
    pb.LineTo(79, 90);
    pb.LineTo(2, 35);
    pb.LineTo(98, 35);
    pb.LineTo(21, 90);
    pb.Close();
    const Path star = pb.ToPath();

    const FillRule rules[2] = {FillRule::NonZero, FillRule::EvenOdd};
    for (const FillRule rule : rules)
    {
        VGContext ctx;
        ctx.SetStencilFills(true);
        ctx.FillPath(star, Color::White, rule, false);
        VGBatch& batch = ctx.GetBatch();
        REQUIRE(batch.commands.Size() == 2u);
        CHECK(batch.commands[0].fillPhase == VGFillPhase::StencilWrite);
        CHECK(batch.commands[0].fillRule == rule);
        CHECK(batch.commands[1].fillPhase == VGFillPhase::StencilCover);
    }
}

TEST_CASE("vg.context: stencil fill respects the current transform and later draws recover")
{
    VGContext ctx;
    ctx.SetStencilFills(true);
    ctx.PushState();
    ctx.Translate(10.0f, 20.0f);
    ctx.FillPath(MakeDonut(), Color::Red, FillRule::EvenOdd, false);
    ctx.PopState();

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() == 2u);
    // First winding vertex carries the translation.
    const VGVertex& first = batch.vertices[0];
    CHECK(first.position.x == doctest::Approx(10.0f));
    CHECK(first.position.y == doctest::Approx(20.0f));

    // A plain rect after the stencil fill batches as an ordinary Direct command.
    ctx.FillRect(Rectangle{0, 0, 5, 5}, Color::Green);
    (void)ctx.GetBatch(); // flushes the pending command
    const VGCommand last = batch.commands[batch.commands.Size() - 1];
    CHECK(last.fillPhase == VGFillPhase::Direct);
}

TEST_CASE("vg.context: DrawImageSnapped lands on the device pixel grid")
{
    VGContext ctx;
    image::ImageDataRef tex(16, 16);

    // Fractional translation (the tab-strip case): the emitted quad must sit on
    // INTEGER device coordinates, not at the fractional offset.
    ctx.PushState();
    ctx.Translate(10.4f, 20.6f);
    ctx.DrawImageSnapped(&tex, Rectangle{0, 0, 16, 16}, Rectangle{0, 0, 16, 16});
    ctx.PopState();

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() >= 4u);
    const VGVertex& v0 = batch.vertices[0];
    CHECK(v0.position.x == doctest::Approx(10.0f)); // Round(10.4)
    CHECK(v0.position.y == doctest::Approx(21.0f)); // Round(20.6)
    // Size preserved exactly (16px source at 16px dest = 1:1 texels).
    const VGVertex& v2 = batch.vertices[2];
    CHECK(v2.position.x - v0.position.x == doctest::Approx(16.0f));

    // Rotated transforms fall through to the unsnapped path (no crash, still draws).
    VGContext rotated;
    rotated.PushState();
    rotated.Rotate(0.3f);
    rotated.DrawImageSnapped(&tex, Rectangle{0, 0, 16, 16}, Rectangle{0, 0, 16, 16});
    rotated.PopState();
    CHECK(rotated.GetBatch().VertexCount() >= 4u);
}

TEST_CASE("vg.context: a plain draw after a gradient is back on the white passthrough + default mode")
{
    // A gradient binds its ramp LUT and (per-pixel) a gradient draw mode; the NEXT plain draw
    // must not stay on either - it is back on texture 0 (the white passthrough) and Default.
    VGContext ctx;
    ctx.SetPerPixelGradients(true);
    PathBuilder pb;
    pb.MoveTo(0, 0);
    pb.LineTo(10, 0);
    pb.LineTo(10, 10);
    pb.Close();
    const Path path = pb.ToPath();

    VGRadialGradientFill grad(Float2{5.0f, 5.0f}, 5.0f);
    grad.AddStop(0.0f, Color::Red);
    grad.AddStop(1.0f, Color::Blue);
    ctx.FillPath(path, grad, FillRule::NonZero, false);
    // Commands close lazily on a state change or on GetBatch(): read through GetBatch each time.
    const usize gradientCommand = ctx.GetBatch().CommandCount() - 1;
    CHECK(ctx.GetBatch().GetCommand(gradientCommand).textureIndex >= 1);
    CHECK(ctx.GetBatch().GetCommand(gradientCommand).drawMode != VGDrawMode::Default);

    ctx.FillPath(path, VGSolidFill(Color::Green), FillRule::NonZero, false);
    REQUIRE(ctx.GetBatch().CommandCount() > gradientCommand + 1);
    const VGCommand plain = ctx.GetBatch().GetCommand(ctx.GetBatch().CommandCount() - 1);
    CHECK(plain.textureIndex == 0);
    CHECK(plain.drawMode == VGDrawMode::Default);
}


// === Box shadow (the UI box-shadow primitive: four quadrant quads in the BoxShadow mode) ===

namespace
{
    [[nodiscard]] bool FindCommandWithMode(const VGBatch& batch, VGDrawMode mode, VGCommand& out)
    {
        for (usize i = 0; i < batch.CommandCount(); ++i)
        {
            const VGCommand cmd = batch.GetCommand(i);
            if (cmd.drawMode == mode && cmd.indexCount > 0)
            {
                out = cmd;
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("vg.context: FillBoxShadow emits four quadrant quads over the blurred extent in sigma units")
{
    VGContext ctx;
    // blur 12 -> sigma 6 -> the shadow reaches 18 past the box on every side.
    ctx.FillBoxShadow(Rectangle{10, 20, 100, 50}, CornerRadii(8.0f), 12.0f, Color{0, 0, 0, 0.5f});
    const VGBatch& batch = ctx.GetBatch();

    VGCommand cmd;
    REQUIRE(FindCommandWithMode(batch, VGDrawMode::BoxShadow, cmd));
    CHECK(cmd.indexCount == 24); // 4 quads
    CHECK(cmd.textureIndex == 0); // no texture: the solid slot

    // The 16 vertices span the box expanded by three sigma.
    f32 minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    usize shadowVertices = 0;
    for (usize i = 0; i < batch.vertices.Size(); ++i)
    {
        const VGVertex& v = batch.vertices[i];
        if (v.color.a > 0.49f && v.color.a < 0.51f)
        {
            ++shadowVertices;
            minX = Min(minX, v.position.x);
            maxX = Max(maxX, v.position.x);
            minY = Min(minY, v.position.y);
            maxY = Max(maxY, v.position.y);
            // Every vertex carries the corner radius in sigma units (outset: positive).
            CHECK(v.coverage == doctest::Approx(8.0f / 6.0f));
        }
    }
    CHECK(shadowVertices == 16);
    CHECK(minX == doctest::Approx(10.0f - 18.0f));
    CHECK(maxX == doctest::Approx(110.0f + 18.0f));
    CHECK(minY == doctest::Approx(20.0f - 18.0f));
    CHECK(maxY == doctest::Approx(70.0f + 18.0f));

    // The centre vertex of each quadrant sits deep inside: q = -(half - r) / sigma.
    usize centres = 0;
    for (usize i = 0; i < batch.vertices.Size(); ++i)
    {
        const VGVertex& v = batch.vertices[i];
        if (v.position.x == doctest::Approx(60.0f) && v.position.y == doctest::Approx(45.0f))
        {
            ++centres;
            CHECK(v.texCoord.x == doctest::Approx(-(50.0f - 8.0f) / 6.0f));
            CHECK(v.texCoord.y == doctest::Approx(-(25.0f - 8.0f) / 6.0f));
        }
    }
    CHECK(centres == 4);

    // The mode is restored: a following plain fill is a Default command.
    ctx.FillRect(Rectangle{0, 0, 5, 5}, Color::Red);
    VGCommand plain;
    CHECK(FindCommandWithMode(ctx.GetBatch(), VGDrawMode::Default, plain));
}

TEST_CASE("vg.context: a zero-blur shadow is a plain rounded rect; an inset shadow stays inside the box")
{
    {
        VGContext ctx;
        ctx.FillBoxShadow(Rectangle{10, 10, 40, 40}, CornerRadii(4.0f), 0.0f, Color::Black);
        VGCommand cmd;
        CHECK_FALSE(FindCommandWithMode(ctx.GetBatch(), VGDrawMode::BoxShadow, cmd));
        CHECK(FindCommandWithMode(ctx.GetBatch(), VGDrawMode::Default, cmd)); // the rounded rect
    }
    {
        VGContext ctx;
        ctx.FillBoxShadow(Rectangle{10, 10, 40, 40}, CornerRadii(4.0f), 8.0f, Color::Black, true);
        const VGBatch& batch = ctx.GetBatch();
        VGCommand cmd;
        REQUIRE(FindCommandWithMode(batch, VGDrawMode::BoxShadow, cmd));
        for (usize i = 0; i < batch.vertices.Size(); ++i)
        {
            const VGVertex& v = batch.vertices[i];
            CHECK(v.position.x >= 10.0f - 0.001f);
            CHECK(v.position.x <= 50.0f + 0.001f);
            CHECK(v.coverage < 0.0f); // inset flag
        }
    }
    {
        VGContext ctx;
        ctx.FillBoxShadow(Rectangle{10, 10, 40, 40}, CornerRadii(4.0f), 0.0f, Color::Black, true);
        CHECK(ctx.GetBatch().vertices.IsEmpty()); // a hard inset shadow is nothing
    }
}

// A card's thumbnail with rounded corners: the picture is cut to the rounded rect (no opaque
// vertex in a corner's cut-away), each vertex samples the texel of its place in the dest rect
// (worked out before the transform), the fringe clamps to the source, and no radius is the
// plain image quad.
TEST_CASE("vg.context: DrawImageRounded maps the texture onto a rounded rect, cut at its corners")
{
    VGContext ctx;
    image::ImageDataRef tex(200, 100);
    const Rectangle dest{10.0f, 20.0f, 100.0f, 50.0f};
    const Rectangle src{100.0f, 0.0f, 100.0f, 100.0f}; // the right half of the texture
    ctx.PushState();
    ctx.Translate(5.0f, 0.0f);
    ctx.DrawImageRounded(&tex, dest, src, CornerRadii(10.0f));
    ctx.PopState();

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.textures.Size() == 2u);
    CHECK(batch.textures[1] == &tex);
    REQUIRE(batch.CommandCount() == 1u);
    CHECK(batch.GetCommand(0).textureIndex == 1);
    REQUIRE(batch.VertexCount() > 4u);

    bool sawLeftEdge = false;
    for (const VGVertex& v : batch.vertices)
    {
        const f32 x = v.position.x - 5.0f; // back to the dest rect's space
        const f32 y = v.position.y;
        CHECK(v.texCoord.x >= 0.5f - 1e-4f); // inside the source half, the fringe clamped
        CHECK(v.texCoord.x <= 1.0f + 1e-4f);
        CHECK(v.texCoord.y >= -1e-4f);
        CHECK(v.texCoord.y <= 1.0f + 1e-4f);
        if (v.coverage >= 1.0f)
        {
            // An opaque vertex lies inside the rounded rect: in the top-left corner, within the
            // radius of the corner's centre.
            if (x < 20.0f && y < 30.0f)
            {
                const f32 dx = x - 20.0f, dy = y - 30.0f;
                CHECK(dx * dx + dy * dy <= 10.0f * 10.0f + 0.5f);
            }
            if (x < 11.0f && y >= 29.0f && y <= 61.0f) // the left edge (inset half a fringe)
            {
                sawLeftEdge = true;
                CHECK(v.texCoord.x == doctest::Approx(0.5f + (x - 10.0f) / 200.0f).epsilon(0.001));
                CHECK(v.texCoord.y == doctest::Approx((y - 20.0f) / 50.0f).epsilon(0.001));
            }
        }
    }
    CHECK(sawLeftEdge);

    VGContext square;
    square.DrawImageRounded(&tex, dest, src, CornerRadii(0.0f));
    CHECK(square.GetBatch().VertexCount() == 4u);
}

namespace
{
    // An atlas whose glyphs are not their codepoints: 'A' is glyph 7 and 'B' has none, as a font's
    // own glyph ids are.
    class GlyphIdAtlas final : public foundation::fonts::IFontAtlas
    {
    public:
        // A distance-field atlas when asked (effects read its range): 4 texels, one texel a pixel.
        foundation::fonts::AtlasMode mode = foundation::fonts::AtlasMode::Coverage;
        [[nodiscard]] foundation::fonts::AtlasMode Mode() const override { return mode; }
        [[nodiscard]] f32 DistanceFieldRange() const override
        {
            return mode == foundation::fonts::AtlasMode::DistanceField ? 4.0f : 0.0f;
        }
        [[nodiscard]] u32 Width() const override { return 64; }
        [[nodiscard]] u32 Height() const override { return 64; }
        [[nodiscard]] Span<const u8> PixelData() const override { return {}; }
        [[nodiscard]] Float2 WhitePixelUV() const override { return Float2(0.5f, 0.5f); }
        [[nodiscard]] i32 GlyphFor(i32 codepoint) const override { return codepoint == 'A' ? 7 : -1; }
        [[nodiscard]] bool Contains(i32 glyph) const override { return glyph == 7; }
        [[nodiscard]] bool TryGetRegion(i32 glyph, foundation::fonts::AtlasRegion& region) const override
        {
            if (glyph != 7)
                return false;
            region = foundation::fonts::AtlasRegion(0, 0, 8, 8, 0.0f, -8.0f, 10.0f);
            return true;
        }
        [[nodiscard]] bool GetGlyphQuad(i32 glyph, f32& cursorX, f32 cursorY,
                                        foundation::fonts::GlyphQuad& quad) const override
        {
            if (glyph != 7)
                return false;
            quad = foundation::fonts::GlyphQuad(cursorX, cursorY - 8.0f, cursorX + 8.0f, cursorY, 0, 0, 0.125f, 0.125f);
            cursorX += 10.0f;
            return true;
        }
        [[nodiscard]] bool GetGlyphQuadAt(i32 glyph, f32 x, f32 y, foundation::fonts::GlyphQuad& quad) const override
        {
            f32 cursor = x;
            return GetGlyphQuad(glyph, cursor, y, quad);
        }
    };
}

TEST_CASE("vg.context: unshaped text draws each codepoint's glyph by the atlas's glyph id")
{
    GlyphIdAtlas atlas;
    image::ImageDataRef texture(64, 64);
    VGContext ctx;
    ctx.DrawText(u8"AAB", &atlas, &texture, Float2{0.0f, 20.0f}, Color::White);
    const VGBatch& batch = ctx.GetBatch();
    // Two quads (glyph 7 twice); 'B' has no glyph and draws nothing.
    usize quads = 0;
    for (const VGCommand& cmd : batch.commands)
        quads += static_cast<usize>(cmd.indexCount) / 6u;
    CHECK(quads == 2u);
}

namespace
{
    // The quads of a batch's text in draw order: each one's first vertex.
    Array<VGVertex> QuadCorners(const VGBatch& batch)
    {
        Array<VGVertex> corners;
        for (usize i = 0; i + 3 < batch.vertices.Size(); i += 4)
            corners.PushBack(batch.vertices[i]);
        return corners;
    }
}

TEST_CASE("vg.context: text effects draw a shadow, a glow and an outline under the glyphs")
{
    GlyphIdAtlas atlas;
    atlas.mode = foundation::fonts::AtlasMode::DistanceField;
    image::ImageDataRef texture(64, 64);
    VGContext ctx;
    TextEffects effects;
    effects.shadowOffset = Float2{3.0f, 2.0f};
    effects.shadowBlur = 1.0f;
    effects.shadowColor = Color{0.0f, 0.0f, 1.0f, 1.0f};
    effects.glowRadius = 1.0f;
    effects.glowColor = Color{0.0f, 1.0f, 0.0f, 1.0f};
    effects.outlineWidth = 1.0f;
    effects.outlineColor = Color{1.0f, 0.0f, 0.0f, 1.0f};
    ctx.SetTextEffects(effects);
    ctx.DrawText(u8"A", &atlas, &texture, Float2{10.0f, 20.0f}, Color::White);

    const Array<VGVertex> quads = QuadCorners(ctx.GetBatch());
    REQUIRE(quads.Size() == 4u);
    // One texel a pixel over a 4-texel range: a pixel is a quarter of the field's unit.
    // The shadow: offset, blurred over a pixel, its edge where the glyph's is.
    CHECK(quads[0].position.x == doctest::Approx(13.0f));
    CHECK(quads[0].position.y == doctest::Approx(14.0f));
    CHECK(quads[0].color.b == doctest::Approx(1.0f));
    CHECK(quads[0].effect.x == doctest::Approx(0.0f));
    CHECK(quads[0].effect.y == doctest::Approx(0.25f));
    // The glow: full at the edge, gone a pixel past it (centred half a pixel out).
    CHECK(quads[1].position.x == doctest::Approx(10.0f));
    CHECK(quads[1].color.g == doctest::Approx(1.0f));
    CHECK(quads[1].effect.x == doctest::Approx(0.125f));
    CHECK(quads[1].effect.y == doctest::Approx(0.25f));
    // The outline: the edge moved a pixel out, sharp.
    CHECK(quads[2].color.r == doctest::Approx(1.0f));
    CHECK(quads[2].effect.x == doctest::Approx(0.25f));
    CHECK(quads[2].effect.y == doctest::Approx(0.0f));
    // The glyphs last, as without effects.
    CHECK(quads[3].position.x == doctest::Approx(10.0f));
    CHECK(quads[3].effect.x == doctest::Approx(0.0f));
    CHECK(quads[3].effect.y == doctest::Approx(0.0f));
}

TEST_CASE("vg.context: a text effect wider than the field holds is scaled down to fit")
{
    GlyphIdAtlas atlas;
    atlas.mode = foundation::fonts::AtlasMode::DistanceField;
    image::ImageDataRef texture(64, 64);
    VGContext ctx;
    TextEffects effects;
    effects.outlineWidth = 6.0f; // 1.5 field units; the field ends half a unit past the edge
    ctx.SetTextEffects(effects);
    ctx.DrawText(u8"A", &atlas, &texture, Float2{0.0f, 20.0f}, Color::White);
    const Array<VGVertex> quads = QuadCorners(ctx.GetBatch());
    REQUIRE(quads.Size() == 2u);
    CHECK(quads[0].effect.x == doctest::Approx(0.49f));
}

TEST_CASE("vg.context: a coverage font draws only a shadow's offset copy, and effects are state")
{
    GlyphIdAtlas atlas; // coverage: no field to grow or soften
    image::ImageDataRef texture(64, 64);
    VGContext ctx;
    TextEffects effects;
    effects.shadowOffset = Float2{2.0f, 2.0f};
    effects.shadowBlur = 3.0f;
    effects.shadowColor = Color{0.0f, 0.0f, 0.0f, 1.0f};
    effects.outlineWidth = 1.0f;
    effects.glowRadius = 2.0f;
    ctx.PushState();
    ctx.SetTextEffects(effects);
    ctx.DrawText(u8"A", &atlas, &texture, Float2{0.0f, 20.0f}, Color::White);
    {
        const Array<VGVertex> quads = QuadCorners(ctx.GetBatch());
        REQUIRE(quads.Size() == 2u); // the shadow, then the glyph
        CHECK(quads[0].position.x == doctest::Approx(2.0f));
        CHECK(quads[0].effect.y == doctest::Approx(0.0f));
    }
    ctx.PopState();
    CHECK_FALSE(ctx.GetTextEffects().Any());
    ctx.DrawText(u8"A", &atlas, &texture, Float2{0.0f, 40.0f}, Color::White);
    CHECK(QuadCorners(ctx.GetBatch()).Size() == 3u); // one more: the glyph alone
}

namespace
{
    // A font with a shaper that turns "fi" into one glyph (77), as a real shaper's ligature does,
    // and an atlas that holds glyphs 70 ('f' alone), 77 and 73 ('i' alone).
    class LigatureFont final : public foundation::fonts::IFont
    {
    public:
        [[nodiscard]] StringView FamilyName() const override { return u8"Lig"; }
        [[nodiscard]] Span<const u8> SourceData() const override { return {}; }
        [[nodiscard]] foundation::fonts::FontMetrics Metrics() const override
        {
            return foundation::fonts::FontMetrics(10.0f, -3.0f, 0.0f, 13.0f, 1.0f);
        }
        [[nodiscard]] f32 PixelHeight() const override { return 13.0f; }
        [[nodiscard]] foundation::fonts::GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            foundation::fonts::GlyphInfo info;
            info.codepoint = codepoint;
            info.glyphIndex = codepoint == 'f' ? 70 : 73;
            info.advanceWidth = 8.0f;
            return info;
        }
        [[nodiscard]] f32 GetKerning(i32, i32) const override { return 0.0f; }
        [[nodiscard]] bool HasGlyph(i32) const override { return true; }
        [[nodiscard]] f32 MeasureString(StringView text) const override { return 8.0f * static_cast<f32>(text.Size()); }
        [[nodiscard]] f32 MeasureString(StringView text, Array<foundation::fonts::GlyphPosition>& out) const override
        {
            out.Clear();
            return MeasureString(text);
        }
    };

    class LigatureShaper final : public foundation::fonts::ClusterTextShaper
    {
    public:
        i32* calls = nullptr;
        [[nodiscard]] Result<f32> ShapeText(foundation::fonts::IFont& font, StringView text,
                                            Array<foundation::fonts::GlyphPosition>& out) override
        {
            return ShapeText(font, text, 0.0f, 0.0f, out);
        }
        [[nodiscard]] Result<f32> ShapeText(foundation::fonts::IFont&, StringView text, f32 x, f32 y,
                                            Array<foundation::fonts::GlyphPosition>& out) override
        {
            ++*calls;
            out.Clear();
            foundation::fonts::GlyphPosition g;
            g.x = x;
            g.y = y;
            g.advance = 12.0f;
            g.glyphId = text == StringView(u8"fi") ? 77 : 70;
            g.clusterLength = static_cast<i32>(text.Size());
            out.PushBack(g);
            return 12.0f;
        }
        [[nodiscard]] Status ShapeTextWrapped(foundation::fonts::IFont& font, StringView text, f32,
                                              Array<foundation::fonts::GlyphPosition>& out, f32& height) override
        {
            height = 13.0f;
            return ShapeText(font, text, out).HasValue() ? Status() : Status(ErrorCode::Unknown);
        }
        void CollectReachableGlyphs(const foundation::fonts::IFont&, Span<const i32>, Array<i32>& out) override
        {
            out.Clear();
        }

    protected:
        Result<f32> ShapeRun(const foundation::fonts::ShapeRequest& request, const Array<u32>&, const Array<usize>&, usize,
                             usize, RunDirection, f32 x, f32 y, Array<foundation::fonts::GlyphPosition>& out) override
        {
            return ShapeText(*request.fonts[0], request.text, x, y, out);
        }
    };

    class LigatureAtlas final : public foundation::fonts::IFontAtlas
    {
    public:
        [[nodiscard]] u32 Width() const override { return 64; }
        [[nodiscard]] u32 Height() const override { return 64; }
        [[nodiscard]] Span<const u8> PixelData() const override { return {}; }
        [[nodiscard]] Float2 WhitePixelUV() const override { return Float2(0.5f, 0.5f); }
        [[nodiscard]] i32 GlyphFor(i32 codepoint) const override { return codepoint == 'f' ? 70 : 73; }
        [[nodiscard]] bool Contains(i32 glyph) const override { return glyph == 70 || glyph == 73 || glyph == 77; }
        [[nodiscard]] bool TryGetRegion(i32 glyph, foundation::fonts::AtlasRegion& region) const override
        {
            if (!Contains(glyph))
                return false;
            region = foundation::fonts::AtlasRegion(0, 0, 8, 8, 0.0f, -8.0f, 8.0f);
            return true;
        }
        [[nodiscard]] bool GetGlyphQuad(i32 glyph, f32& cursorX, f32 cursorY,
                                        foundation::fonts::GlyphQuad& quad) const override
        {
            if (!Contains(glyph))
                return false;
            quad = foundation::fonts::GlyphQuad(cursorX, cursorY - 8.0f, cursorX + 8.0f, cursorY, 0, 0, 0.125f, 0.125f);
            cursorX += 8.0f;
            return true;
        }
        [[nodiscard]] bool GetGlyphQuadAt(i32 glyph, f32 x, f32 y, foundation::fonts::GlyphQuad& quad) const override
        {
            f32 cursor = x;
            return GetGlyphQuad(glyph, cursor, y, quad);
        }
    };

    class OneFontService final : public foundation::fonts::IFontService
    {
    public:
        foundation::fonts::CachedFont* font = nullptr;
        image::ImageDataRef texture{64, 64};
        [[nodiscard]] foundation::fonts::CachedFont* GetFont(f32) override { return font; }
        [[nodiscard]] foundation::fonts::CachedFont* GetFont(StringView, f32) override { return font; }
        [[nodiscard]] image::ImageData* GetAtlasTexture(foundation::fonts::CachedFont*) override { return &texture; }
        [[nodiscard]] image::ImageData* GetAtlasTexture(StringView, f32) override { return &texture; }
        void ReleaseFont(foundation::fonts::CachedFont*) override {}
        [[nodiscard]] StringView DefaultFontFamily() const override { return u8"Lig"; }
        [[nodiscard]] bool HasFamily(StringView) const override { return true; }
    };
}

TEST_CASE("vg.context: text drawn in a font with a shaper is shaped (once), so ligatures draw")
{
    i32 calls = 0;
    auto* shaper = DefaultAllocator().New<LigatureShaper>();
    shaper->calls = &calls;
    foundation::fonts::CachedFont font(DefaultAllocator(), DefaultAllocator().New<LigatureFont>(),
                                       DefaultAllocator().New<LigatureAtlas>(), shaper);
    OneFontService service;
    service.font = &font;
    VGContext ctx;
    ctx.SetFontService(&service);

    const auto quads = [&]()
    {
        usize n = 0;
        for (const VGCommand& cmd : ctx.GetBatch().commands)
            n += static_cast<usize>(cmd.indexCount) / 6u;
        return n;
    };
    ctx.DrawText(u8"fi", &font, Float2{0.0f, 20.0f}, Color::White);
    CHECK(quads() == 1u); // one ligature glyph, not f and i
    ctx.DrawText(u8"fi", &font, Rectangle{0, 0, 100, 20}, foundation::fonts::TextAlignment::Center,
                 foundation::fonts::VerticalAlignment::Middle, Color::White);
    CHECK(quads() == 2u);
    CHECK(calls == 1); // shaped once, then drawn from the cache
    CHECK(ctx.ShapedTextCount() == 1u);
    ctx.Clear();
    ctx.DrawText(u8"fi", &font, Float2{0.0f, 20.0f}, Color::White);
    CHECK(calls == 1); // across frames too
}

TEST_CASE("vg.context: a box takes a border colour per side, analytic or tessellated")
{
    const Color top{1.0f, 0.0f, 0.0f, 1.0f};
    const Color right{0.0f, 1.0f, 0.0f, 1.0f};
    const Color bottom{0.0f, 0.0f, 1.0f, 1.0f};
    const Color left{1.0f, 1.0f, 0.0f, 1.0f};
    {
        VGContext ctx;
        ctx.SetAnalyticBoxes(true);
        ctx.DrawBox(Rectangle{0, 0, 40, 40}, CornerRadii(), Color{0, 0, 0, 0}, BorderWidths(4.0f),
                    BorderColors(top, right, bottom, left));
        REQUIRE(ctx.GetBatch().boxVertices.Size() == 6u);
        const VGBoxVertex& v = ctx.GetBatch().boxVertices[0];
        CHECK(v.borderTop.r == doctest::Approx(1.0f));
        CHECK(v.borderRight.g == doctest::Approx(1.0f));
        CHECK(v.borderBottom.b == doctest::Approx(1.0f));
        CHECK(v.borderLeft.g == doctest::Approx(1.0f));
    }
    {
        // Tessellated: the ring is filled once per side, clipped to that side's wedge.
        VGContext ctx;
        ctx.SetStencilFills(true);
        ctx.DrawBox(Rectangle{0, 0, 40, 40}, CornerRadii(), Color{0, 0, 0, 0}, BorderWidths(4.0f),
                    BorderColors(top, right, bottom, left));
        bool seen[4] = {};
        for (const VGVertex& vertex : ctx.GetBatch().vertices)
        {
            seen[0] = seen[0] || vertex.color == top;
            seen[1] = seen[1] || vertex.color == right;
            seen[2] = seen[2] || vertex.color == bottom;
            seen[3] = seen[3] || vertex.color == left;
        }
        CHECK(seen[0]);
        CHECK(seen[1]);
        CHECK(seen[2]);
        CHECK(seen[3]);
    }
}

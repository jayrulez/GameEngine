// VGContext: batch production, transform/opacity, immediate-mode, images, commands.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.image;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;
namespace image = draconic::image;

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
    const draconic::image::ImageData* firstFrameLut = batch.textures[1];

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
    using draconic::vg::ApplyGradientSpread;
    using draconic::vg::VGGradientSpread;
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
    repeat.spread = draconic::vg::VGGradientSpread::Repeat;

    ctx.FillPath(path, pad, FillRule::NonZero, false);
    ctx.FillPath(path, repeat, FillRule::NonZero, false);
    VGBatch& batch = ctx.GetBatch(); // flushes the open command
    CHECK(batch.textures.Size() == 2u); // white + ONE shared LUT
    REQUIRE(batch.commands.Size() >= 2u);
    const VGCommand& first = batch.commands[batch.commands.Size() - 2];
    const VGCommand& second = batch.commands[batch.commands.Size() - 1];
    CHECK(first.gradientSpread == draconic::vg::VGGradientSpread::Pad);
    CHECK(second.gradientSpread == draconic::vg::VGGradientSpread::Repeat);
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
    repeat.spread = draconic::vg::VGGradientSpread::Repeat;
    const Rectangle bounds{0.0f, 0.0f, 10.0f, 10.0f};
    const Float2 rawFar = FillTessellator::GradientTexCoord(
        draconic::vg::VGGradientTess::LinearLut, repeat, Float2{10.0f, 0.0f}, bounds);
    CHECK(rawFar.x == doctest::Approx(2.0f)); // raw, NOT clamped/compressed

    VGLinearGradientFill pad = repeat;
    pad.spread = draconic::vg::VGGradientSpread::Pad;
    const Float2 padFar = FillTessellator::GradientTexCoord(
        draconic::vg::VGGradientTess::LinearLut, pad, Float2{10.0f, 0.0f}, bounds);
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
    ctx.SetBlendMode(draconic::vg::VGBlendMode::Additive);
    ctx.FillPath(path, Color::Blue, FillRule::NonZero, false);
    ctx.SetBlendMode(draconic::vg::VGBlendMode::Normal);
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.commands.Size() >= 2u);
    const VGCommand& first = batch.commands[batch.commands.Size() - 2];
    const VGCommand& second = batch.commands[batch.commands.Size() - 1];
    CHECK(first.blendMode == draconic::vg::VGBlendMode::Normal);
    CHECK(second.blendMode == draconic::vg::VGBlendMode::Additive);
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
    CHECK(batch.commands[0].fillPhase == draconic::vg::VGFillPhase::StencilWrite);
    CHECK(batch.commands[0].clipMode == draconic::vg::VGClipMode::None); // mask writing
    CHECK(batch.commands[1].fillPhase == draconic::vg::VGFillPhase::ClipApply);
    CHECK(batch.commands[2].fillPhase == draconic::vg::VGFillPhase::Direct);
    CHECK(batch.commands[2].clipMode == draconic::vg::VGClipMode::Stencil);
    CHECK(batch.commands[3].fillPhase == draconic::vg::VGFillPhase::ClipClear);
    CHECK(batch.commands[4].clipMode == draconic::vg::VGClipMode::None);
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
    CHECK(batch.commands[2].fillPhase == draconic::vg::VGFillPhase::StencilWrite);
    CHECK(batch.commands[2].clipMode == draconic::vg::VGClipMode::Stencil);
    CHECK(batch.commands[3].fillPhase == draconic::vg::VGFillPhase::StencilCover);
    CHECK(batch.commands[3].clipMode == draconic::vg::VGClipMode::Stencil);
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
    CHECK(cmd.clipMode == draconic::vg::VGClipMode::Scissor);
    CHECK(cmd.clipRect.x == doctest::Approx(10.0f)); // TRANSFORMED bounds
    CHECK(cmd.clipRect.width == doctest::Approx(20.0f));
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

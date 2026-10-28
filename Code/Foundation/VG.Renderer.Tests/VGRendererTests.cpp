// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless VGRenderer test via the Null RHI backend: initialize the pipeline,
// build a VGBatch with VGContext, Prepare it into the frame buffers, and check
// the returned slice. (Render needs a live RenderPassEncoder, exercised by the
// real backends; this covers init + the upload/slice path device-agnostically.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <new>
import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.image;
import foundation.vg;
import foundation.vg.renderer;

using namespace foundation::core;
using namespace foundation::vg;
using namespace foundation::vg::renderer;
namespace rhi = foundation::rhi;
namespace image = foundation::image;

namespace
{
    rhi::ShaderModule* MakeModule(rhi::Device& d)
    {
        const u8 dummy[4] = {0, 0, 0, 0};
        rhi::ShaderModuleDesc desc{};
        desc.code = Span<const u8>(dummy, 4);
        desc.entryPoint = u8"main"; // what the shader system records on a module it compiles
        rhi::ShaderModule* m = nullptr;
        (void)d.CreateShaderModule(desc, m);
        return m;
    }
}

TEST_CASE("vg.renderer: render-vertex passes color through RAW (the shader owns the decode)")
{
    // White stays white (1,1,1); opaque alpha passes through.
    const VGRenderVertex v(VGVertex::Solid(Float2{2.0f, 3.0f}, Color::White));
    CHECK(v.position[0] == doctest::Approx(2.0f));
    CHECK(v.position[1] == doctest::Approx(3.0f));
    CHECK(v.color[0] == doctest::Approx(1.0f));
    CHECK(v.color[3] == doctest::Approx(1.0f));
    CHECK(v.coverage == doctest::Approx(1.0f));

    // A mid-grey sRGB byte (188) stays 188/255 - the SINGLE sRGB->linear decode
    // happens in vg.vs, never CPU-side (a decode here too was the 16107056 double-
    // decode bug: solids rendered enc(dec^2(authored)) while gradients were right).
    const VGRenderVertex g(VGVertex::Solid(Float2{}, ToColor(Color32{188, 188, 188, 255})));
    CHECK(g.color[0] == doctest::Approx(188.0f / 255.0f));
}

TEST_CASE("vg.renderer: initialize + prepare a batch (headless Null backend)")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    VGRenderer renderer{DefaultAllocator()};
    REQUIRE(
        renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, /*frameCount*/ 2)
            .IsOk());
    CHECK(renderer.IsInitialized());

    // Produce a batch with VGContext.
    VGContext ctx;
    ctx.FillRect(Rectangle{10, 10, 100, 50}, Color::Red);
    ctx.FillCircle(Float2{50, 50}, 20, Color::Blue);
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() > 0u);

    renderer.BeginFrame(0);
    const VGRenderSlice slice = renderer.Prepare(batch, /*frameIndex*/ 0, 800, 600);
    CHECK(slice.isValid);
    CHECK(slice.drawCommandCount == static_cast<i32>(batch.CommandCount()));
    CHECK(slice.vertexByteOffset == 0u);

    // A second batch in the same frame gets a non-overlapping vertex range.
    VGContext ctx2;
    ctx2.FillRect(Rectangle{0, 0, 10, 10}, Color::Green);
    const VGRenderSlice slice2 = renderer.Prepare(ctx2.GetBatch(), 0, 800, 600);
    CHECK(slice2.isValid);
    CHECK(slice2.vertexByteOffset > 0u);

    renderer.Dispose();
    CHECK_FALSE(renderer.IsInitialized());

    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: external texture register / rebind / unregister")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    VGRenderer renderer{DefaultAllocator()};
    REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::RGBA16Float, /*frameCount*/ 2)
                .IsOk());

    // A caller-owned view standing in for a viewport's offscreen color target.
    rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA16Float, 64, 48);
    rhi::Texture* tex = nullptr;
    REQUIRE(device.CreateTexture(td, tex).IsOk());
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(tex, rhi::TextureViewDesc{}, view).IsOk());

    // The identity key: a pixel-less ImageDataRef (dimensions only).
    image::ImageDataRef key(64, 48);
    CHECK_FALSE(renderer.IsExternalTextureRegistered(&key));

    renderer.RegisterExternalTexture(&key, view);
    CHECK(renderer.IsExternalTextureRegistered(&key));

    // DrawImage(key) now Prepares without trying to upload CPU pixels (the ref
    // has none) - the pre-registered external view is used instead.
    VGContext ctx;
    ctx.DrawImage(&key, Rectangle{0, 0, 64, 48});
    renderer.BeginFrame(0);
    const VGRenderSlice slice = renderer.Prepare(ctx.GetBatch(), 0, 800, 600);
    CHECK(slice.isValid);

    // Rebinding the same key to a new view keeps a single entry (still external).
    rhi::TextureView* view2 = nullptr;
    REQUIRE(device.CreateTextureView(tex, rhi::TextureViewDesc{}, view2).IsOk());
    renderer.RegisterExternalTexture(&key, view2);
    CHECK(renderer.IsExternalTextureRegistered(&key));

    // Unregister drops it (without destroying the caller-owned view/texture).
    renderer.UnregisterExternalTexture(&key);
    CHECK_FALSE(renderer.IsExternalTextureRegistered(&key));
    renderer.UnregisterExternalTexture(&key); // unknown key -> no-op, no crash

    // The views/texture are still ours to destroy.
    device.DestroyTextureView(view);
    device.DestroyTextureView(view2);
    device.DestroyTexture(tex);

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: Dispose leaves a still-registered external view intact")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);

    rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA16Float, 32, 32);
    rhi::Texture* tex = nullptr;
    REQUIRE(device.CreateTexture(td, tex).IsOk());
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(tex, rhi::TextureViewDesc{}, view).IsOk());

    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::RGBA16Float, 1).IsOk());
        image::ImageDataRef key(32, 32);
        renderer.RegisterExternalTexture(&key, view);
        // Dispose() -> ClearTextureCache() must NOT destroy the external view.
    }

    // Still valid: we can destroy it ourselves (a double-free would trip ASAN).
    device.DestroyTextureView(view);
    device.DestroyTexture(tex);

    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: empty batch yields an invalid slice")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);

    VGRenderer renderer{DefaultAllocator()};
    REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1).IsOk());

    VGContext ctx; // nothing drawn
    renderer.BeginFrame(0);
    const VGRenderSlice slice = renderer.Prepare(ctx.GetBatch(), 0, 100, 100);
    CHECK_FALSE(slice.isValid);

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: ComputeScissor clamps to content then offsets to the viewport")
{
    using foundation::vg::renderer::VGRenderer;

    // Fully inside: clamp is a no-op, the viewport origin offsets the rect.
    {
        const auto s =
            VGRenderer::ComputeScissor(Rectangle{10.0f, 20.0f, 100.0f, 50.0f}, 400, 300, 400, 300);
        CHECK(s.x == 410);
        CHECK(s.y == 320);
        CHECK(s.width == 100u);
        CHECK(s.height == 50u);
    }
    // Overhanging the content box: clamped to (0..w, 0..h) BEFORE the offset - a clip
    // rect can never reach outside its view's rect (split-screen halves stay sealed).
    {
        const auto s =
            VGRenderer::ComputeScissor(Rectangle{-30.0f, -10.0f, 500.0f, 400.0f}, 400, 0, 400, 300);
        CHECK(s.x == 400);
        CHECK(s.y == 0);
        CHECK(s.width == 400u);
        CHECK(s.height == 300u);
    }
    // Entirely outside the content box: degenerates to zero size (nothing drawn).
    {
        const auto s =
            VGRenderer::ComputeScissor(Rectangle{500.0f, 0.0f, 50.0f, 50.0f}, 0, 0, 400, 300);
        CHECK(s.width == 0u);
    }
    // Full-target viewport (the classic overload's path): identity behavior.
    {
        const auto s =
            VGRenderer::ComputeScissor(Rectangle{10.0f, 10.0f, 50.0f, 50.0f}, 0, 0, 800, 600);
        CHECK(s.x == 10);
        CHECK(s.y == 10);
        CHECK(s.width == 50u);
        CHECK(s.height == 50u);
    }
}

TEST_CASE("vg.renderer: batch eviction retires cached textures until frames age out")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    VGRenderer renderer{DefaultAllocator()};
    REQUIRE(
        renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, /*frameCount*/ 2)
            .IsOk());

    // A gradient fill produces a LUT texture the renderer caches at first sight.
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
    ctx.FillPath(pb.ToPath(), grad, FillRule::NonZero, false);

    renderer.BeginFrame(0);
    REQUIRE(renderer.Prepare(ctx.GetBatch(), 0, 800, 600).isValid);
    const usize cachedAfterDraw = renderer.CachedTextureCount();
    CHECK(cachedAfterDraw >= 2u); // white + the LUT

    // The producer announces the LUT's death through the batch's eviction list: the
    // entry must leave the cache IMMEDIATELY (the address may be recycled this frame)
    // but its GPU resources retire until every in-flight frame has aged past them.
    const foundation::image::ImageData* lut = ctx.GetBatch().textures[1];
    VGBatch evictionBatch;
    evictionBatch.evictedTextures.PushBack(lut);
    renderer.BeginFrame(1);
    (void)renderer.Prepare(evictionBatch, 1, 800, 600); // empty batch: evictions still apply
    CHECK(renderer.CachedTextureCount() == cachedAfterDraw - 1u);
    CHECK(renderer.RetiredTextureCount() == 1u);

    renderer.BeginFrame(0);
    renderer.BeginFrame(1); // frameCount=2: two BeginFrames age the retiree out
    CHECK(renderer.RetiredTextureCount() == 0u);

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: texture cache detects address reuse via ImageData::InstanceId")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    VGRenderer renderer{DefaultAllocator()};
    REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1).IsOk());

    // Two DIFFERENT images constructed at the SAME address (placement new models the
    // allocator recycling a freed image's storage - the editor's stale rainbow previews).
    alignas(image::OwnedImageData) u8 storage[sizeof(image::OwnedImageData)];
    Array<u8> pixelsA(16 * 16 * 4);
    auto* first = new (storage)
        image::OwnedImageData(16, 16, image::PixelFormat::RGBA8, Move(pixelsA));
    const u64 firstId = first->InstanceId();

    VGContext ctx; // one long-lived context, like a real window's
    ctx.DrawImage(first, Float2{0, 0});
    renderer.BeginFrame(0);
    (void)renderer.Prepare(ctx.GetBatch(), 0, 100, 100);
    const usize baseline = renderer.CachedTextureCount(); // image + batch-internal sources
    CHECK(baseline >= 1);

    first->~OwnedImageData();
    Array<u8> pixelsB(16 * 16 * 4);
    auto* second = new (storage)
        image::OwnedImageData(16, 16, image::PixelFormat::RGBA8, Move(pixelsB));
    CHECK(second->InstanceId() != firstId); // fresh identity at the same address

    ctx.Clear();
    ctx.DrawImage(second, Float2{0, 0});
    renderer.BeginFrame(0);
    (void)renderer.Prepare(ctx.GetBatch(), 0, 100, 100);
    // The stale entry was retired and the new identity REPLACED it (count unchanged) -
    // a pointer-only key would have served the dead image's texture instead.
    CHECK(renderer.CachedTextureCount() == baseline);

    second->~OwnedImageData();
    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("vg.renderer: stencil target config builds the stencil-then-cover pipelines")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    // Without a stencil format: no stencil support, stencil commands would be skipped.
    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1)
                    .IsOk());
        CHECK_FALSE(renderer.StencilFillsSupported());
        renderer.Dispose();
    }

    // With MSAA + a stencil format: the cover pipelines exist and a stencil batch prepares.
    {
        VGRenderer renderer{DefaultAllocator()};
        VGTargetConfig config;
        config.sampleCount = 4;
        config.depthStencilFormat = rhi::TextureFormat::Depth24PlusStencil8;
        REQUIRE(renderer
                    .Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1, nullptr,
                                nullptr, nullptr, config)
                    .IsOk());
        CHECK(renderer.StencilFillsSupported());

        VGContext ctx;
        ctx.SetStencilFills(true);
        PathBuilder pb; // donut -> one write + one cover command
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
        ctx.FillPath(pb.ToPath(), Color::Red, FillRule::NonZero, false);

        renderer.BeginFrame(0);
        const VGRenderSlice slice = renderer.Prepare(ctx.GetBatch(), 0, 128, 128);
        CHECK(slice.isValid);
        CHECK(slice.drawCommandCount == 2);
        renderer.Dispose();
    }

    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

namespace
{
    // A headless render pass on the Null device: a colour target + an encoder.
    struct NullPass
    {
        rhi::Texture* texture = nullptr;
        rhi::TextureView* view = nullptr;
        rhi::CommandPool* pool = nullptr;
        rhi::CommandEncoder* encoder = nullptr;
        rhi::RenderPassEncoder* pass = nullptr;

        bool Begin(rhi::Device& device) { return Open(device) && BeginPass(); }

        // The target and the encoder, the pass not yet begun (work that goes before it, such as
        // VGRenderer::RenderLayers, records on the encoder first).
        bool Open(rhi::Device& device)
        {
            rhi::TextureDesc td{};
            td.width = 64;
            td.height = 64;
            td.format = rhi::TextureFormat::BGRA8UnormSrgb;
            td.usage = rhi::TextureUsage::RenderTarget;
            if (!device.CreateTexture(td, texture).IsOk() || texture == nullptr)
                return false;
            if (!device.CreateTextureView(texture, rhi::TextureViewDesc{}, view).IsOk())
                return false;
            if (!device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk())
                return false;
            return pool->CreateEncoder(encoder).IsOk();
        }
        bool BeginPass()
        {
            rhi::RenderPassDesc desc;
            rhi::ColorAttachment color;
            color.view = view;
            desc.colorAttachments.Add(color);
            pass = encoder->BeginRenderPass(desc);
            return pass != nullptr;
        }
        void End(rhi::Device& device)
        {
            if (pass != nullptr)
                pass->End();
            if (encoder != nullptr)
                pool->DestroyEncoder(encoder);
            if (pool != nullptr)
                device.DestroyCommandPool(pool);
            if (view != nullptr)
                device.DestroyTextureView(view);
            if (texture != nullptr)
                device.DestroyTexture(texture);
        }
    };

    // Every dispatch decision the renderer makes, in one batch: each blend mode x (a plain
    // fill, a per-pixel radial, a per-pixel conic), a stencil fill under both rules, the same
    // inside a clip path, an analytic box plain and clipped, and every gradient spread.
    Path CirclePath(Float2 center, f32 radius)
    {
        PathBuilder b;
        ShapeBuilder::BuildCircle(center, radius, b);
        return b.ToPath();
    }
    void BuildDispatchBatch(VGContext& ctx)
    {
        ctx.SetStencilFills(true);
        ctx.SetPerPixelGradients(true);
        ctx.SetAnalyticBoxes(true);
        PathBuilder clipBuilder;
        clipBuilder.MoveTo(0, 0);
        clipBuilder.LineTo(60, 0);
        clipBuilder.LineTo(60, 60);
        clipBuilder.Close();
        const Path clip = clipBuilder.ToPath();
        const VGBlendMode blends[] = {VGBlendMode::Normal, VGBlendMode::Additive,
                                      VGBlendMode::Multiply, VGBlendMode::Screen};
        for (const VGBlendMode blend : blends)
        {
            ctx.SetBlendMode(blend);
            ctx.FillRect(Rectangle{1, 1, 20, 20}, Color::Red);
            ctx.FillBoxShadow(Rectangle{5, 5, 30, 30}, CornerRadii(4.0f), 8.0f, Color::Black);
            // Boxes sit last among the pipeline kinds, so a variant table one kind short shows here.
            ctx.DrawBox(Rectangle{2, 2, 24, 16}, CornerRadii(3.0f), Color::Red, BorderWidths(1.0f), Color::White);
            ctx.PushClipPath(clip);
            ctx.DrawBox(Rectangle{2, 2, 24, 16}, CornerRadii(3.0f), Color::Red, BorderWidths(1.0f), Color::White);
            ctx.PopClipPath();
            VGRadialGradientFill radial(Float2{30, 30}, 10.0f);
            radial.AddStop(0.0f, Color::Red);
            radial.AddStop(1.0f, Color::Blue);
            ctx.FillPath(CirclePath(Float2{30, 30}, 10.0f), radial, FillRule::NonZero, false);
            VGConicGradientFill conic(Float2{30, 30});
            conic.AddStop(0.0f, Color::Green);
            conic.AddStop(1.0f, Color::White);
            ctx.FillPath(CirclePath(Float2{30, 30}, 8.0f), conic, FillRule::NonZero, false);
            const FillRule rules[] = {FillRule::NonZero, FillRule::EvenOdd};
            for (const FillRule rule : rules)
            {
                ctx.FillStar(Float2{30, 30}, 25.0f, 10.0f, 5, Color::White); // convex, direct
                PathBuilder star;
                star.MoveTo(30, 5);
                star.LineTo(45, 55);
                star.LineTo(5, 20);
                star.LineTo(55, 20);
                star.LineTo(15, 55);
                star.Close();
                ctx.FillPath(star.ToPath(), VGSolidFill(Color::Blue), rule, true); // stencil
                ctx.PushClipPath(clip);
                ctx.FillPath(star.ToPath(), VGSolidFill(Color::Blue), rule, true); // clipped
                ctx.PopClipPath();
            }
        }
        ctx.SetBlendMode(VGBlendMode::Normal);
        const VGGradientSpread spreads[] = {VGGradientSpread::Pad, VGGradientSpread::Repeat,
                                            VGGradientSpread::Reflect};
        for (const VGGradientSpread spread : spreads)
        {
            VGLinearGradientFill linear(Float2{0, 0}, Float2{20, 0});
            linear.spread = spread;
            linear.AddStop(0.0f, Color::Red);
            linear.AddStop(1.0f, Color::Blue);
            ctx.SetGradientSpread(spread);
            PathBuilder rect;
            rect.MoveTo(0, 0);
            rect.LineTo(40, 0);
            rect.LineTo(40, 10);
            rect.LineTo(0, 10);
            rect.Close();
            ctx.FillPath(rect.ToPath(), linear, FillRule::NonZero, false);
        }
    }
}

TEST_CASE("vg.renderer: every blend x draw mode, stencil phase x rule x clip, and spread dispatch headlessly")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    rhi::ShaderModule* df = MakeModule(device);
    rhi::ShaderModule* radial = MakeModule(device);
    rhi::ShaderModule* conic = MakeModule(device);
    rhi::ShaderModule* shadow = MakeModule(device);
    rhi::ShaderModule* boxVs = MakeModule(device);
    rhi::ShaderModule* boxFs = MakeModule(device);
    REQUIRE(vs != nullptr);

    VGContext ctx;
    BuildDispatchBatch(ctx);
    VGBatch& batch = ctx.GetBatch();
    i32 live = 0;
    i32 stencilPhase = 0;
    i32 shadowCommands = 0;
    i32 boxCommands = 0;
    for (usize i = 0; i < batch.CommandCount(); ++i)
    {
        const VGCommand cmd = batch.GetCommand(i);
        if (cmd.indexCount == 0)
            continue;
        ++live;
        if (cmd.fillPhase != VGFillPhase::Direct)
            ++stencilPhase;
        if (cmd.drawMode == VGDrawMode::BoxShadow)
            ++shadowCommands;
        if (cmd.drawMode == VGDrawMode::Box)
            ++boxCommands;
    }
    REQUIRE(live > 8);
    REQUIRE(stencilPhase > 0);
    REQUIRE(shadowCommands == 4); // one per blend mode
    REQUIRE(boxCommands == 8);    // plain and clipped, per blend mode

    // With stencil pipelines + gradient shaders: NOTHING is skipped - every variant is built
    // (the blend variants lazily, on first use) and every command dispatches.
    {
        VGRenderer renderer{DefaultAllocator()};
        VGTargetConfig config;
        config.sampleCount = 4;
        config.depthStencilFormat = rhi::TextureFormat::Depth24PlusStencil8;
        REQUIRE(renderer
                    .Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1, df, radial,
                                conic, config, shadow, boxVs, boxFs)
                    .IsOk());
        REQUIRE(renderer.StencilFillsSupported());
        renderer.BeginFrame(0);
        const VGRenderSlice slice = renderer.Prepare(batch, 0, 64, 64);
        REQUIRE(slice.isValid);
        NullPass pass;
        REQUIRE(pass.Begin(device));
        renderer.Render(*pass.pass, 64, 64, 0, slice);
        pass.End(device);
        CHECK(renderer.LastRenderStats().skipped == 0);
        CHECK(renderer.LastRenderStats().drawn == live);
        renderer.Dispose();
    }

    // Without stencil pipelines, the shadow shader or the box shaders: the stencil-phase, box-shadow
    // and box commands are SKIPPED, never drawn with a substitute (winding fans and shadow quadrants
    // would show as flat colour, box vertices are not VG vertices); everything else dispatches.
    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1)
                    .IsOk());
        REQUIRE_FALSE(renderer.StencilFillsSupported());
        renderer.BeginFrame(0);
        const VGRenderSlice slice = renderer.Prepare(batch, 0, 64, 64);
        REQUIRE(slice.isValid);
        NullPass pass;
        REQUIRE(pass.Begin(device));
        renderer.Render(*pass.pass, 64, 64, 0, slice);
        pass.End(device);
        CHECK(renderer.LastRenderStats().skipped == stencilPhase + shadowCommands + boxCommands);
        CHECK(renderer.LastRenderStats().drawn == live - stencilPhase - shadowCommands - boxCommands);
        renderer.Dispose();
    }

    device.DestroyShaderModule(boxFs);
    device.DestroyShaderModule(boxVs);
    device.DestroyShaderModule(shadow);
    device.DestroyShaderModule(conic);
    device.DestroyShaderModule(radial);
    device.DestroyShaderModule(df);
    device.DestroyShaderModule(fs);
    device.DestroyShaderModule(vs);
}


TEST_CASE("vg.renderer: layers draw into targets of their own, then composite into what holds them")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    rhi::ShaderModule* layerFs = MakeModule(device);
    REQUIRE(layerFs != nullptr);

    VGContext ctx;
    ctx.SetLayers(true);
    ctx.FillRect(Rectangle{0, 0, 8, 8}, Color::White); // into the target
    ctx.PushLayer(0.5f);
    ctx.FillRect(Rectangle{10, 10, 20, 20}, Color::Red);
    ctx.PushLayer(0.5f, VGBlendMode::Screen); // nested, with a blend of its own
    ctx.FillRect(Rectangle{20, 20, 20, 20}, Color::Blue);
    ctx.PopLayer();
    ctx.PopLayer();
    ctx.PushLayer(0.5f); // wholly off the canvas: never drawn, its composite skipped
    ctx.FillRect(Rectangle{200, 200, 10, 10}, Color::Red);
    ctx.PopLayer();
    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.layers.Size() == 3u);

    VGTargetConfig config;
    config.sampleCount = 4; // layer targets resolve their own multisampled draws
    config.depthStencilFormat = rhi::TextureFormat::Depth24PlusStencil8;
    const auto run = [&](rhi::ShaderModule* layerShader, VGRenderer::RenderStats& layers,
                         VGRenderer::RenderStats& main)
    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer
                    .Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 2, nullptr, nullptr,
                                nullptr, config, nullptr, nullptr, nullptr, layerShader)
                    .IsOk());
        for (i32 frame = 0; frame < 3; ++frame) // the frame slots' targets are reused
        {
            renderer.BeginFrame(frame % 2);
            const VGRenderSlice slice = renderer.Prepare(batch, frame % 2, 64, 64);
            REQUIRE(slice.isValid);
            CHECK(slice.layerCount == 3);
            NullPass pass;
            REQUIRE(pass.Open(device));
            renderer.RenderLayers(*pass.encoder, frame % 2, slice);
            REQUIRE(pass.BeginPass());
            renderer.Render(*pass.pass, 64, 64, frame % 2, slice);
            pass.End(device);
            layers = renderer.LastLayerStats();
            main = renderer.LastRenderStats();
        }
        renderer.Dispose();
    };

    // With the composite shader: the two layers on the canvas are drawn (each its own fill, the
    // outer one also the inner one's composite); the target gets the white fill and the outer
    // composite, the off-canvas layer's composite skipped.
    {
        VGRenderer::RenderStats layers;
        VGRenderer::RenderStats main;
        run(layerFs, layers, main);
        CHECK(layers.layers == 2);
        CHECK(layers.drawn == 3);
        CHECK(layers.skipped == 0);
        CHECK(main.drawn == 2);
        CHECK(main.skipped == 1);
    }
    // Without it: no layer is drawn and every composite is skipped, never drawn by the default
    // shader (it would multiply the premultiplied colour again).
    {
        VGRenderer::RenderStats layers;
        VGRenderer::RenderStats main;
        run(nullptr, layers, main);
        CHECK(layers.layers == 0);
        CHECK(main.drawn == 1);
        CHECK(main.skipped == 2);
    }

    device.DestroyShaderModule(layerFs);
    device.DestroyShaderModule(fs);
    device.DestroyShaderModule(vs);
}

TEST_CASE("vg.renderer: a blurred layer runs two blur passes before its composite, or none without the shader")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    rhi::ShaderModule* layerFs = MakeModule(device);
    rhi::ShaderModule* blurVs = MakeModule(device);
    rhi::ShaderModule* blurFs = MakeModule(device);

    VGContext ctx;
    ctx.SetLayers(true);
    ctx.PushLayer(1.0f, VGBlendMode::Normal, 3.0f);
    ctx.FillRect(Rectangle{20, 20, 16, 16}, Color::White);
    ctx.PopLayer();
    VGBatch& batch = ctx.GetBatch();

    VGTargetConfig config;
    config.sampleCount = 4; // the content resolves; the blur passes run single-sampled
    config.depthStencilFormat = rhi::TextureFormat::Depth24PlusStencil8;
    const auto run = [&](rhi::ShaderModule* blurVertex, rhi::ShaderModule* blurFragment, u64& passes)
    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer
                    .Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1, nullptr, nullptr, nullptr,
                                config, nullptr, nullptr, nullptr, layerFs, blurVertex, blurFragment)
                    .IsOk());
        renderer.BeginFrame(0);
        const VGRenderSlice slice = renderer.Prepare(batch, 0, 64, 64);
        REQUIRE(slice.isValid);
        rhi::null::NullCommandEncoder encoder;
        renderer.RenderLayers(encoder, 0, slice);
        passes = encoder.counters->Snapshot().renderPasses;
        CHECK(renderer.LastLayerStats().layers == 1);
        renderer.Dispose();
    };
    u64 blurred = 0;
    u64 sharp = 0;
    run(blurVs, blurFs, blurred);
    run(nullptr, nullptr, sharp);
    CHECK(blurred == 3); // the content, across, down
    CHECK(sharp == 1);   // the content alone: drawn, composited, not blurred

    device.DestroyShaderModule(blurFs);
    device.DestroyShaderModule(blurVs);
    device.DestroyShaderModule(layerFs);
    device.DestroyShaderModule(fs);
    device.DestroyShaderModule(vs);
}

TEST_CASE("vg.renderer: pass targets configure, create, resize and describe the quality pass")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    VGPassTargets targets;
    CHECK_FALSE(targets.IsValid());
    CHECK(targets.ConfigFor().sampleCount == 1u); // off: a plain pass

    targets.Configure(device);
    REQUIRE(targets.IsValid());
    CHECK(targets.StencilFormat() != rhi::TextureFormat::Undefined);
    const VGTargetConfig config = targets.ConfigFor();
    CHECK(config.sampleCount == u32{kVGPassTargetsDefaultSamples}); // a copy: a DLL exports no definition to bind
    CHECK(config.depthStencilFormat == targets.StencilFormat());

    REQUIRE(targets.Create(rhi::TextureFormat::BGRA8Unorm, 64, 32));
    CHECK(targets.Matches(64, 32));
    CHECK_FALSE(targets.NeedsResize(64, 32));
    CHECK(targets.NeedsResize(80, 32));
    CHECK_FALSE(targets.NeedsResize(0, 0)); // a minimised window keeps its targets
    REQUIRE(targets.Resize(80, 32));
    CHECK(targets.Matches(80, 32));

    // A zero size creates nothing, and leaves the targets off until the next Create.
    CHECK_FALSE(targets.Create(rhi::TextureFormat::BGRA8Unorm, 0, 0));
    CHECK_FALSE(targets.Matches(80, 32));
    CHECK_FALSE(targets.NeedsResize(80, 32));
    // A resize does not bring targets that are off back (a host resizing after a failed Create).
    CHECK_FALSE(targets.Resize(96, 48));
    CHECK_FALSE(targets.Matches(96, 48));
}

TEST_CASE("vg.renderer: a premultiplied image draws through the composite shader with its own texture, or not at all")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    rhi::ShaderModule* layerFs = MakeModule(device);
    REQUIRE(layerFs != nullptr);

    static const u8 pixels[16]{};
    const image::OwnedImageData picture(2, 2, image::PixelFormat::RGBA8, Span<const u8>(pixels, 16));
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 8, 8}, Color::White);
    ctx.DrawImagePremultiplied(&picture, Rectangle{10, 10, 20, 20}, Rectangle{0, 0, 2, 2});
    VGBatch& batch = ctx.GetBatch();

    const auto run = [&](rhi::ShaderModule* layerShader)
    {
        VGRenderer renderer{DefaultAllocator()};
        REQUIRE(renderer
                    .Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 2, nullptr, nullptr, nullptr,
                                VGTargetConfig{}, nullptr, nullptr, nullptr, layerShader)
                    .IsOk());
        renderer.BeginFrame(0);
        const VGRenderSlice slice = renderer.Prepare(batch, 0, 64, 64);
        REQUIRE(slice.isValid);
        CHECK(slice.layerCount == 0);
        NullPass pass;
        REQUIRE(pass.Open(device));
        renderer.RenderLayers(*pass.encoder, 0, slice);
        REQUIRE(pass.BeginPass());
        renderer.Render(*pass.pass, 64, 64, 0, slice);
        pass.End(device);
        const VGRenderer::RenderStats stats = renderer.LastRenderStats();
        renderer.Dispose();
        return stats;
    };
    const VGRenderer::RenderStats with = run(layerFs);
    CHECK(with.drawn == 2); // the fill and the picture, no layer target asked for
    CHECK(with.skipped == 0);
    // Without the composite shader the picture is skipped, never premultiplied again by the
    // default one.
    const VGRenderer::RenderStats without = run(nullptr);
    CHECK(without.drawn == 1);
    CHECK(without.skipped == 1);

    device.DestroyShaderModule(layerFs);
    device.DestroyShaderModule(fs);
    device.DestroyShaderModule(vs);
}

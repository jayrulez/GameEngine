// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG pixel probes - the VG "golden" tests. Deterministic VG scenes rendered on
// REAL devices (Vulkan + WebGPU), single-sampled with a stencil attachment, pixels read
// back and asserted STRUCTURALLY: fill-rule correctness, stencil path clipping, the
// solid-vs-gradient color-pipeline agreement, gradient spreads, and blend modes. Semantic
// probes instead of stored image diffs - no cross-driver golden drift, and each assertion
// names the property it guards. Skips cleanly when a backend/GPU is unavailable.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
import foundation.rhi.testsupport;
import foundation.shaders;
import foundation.shaders.system;
import foundation.vg;
import foundation.vg.renderer;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace vg = foundation::vg;
namespace shaders = foundation::shaders;
namespace testsupport = foundation::rhi::testsupport;

namespace
{
    constexpr u32 kSize = 128; // bytesPerRow 512 (256-aligned)

    // The readback image is the shared CapturedImage (valid + At/Luma/CountWhere) from
    // Foundation::RHI.TestSupport - same probe surface, no per-test copy of the readback plumbing.
    using Pixels = testsupport::CapturedImage;

    // Render one VG scene (recorded by `record`) and read the target back. sampleCount > 1
    // renders into an MSAA target resolved into the readback texture - the same
    // arrangement the UI canvas-RTT and window hosts use.
    // An Encoded scene draws into the sRGB target through its UNORM view, as a host does for an
    // Encoded canvas; the readback is the target's bytes either way.
    template <typename RecordFn>
    Pixels RenderScene(rhi::Device& device, RecordFn&& record, u32 sampleCount = 1,
                       vg::VGBlendSpace blendSpace = vg::VGBlendSpace::Linear)
    {
        const bool encoded = blendSpace == vg::VGBlendSpace::Encoded;
        const rhi::TextureFormat drawFormat =
            encoded ? rhi::TextureFormat::RGBA8Unorm : rhi::TextureFormat::RGBA8UnormSrgb;
        Pixels out;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return out;
        }
        {
            rhi::ShaderModule* vs = host.GetVariant(u8"vg", shaders::ShaderStage::Vertex,
                                                    shaders::ShaderFlags::None);
            rhi::ShaderModule* fs = host.GetVariant(u8"vg", shaders::ShaderStage::Fragment,
                                                    shaders::ShaderFlags::None);
            rhi::ShaderModule* distanceFieldFragmentShader = host.GetVariant(u8"vg_df", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
            rhi::ShaderModule* gradR = host.GetVariant(
                u8"vg_grad_radial", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            rhi::ShaderModule* gradC = host.GetVariant(
                u8"vg_grad_conic", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            REQUIRE(vs != nullptr);
            REQUIRE(fs != nullptr);

            vg::renderer::VGTargetConfig config;
            config.sampleCount = sampleCount;
            config.depthStencilFormat = rhi::TextureFormat::Depth24PlusStencil8;
            config.blendSpace = blendSpace;

            vg::renderer::VGRenderer renderer{DefaultAllocator()};
            REQUIRE(renderer
                        .Initialize(device, *vs, *fs, drawFormat, 2, distanceFieldFragmentShader,
                                    gradR, gradC, config)
                        .IsOk());

            rhi::TextureDesc cd = rhi::TextureDesc::RenderTarget(
                rhi::TextureFormat::RGBA8UnormSrgb, kSize, kSize);
            cd.usage = cd.usage | rhi::TextureUsage::CopySrc;
            cd.alternateViewFormat = encoded ? rhi::TextureFormat::RGBA8Unorm : rhi::TextureFormat::Undefined;
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(cd, target).IsOk());
            rhi::TextureViewDesc targetViewDesc{};
            targetViewDesc.format = drawFormat;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, targetViewDesc, targetView).IsOk());

            rhi::Texture* msaa = nullptr;
            rhi::TextureView* msaaView = nullptr;
            if (sampleCount > 1)
            {
                rhi::TextureDesc md = rhi::TextureDesc::RenderTarget(drawFormat, kSize, kSize, sampleCount);
                md.usage = rhi::TextureUsage::RenderTarget;
                REQUIRE(device.CreateTexture(md, msaa).IsOk());
                REQUIRE(device.CreateTextureView(msaa, rhi::TextureViewDesc{}, msaaView).IsOk());
            }

            rhi::TextureDesc dd{};
            dd.dimension = rhi::TextureDimension::Texture2D;
            dd.format = rhi::TextureFormat::Depth24PlusStencil8;
            dd.width = kSize;
            dd.height = kSize;
            dd.depth = 1;
            dd.usage = rhi::TextureUsage::DepthStencil;
            dd.sampleCount = sampleCount;
            rhi::Texture* depthStencil = nullptr;
            REQUIRE(device.CreateTexture(dd, depthStencil).IsOk());
            rhi::TextureView* dsView = nullptr;
            REQUIRE(device.CreateTextureView(depthStencil, rhi::TextureViewDesc{}, dsView).IsOk());

            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            vg::VGContext ctx;
            ctx.SetStencilFills(true);
            ctx.SetPerPixelGradients(gradR != nullptr && gradC != nullptr);
            record(ctx);
            vg::VGBatch& batch = ctx.GetBatch();

            renderer.BeginFrame(0);
            const vg::renderer::VGRenderSlice slice = renderer.Prepare(batch, 0, kSize, kSize);

            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            encoder->TransitionTexture(target, rhi::ResourceState::Undefined,
                                       rhi::ResourceState::RenderTarget);
            if (msaa != nullptr)
            {
                encoder->TransitionTexture(msaa, rhi::ResourceState::Undefined,
                                           rhi::ResourceState::RenderTarget);
            }
            encoder->TransitionTexture(depthStencil, rhi::ResourceState::Undefined,
                                       rhi::ResourceState::DepthStencilWrite);
            rhi::RenderPassDesc rp{};
            rhi::ColorAttachment color{};
            color.view = msaaView != nullptr ? msaaView : targetView;
            color.resolveTarget = msaaView != nullptr ? targetView : nullptr;
            color.loadOp = rhi::LoadOp::Clear;
            color.storeOp = msaaView != nullptr ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
            color.clearValue = rhi::ClearColor::Black();
            rp.colorAttachments.Add(color);
            rhi::DepthStencilAttachment ds{};
            ds.view = dsView;
            ds.depthLoadOp = rhi::LoadOp::Clear;
            ds.depthStoreOp = rhi::StoreOp::DontCare;
            ds.stencilLoadOp = rhi::LoadOp::Clear;
            ds.stencilStoreOp = rhi::StoreOp::DontCare;
            ds.stencilClearValue = 0;
            rp.depthStencilAttachment = ds;
            rhi::RenderPassEncoder* pass = encoder->BeginRenderPass(rp);
            REQUIRE(pass != nullptr);
            renderer.Render(*pass, kSize, kSize, 0, slice);
            pass->End();
            encoder->TransitionTexture(target, rhi::ResourceState::RenderTarget,
                                       rhi::ResourceState::CopySrc);
            rhi::CommandBuffer* commands = encoder->Finish();
            REQUIRE(commands != nullptr);
            rhi::CommandBuffer* list[] = {commands};
            queue->Submit(Span<rhi::CommandBuffer* const>(list, 1), fence, 1);
            REQUIRE(fence->Wait(1, ~0ull));
            pool->DestroyEncoder(encoder);

            // Target is left in CopySrc; the shared substrate owns the copy + map + unpack.
            out = testsupport::Readback(device, target, kSize, kSize);

            device.WaitIdle();
            renderer.Dispose();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(dsView);
            device.DestroyTexture(depthStencil);
            if (msaaView != nullptr)
            {
                device.DestroyTextureView(msaaView);
            }
            if (msaa != nullptr)
            {
                device.DestroyTexture(msaa);
            }
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }

    rhi::Device* MakeDevice(rhi::Backend* backend) { return testsupport::MakeTestDevice(backend); }

    Color ByteColor(u8 r, u8 g, u8 b) { return ToColor(Color32{r, g, b, 255}); }

    void ProbeDevice(rhi::Device& device, const char* backendName)
    {
        INFO("backend: ", backendName);

        // --- 1) Fill-rule correctness + stencil path clip -------------------------
        {
            const Pixels px = RenderScene(
                device,
                [](vg::VGContext& ctx)
                {
                    // EvenOdd donut at (32,64): ring filled, core open.
                    vg::PathBuilder donut;
                    vg::ShapeBuilder::BuildCircle(Float2{32.0f, 64.0f}, 28.0f, donut);
                    vg::ShapeBuilder::BuildCircle(Float2{32.0f, 64.0f}, 12.0f, donut);
                    ctx.FillPath(donut.ToPath(), ByteColor(255, 160, 60), vg::FillRule::EvenOdd,
                                 false);
                    // Star clip over stripes at (96,64): stripes only inside the star.
                    ctx.PushState();
                    ctx.Translate(96.0f, 64.0f);
                    vg::PathBuilder star;
                    vg::ShapeBuilder::BuildStar(Float2{0.0f, 0.0f}, 30.0f, 12.0f, 5, star);
                    ctx.PushClipPath(star.ToPath());
                    for (i32 i = -4; i <= 4; ++i)
                    {
                        vg::PathBuilder stripe;
                        const f32 sy = static_cast<f32>(i) * 8.0f - 2.0f;
                        stripe.MoveTo(-32.0f, sy);
                        stripe.LineTo(32.0f, sy);
                        stripe.LineTo(32.0f, sy + 4.0f);
                        stripe.LineTo(-32.0f, sy + 4.0f);
                        stripe.Close();
                        ctx.FillPath(stripe.ToPath(), ByteColor(240, 200, 60),
                                     vg::FillRule::NonZero, false);
                    }
                    ctx.PopClipPath();
                    ctx.PopState();
                });
            REQUIRE(px.valid);
            const u8* ring = px.At(32 + 20, 64); // inside the ring band
            CHECK(ring[0] > 200);                // orange
            const u8* core = px.At(32, 64); // EvenOdd core: OPEN (background)
            CHECK(core[0] < 40);
            const u8* clipCenter = px.At(96, 64); // inside the star: stripe color
            CHECK(clipCenter[0] > 180);
            const u8* clipOutside = px.At(96 + 31, 64 - 31); // stripe row, outside the star
            CHECK(clipOutside[0] < 40);
        }

        // --- 2) Color-pipeline agreement + repeat spread --------------------------
        {
            const Pixels px = RenderScene(
                device,
                [](vg::VGContext& ctx)
                {
                    // Left: solid vs same-color two-stop gradient, butted at x=32.
                    vg::PathBuilder solid;
                    solid.MoveTo(0, 0);
                    solid.LineTo(32, 0);
                    solid.LineTo(32, 40);
                    solid.LineTo(0, 40);
                    solid.Close();
                    ctx.FillPath(solid.ToPath(), ByteColor(180, 60, 40), vg::FillRule::NonZero,
                                 false);
                    vg::VGLinearGradientFill flat(Float2{32.0f, 0.0f}, Float2{64.0f, 0.0f});
                    flat.AddStop(0.0f, ByteColor(180, 60, 40));
                    flat.AddStop(1.0f, ByteColor(180, 60, 40));
                    vg::PathBuilder grad;
                    grad.MoveTo(32, 0);
                    grad.LineTo(64, 0);
                    grad.LineTo(64, 40);
                    grad.LineTo(32, 40);
                    grad.Close();
                    ctx.FillPath(grad.ToPath(), flat, vg::FillRule::NonZero, false);

                    // Bottom: red->blue over 1/3 of the rect, REPEAT: x=8 red-ish,
                    // x=40 (start of period 2) red-ish again, x=30 (end of period 1) blue.
                    vg::VGLinearGradientFill rep(Float2{0.0f, 0.0f}, Float2{32.0f, 0.0f});
                    rep.AddStop(0.0f, ByteColor(220, 40, 40));
                    rep.AddStop(1.0f, ByteColor(40, 40, 220));
                    rep.spread = vg::VGGradientSpread::Repeat;
                    vg::PathBuilder band;
                    band.MoveTo(0, 80);
                    band.LineTo(96, 80);
                    band.LineTo(96, 120);
                    band.LineTo(0, 120);
                    band.Close();
                    ctx.FillPath(band.ToPath(), rep, vg::FillRule::NonZero, false);
                });
            REQUIRE(px.valid);
            const u8* solid = px.At(16, 20);
            const u8* grad = px.At(48, 20);
            // The seam check: the vertex-color decode and the LUT decode must agree.
            CHECK(Abs(static_cast<i32>(solid[0]) - static_cast<i32>(grad[0])) <= 2);
            CHECK(Abs(static_cast<i32>(solid[1]) - static_cast<i32>(grad[1])) <= 2);
            CHECK(Abs(static_cast<i32>(solid[2]) - static_cast<i32>(grad[2])) <= 2);
            CHECK(Abs(static_cast<i32>(solid[0]) - 180) <= 2); // AND both are the authored color
            const u8* p1 = px.At(4, 100);   // period 1 start: red dominates
            const u8* p1e = px.At(30, 100); // period 1 end: blue dominates
            const u8* p2 = px.At(36, 100);  // period 2 start: red again (the repeat)
            CHECK(p1[0] > p1[2]);
            CHECK(p1e[2] > p1e[0]);
            CHECK(p2[0] > p2[2]);
        }

        // --- 3) Blend modes over a light strip ------------------------------------
        {
            const Pixels px = RenderScene(
                device,
                [](vg::VGContext& ctx)
                {
                    vg::PathBuilder strip;
                    strip.MoveTo(0, 48);
                    strip.LineTo(128, 48);
                    strip.LineTo(128, 80);
                    strip.LineTo(0, 80);
                    strip.Close();
                    ctx.FillPath(strip.ToPath(), ByteColor(220, 220, 225), vg::FillRule::NonZero,
                                 false);
                    auto circle = [&](f32 cx, vg::VGBlendMode mode)
                    {
                        ctx.SetBlendMode(mode);
                        vg::PathBuilder pb;
                        vg::ShapeBuilder::BuildCircle(Float2{cx, 64.0f}, 14.0f, pb);
                        ctx.FillPath(pb.ToPath(), ByteColor(180, 60, 40), vg::FillRule::NonZero,
                                     false);
                        ctx.SetBlendMode(vg::VGBlendMode::Normal);
                    };
                    circle(20.0f, vg::VGBlendMode::Additive);
                    circle(60.0f, vg::VGBlendMode::Multiply);
                    circle(100.0f, vg::VGBlendMode::Normal);
                });
            REQUIRE(px.valid);
            const u8* stripPx = px.At(40, 64);     // bare strip
            const u8* additive = px.At(20, 64);    // brighter than the strip (red clips)
            const u8* multiply = px.At(60, 64);    // darker than the strip
            const u8* normal = px.At(100, 64);     // the authored color
            CHECK(additive[0] >= 250);
            CHECK(static_cast<i32>(multiply[1]) < static_cast<i32>(stripPx[1]) - 40);
            CHECK(Abs(static_cast<i32>(normal[0]) - 180) <= 2);
            CHECK(Abs(static_cast<i32>(normal[1]) - 60) <= 2);
        }

        // --- 4) MSAA: 4x resolve produces fractional edge coverage ----------------
        {
            // Stencil-then-cover fills have HARD edges (no analytic fringes) - edge AA
            // comes exclusively from MSAA + resolve, the arrangement the canvas-RTT and
            // window hosts use. A diagonal edge must alias at 1x (every pixel either
            // background or fill) and antialias at 4x (some pixels in between).
            auto diagonal = [](vg::VGContext& ctx)
            {
                vg::PathBuilder tri;
                tri.MoveTo(10, 10);
                tri.LineTo(110, 10);
                tri.LineTo(10, 110);
                tri.Close();
                ctx.FillPath(tri.ToPath(), ByteColor(180, 60, 40), vg::FillRule::NonZero, false);
            };
            const Pixels aliased = RenderScene(device, diagonal, 1);
            const Pixels smooth = RenderScene(device, diagonal, 4);
            REQUIRE(aliased.valid);
            REQUIRE(smooth.valid);
            // Interior stays the exact authored color under the resolve.
            CHECK(Abs(static_cast<i32>(smooth.At(20, 20)[0]) - 180) <= 2);
            CHECK(Abs(static_cast<i32>(smooth.At(20, 20)[1]) - 60) <= 2);
            // Count in-between red bytes crossing the hypotenuse (columns x=30..90 all
            // cross it once). Fill red is 180, background 0.
            auto countIntermediate = [](const Pixels& px)
            {
                i32 count = 0;
                for (u32 x = 30; x <= 90; ++x)
                {
                    for (u32 y = 10; y <= 110; ++y)
                    {
                        const u8 r = px.At(x, y)[0];
                        if (r > 15 && r < 165)
                        {
                            ++count;
                        }
                    }
                }
                return count;
            };
            CHECK(countIntermediate(aliased) == 0);
            CHECK(countIntermediate(smooth) >= 30);
        }

        MESSAGE(backendName << ": all pixel probes passed");
    }
}

TEST_CASE("vg.pixels: fills, clip, colors, spreads and blends on real backends")
{
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    bool any = false;
    if (rhi::Device* device = MakeDevice(vulkan))
    {
        ProbeDevice(*device, "vulkan");
        device->Destroy();
        any = true;
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan probes skipped");
    }
    if (rhi::Device* device = MakeDevice(webgpu))
    {
        ProbeDevice(*device, "webgpu");
        device->Destroy();
        any = true;
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu probes skipped");
    }
    if (!any)
    {
        MESSAGE("no GPU backend available - VG pixel probes skipped entirely");
    }
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}

// ============================================================================================
// Blend space (V1 of Specs/vg-and-gui-lab.md): the same scenes in a Linear and an Encoded canvas.
// Linear blends linear light (half black over white is 73% grey, byte 188); Encoded blends the
// encoded values as browsers do (50% grey, byte 128). A gradient's ramp is baked from the authored
// stops, so it runs as authored in both (its middle is byte 128), and a solid colour reads as
// authored in both.
// ============================================================================================

namespace
{
    struct BlendSpaceReadings
    {
        i32 halfBlack = 0;    // half-transparent black over white
        i32 gradientMid = 0;  // a black-to-white gradient's middle
        i32 solid[3] = {0, 0, 0};
    };

    BlendSpaceReadings ReadBlendSpace(rhi::Device& device, vg::VGBlendSpace space)
    {
        const Pixels px = RenderScene(
            device,
            [](vg::VGContext& ctx)
            {
                const auto rect = [](f32 x0, f32 y0, f32 x1, f32 y1)
                {
                    vg::PathBuilder b;
                    b.MoveTo(x0, y0);
                    b.LineTo(x1, y0);
                    b.LineTo(x1, y1);
                    b.LineTo(x0, y1);
                    b.Close();
                    return b.ToPath();
                };
                ctx.FillPath(rect(0, 0, 128, 40), ByteColor(255, 255, 255), vg::FillRule::NonZero, false);
                ctx.FillPath(rect(0, 0, 128, 40), Color{0.0f, 0.0f, 0.0f, 0.5f}, vg::FillRule::NonZero, false);
                vg::VGLinearGradientFill ramp(Float2{0.0f, 0.0f}, Float2{128.0f, 0.0f});
                ramp.AddStop(0.0f, ByteColor(0, 0, 0));
                ramp.AddStop(1.0f, ByteColor(255, 255, 255));
                ctx.FillPath(rect(0, 44, 128, 84), ramp, vg::FillRule::NonZero, false);
                ctx.FillPath(rect(0, 88, 128, 128), ByteColor(180, 60, 40), vg::FillRule::NonZero, false);
            },
            1, space);
        REQUIRE(px.valid);
        BlendSpaceReadings out;
        out.halfBlack = px.At(64, 20)[0];
        out.gradientMid = px.At(64, 64)[0];
        const u8* solid = px.At(64, 108);
        out.solid[0] = solid[0];
        out.solid[1] = solid[1];
        out.solid[2] = solid[2];
        return out;
    }

    void ProbeBlendSpaces(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        const BlendSpaceReadings linear = ReadBlendSpace(device, vg::VGBlendSpace::Linear);
        const BlendSpaceReadings encoded = ReadBlendSpace(device, vg::VGBlendSpace::Encoded);
        INFO("linear: half black ", linear.halfBlack, ", gradient middle ", linear.gradientMid);
        INFO("encoded: half black ", encoded.halfBlack, ", gradient middle ", encoded.gradientMid);
        CHECK(Abs(linear.halfBlack - 188) <= 2);
        CHECK(Abs(encoded.halfBlack - 128) <= 2);
        CHECK(Abs(linear.gradientMid - 128) <= 6);
        CHECK(Abs(encoded.gradientMid - 128) <= 6);
        for (i32 c = 0; c < 3; ++c)
        {
            const i32 authored[3] = {180, 60, 40};
            CHECK(Abs(linear.solid[c] - authored[c]) <= 2);
            CHECK(Abs(encoded.solid[c] - authored[c]) <= 2);
        }
    }
}

TEST_CASE("vg.pixels: an encoded canvas blends encoded values, a linear one linear light")
{
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
    if (rhi::Device* device = MakeDevice(vulkan))
    {
        ProbeBlendSpaces(*device, "vulkan");
        device->Destroy();
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan blend space probe skipped");
    }
    if (rhi::Device* device = MakeDevice(webgpu))
    {
        ProbeBlendSpaces(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu blend space probe skipped");
    }
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}

// ============================================================================================
// Baked-font consistency probe (fonts triad regression net, added during the 2026-08-12
// jumbled-text incident): the SAME text at the SAME size through the TTF service (rasterize-
// on-demand - the dev path) and through the BAKED wrappers (BakedFont + BakedFontAtlas, the
// exact objects the cooked FontResource loads into) must produce near-identical pixels - the
// bake IS a snapshot of the same rasterizer. Divergence = the baked draw path lies (wrong
// regions/UVs/metrics), which manifests as jumbled game-UI text. PNGs of both strips are
// written to the test scratch dir for eyes-on diagnosis on failure.
// ============================================================================================

import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.coverage.baker;
import foundation.fonts.coverage;
import foundation.fonts.resource;
import foundation.image;
import foundation.image.io;

namespace
{
    namespace fonts = foundation::fonts;
    namespace image = foundation::image;

    constexpr const char8_t* kProbeText = u8"AVWaji 42";
    constexpr f32 kProbeSize = 20.0f;

    void SaveProbePng(const Pixels& pixels, const char8_t* name)
    {
        if (!pixels.valid)
        {
            return;
        }
        image::Image img(kSize, kSize, image::PixelFormat::RGBA8,
                         Span<const u8>(pixels.rgba.Data(), pixels.rgba.Size()));
        (void)image::io::SaveImage(img, StringView(name), image::io::ImageFileFormat::PNG);
    }

    // Fraction of pixels with no acceptable match within a 1px neighborhood - tolerant of the
    // sub-pixel placement difference between stb's float-precision packed quads (TTF path) and
    // the baked integer regions, while still catching the jumble class (overlaps / scaled
    // glyphs mismatch regardless of a 1px shift).
    f64 MismatchFraction(const Pixels& a, const Pixels& b)
    {
        if (!a.valid || !b.valid)
        {
            return 1.0;
        }
        const auto channelDelta = [](const u8* pa, const u8* pb)
        {
            i32 delta = 0;
            for (i32 c = 0; c < 3; ++c)
            {
                const i32 d = static_cast<i32>(pa[c]) - static_cast<i32>(pb[c]);
                delta = Max(delta, d < 0 ? -d : d);
            }
            return delta;
        };
        usize mismatched = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const u8* pa = a.At(x, y);
                i32 best = 255;
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        const i32 nx = static_cast<i32>(x) + dx;
                        const i32 ny = static_cast<i32>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<i32>(kSize) ||
                            ny >= static_cast<i32>(kSize))
                        {
                            continue;
                        }
                        best = Min(best, channelDelta(pa, b.At(static_cast<u32>(nx),
                                                               static_cast<u32>(ny))));
                    }
                }
                if (best > 32)
                {
                    ++mismatched;
                }
            }
        }
        return static_cast<f64>(mismatched) / (static_cast<f64>(kSize) * kSize);
    }

    usize InkedPixels(const Pixels& p)
    {
        if (!p.valid)
        {
            return 0;
        }
        usize inked = 0;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                if (p.At(x, y)[0] > 40)
                {
                    ++inked;
                }
            }
        }
        return inked;
    }
}

TEST_CASE("vg.pixels: the baked-font draw path matches the TTF path (fonts triad net)")
{
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Device* device = MakeDevice(vulkan);
    if (device == nullptr)
    {
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        MESSAGE("Vulkan unavailable - baked-font probe skipped");
        return;
    }

    Result<Array<byte>> ttfFile =
        ReadFile(testsupport::DataPath(u8"Assets/fonts/roboto/Roboto-Regular.ttf").AsView());
    REQUIRE(ttfFile.HasValue());
    const Array<byte>& ttf = ttfFile.Value();
    REQUIRE(!ttf.IsEmpty());

    // Path A: the TTF service (rasterize-on-demand; the dev-tree path - ground truth).
    fonts::TrueTypeFontService ttfService(DefaultAllocator());
    REQUIRE(ttfService.LoadFont(u8"Roboto",
                                testsupport::DataPath(u8"Assets/fonts/roboto/Roboto-Regular.ttf")
                                    .AsView()) == fonts::FontLoadResult::Success);
    fonts::CachedFont* ttfFont = ttfService.GetFont(u8"Roboto", kProbeSize);
    REQUIRE(ttfFont != nullptr);
    // The TTF service serves its CLOSEST loaded size, not necessarily the request - bake strip
    // B at the size strip A actually renders, or the probe compares two different sizes.
    const f32 servedSize = ttfFont->font->PixelHeight();

    Pixels ttfPixels = RenderScene(*device,
                                   [&](vg::VGContext& ctx)
                                   {
                                       ctx.SetFontService(&ttfService);
                                       ctx.DrawText(StringView(kProbeText), ttfFont,
                                                    Float2{4.0f, 60.0f},
                                                    Color{1.0f, 1.0f, 1.0f, 1.0f});
                                   });

    // Path B: the BAKED wrappers - the exact objects a cooked FontResource loads into
    // (FontBaker::Bake is the same bake the cook runs; the wrap mirrors FontFactory).
    fonts::FontLoadOptions options = fonts::FontLoadOptions::Default();
    options.pixelHeight = servedSize;
    auto bakedResult = fonts::FontBaker::Bake(
        Span<const u8>(reinterpret_cast<const u8*>(ttf.Data()), ttf.Size()), options,
        DefaultAllocator());
    REQUIRE(bakedResult.HasValue());
    fonts::BakedFont* bakedFont = nullptr;
    fonts::BakedFontAtlas* bakedAtlas = nullptr;
    bakedResult.Value()->TakeOwnership(bakedFont, bakedAtlas);
    DefaultAllocator().Delete(bakedResult.Value());

    // Heap + refcounted like a real resource product: ResourceFontService takes a STRONG ref
    // on registered fonts (the mid-session re-cook UAF guard), so a stack Font would die at
    // the service's release.
    RefPtr<fonts::Font> product = MakeRef<fonts::Font>(DefaultAllocator());
    product->SetFamily(u8"Roboto");
    {
        fonts::Font::Entry entry;
        entry.pixelHeight = servedSize;
        entry.font = UniquePtr<fonts::BakedFont>(bakedFont, DefaultAllocator());
        entry.atlasImage = UniquePtr<image::OwnedImageData>(
            fonts::FontAtlasTexture::ExpandR8ToRGBA8(bakedAtlas, DefaultAllocator()),
            DefaultAllocator());
        entry.atlas = UniquePtr<fonts::IFontAtlas>(bakedAtlas, DefaultAllocator());
        REQUIRE(entry.atlasImage);
        product->AddEntry(Move(entry));
    }
    fonts::ResourceFontService bakedService(DefaultAllocator());
    bakedService.AddFont(product.Get());
    fonts::CachedFont* cookedFont = bakedService.GetFont(u8"Roboto", servedSize);
    REQUIRE(cookedFont != nullptr);

    Pixels bakedPixels = RenderScene(*device,
                                     [&](vg::VGContext& ctx)
                                     {
                                         ctx.SetFontService(&bakedService);
                                         ctx.DrawText(StringView(kProbeText), cookedFont,
                                                      Float2{4.0f, 60.0f},
                                                      Color{1.0f, 1.0f, 1.0f, 1.0f});
                                     });

    // Numeric probe: the same glyph at the same pen through both atlases.
    {
        fonts::AtlasRegion ttfRegion;
        fonts::AtlasRegion bakedRegion;
        (void)ttfFont->atlas->TryGetRegion('A', ttfRegion);
        (void)cookedFont->atlas->TryGetRegion('A', bakedRegion);
        fonts::GlyphQuad ttfQuad;
        fonts::GlyphQuad bakedQuad;
        f32 cx = 4.0f;
        (void)ttfFont->atlas->GetGlyphQuad('A', cx, 60.0f, ttfQuad);
        cx = 4.0f;
        (void)cookedFont->atlas->GetGlyphQuad('A', cx, 60.0f, bakedQuad);
        MESSAGE("ttf   region 'A': xy=" << ttfRegion.x << "," << ttfRegion.y
                << " wh=" << ttfRegion.width << "x" << ttfRegion.height
                << " off=" << ttfRegion.offsetX << "," << ttfRegion.offsetY
                << " adv=" << ttfRegion.advanceX);
        MESSAGE("baked region 'A': xy=" << bakedRegion.x << "," << bakedRegion.y
                << " wh=" << bakedRegion.width << "x" << bakedRegion.height
                << " off=" << bakedRegion.offsetX << "," << bakedRegion.offsetY
                << " adv=" << bakedRegion.advanceX);
        MESSAGE("ttf   quad 'A': x=" << ttfQuad.x0 << ".." << ttfQuad.x1
                << " y=" << ttfQuad.y0 << ".." << ttfQuad.y1);
        MESSAGE("baked quad 'A': x=" << bakedQuad.x0 << ".." << bakedQuad.x1
                << " y=" << bakedQuad.y0 << ".." << bakedQuad.y1);
        MESSAGE("ttf metrics: ascent=" << ttfFont->font->Metrics().ascent
                << " descent=" << ttfFont->font->Metrics().descent
                << " scale=" << ttfFont->font->Metrics().scale);
        MESSAGE("baked metrics: ascent=" << cookedFont->font->Metrics().ascent
                << " descent=" << cookedFont->font->Metrics().descent
                << " scale=" << cookedFont->font->Metrics().scale);
    }

    SaveProbePng(ttfPixels, u8"font-probe-ttf.png");
    SaveProbePng(bakedPixels, u8"font-probe-baked.png");
    // Diff visualization for failures: red = TTF-only ink, green = baked-only ink.
    if (ttfPixels.valid && bakedPixels.valid)
    {
        Pixels diff = ttfPixels;
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                u8* d = diff.rgba.Data() + (static_cast<usize>(y) * kSize + x) * 4;
                const u8 ta = ttfPixels.At(x, y)[0];
                const u8 ba = bakedPixels.At(x, y)[0];
                d[0] = ta;
                d[1] = ba;
                d[2] = 0;
                d[3] = 255;
            }
        }
        SaveProbePng(diff, u8"font-probe-diff.png");
    }

    // Both strips drew SOMETHING...
    REQUIRE(ttfPixels.valid);
    REQUIRE(bakedPixels.valid);
    CHECK(InkedPixels(ttfPixels) > 100);
    CHECK(InkedPixels(bakedPixels) > 100);
    // ...and the same something: the bake is a snapshot of the same rasterizer, so beyond
    // AA noise the images must agree. Jumbled = regions/UVs/metrics lie in the baked path.
    CHECK(MismatchFraction(ttfPixels, bakedPixels) < 0.02);

    device->Destroy();
    vulkan->Destroy();
}

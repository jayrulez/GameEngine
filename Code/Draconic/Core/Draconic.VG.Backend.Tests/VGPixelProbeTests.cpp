// Draconic::VG pixel probes - the VG "golden" tests. Deterministic VG scenes rendered on
// REAL devices (Vulkan + WebGPU), single-sampled with a stencil attachment, pixels read
// back and asserted STRUCTURALLY: fill-rule correctness, stencil path clipping, the
// solid-vs-gradient color-pipeline agreement, gradient spreads, and blend modes. Semantic
// probes instead of stored image diffs - no cross-driver golden drift, and each assertion
// names the property it guards. Skips cleanly when a backend/GPU is unavailable.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.vulkan;
import draconic.rhi.webgpu;
import draconic.shaders;
import draconic.shaders.system;
import draconic.vg;
import draconic.vg.renderer;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace vg = draconic::vg;
namespace shaders = draconic::shaders;

namespace
{
    constexpr u32 kSize = 128; // bytesPerRow 512 (256-aligned)

    struct Pixels
    {
        bool valid = false;
        Array<u8> data; // kSize*kSize*4, RGBA (sRGB-encoded bytes)

        [[nodiscard]] const u8* At(u32 x, u32 y) const
        {
            return data.Data() + (static_cast<usize>(y) * kSize + x) * 4;
        }
    };

    // Render one VG scene (recorded by `record`) and read the target back. sampleCount > 1
    // renders into an MSAA target resolved into the readback texture - the same
    // arrangement the UI canvas-RTT and window hosts use.
    template <typename RecordFn>
    Pixels RenderScene(rhi::Device& device, RecordFn&& record, u32 sampleCount = 1)
    {
        Pixels out;
        shaders::ShaderSystemHost host;
        if (!host.Initialize(device, StringView(reinterpret_cast<const char8_t*>(
                                         DRACONIC_ENGINE_SHADER_DIR))))
        {
            return out;
        }
        {
            rhi::ShaderModule* vs = host.GetVariant(u8"vg", shaders::ShaderStage::Vertex,
                                                    shaders::ShaderFlags::None);
            rhi::ShaderModule* fs = host.GetVariant(u8"vg", shaders::ShaderStage::Fragment,
                                                    shaders::ShaderFlags::None);
            rhi::ShaderModule* dfFs = host.GetVariant(u8"vg_df", shaders::ShaderStage::Fragment,
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

            vg::renderer::VGRenderer renderer;
            REQUIRE(renderer
                        .Initialize(device, *vs, *fs, rhi::TextureFormat::RGBA8UnormSrgb, 2, dfFs,
                                    gradR, gradC, config)
                        .IsOk());

            rhi::TextureDesc cd = rhi::TextureDesc::RenderTarget(
                rhi::TextureFormat::RGBA8UnormSrgb, kSize, kSize);
            cd.usage = cd.usage | rhi::TextureUsage::CopySrc;
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(cd, target).IsOk());
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, rhi::TextureViewDesc{}, targetView).IsOk());

            rhi::Texture* msaa = nullptr;
            rhi::TextureView* msaaView = nullptr;
            if (sampleCount > 1)
            {
                rhi::TextureDesc md = rhi::TextureDesc::RenderTarget(
                    rhi::TextureFormat::RGBA8UnormSrgb, kSize, kSize, sampleCount);
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

            rhi::BufferDesc rb{};
            rb.size = 512ull * kSize;
            rb.usage = rhi::BufferUsage::CopyDst;
            rb.memory = rhi::MemoryLocation::GpuToCpu;
            rhi::Buffer* readback = nullptr;
            REQUIRE(device.CreateBuffer(rb, readback).IsOk());

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
            rhi::BufferTextureCopyRegion region;
            region.bytesPerRow = 512;
            region.rowsPerImage = kSize;
            region.textureExtent = rhi::Extent3D{kSize, kSize, 1};
            encoder->CopyTextureToBuffer(target, readback, region);
            rhi::CommandBuffer* commands = encoder->Finish();
            REQUIRE(commands != nullptr);
            rhi::CommandBuffer* list[] = {commands};
            queue->Submit(Span<rhi::CommandBuffer* const>(list, 1), fence, 1);
            REQUIRE(fence->Wait(1, ~0ull));

            const u8* mapped = static_cast<const u8*>(readback->Map());
            REQUIRE(mapped != nullptr);
            out.data.Resize(static_cast<usize>(kSize) * kSize * 4);
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* src = mapped + static_cast<usize>(y) * 512 + x * 4;
                    u8* dst = out.data.Data() + (static_cast<usize>(y) * kSize + x) * 4;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                }
            }
            out.valid = true;

            device.WaitIdle();
            renderer.Dispose();
            device.DestroyBuffer(readback);
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

    rhi::Device* MakeDevice(rhi::Backend* backend)
    {
        if (backend == nullptr || backend->EnumerateAdapters().IsEmpty())
        {
            return nullptr;
        }
        rhi::Device* device = nullptr;
        if (!backend->EnumerateAdapters()[0]->CreateDevice(rhi::DeviceDesc{}, device).IsOk())
        {
            return nullptr;
        }
        return device;
    }

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
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu);

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
}

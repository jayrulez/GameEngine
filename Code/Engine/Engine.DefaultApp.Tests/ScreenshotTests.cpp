// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ScreenshotCapture: the flags, the row unpack, and the whole path on real GPUs - a texture
// cleared to a known colour in RGBA8 and BGRA8, recorded the way DefaultApplication::FinishFrame
// records the backbuffer, completed, written as a PNG and read back through the image loader.
// Legacy Sedulous stopped at the GPU copy; this pins that the file exists and is right.
#include <doctest/doctest.h>
#include <cstring>
#include <filesystem>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
#if OPTION_HAS_VULKAN
import foundation.rhi.vulkan;
#endif
#if OPTION_HAS_WEBGPU
import foundation.rhi.webgpu;
#endif
import foundation.rhi.testsupport;
import foundation.image;
import foundation.image.io;
import engine.defaultapp;

using namespace foundation::core;
using engine::runtime::ScreenshotCapture;
using engine::runtime::ScreenshotOptions;
using engine::runtime::ScreenshotOptionsFromArguments;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace image = foundation::image;

namespace
{
    ScreenshotOptions Parse(std::initializer_list<const char*> args)
    {
        Array<char*> argv(DefaultAllocator());
        argv.PushBack(const_cast<char*>("app"));
        for (const char* a : args)
        {
            argv.PushBack(const_cast<char*>(a));
        }
        return ScreenshotOptionsFromArguments(static_cast<int>(argv.Size()), argv.Data());
    }
}

TEST_CASE("screenshot: the flags parse, default to frame 30, and ignore what is not theirs")
{
    const ScreenshotOptions none = Parse({"--data-root", "x", "--vulkan"});
    CHECK_FALSE(none.Requested());

    const ScreenshotOptions byFrame = Parse({"--screenshot", "out.png", "--screenshot-frame", "7"});
    CHECK(byFrame.Requested());
    CHECK(byFrame.path == StringView(u8"out.png"));
    CHECK(byFrame.frame == 7u);
    CHECK(byFrame.afterSeconds == 0.0f);
    CHECK_FALSE(byFrame.exitAfter);
    CHECK(byFrame.Due(7, 0.0f));
    CHECK_FALSE(byFrame.Due(6, 100.0f)); // frames decide when no time was asked for

    const ScreenshotOptions bySeconds =
        Parse({"--screenshot", "shot.png", "--screenshot-after", "15.5", "--screenshot-exit"});
    CHECK(bySeconds.afterSeconds == doctest::Approx(15.5f));
    CHECK(bySeconds.exitAfter);
    CHECK_FALSE(bySeconds.Due(30, 15.4f));
    CHECK(bySeconds.Due(1, 15.5f)); // time decides, whatever the frame

    CHECK(Parse({"--screenshot", "a.png"}).frame == 30u);
    CHECK(Parse({"--screenshot", "a.png", "--screenshot-frame", "0"}).frame == 1u); // never "frame 0"
    CHECK_FALSE(Parse({"--screenshot"}).Requested()); // a dangling flag asks for nothing
}

// Consecutive frames (frame-to-frame change in an exported build: TAA's jitter in PaperKid's player):
// one file each, numbered before the extension; one frame keeps the path as given.
TEST_CASE("screenshot: a count takes that many consecutive frames, one numbered file each")
{
    const ScreenshotOptions one = Parse({"--screenshot", "dir/shot.png"});
    CHECK(one.count == 1u);
    CHECK(one.PathFor(0) == StringView(u8"dir/shot.png"));

    const ScreenshotOptions run = Parse({"--screenshot", "dir.v2/shot.png", "--screenshot-count", "8"});
    CHECK(run.count == 8u);
    CHECK(run.PathFor(0) == StringView(u8"dir.v2/shot-0.png"));
    CHECK(run.PathFor(7) == StringView(u8"dir.v2/shot-7.png"));
    CHECK(Parse({"--screenshot", "noext", "--screenshot-count", "2"}).PathFor(1) == StringView(u8"noext-1"));
    CHECK(Parse({"--screenshot", "a.png", "--screenshot-count", "0"}).count == 1u); // never "no frames"
}

TEST_CASE("screenshot: rows unpack from the aligned pitch, and BGRA swizzles to RGBA")
{
    // 3x2 pixels in a 256-byte pitch; each row's tail is garbage the unpack must skip.
    constexpr u32 w = 3, h = 2, pitch = 256;
    u8 mapped[pitch * h];
    for (u8& b : mapped)
    {
        b = 0xEE;
    }
    const u8 px[h][w][4] = {{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}},
                            {{13, 14, 15, 16}, {17, 18, 19, 20}, {21, 22, 23, 24}}};
    for (u32 y = 0; y < h; ++y)
    {
        for (u32 x = 0; x < w; ++x)
        {
            for (u32 c = 0; c < 4; ++c)
            {
                mapped[y * pitch + x * 4 + c] = px[y][x][c];
            }
        }
    }
    u8 rgba[w * h * 4];
    ScreenshotCapture::UnpackRows(mapped, pitch, w, h, rhi::TextureFormat::RGBA8Unorm, Span<u8>{rgba, sizeof rgba});
    CHECK(rgba[0] == 1);
    CHECK(rgba[3] == 4);
    CHECK(rgba[(1 * w + 2) * 4 + 0] == 21); // last pixel, straight through
    CHECK(rgba[(1 * w + 2) * 4 + 2] == 23);

    ScreenshotCapture::UnpackRows(mapped, pitch, w, h, rhi::TextureFormat::BGRA8Unorm, Span<u8>{rgba, sizeof rgba});
    CHECK(rgba[0] == 3); // B -> R
    CHECK(rgba[1] == 2);
    CHECK(rgba[2] == 1); // R -> B
    CHECK(rgba[3] == 4);
    CHECK(rgba[(1 * w + 2) * 4 + 0] == 23);
    CHECK(rgba[(1 * w + 2) * 4 + 2] == 21);

    CHECK(ScreenshotCapture::CanCapture(rhi::TextureFormat::BGRA8UnormSrgb));
    CHECK(ScreenshotCapture::CanCapture(rhi::TextureFormat::RGBA8Unorm));
    CHECK(ScreenshotCapture::CanCapture(rhi::TextureFormat::RGBA16Float)); // the viewports' target
    CHECK_FALSE(ScreenshotCapture::CanCapture(rhi::TextureFormat::RGBA32Float));

    // A 16-bit float row: linear values, clamped and sRGB encoded to bytes (alpha stays linear);
    // the pitch holds 8-byte texels.
    const u16 halves[2][4] = {{0x3C00, 0x3800, 0x0000, 0x3C00},  // 1.0, 0.5, 0.0, 1.0
                              {0x4400, 0xBC00, 0x3555, 0x3C00}}; // 4.0 -> 1, -1 -> 0, 1/3, 1.0
    u8 halfRow[pitch];
    for (u8& b : halfRow)
    {
        b = 0xEE;
    }
    std::memcpy(halfRow, halves, sizeof(halves));
    u8 fromHalf[2 * 4];
    ScreenshotCapture::UnpackRows(halfRow, pitch, 2, 1, rhi::TextureFormat::RGBA16Float,
                                  Span<u8>{fromHalf, sizeof fromHalf});
    CHECK(fromHalf[0] == 255);
    CHECK(fromHalf[1] == 188); // linear 0.5 is sRGB 0.735
    CHECK(fromHalf[2] == 0);
    CHECK(fromHalf[3] == 255);
    CHECK(fromHalf[4] == 255); // clamped high
    CHECK(fromHalf[5] == 0);   // clamped low
    CHECK(fromHalf[6] == 156); // linear 1/3 is sRGB 0.612
    CHECK(fromHalf[7] == 255);
    CHECK(ScreenshotCapture::IsBgra(rhi::TextureFormat::BGRA8Unorm));
    CHECK_FALSE(ScreenshotCapture::IsBgra(rhi::TextureFormat::RGBA8UnormSrgb));
}

namespace
{
    // Clear a `format` texture to `clear`, capture it the way the app captures the backbuffer
    // (RenderTarget state) or the editor captures a viewport (the target already handed to the
    // UI in ShaderRead), write the PNG, load it back: the pixel at (5, 5) and the size must match.
    void CaptureProbe(rhi::Device& device, rhi::TextureFormat format, const char* name,
                      rhi::ResourceState state = rhi::ResourceState::RenderTarget)
    {
        constexpr u32 w = 64, h = 48;
        rhi::TextureDesc td{};
        td.format = format;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc |
                   rhi::TextureUsage::Sampled;
        td.label = u8"screenshot.probe";
        rhi::Texture* texture = nullptr;
        REQUIRE(device.CreateTexture(td, texture).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = format;
        rhi::TextureView* view = nullptr;
        REQUIRE(device.CreateTextureView(texture, vd, view).IsOk());

        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::Fence* fence = nullptr;
        REQUIRE(device.CreateFence(0, fence).IsOk());
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
        REQUIRE(queue != nullptr);
        rhi::CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());

        // The frame: a clear to a colour with distinct channels (so a swizzle slip shows),
        // the texture left in RenderTarget state as the host leaves the backbuffer.
        encoder->TransitionTexture(texture, rhi::ResourceState::Undefined,
                                   rhi::ResourceState::RenderTarget);
        rhi::ColorAttachment ca{};
        ca.view = view;
        ca.loadOp = rhi::LoadOp::Clear;
        ca.storeOp = rhi::StoreOp::Store;
        ca.clearValue = rhi::ClearColor{0.2f, 0.6f, 1.0f, 1.0f};
        rhi::RenderPassDesc rpd{};
        rpd.colorAttachments.Add(ca);
        rhi::RenderPassEncoder* pass = encoder->BeginRenderPass(rpd);
        REQUIRE(pass != nullptr);
        pass->End();
        if (state != rhi::ResourceState::RenderTarget)
        {
            encoder->TransitionTexture(texture, rhi::ResourceState::RenderTarget, state);
        }

        String path;
        AppendFormat(path, u8"screenshot_probe_{}.png", StringView(reinterpret_cast<const utf8char*>(name)));
        std::error_code ec;
        std::filesystem::remove(reinterpret_cast<const char*>(path.Data()), ec);

        ScreenshotCapture capture;
        CHECK_FALSE(capture.Record(device, *encoder, texture, format, w, h, state)); // not armed: nothing
        capture.Request(path.AsView());
        CHECK(capture.Armed());
        REQUIRE(capture.Record(device, *encoder, texture, format, w, h, state));
        CHECK_FALSE(capture.Armed());
        CHECK(capture.Recorded());

        rhi::CommandBuffer* cmd = encoder->Finish();
        REQUIRE(cmd != nullptr);
        rhi::CommandBuffer* list[] = {cmd};
        queue->Submit(Span<rhi::CommandBuffer* const>(list, 1), fence, 1);
        REQUIRE(fence->Wait(1, ~0ull));

        image::Image written;
        REQUIRE(capture.Complete(device, DefaultAllocator(), written).IsOk());
        CHECK_FALSE(capture.Recorded());
        CHECK(written.Width() == w);
        CHECK(written.Height() == h);

        // The file, through the loader: the same pixels in RGBA order regardless of the surface.
        image::Image loaded;
        REQUIRE(image::io::LoadImage(path.AsView(), loaded).IsOk());
        CHECK(loaded.Width() == w);
        CHECK(loaded.Height() == h);
        REQUIRE(loaded.Format() == image::PixelFormat::RGBA8);
        const u8* p = loaded.PixelData().Data() + (5 * w + 5) * 4;
        INFO(doctest::String(name) << ": " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]) << "," << int(p[3]));
        // 0.2 / 0.6 / 1.0: a UNORM probe stores the plain values; the float target holds
        // linear values, which the capture sRGB encodes (0.2 is 124, 0.6 is 203).
        const int wantRed = format == rhi::TextureFormat::RGBA16Float ? 124 : 51;
        const int wantGreen = format == rhi::TextureFormat::RGBA16Float ? 203 : 153;
        CHECK(Abs(int(p[0]) - wantRed) <= 2);
        CHECK(Abs(int(p[1]) - wantGreen) <= 2);
        CHECK(p[2] == 255);
        CHECK(p[3] == 255);

        capture.Release(device);
        device.WaitIdle();
        pool->DestroyEncoder(encoder);
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        device.DestroyTextureView(view);
        device.DestroyTexture(texture);
        std::filesystem::remove(reinterpret_cast<const char*>(path.Data()), ec);
    }
}

TEST_CASE("screenshot: a cleared RGBA8 and BGRA8 target round-trips to a PNG on Vulkan + WebGPU")
{
    rhi::Backend* vulkan = nullptr;
    rhi::Backend* webgpu = nullptr;
#if OPTION_HAS_VULKAN
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
#endif
#if OPTION_HAS_WEBGPU
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
#endif

    if (rhi::Device* device = vulkan != nullptr ? testsupport::MakeTestDevice(vulkan) : nullptr)
    {
        CaptureProbe(*device, rhi::TextureFormat::RGBA8Unorm, "vulkan-rgba");
        CaptureProbe(*device, rhi::TextureFormat::BGRA8Unorm, "vulkan-bgra");
        CaptureProbe(*device, rhi::TextureFormat::RGBA8Unorm, "vulkan-rgba-shaderread",
                     rhi::ResourceState::ShaderRead); // a viewport's finished target
        CaptureProbe(*device, rhi::TextureFormat::RGBA16Float, "vulkan-rgba16f",
                     rhi::ResourceState::ShaderRead); // the viewports' real format
        device->Destroy();
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan screenshot probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        CaptureProbe(*device, rhi::TextureFormat::RGBA8Unorm, "webgpu-rgba");
        CaptureProbe(*device, rhi::TextureFormat::BGRA8Unorm, "webgpu-bgra");
        CaptureProbe(*device, rhi::TextureFormat::RGBA8Unorm, "webgpu-rgba-shaderread",
                     rhi::ResourceState::ShaderRead);
        CaptureProbe(*device, rhi::TextureFormat::RGBA16Float, "webgpu-rgba16f",
                     rhi::ResourceState::ShaderRead);
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu screenshot probe skipped");
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

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Block-compressed texture upload+sample acceptance probe (asset-variants P1f). On real Vulkan +
// WebGPU via the shared RHI.TestSupport readback substrate; STRUCTURAL assertions (no golden images).
//
// Proves the whole compressed path end to end on the GPU: encode a solid-color image to a BC format
// (texture.compression, the cook's encoder), create a BC RHI texture, upload it with the block-aware
// layout the runtime loader uses (bytesPerRow = one block-row, rowsPerImage = block rows - the same
// rhi:: helpers TextureResource's factory calls), then sample it on an unlit cube and read the color
// back. If the block-row math or the backend's compressed buffer->image copy were wrong, the sampled
// color would be garbage. Runs on both backends, so it also covers WebGPU's 256-aligned upload path.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.rhi;
#ifdef OPTION_HAS_VULKAN
import foundation.rhi.vulkan;
#endif
#ifdef OPTION_HAS_WEBGPU
import foundation.rhi.webgpu;
#endif
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12;
#endif
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import texture.compression;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 128;    // render target
    constexpr u32 kTexSize = 64;  // source texture (16x16 blocks)

    // Encode a solid RGBA8 texel to `format` and create a live BC GPU texture uploaded through the
    // runtime's block-aware layout. Returns the sampled view (null on failure). `outTexture` receives
    // the owning texture for cleanup.
    rhi::TextureView* MakeSolidBcTexture(rhi::Device& device, rhi::TextureFormat format, u8 r, u8 g,
                                         u8 b, rhi::Texture*& outTexture)
    {
        outTexture = nullptr;
        Array<u8> src;
        src.Resize(static_cast<usize>(kTexSize) * kTexSize * 4);
        for (usize i = 0; i < static_cast<usize>(kTexSize) * kTexSize; ++i)
        {
            src[i * 4 + 0] = r;
            src[i * 4 + 1] = g;
            src[i * 4 + 2] = b;
            src[i * 4 + 3] = 255;
        }
        Array<byte> blocks;
        if (format == rhi::TextureFormat::BC6HRGBUfloat)
        {
            // HDR leg: the same solid colour as linear radiance through the BC6H encoder.
            Array<f32> hdr;
            hdr.Resize(static_cast<usize>(kTexSize) * kTexSize * 4);
            for (usize i = 0; i < static_cast<usize>(kTexSize) * kTexSize; ++i)
            {
                hdr[i * 4 + 0] = static_cast<f32>(r) / 255.0f;
                hdr[i * 4 + 1] = static_cast<f32>(g) / 255.0f;
                hdr[i * 4 + 2] = static_cast<f32>(b) / 255.0f;
                hdr[i * 4 + 3] = 1.0f;
            }
            blocks = texcomp::EncodeBlockCompressedHdr(hdr.Data(), kTexSize, kTexSize, 255);
        }
        else
        {
            blocks = texcomp::EncodeBlockCompressed(src.Data(), kTexSize, kTexSize, format, 255);
        }
        if (blocks.Size() != rhi::CompressedLevelBytes(format, kTexSize, kTexSize))
        {
            return nullptr;
        }

        rhi::TextureDesc td{};
        td.format = format;
        td.width = kTexSize;
        td.height = kTexSize;
        td.mipLevelCount = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"bc.probe.source";
        rhi::Texture* texture = nullptr;
        if (!device.CreateTexture(td, texture).IsOk())
        {
            return nullptr;
        }

        // Upload exactly as TextureResource's factory does: block-row pitch + block rows.
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics, 0);
        rhi::TransferBatch* batch = nullptr;
        if (queue == nullptr || !queue->CreateTransferBatch(batch).IsOk() || batch == nullptr)
        {
            device.DestroyTexture(texture);
            return nullptr;
        }
        rhi::TextureDataLayout layout{};
        layout.bytesPerRow = rhi::CompressedRowPitch(format, kTexSize);
        layout.rowsPerImage = (kTexSize + rhi::BlockHeight(format) - 1) / rhi::BlockHeight(format);
        batch->WriteTexture(texture, Span<const u8>(reinterpret_cast<const u8*>(blocks.Data()),
                                                    blocks.Size()),
                            layout, rhi::Extent3D{kTexSize, kTexSize, 1}, /*mip*/ 0, /*layer*/ 0);
        (void)batch->Submit();
        queue->DestroyTransferBatch(batch);

        rhi::TextureViewDesc vd{};
        vd.format = format;
        vd.dimension = rhi::TextureViewDimension::Texture2D;
        rhi::TextureView* view = nullptr;
        if (!device.CreateTextureView(texture, vd, view).IsOk())
        {
            device.DestroyTexture(texture);
            return nullptr;
        }
        outTexture = texture;
        return view;
    }

    // Render a screen-filling unlit cube textured with `albedo` (BaseColor white so the sampled
    // texel passes straight through) and read the LDR back.
    testsupport::CapturedImage RenderTexturedCube(rhi::Device& device, rhi::TextureView* albedo)
    {
        testsupport::CapturedImage out;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return out;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            TonemapPass tonemap(device, shaderSystem, 2);
            REQUIRE(tonemap.Initialize().IsOk());
            RenderFrame frame(DefaultAllocator(), device, registry, 2, nullptr, &tonemap);

            RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(DefaultAllocator(), 3.2f);
            RefPtr<materials::Material> mat =
                materials::CreateUnlit(u8"bc.probe", Float4{1, 1, 1, 1});
            mat->SetDefaultTexture(u8"AlbedoMap", albedo);

            ExtractedScene scene{DefaultAllocator()};
            MeshRenderData* cube = scene.Add<MeshRenderData>();
            cube->world = Float4x4::RotationY(0.3f) * Float4x4::RotationX(0.2f);
            cube->worldCenter = Float3{0, 0, 0};
            cube->mesh = cubeMesh.Get();
            cube->material = mat.Get();
            cube->category = RenderCategories::Opaque;

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 0, 4}, Float3{0, 0, 0}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"bc.probe.target";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());

            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.bloomEnabled = false;

            for (u32 i = 0; i < 2; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
                              kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
            }

            out = testsupport::Readback(device, target, kSize, kSize);

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }

    // The brightest textured pixel (the cube face most head-on): its channels should match the
    // decoded BC texel. Returns that pixel's RGB.
    // An 8x8 RGBA8 texture uploaded with a PADDED row pitch: 32 bytes of red texels then 32
    // bytes of blue padding per row. A backend that ignores TextureDataLayout::bytesPerRow reads
    // the padding as the next row's texels and the texture comes out half blue.
    rhi::TextureView* MakePaddedRgba8Texture(rhi::Device& device, rhi::Texture*& outTexture)
    {
        outTexture = nullptr;
        constexpr u32 kEdge = 8;
        constexpr u32 kBytesPerRow = kEdge * 4 + 32; // 32 bytes of padding per row
        Array<u8> src;
        src.Resize(static_cast<usize>(kBytesPerRow) * kEdge);
        for (u32 y = 0; y < kEdge; ++y)
        {
            u8* row = src.Data() + static_cast<usize>(y) * kBytesPerRow;
            for (u32 x = 0; x < kBytesPerRow / 4; ++x)
            {
                const bool padding = x >= kEdge;
                row[x * 4 + 0] = padding ? 20 : 230;
                row[x * 4 + 1] = 20;
                row[x * 4 + 2] = padding ? 230 : 20;
                row[x * 4 + 3] = 255;
            }
        }
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = kEdge;
        td.height = kEdge;
        td.mipLevelCount = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"padded.probe.source";
        rhi::Texture* texture = nullptr;
        if (!device.CreateTexture(td, texture).IsOk())
        {
            return nullptr;
        }
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics, 0);
        rhi::TransferBatch* batch = nullptr;
        if (queue == nullptr || !queue->CreateTransferBatch(batch).IsOk() || batch == nullptr)
        {
            device.DestroyTexture(texture);
            return nullptr;
        }
        rhi::TextureDataLayout layout{};
        layout.bytesPerRow = kBytesPerRow;
        layout.rowsPerImage = kEdge;
        batch->WriteTexture(texture, Span<const u8>(src.Data(), src.Size()), layout,
                            rhi::Extent3D{kEdge, kEdge, 1}, /*mip*/ 0, /*layer*/ 0);
        const Status submitted = batch->Submit();
        queue->DestroyTransferBatch(batch);
        if (!submitted.IsOk())
        {
            device.DestroyTexture(texture);
            return nullptr;
        }
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8Unorm;
        rhi::TextureView* view = nullptr;
        if (!device.CreateTextureView(texture, vd, view).IsOk())
        {
            device.DestroyTexture(texture);
            return nullptr;
        }
        outTexture = texture;
        return view;
    }

    // Lit pixels whose given channel is the clear maximum (the cube's faces show the texture;
    // the background is black and never counts).
    u32 CountDominant(const testsupport::CapturedImage& img, int channel)
    {
        u32 count = 0;
        for (u32 y = 0; y < img.height; ++y)
        {
            for (u32 x = 0; x < img.width; ++x)
            {
                const u8* p = img.At(x, y);
                const u32 r = p[0], g = p[1], b = p[2];
                if (r + g + b < 90) continue; // background
                const u32 v = channel == 0 ? r : (channel == 1 ? g : b);
                const u32 other = channel == 0 ? Max(g, b) : (channel == 1 ? Max(r, b) : Max(r, g));
                if (v > other + 40) ++count;
            }
        }
        return count;
    }

    void BrightestRgb(const testsupport::CapturedImage& img, u32& outR, u32& outG, u32& outB)
    {
        u32 best = 0;
        outR = outG = outB = 0;
        for (u32 y = 0; y < img.height; ++y)
        {
            for (u32 x = 0; x < img.width; ++x)
            {
                const u32 luma = img.Luma(x, y);
                if (luma > best)
                {
                    best = luma;
                    const u8* p = img.At(x, y);
                    outR = p[0];
                    outG = p[1];
                    outB = p[2];
                }
            }
        }
    }

    // A format the device can actually sample (spec/driver capability). BC is desktop; ASTC is
    // mobile - desktop GPUs usually lack it, so the ASTC leg self-skips here rather than failing.
    bool SupportsSampled(rhi::Device& device, rhi::TextureFormat format)
    {
        return (device.GetFormatSupport(format) & rhi::FormatSupport::Texture) ==
               rhi::FormatSupport::Texture;
    }

    void ProbeBcSampling(rhi::Device& device, const char* backendName)
    {
        struct Case
        {
            const char* name;
            rhi::TextureFormat format;
            u8 r, g, b;
            int dominant; // 0=R, 1=G, 2=B
        };
        const Case cases[] = {
            {"BC1-red", rhi::TextureFormat::BC1RGBAUnorm, 230, 20, 20, 0},
            {"BC7-green", rhi::TextureFormat::BC7RGBAUnorm, 20, 220, 20, 1},
            {"BC5-normalRG", rhi::TextureFormat::BC5RGUnorm, 200, 40, 0, 0}, // BC5 keeps R,G; B=0
            {"ASTC-blue", rhi::TextureFormat::ASTC4x4Unorm, 20, 20, 225, 2}, // mobile-web family
            {"BC6H-red", rhi::TextureFormat::BC6HRGBUfloat, 230, 20, 20, 0},  // HDR (unsigned half)
        };
        for (const Case& c : cases)
        {
            if (!SupportsSampled(device, c.format))
            {
                // Expected for ASTC on desktop GPUs (a mobile family); the leg runs on capable HW.
                std::printf("[bc] %-8s %-12s format unsupported - skipped\n", backendName, c.name);
                continue;
            }
            rhi::Texture* tex = nullptr;
            rhi::TextureView* view = MakeSolidBcTexture(device, c.format, c.r, c.g, c.b, tex);
            REQUIRE(view != nullptr);

            const testsupport::CapturedImage img = RenderTexturedCube(device, view);
            REQUIRE(img.valid);
            u32 pr = 0, pg = 0, pb = 0;
            BrightestRgb(img, pr, pg, pb);
            std::printf("[bc] %-8s %-12s sampled rgb=(%u,%u,%u)\n", backendName, c.name, pr, pg, pb);
            INFO(backendName, " ", c.name, " sampled rgb=(", pr, ",", pg, ",", pb, ")");

            // The cube actually shows the texture (not black): the dominant channel is clearly lit.
            const u32 dom = c.dominant == 0 ? pr : (c.dominant == 1 ? pg : pb);
            CHECK(dom > 120);
            // ...and the dominant channel really dominates - the BC decode preserved the hue, so the
            // upload placed the right block bytes at the right pitch (a wrong pitch smears/garbles).
            if (c.dominant != 0) CHECK(dom > pr);
            if (c.dominant != 1) CHECK(dom > pg);
            if (c.dominant != 2) CHECK(dom > pb);

            device.DestroyTextureView(view);
            device.DestroyTexture(tex);
        }
    }

    void ProbePaddedUpload(rhi::Device& device, const char* backendName)
    {
        rhi::Texture* tex = nullptr;
        rhi::TextureView* view = MakePaddedRgba8Texture(device, tex);
        REQUIRE(view != nullptr);
        const testsupport::CapturedImage img = RenderTexturedCube(device, view);
        REQUIRE(img.valid);
        const u32 red = CountDominant(img, 0);
        const u32 blue = CountDominant(img, 2);
        std::printf("[padded] %-8s red-dominant=%u blue-dominant=%u\n", backendName, red, blue);
        INFO(backendName, " red=", red, " blue=", blue);
        CHECK(red > 0);
        // Padding never reaches the texture: no face pixel reads blue. (With the row pitch
        // ignored, half the texels are padding and the cube shows blue columns.)
        CHECK(blue == 0u);
        device.DestroyTextureView(view);
        device.DestroyTexture(tex);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        bool any = false;
#ifdef OPTION_HAS_VULKAN
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
#endif
#ifdef OPTION_HAS_WEBGPU
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
#endif
#ifdef OPTION_HAS_DX12
        rhi::Backend* dx12 = nullptr;
        (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
        if (rhi::Device* device = testsupport::MakeTestDevice(dx12))
        {
            probe(*device, "dx12");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("DX12 unavailable - skipped");
        }
        if (dx12 != nullptr)
        {
            dx12->Destroy();
        }
#endif
        if (!any)
        {
            MESSAGE("no GPU backend available - BC texture probe skipped entirely");
        }
    }
}

TEST_CASE("bc-texture: block-compressed textures upload + sample correctly on real backends")
{
    ForEachBackend(&ProbeBcSampling);
}

TEST_CASE("bc-texture: a padded row pitch uploads without its padding becoming pixels")
{
    // Found by the Beef port: the Vulkan transfer batch stored TextureDataLayout and never
    // passed it to VkBufferImageCopy, so a padded upload copied its padding as texels.
    ForEachBackend(&ProbePaddedUpload);
}


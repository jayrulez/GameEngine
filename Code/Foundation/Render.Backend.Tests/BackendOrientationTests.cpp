// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Cross-backend orientation probe: render ONE asymmetric scene (a bright cube in the TOP half
// over a dim ground plane in the bottom) through the FULL RenderFrame chain - forward + tonemap,
// with and without the TAA resolve - on REAL Vulkan and WebGPU devices, read the pixels back,
// and assert each object lights its own half, on every backend, matching Vulkan. This is the
// pixel-level ground truth for the Y-flip bug class: every regression in that class swaps the
// halves (or culls the plane), so it fails loudly here instead of in someone's eyes.
//
// Run it on the cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cstdio>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 128; // square target; bytesPerRow = 512 (already 256-aligned)

    struct Probe
    {
        bool valid = false;
        f64 topLuma = 0.0;    // summed rgb over the top half's rows
        f64 bottomLuma = 0.0; // summed rgb over the bottom half's rows
    };

    struct ProbeConfig
    {
        bool taaEnabled = false;
        bool useTonemap = true; // false = raw forward straight into the LDR target
        bool includeCube = true;
        bool includePlane = true;
    };

    // Render the probe scene on `device` and read the final LDR pixels back.
    Probe RenderProbe(rhi::Device& device, const ProbeConfig& cfg)
    {
        Probe probe;

        // ShaderSystemHost: dev DXC/SPIR-V by default; OPTION_USE_SHADER_PACK=1 (+
        // ENV_WEBGPU_WGSL=1) runs the probe on the cooked pack - i.e. the BROWSER'S
        // WGSL shader path on wgpu-native, so an ingestion-convention divergence between
        // the SPIR-V and WGSL frontends shows up right here, locally.
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return probe;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();

            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem,
                                      /*framesInFlight*/ 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);

            TonemapPass tonemap(device, shaderSystem, /*framesInFlight*/ 2);
            REQUIRE(tonemap.Initialize().IsOk());
            TaaPass taa(device, shaderSystem);
            REQUIRE(taa.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2,
                              /*clusters*/ nullptr, cfg.useTonemap ? &tonemap : nullptr,
                              /*shadows*/ nullptr,
                              /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              cfg.taaEnabled ? &taa : nullptr,
                              /*ao*/ nullptr, /*fxaa*/ nullptr);

            // The asymmetric scene: bright white cube ABOVE eye level (projects into the TOP
            // half), dim wide ground plane below. Flat bright ambient - no lights needed.
            RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(DefaultAllocator(), 1.6f);
            RefPtr<geometry::StaticMesh> planeMesh = geometry::Primitives::Plane(DefaultAllocator(), 24.0f, 24.0f);
            RefPtr<materials::Material> cubeMat =
                materials::CreatePBR(u8"probe.cube", Float4{1, 1, 1, 1}, 0.0f, 0.6f);
            RefPtr<materials::Material> planeMat =
                // 18% grey (linear 0.18), written as the sRGB colour it is entered as.
                materials::CreatePBR(u8"probe.plane", Float4{0.461f, 0.461f, 0.461f, 1}, 0.0f, 0.8f);
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            if (cfg.includeCube)
            {
                MeshRenderData* cube = scene.Add<MeshRenderData>();
                cube->world = Float4x4::Translation(Float3{0.0f, 2.2f, 0.0f});
                cube->worldCenter = Float3{0.0f, 2.2f, 0.0f};
                cube->mesh = cubeMesh.Get();
                cube->material = cubeMat.Get();
                cube->category = RenderCategories::Opaque;
            }
            if (cfg.includePlane)
            {
                MeshRenderData* plane = scene.Add<MeshRenderData>();
                plane->world = Float4x4::Translation(Float3{0.0f, -1.0f, 0.0f});
                plane->worldCenter = Float3{0.0f, -1.0f, 0.0f};
                plane->mesh = planeMesh.Get();
                plane->material = planeMat.Get();
                plane->category = RenderCategories::Opaque;
            }

            ViewCamera camera;
            camera.view =
                Float4x4::LookAtRH(Float3{0, 0.5f, 7}, Float3{0, 0.5f, 0}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

            // Offscreen LDR target the chain resolves into; left in CopySrc for the readback.
            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"probe.target";
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
            // The TAA resolve is gated on the per-view post CONFIG, not just the pass pointer.
            settings.post.taaEnabled = cfg.taaEnabled;
            settings.post.bloomEnabled = false;

            // A few frames (TAA warms its history on a static camera), then read back.
            const u32 frames = cfg.taaEnabled ? 8u : 2u;
            for (u32 i = 0; i < frames; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView,
                              rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
            }

            // Target is left in CopySrc; the shared substrate does the copy + map + unpack.
            const testsupport::CapturedImage img = testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(img.valid);
            for (u32 y = 0; y < kSize; ++y)
            {
                f64 row = 0.0;
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    row += p[0] + p[1] + p[2];
                }
                (y < kSize / 2 ? probe.topLuma : probe.bottomLuma) += row;
            }
            probe.valid = true;

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return probe;
    }

    rhi::Device* MakeDevice(rhi::Backend* backend) { return testsupport::MakeTestDevice(backend); }
}

TEST_CASE("orientation: WebGPU matches Vulkan at every stage, cube on top, plane visible")
{
    // Vulkan is the reference; WebGPU must agree at every stage of the chain. Each historical
    // regression in the Y-flip class shows up here as a loud, specific failure:
    //  - mirrored scene content (naga cook missing --keep-coordinate-space): cube drops to the
    //    bottom half on raw-forward;
    //  - TAA-off whole-image flip (tonemap-side compensation firing wrongly): cube drops on
    //    "tonemap" but not "tonemap+taa" (or vice versa);
    //  - front-face winding hacks: the plane-only probe goes black (culled).
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());

    const struct
    {
        const char* name;
        ProbeConfig cfg;
    } runs[] = {
        {"raw-forward", {.taaEnabled = false, .useTonemap = false}},
        {"tonemap", {.taaEnabled = false, .useTonemap = true}},
        {"tonemap+taa", {.taaEnabled = true, .useTonemap = true}},
        {"plane-only-raw", {.taaEnabled = false, .useTonemap = false, .includeCube = false}},
        {"cube-only-raw", {.taaEnabled = false, .useTonemap = false, .includePlane = false}},
    };
    constexpr usize kRunCount = sizeof(runs) / sizeof(runs[0]);
    Probe vkProbes[kRunCount];
    Probe wgProbes[kRunCount];

    const auto probeAll = [&runs](rhi::Device& device, const char* backendName, Probe* out)
    {
        for (usize i = 0; i < kRunCount; ++i)
        {
            out[i] = RenderProbe(device, runs[i].cfg);
            std::printf("[probe] %-8s %-16s top=%.0f bottom=%.0f\n", backendName, runs[i].name,
                        out[i].topLuma, out[i].bottomLuma);
            INFO(backendName, " ", runs[i].name, ": top=", out[i].topLuma,
                 " bottom=", out[i].bottomLuma);
            REQUIRE(out[i].valid);
            if (runs[i].cfg.includeCube && runs[i].cfg.includePlane)
            {
                // Combined scene: the small bright cube lights the top half, the huge dim
                // plane sums to MORE in the bottom half (area beats brightness). A flip swaps
                // the halves - the plane's luma lands on top and dominance inverts.
                CHECK(out[i].topLuma > 100000.0);
                CHECK(out[i].bottomLuma > out[i].topLuma * 1.3);
            }
            else if (runs[i].cfg.includeCube)
            {
                // Cube only, above eye level: ALL its light is in the top half.
                CHECK(out[i].topLuma > 100000.0);
                CHECK(out[i].bottomLuma < out[i].topLuma * 0.05);
            }
            else
            {
                // Plane only: it must RENDER (a winding hack culls it -> black) and it must
                // land entirely in the bottom half (a flip mirrors it up).
                CHECK(out[i].bottomLuma > 100000.0);
                CHECK(out[i].topLuma < out[i].bottomLuma * 0.05);
            }
        }
    };

    bool haveVulkan = false;
    if (rhi::Device* device = vulkan != nullptr ? MakeDevice(vulkan) : nullptr)
    {
        probeAll(*device, "vulkan", vkProbes);
        haveVulkan = true;
        device->Destroy();
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan probes skipped");
    }

    bool haveWebGpu = false;
    if (rhi::Device* device = webgpu != nullptr ? MakeDevice(webgpu) : nullptr)
    {
        probeAll(*device, "webgpu", wgProbes);
        haveWebGpu = true;
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu probes skipped");
    }

    // Cross-backend agreement: the halves match Vulkan within a small tolerance (identical on a
    // shared GPU; the slack absorbs driver/compiler rounding on other hardware, while any flip
    // or cull regression is a >100% divergence).
    if (haveVulkan && haveWebGpu)
    {
        for (usize i = 0; i < kRunCount; ++i)
        {
            INFO("cross-backend ", runs[i].name, ": vulkan top=", vkProbes[i].topLuma,
                 " bottom=", vkProbes[i].bottomLuma, " webgpu top=", wgProbes[i].topLuma,
                 " bottom=", wgProbes[i].bottomLuma);
            CHECK(wgProbes[i].topLuma == doctest::Approx(vkProbes[i].topLuma).epsilon(0.05));
            CHECK(wgProbes[i].bottomLuma ==
                  doctest::Approx(vkProbes[i].bottomLuma).epsilon(0.05));
        }
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

// The per-frame rings on an EMULATED mapping (WebGPU keeps a CPU shadow and compares it
// against the last upload on Unmap). Before the lazy map + ranged flush, every ring paid a
// full-buffer compare per frame whether or not anything wrote it - ~145 MB per frame in a
// scene using none of the skinning/terrain/sprite/particle rings. Pinned through the
// buffer's upload counter: an untouched ring never uploads, a written ring uploads exactly its
// window once per frame, and an unchanged re-write is skipped by the ranged compare.
TEST_CASE("rings: an untouched DynamicUniformRing never flushes; a used one flushes its window")
{
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
    rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("WebGPU unavailable - ring flush probe skipped");
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
        return;
    }
    {
        // 2 frames x 1024 slots x 256 B = 512 KB: big enough that a whole-buffer compare would
        // be the wrong shape, small enough for a probe.
        DynamicUniformRing ring(*device, 2, 256, rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                                u8"probe.ring");
        REQUIRE(ring.Reserve(1024));
        auto* buffer = static_cast<rhi::webgpu::WebGpuBuffer*>(ring.Buffer());
        REQUIRE(buffer != nullptr);

        // Ten idle frames: nothing maps, nothing uploads.
        for (u32 f = 0; f < 10; ++f)
        {
            ring.BeginFrame(f);
            CHECK_FALSE(ring.IsMappedThisFrame());
            ring.EndFrame();
        }
        CHECK(buffer->UploadCount() == 0u);

        // One written slot per frame: exactly one upload per frame (the window), not a
        // whole-buffer compare deciding it.
        for (u32 f = 0; f < 4; ++f)
        {
            ring.BeginFrame(f);
            DynamicUniformRing::Range r = ring.Allocate();
            REQUIRE(r.ok);
            MemSet(r.ptr, static_cast<int>(0x10 + f), 256);
            ring.EndFrame();
            CHECK(buffer->UploadCount() == f + 1u);
        }

        // Re-writing a region with the bytes it already holds is a skipped upload. Two
        // frames in flight: frame 4 lands in region 0, which frame 2 last wrote (0x12).
        ring.BeginFrame(4);
        DynamicUniformRing::Range same = ring.Allocate();
        REQUIRE(same.ok);
        MemSet(same.ptr, 0x12, 256);
        ring.EndFrame();
        CHECK(buffer->UploadCount() == 4u);
        // ...and a real change to that region uploads again.
        ring.BeginFrame(6);
        DynamicUniformRing::Range changed = ring.Allocate();
        REQUIRE(changed.ok);
        MemSet(changed.ptr, 0x77, 256);
        ring.EndFrame();
        CHECK(buffer->UploadCount() == 5u);
        CHECK(!device->IsLost());
    }
    device->WaitIdle();
    device->Destroy();
    webgpu->Destroy();
}

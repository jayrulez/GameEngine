// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Scene-pass MSAA acceptance probes. All on real Vulkan + WebGPU via the shared
// RHI.TestSupport readback substrate; STRUCTURAL assertions (no golden images).
//   1) 4x resolve produces silhouette edge coverage that 1x does not (the core acceptance property).
//   2) MSAA composes with the post-effect stack (TAA/FXAA/AO/SSR) at 4x - each still renders a sane
//      lit image, guarding the resolve-rebind (those effects read the RESOLVED 1x buffers under MSAA).
//
// Cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
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
    constexpr u32 kSize = 128;

    struct MsaaConfig
    {
        u32 samples = 1;
        bool taa = false;
        bool fxaa = false;
        bool ssr = false;
        u32 aoMode = 0; // 0 = off, 1 = GTAO, 2 = SSAO
        // A large thin slab tilted so its top face crosses the whole view at a grazing angle
        // (a floor seen from low above it) instead of the small rotated cube: the flat-surface
        // AO probe. Thin, so the camera at z=6 is never inside it (a cube's back faces cull away).
        bool obliquePlane = false;
    };

    // Render a flat-lit rotated cube on black through the full RenderFrame chain (forward +
    // MsaaResolvePass + tonemap, plus any effects the config enables) and read the LDR back.
    testsupport::CapturedImage RenderMsaa(rhi::Device& device, const MsaaConfig& cfg)
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
            MsaaResolvePass msaaResolve(device, shaderSystem);
            REQUIRE(msaaResolve.Initialize().IsOk());
            TaaPass taa(device, shaderSystem);
            REQUIRE(taa.Initialize().IsOk());
            FxaaPass fxaa(device, shaderSystem, 2);
            REQUIRE(fxaa.Initialize().IsOk());
            AoPass ao(device, shaderSystem);
            REQUIRE(ao.Initialize().IsOk());
            SsrPass ssr(device, shaderSystem);
            REQUIRE(ssr.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap, /*shadows*/ nullptr,
                              /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              cfg.taa ? &taa : nullptr, cfg.aoMode != 0 ? &ao : nullptr,
                              cfg.fxaa ? &fxaa : nullptr);
            frame.SetMsaaResolve(&msaaResolve);
            if (cfg.ssr)
            {
                frame.SetSsr(&ssr);
                frame.SetSsrParams(true, SsrPass::Params{});
            }

            // A bright white cube, ROTATED so its silhouette is diagonal (long edges = a clear
            // coverage signal), on a black clear. Flat bright ambient - no lights needed.
            RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(DefaultAllocator(), 2.2f);
            RefPtr<materials::Material> cubeMat =
                materials::CreatePBR(u8"msaa.cube", Float4{1, 1, 1, 1}, 0.0f, 0.6f);
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            MeshRenderData* cube = scene.Add<MeshRenderData>();
            // Slab: 40 x 0.2 x 40, tilted 0.35 rad about X and pushed to z=-8, so the top face
            // spans the view with its normal ~70 degrees off the view direction (grazing).
            cube->world = cfg.obliquePlane
                              ? Float4x4::Scale(Float3{40.0f, 0.2f, 40.0f}) *
                                    Float4x4::RotationX(0.35f) * Float4x4::Translation(Float3{0, 0, -8})
                              : Float4x4::RotationY(0.6f) * Float4x4::RotationX(0.5f);
            cube->worldCenter = cfg.obliquePlane ? Float3{0.0f, 0.0f, -8.0f} : Float3{0.0f, 0.0f, 0.0f};
            cube->worldRadius = cfg.obliquePlane ? 30.0f : 2.0f;
            cube->mesh = cubeMesh.Get();
            cube->material = cubeMat.Get();
            cube->category = RenderCategories::Opaque;

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 0, 6}, Float3{0, 0, 0}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"msaa.probe.target";
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
            settings.post.taaEnabled = cfg.taa;
            settings.post.fxaaEnabled = cfg.fxaa;
            settings.post.ssrEnabled = cfg.ssr;
            settings.post.aoMode = cfg.aoMode;
            settings.post.needsMotion = cfg.taa || cfg.ssr; // TAA + (temporal) SSR read motion vectors
            settings.post.msaaSamples = static_cast<u8>(cfg.samples);

            // TAA warms its history over a few frames on a static camera; others need only one.
            const u32 frames = cfg.taa ? 8u : 2u;
            for (u32 i = 0; i < frames; ++i)
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

            // Target is left in CopySrc; the shared substrate does the copy + map.
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

    u32 MaxLuma(const testsupport::CapturedImage& img)
    {
        u32 m = 0;
        for (u32 y = 0; y < img.height; ++y)
        {
            for (u32 x = 0; x < img.width; ++x)
            {
                m = Max(m, img.Luma(x, y));
            }
        }
        return m;
    }

    // Partial-coverage edge pixels: luma strictly BETWEEN background (black) and the solid cube tone.
    // The band is relative to the image's own max luma (the flat-lit cube's tonemapped value), so it
    // isolates the silhouette FRINGE regardless of what the tonemap maps "white" to - the solid cube
    // (near max) and the background (near 0) are both excluded.
    u32 CountEdgeFringe(const testsupport::CapturedImage& img, u32 maxLuma)
    {
        const u32 lo = maxLuma * 15u / 100u;
        const u32 hi = maxLuma * 85u / 100u;
        return img.CountWhere(
            [lo, hi](const u8* p) -> bool
            {
                const u32 luma = static_cast<u32>(p[0]) + p[1] + p[2];
                return luma > lo && luma < hi;
            });
    }

    [[nodiscard]] bool Has4xMsaa(rhi::Device& device, const char* backendName)
    {
        if (device.MaxColorDepthSampleCount() < 4 || !device.SupportsSampleCount(4))
        {
            MESSAGE("device has no 4x MSAA - scene-pass probe skipped on ", backendName);
            return false;
        }
        return true;
    }

    void ProbeEdgeCoverage(rhi::Device& device, const char* backendName)
    {
        if (!Has4xMsaa(device, backendName))
        {
            return;
        }
        const testsupport::CapturedImage at1x = RenderMsaa(device, MsaaConfig{.samples = 1});
        const testsupport::CapturedImage at4x = RenderMsaa(device, MsaaConfig{.samples = 4});
        REQUIRE(at1x.valid);
        REQUIRE(at4x.valid);

        const u32 max1x = MaxLuma(at1x);
        const u32 max4x = MaxLuma(at4x);
        const u32 fringe1x = CountEdgeFringe(at1x, max1x);
        const u32 fringe4x = CountEdgeFringe(at4x, max4x);
        std::printf("[msaa] %-8s maxLuma 1x=%u 4x=%u | fringe 1x=%u 4x=%u\n", backendName, max1x,
                    max4x, fringe1x, fringe4x);
        INFO(backendName, ": maxLuma 1x=", max1x, " 4x=", max4x, " fringe 1x=", fringe1x,
             " 4x=", fringe4x);

        // The cube must actually render (a clearly-lit solid) at both counts.
        CHECK(max1x > 300);
        CHECK(max4x > 300);

        // The acceptance property: 4x fills the silhouette with partial-coverage pixels that the
        // hard-edged 1x image (each pixel fully cube or fully background) lacks.
        CHECK(fringe4x > fringe1x * 3);
        CHECK(fringe4x > 40);
    }

    void ProbeEffectStack(rhi::Device& device, const char* backendName)
    {
        if (!Has4xMsaa(device, backendName))
        {
            return;
        }
        const struct
        {
            const char* name;
            MsaaConfig cfg;
        } runs[] = {
            {"taa", {.samples = 4, .taa = true}},
            {"fxaa", {.samples = 4, .fxaa = true}},
            {"ao", {.samples = 4, .aoMode = 1}},
            {"ssr", {.samples = 4, .ssr = true}},
        };
        for (const auto& run : runs)
        {
            const testsupport::CapturedImage img = RenderMsaa(device, run.cfg);
            REQUIRE(img.valid);
            const u32 maxL = MaxLuma(img);
            const u32 lit =
                img.CountWhere([](const u8* p) { return static_cast<u32>(p[0]) + p[1] + p[2] > 100; });
            std::printf("[msaa+fx] %-8s %-4s maxLuma=%u lit=%u\n", backendName, run.name, maxL, lit);
            INFO(backendName, " 4x + ", run.name, ": maxLuma=", maxL, " lit=", lit);
            // The effect composed with the MSAA resolve without breaking: the cube is still a
            // clearly-lit solid covering a real area, and the values are sane (not garbage/overflow).
            CHECK(maxL > 300);
            CHECK(maxL <= 765);
            CHECK(lit > 200);
        }
    }

    u32 MeanLuma(const testsupport::CapturedImage& img, u32 x0, u32 y0, u32 x1, u32 y1)
    {
        u64 sum = 0;
        for (u32 y = y0; y < y1; ++y)
        {
            for (u32 x = x0; x < x1; ++x)
            {
                sum += img.Luma(x, y);
            }
        }
        return static_cast<u32>(sum / static_cast<u64>((x1 - x0) * (y1 - y0)));
    }

    // A flat surface has NO ambient occlusion: a floor seen at a grazing angle must come out as
    // bright with SSAO (and GTAO) on as with AO off. SSAO used to compare each sample's scene
    // depth against the CENTER pixel and draw its kernel from a sphere, so an oblique floor's own
    // depth gradient read as occlusion and moved with the camera (Bistro's ground, 2026-09-24).
    void ProbeFlatSurfaceAo(rhi::Device& device, const char* backendName)
    {
        const testsupport::CapturedImage off = RenderMsaa(device, MsaaConfig{.obliquePlane = true});
        REQUIRE(off.valid);
        const u32 base = MeanLuma(off, 32, 32, 96, 96);
        INFO(backendName, " flat surface, AO off: mean luma ", base);
        REQUIRE(base > 100); // the plane fills the probe region and is lit

        const struct
        {
            const char* name;
            u32 mode;
        } modes[] = {{"gtao", 1u}, {"ssao", 2u}};
        for (const auto& m : modes)
        {
            const testsupport::CapturedImage on =
                RenderMsaa(device, MsaaConfig{.aoMode = m.mode, .obliquePlane = true});
            REQUIRE(on.valid);
            const u32 lit = MeanLuma(on, 32, 32, 96, 96);
            std::printf("[ao-flat] %-8s %-5s off=%u on=%u\n", backendName, m.name, base, lit);
            INFO(backendName, " ", m.name, ": flat surface mean luma ", lit, " vs AO off ", base);
            // Within a few percent of the unoccluded surface: no self-occlusion on a plane.
            CHECK(lit * 100 >= base * 95);
        }
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
#ifdef OPTION_HAS_DX12
        rhi::Backend* dx12 = nullptr;
        (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
#endif

        bool any = false;
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
#ifdef OPTION_HAS_DX12
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
#endif
        if (!any)
        {
            MESSAGE("no GPU backend available - MSAA probe skipped entirely");
        }
        // Backends self-free via Destroy() (created even when no device came up).
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
#ifdef OPTION_HAS_DX12
        if (dx12 != nullptr)
        {
            dx12->Destroy();
        }
#endif
    }
}

TEST_CASE("msaa: 4x scene-pass resolve produces silhouette edge coverage that 1x does not")
{
    ForEachBackend(&ProbeEdgeCoverage);
}

TEST_CASE("msaa: composes with the post-effect stack (TAA/FXAA/AO/SSR) at 4x")
{
    ForEachBackend(&ProbeEffectStack);
}

TEST_CASE("ao: a flat surface at a grazing angle is not self-occluded by SSAO or GTAO")
{
    ForEachBackend(&ProbeFlatSurfaceAo);
}


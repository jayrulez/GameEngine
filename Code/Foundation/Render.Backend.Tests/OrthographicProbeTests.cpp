// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ambient occlusion, TAA, SSR and SSGI under an orthographic camera, proven at the pixel on real Vulkan and
// WebGPU devices through the full RenderFrame chain (structural assertions, no golden images).
// Each is easy to write for a perspective eye only (a divide by view depth, a view vector to the
// eye, a perspective depth linearization or march), so these probes check an orthographic view
// gets the same result.
//  - AO: an inside corner seen through an orthographic camera takes the occlusion a nearly
//    orthographic perspective view of it takes, GTAO and SSAO both.
//  - SSR and SSGI: a mirror floor's reflection and a corner's bounce, as a nearly orthographic
//    perspective view of them shows.
//  - TAA: a still orthographic view settles on what it shows without TAA, its slanted edge
//    antialiased and nothing else changed: the jitter in the row an orthographic projection needs
//    (a constant shift, not one that grows with depth) and its linear depth in the disocclusion
//    test.
// Cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
#include <doctest/doctest.h>
#include "Core/Prelude.h"

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
    constexpr u32 kSize = 128;

    struct Item
    {
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        Float4x4 world = Float4x4::Identity();
        Float3 center = Float3{0, 0, 0};
        f32 radius = 1.0f;
    };

    struct ProbeScene
    {
        Array<Item> items;
        ViewCamera camera;
        AoMode ao = AoMode::Off;
        bool taa = false;
        bool ssr = false;
        bool ssgi = false;
    };

    // Render `spec` through the frame chain (forward + tonemap, plus AO or TAA when asked; no
    // bloom) and read the LDR pixels back.
    testsupport::CapturedImage Render(rhi::Device& device, const ProbeScene& spec)
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
            AoPass ao(device, shaderSystem);
            REQUIRE(ao.Initialize().IsOk());
            TaaPass taa(device, shaderSystem);
            REQUIRE(taa.Initialize().IsOk());
            SsrPass ssr(device, shaderSystem);
            REQUIRE(ssr.Initialize().IsOk());
            SsgiPass ssgi(device, shaderSystem);
            REQUIRE(ssgi.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap,
                              /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              spec.taa ? &taa : nullptr, &ao, /*fxaa*/ nullptr);
            if (spec.ssr)
            {
                SsrPass::Params params;
                params.temporal = false; // one frame's trace, not an accumulation still settling
                frame.SetSsr(&ssr);
                frame.SetSsrParams(true, params);
            }
            if (spec.ssgi)
            {
                frame.SetSsgi(&ssgi);
            }

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            for (const Item& item : spec.items)
            {
                MeshRenderData* data = scene.Add<MeshRenderData>();
                data->world = item.world;
                data->worldCenter = item.center;
                data->worldRadius = item.radius;
                data->mesh = item.mesh.Get();
                data->material = item.material.Get();
                data->category = RenderCategories::Opaque;
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"orthographic.probe.target";
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
            settings.post.aoMode = static_cast<u32>(spec.ao);
            settings.post.aoStrength = 1.0f;
            settings.post.taaEnabled = spec.taa;
            settings.post.ssrEnabled = spec.ssr;
            settings.post.ssgiEnabled = spec.ssgi;
            settings.post.needsMotion = spec.taa || spec.ssgi; // SSGI's history reprojects too

            // TAA and SSGI settle over their history on a still camera.
            const u32 frames = (spec.taa || spec.ssgi) ? 24u : 2u;
            for (u32 i = 0; i < frames; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, spec.camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
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

    // Two grey slabs meeting at a vertical edge (the corner of x >= 0, z >= 0), seen from inside
    // the corner along its bisector through an orthographic camera 4 m tall and 30 m back: the edge
    // stands at the screen's centre column, a wall to each side. Flat white ambient, no lights.
    ProbeScene Corner(AoMode ao, bool taa, bool perspective = false, bool coloured = false)
    {
        ProbeScene spec;
        spec.ao = ao;
        spec.taa = taa;
        const Float4 grey{0.5f, 0.5f, 0.5f, 1.0f};
        const Float4 red{0.8f, 0.05f, 0.05f, 1.0f};
        const Float4 blue{0.05f, 0.05f, 0.8f, 1.0f};
        Item x;
        x.mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        x.material = materials::CreatePBR(u8"probe.wallX", coloured ? red : grey, 0.0f, 0.9f);
        x.world = Float4x4::Scale(Float3{0.1f, 8.0f, 8.0f}) * Float4x4::Translation(Float3{-0.05f, 0.0f, 4.0f});
        x.center = Float3{0.0f, 0.0f, 4.0f};
        x.radius = 6.0f;
        spec.items.PushBack(x);
        Item z;
        z.mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        z.material = materials::CreatePBR(u8"probe.wallZ", coloured ? blue : grey, 0.0f, 0.9f);
        z.world = Float4x4::Scale(Float3{8.0f, 8.0f, 0.1f}) * Float4x4::Translation(Float3{4.0f, 0.0f, -0.05f});
        z.center = Float3{4.0f, 0.0f, 0.0f};
        z.radius = 6.0f;
        spec.items.PushBack(z);
        const Float3 along = Normalized(Float3{1.0f, 0.0f, 1.0f});
        spec.camera.position = along * 30.0f;
        // Rolled a little when coloured, so the edge between the walls runs slanted across the
        // pixel centres (a pixel-aligned edge gives a half-pixel jitter nothing to antialias).
        const Float3 up = coloured ? Normalized(Float3{0.12f, 1.0f, -0.12f}) : Float3{0, 1, 0};
        spec.camera.view = Float4x4::LookAtRH(spec.camera.position, Float3{0, 0, 0}, up);
        spec.camera.projection = Float4x4::OrthographicRH(4.0f, 4.0f, 0.1f, 100.0f);
        if (perspective)
        {
            // The same 4 m at the edge, seen from 30 m: about 7.6 degrees.
            spec.camera.projection = Float4x4::PerspectiveFovRH(2.0f * Atan2(2.0f, 30.0f), 1.0f, 0.1f, 100.0f);
        }
        return spec;
    }

    // A mirror floor and a red pillar 8 m tall twenty metres off, seen from 60 m back, tipped 20
    // degrees down so the floor shows: through an orthographic camera 16 m tall, or a perspective
    // one that sees about the same at the pillar (about 15 degrees). The floor is dark, so what reads
    // red below the pillar is its reflection.
    ProbeScene PillarOverMirror(bool ssr, bool perspective)
    {
        ProbeScene spec;
        spec.ssr = ssr;
        Item floor;
        floor.mesh = geometry::Primitives::Plane(DefaultAllocator(), 200.0f, 200.0f);
        floor.material = materials::CreatePBR(u8"probe.mirror", Float4{0.02f, 0.02f, 0.02f, 1.0f}, 0.0f, 0.02f);
        floor.radius = 150.0f;
        spec.items.PushBack(floor);
        Item pillar;
        pillar.mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        pillar.material = materials::CreatePBR(u8"probe.pillar", Float4{1.0f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f);
        pillar.world = Float4x4::Scale(Float3{2.0f, 8.0f, 2.0f}) * Float4x4::Translation(Float3{0.0f, 4.0f, -20.0f});
        pillar.center = Float3{0.0f, 4.0f, -20.0f};
        pillar.radius = 5.0f;
        spec.items.PushBack(pillar);
        const Float3 target{0.0f, 0.0f, -20.0f};
        const f32 pitch = 20.0f * 3.14159265f / 180.0f;
        spec.camera.position = target + Float3{0.0f, Sin(pitch), Cos(pitch)} * 60.0f;
        spec.camera.view = Float4x4::LookAtRH(spec.camera.position, target, Float3{0, 1, 0});
        spec.camera.projection = perspective
                                     ? Float4x4::PerspectiveFovRH(2.0f * Atan2(8.0f, 60.0f), 1.0f, 0.1f, 200.0f)
                                     : Float4x4::OrthographicRH(16.0f, 16.0f, 0.1f, 200.0f);
        return spec;
    }

    bool Reddish(const u8* p) { return p[0] > 60 && p[0] > p[1] * 2 && p[0] > p[2] * 2; }

    // Red pixels in the centre columns (where the pillar and its reflection stand).
    u32 RedInCentre(const testsupport::CapturedImage& image)
    {
        u32 count = 0;
        for (u32 x = kSize / 2 - 2; x < kSize / 2 + 2; ++x)
        {
            for (u32 y = 0; y < image.height; ++y)
            {
                count += Reddish(image.At(x, y)) ? 1u : 0u;
            }
        }
        return count;
    }

    // Mean r+g+b over columns [x0, x1), every row.
    f32 ColumnsMean(const testsupport::CapturedImage& image, u32 x0, u32 x1)
    {
        f64 sum = 0.0;
        u32 n = 0;
        for (u32 x = x0; x < x1; ++x)
        {
            for (u32 y = 0; y < image.height; ++y)
            {
                const u8* p = image.At(x, y);
                sum += p[0] + p[1] + p[2];
                ++n;
            }
        }
        return n > 0 ? static_cast<f32>(sum / n) : 0.0f;
    }

    // How much AO takes off the edge and the open wall: on over off.
    struct AoRatios
    {
        f32 edge = 0.0f;
        f32 open = 0.0f;
        f32 edgeOff = 0.0f;
    };
    AoRatios MeasureAo(rhi::Device& device, AoMode mode, bool perspective)
    {
        const testsupport::CapturedImage off = Render(device, Corner(AoMode::Off, false, perspective));
        const testsupport::CapturedImage on = Render(device, Corner(mode, false, perspective));
        REQUIRE(off.valid);
        REQUIRE(on.valid);
        const u32 edge = kSize / 2;
        AoRatios r;
        r.edgeOff = ColumnsMean(off, edge - 4, edge + 4);
        r.edge = ColumnsMean(on, edge - 4, edge + 4) / Max(r.edgeOff, 1.0f);
        // The open wall, well over an AO radius (0.5 m, about 11 pixels here) from the edge.
        const f32 openOff = ColumnsMean(off, 4, edge - 32) + ColumnsMean(off, edge + 32, kSize - 4);
        const f32 openOn = ColumnsMean(on, 4, edge - 32) + ColumnsMean(on, edge + 32, kSize - 4);
        r.open = openOn / Max(openOff, 1.0f);
        return r;
    }

    // The orthographic view against a perspective one that is nearly orthographic (the same 4 m
    // across at the edge, seen from 30 m: 7.6 degrees): the same occlusion. Passes that divide an
    // orthographic view's x and y by its 30 m depth put their samples in the wrong place (SSAO) or a
    // pixel away (GTAO's radius).
    void ProbeAo(rhi::Device& device, const char* backend, AoMode mode, const char* name)
    {
        CAPTURE(backend);
        CAPTURE(name);
        const AoRatios ortho = MeasureAo(device, mode, false);
        const AoRatios reference = MeasureAo(device, mode, true);
        MESSAGE(doctest::String(backend) << " " << doctest::String(name) << ": AO on/off at the edge " << ortho.edge
                                         << " (perspective " << reference.edge << "), on the open wall "
                                         << ortho.open << " (perspective " << reference.open << ")");
        REQUIRE(ortho.edgeOff > 100.0f); // the walls are where we look
        CHECK(ortho.edge < 0.99f);       // the corner occludes itself
        // Tight: the right projection matches to a few thousandths, while SSAO with its orthographic
        // flag off comes within 0.02, so a looser band would let that regression through.
        CHECK(Abs(ortho.edge - reference.edge) < 0.005f);
        CHECK(Abs(ortho.open - reference.open) < 0.06f);
    }

    // The red down the pillar's columns with SSR off and on: the reflection mirrors the pillar about
    // the floor, adding about as much again, as it does in the nearly orthographic perspective view.
    // A march that divides an orthographic view's x and y by its depth, and steps 1/depth along a ray
    // whose depth is linear on screen, misses it.
    void ProbeSsr(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const auto ratio = [&](bool perspective, u32& pillar)
        {
            const testsupport::CapturedImage off = Render(device, PillarOverMirror(false, perspective));
            const testsupport::CapturedImage on = Render(device, PillarOverMirror(true, perspective));
            REQUIRE(off.valid);
            REQUIRE(on.valid);
            pillar = RedInCentre(off);
            return static_cast<f32>(RedInCentre(on)) / static_cast<f32>(Max(pillar, 1u));
        };
        u32 orthoPillar = 0, perspectivePillar = 0;
        const f32 ortho = ratio(false, orthoPillar);
        const f32 reference = ratio(true, perspectivePillar);
        MESSAGE(doctest::String(backend) << ": red with SSR on over off, orthographic " << ortho << " (pillar "
                                         << orthoPillar << " px), perspective " << reference << " (pillar "
                                         << perspectivePillar << " px)");
        REQUIRE(orthoPillar > 40u); // the pillar is on screen
        CHECK(ortho > 1.5f);        // its reflection is there
        CHECK(Abs(ortho - reference) < 0.15f);
    }

    // The inside corner's edge with SSGI off and on: the rays there hit the other wall (half as
    // bright as the sky they hide), darkening it as much as in the nearly orthographic perspective
    // view.
    void ProbeSsgi(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const auto ratio = [&](bool perspective)
        {
            ProbeScene off = Corner(AoMode::Off, false, perspective);
            ProbeScene on = Corner(AoMode::Off, false, perspective);
            on.ssgi = true;
            const testsupport::CapturedImage a = Render(device, off);
            const testsupport::CapturedImage b = Render(device, on);
            REQUIRE(a.valid);
            REQUIRE(b.valid);
            const u32 edge = kSize / 2;
            return ColumnsMean(b, edge - 6, edge + 6) / Max(ColumnsMean(a, edge - 6, edge + 6), 1.0f);
        };
        const f32 ortho = ratio(false);
        const f32 reference = ratio(true);
        MESSAGE(doctest::String(backend) << ": corner edge with SSGI on over off, orthographic " << ortho
                                         << ", perspective " << reference);
        CHECK(ortho < 0.97f);
        CHECK(Abs(ortho - reference) < 0.03f);
    }

    void ProbeGtao(rhi::Device& device, const char* backend) { ProbeAo(device, backend, AoMode::GTAO, "gtao"); }
    void ProbeSsao(rhi::Device& device, const char* backend) { ProbeAo(device, backend, AoMode::SSAO, "ssao"); }

    void ProbeTaa(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        // A red wall and a blue one: the edge between them is what the jitter antialiases.
        const testsupport::CapturedImage still = Render(device, Corner(AoMode::Off, false, false, true));
        const testsupport::CapturedImage resolved = Render(device, Corner(AoMode::Off, true, false, true));
        REQUIRE(still.valid);
        REQUIRE(resolved.valid);
        f64 diff = 0.0;
        u32 lost = 0; // a pixel the history blacked out (a NaN, or a rejection gone wrong)
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const u8* a = still.At(x, y);
                const u8* b = resolved.At(x, y);
                const i32 la = a[0] + a[1] + a[2];
                const i32 lb = b[0] + b[1] + b[2];
                diff += Abs(a[0] - b[0]) + Abs(a[1] - b[1]) + Abs(a[2] - b[2]);
                lost += (la > 100 && lb < 20) ? 1u : 0u;
            }
        }
        const f32 meanDiff = static_cast<f32>(diff / (kSize * kSize));
        MESSAGE(doctest::String(backend) << ": orthographic TAA mean colour difference " << meanDiff
                                         << ", pixels lost " << lost);
        CHECK(lost == 0u);
        // The jittered samples average back to the still image but for the slanted edge, which
        // they antialias (so TAA ran). A jitter that grows with depth (row 2 under orthographic, 30 m
        // back: thirty times the shift) would smear the edge over many pixels.
        CHECK(meanDiff > 0.1f);
        CHECK(meanDiff < 4.0f);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
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
        if (!any)
        {
            MESSAGE("no GPU backend available - orthographic probe skipped entirely");
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
}

TEST_CASE("orthographic: GTAO occludes a corner as a nearly orthographic perspective view does")
{
    ForEachBackend(&ProbeGtao);
}

TEST_CASE("orthographic: SSAO occludes a corner as a nearly orthographic perspective view does")
{
    ForEachBackend(&ProbeSsao);
}

TEST_CASE("orthographic: SSR reflects a pillar in a mirror floor as a nearly orthographic perspective view does")
{
    ForEachBackend(&ProbeSsr);
}

TEST_CASE("orthographic: SSGI darkens a corner as a nearly orthographic perspective view does")
{
    ForEachBackend(&ProbeSsgi);
}

TEST_CASE("orthographic: a still view under TAA settles on what it shows without it")
{
    ForEachBackend(&ProbeTaa);
}

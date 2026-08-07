// Cross-backend orientation probe: render ONE asymmetric scene (a bright cube in the TOP half
// over a dim ground plane in the bottom) through the FULL RenderFrame chain - forward + tonemap,
// with and without the TAA resolve - on REAL Vulkan and WebGPU devices, read the pixels back,
// and assert each object lights its own half, on every backend, matching Vulkan. This is the
// pixel-level ground truth for the Y-flip bug class: every regression in that class swaps the
// halves (or culls the plane), so it fails loudly here instead of in someone's eyes.
//
// Run it on the cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   DRACONIC_USE_SHADER_PACK=1 DRACONIC_WEBGPU_WGSL=1 ./Draconic.Render.Backend.Tests
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

#include <cstdio>
#include <cstring>

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.vulkan;
import draconic.rhi.webgpu;
import draconic.geometry;
import draconic.materials;
import draconic.materials.pipelinecache;
import draconic.shaders;
import draconic.shaders.system;
import draconic.render;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::render;
namespace rhi = draconic::rhi;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace shaders = draconic::shaders;

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

        // ShaderSystemHost: dev DXC/SPIR-V by default; DRACONIC_USE_SHADER_PACK=1 (+
        // DRACONIC_WEBGPU_WGSL=1) runs the probe on the cooked pack - i.e. the BROWSER'S
        // WGSL shader path on wgpu-native, so an ingestion-convention divergence between
        // the SPIR-V and WGSL frontends shows up right here, locally.
        shaders::ShaderSystemHost host;
        if (!host.Initialize(device, StringView(reinterpret_cast<const char8_t*>(
                                         DRACONIC_ENGINE_SHADER_DIR))))
        {
            return probe;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();

            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(device, shaderSystem, psoCache, materialSystem,
                                      /*framesInFlight*/ 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);

            TonemapPass tonemap(device, shaderSystem, /*framesInFlight*/ 2);
            REQUIRE(tonemap.Initialize().IsOk());
            TaaPass taa(device, shaderSystem);
            REQUIRE(taa.Initialize().IsOk());

            RenderFrame frame(device, registry, /*framesInFlight*/ 2,
                              /*clusters*/ nullptr, cfg.useTonemap ? &tonemap : nullptr,
                              /*shadows*/ nullptr,
                              /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              cfg.taaEnabled ? &taa : nullptr,
                              /*ao*/ nullptr, /*fxaa*/ nullptr);

            // The asymmetric scene: bright white cube ABOVE eye level (projects into the TOP
            // half), dim wide ground plane below. Flat bright ambient - no lights needed.
            RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(1.6f);
            RefPtr<geometry::StaticMesh> planeMesh = geometry::Primitives::Plane(24.0f, 24.0f);
            RefPtr<materials::Material> cubeMat =
                materials::CreatePBR(u8"probe.cube", Float4{1, 1, 1, 1}, 0.0f, 0.6f);
            RefPtr<materials::Material> planeMat =
                materials::CreatePBR(u8"probe.plane", Float4{0.18f, 0.18f, 0.18f, 1}, 0.0f, 0.8f);
            ExtractedScene scene;
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

            rhi::BufferDesc rbd{};
            rbd.size = 512ull * kSize;
            rbd.usage = rhi::BufferUsage::CopyDst;
            rbd.memory = rhi::MemoryLocation::GpuToCpu;
            rhi::Buffer* readback = nullptr;
            REQUIRE(device.CreateBuffer(rbd, readback).IsOk());

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
                if (i == frames - 1)
                {
                    rhi::BufferTextureCopyRegion region;
                    region.bytesPerRow = 512;
                    region.rowsPerImage = kSize;
                    region.textureExtent = rhi::Extent3D{kSize, kSize, 1};
                    encoder->CopyTextureToBuffer(target, readback, region);
                }
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
            }

            const u8* pixels = static_cast<const u8*>(readback->Map());
            REQUIRE(pixels != nullptr);
            for (u32 y = 0; y < kSize; ++y)
            {
                f64 row = 0.0;
                const u8* p = pixels + static_cast<usize>(y) * 512;
                for (u32 x = 0; x < kSize; ++x)
                {
                    row += p[x * 4 + 0] + p[x * 4 + 1] + p[x * 4 + 2];
                }
                (y < kSize / 2 ? probe.topLuma : probe.bottomLuma) += row;
            }
            readback->Unmap();
            probe.valid = true;

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyBuffer(readback);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return probe;
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
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu);

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

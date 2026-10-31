// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The profiler's GPU lane on real devices, through the full RenderFrame:
// with the profiler armed, a frame's passes come back a frame or two later tagged with that
// frame, on the CPU's clock (inside the CPU span that recorded and waited for them), in order,
// under the names the CPU lane gives the same passes; where the device counts pipeline
// statistics, the passes drawing a cube count its twelve triangles. Vulkan aligns the clocks by
// calibrated timestamps, WebGPU by one sync point, and has no statistics.
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
import foundation.profiler;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;
namespace profiler = foundation::profiler;

namespace
{
    constexpr u32 kSize = 128;
    constexpr u32 kFrames = 8;

    struct Measure
    {
        u32 framesWithGpu = 0;   // frames 1..5 whose GPU passes arrived
        u32 passes = 0;          // GPU passes over those frames
        u32 outsideFrame = 0;    // passes not inside the CPU span of their frame
        u32 outOfOrder = 0;      // passes that began before the one before them ended
        u32 unnamedOnCpu = 0;    // GPU passes whose name no CPU scope of the frame has
        u32 withCounts = 0;      // passes carrying pipeline statistics
        u64 primitives = 0;      // input primitives over one frame's passes
        u64 fragments = 0;       // fragment invocations over one frame's passes
        u64 fewestDraws = ~0ull; // the device's counts over one frame (Device::Counters), fewest
        u64 fewestPasses = ~0ull;
        u64 targetBytes = 0;     // render-target memory held during the frames
    };

    Measure Run(rhi::Device& device)
    {
        Measure out;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return out;
        }
        profiler::Profiler& p = profiler::Profiler::Get();
        REQUIRE_FALSE(p.Armed());
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
            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap,
                              /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              /*taa*/ nullptr, /*ao*/ nullptr, /*fxaa*/ nullptr);

            RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
            RefPtr<materials::Material> grey =
                materials::CreatePBR(u8"gpuprofile.grey", Float4{0.6f, 0.6f, 0.6f, 1.0f}, 0.0f, 0.8f);
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.3f, 0.3f, 0.3f});
            MeshRenderData* box = scene.Add<MeshRenderData>();
            box->entityId = 1;
            box->world = Float4x4::Identity();
            box->worldRadius = 1.0f;
            box->mesh = cube.Get();
            box->material = grey.Get();
            box->category = RenderCategories::Opaque;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"gpuprofile.target";
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
            settings.post.taaEnabled = false;
            ViewCamera camera;
            camera.position = Float3{2.0f, 1.5f, 3.0f};
            camera.view = Float4x4::LookAtRH(camera.position, Float3{0, 0, 0}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

            // Each profiler frame records, submits and waits for its GPU work, so every GPU pass of
            // frame k lies inside frame k's CPU span.
            p.Arm();
            u64 firstFrame = 0;
            for (u32 i = 0; i < kFrames; ++i)
            {
                p.BeginFrame();
                if (i == 0)
                {
                    firstFrame = p.CurrentFrameNumber();
                }
                const rhi::DeviceCounters before = device.Counters();
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState = (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
                const rhi::DeviceCounters after = device.Counters();
                out.fewestDraws = Min(out.fewestDraws, after.draws - before.draws);
                out.fewestPasses = Min(out.fewestPasses, after.renderPasses - before.renderPasses);
                out.targetBytes = after.renderTargetBytes;
                p.EndFrame();
            }
            p.Disarm();

            // A slot's passes arrive when it comes round, two frames on: frames first..first+5 have
            // theirs by now. The clocks may disagree by a sync point's half round trip.
            const u64 slack = GetTickFrequency() / 500u; // 2 ms
            for (u64 k = firstFrame; k < firstFrame + kFrames - 2; ++k)
            {
                const profiler::FrameRecord* record = p.Frame(k);
                REQUIRE(record != nullptr);
                if (!record->gpuValid)
                {
                    continue;
                }
                ++out.framesWithGpu;
                u64 previousEnd = 0;
                u64 primitives = 0, fragments = 0;
                for (const profiler::GpuPassEvent& pass : record->gpu)
                {
                    ++out.passes;
                    if (pass.beginTick + slack < record->beginTick || pass.endTick > record->endTick + slack)
                    {
                        ++out.outsideFrame;
                    }
                    if (previousEnd != 0 && pass.beginTick + slack < previousEnd)
                    {
                        ++out.outOfOrder;
                    }
                    previousEnd = pass.endTick;
                    bool named = false;
                    for (const profiler::ThreadFrame& thread : record->threads)
                    {
                        for (const profiler::ScopeEvent& scope : thread.scopes)
                        {
                            named = named || scope.name == pass.name;
                        }
                    }
                    out.unnamedOnCpu += named ? 0u : 1u;
                    if (pass.hasCounts)
                    {
                        ++out.withCounts;
                        primitives += pass.counts.inputPrimitives;
                        fragments += pass.counts.fragmentInvocations;
                    }
                }
                out.primitives = Max(out.primitives, primitives);
                out.fragments = Max(out.fragments, fragments);
            }

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }

    void Probe(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        if (!device.features.timestampQueries)
        {
            MESSAGE(backend, ": no encoder timestamps - the GPU lane is absent, skipped");
            return;
        }
        const Measure m = Run(device);
        MESSAGE(doctest::String(backend) << ": " << m.framesWithGpu << " frames with GPU passes, "
                                         << m.passes << " passes, " << m.outsideFrame << " outside their frame, "
                                         << m.outOfOrder << " out of order, " << m.withCounts
                                         << " with counts; one frame: " << m.primitives << " primitives, "
                                         << m.fragments << " fragments; the device counts at least "
                                         << m.fewestDraws << " draws and " << m.fewestPasses
                                         << " render passes a frame, " << (m.targetBytes >> 10)
                                         << " KiB of render targets");
        CHECK(m.framesWithGpu == kFrames - 2);
        CHECK(m.passes >= m.framesWithGpu);
        CHECK(m.outsideFrame == 0u);
        CHECK(m.outOfOrder == 0u);
        CHECK(m.unnamedOnCpu == 0u);
        // The depth prepass and the forward pass draw the cube, the tonemap its triangle.
        CHECK(m.fewestDraws >= 3u);
        CHECK(m.fewestPasses >= 3u);
        CHECK(m.targetBytes >= kSize * kSize * 4u); // the probe's own target, at least
        if (device.features.pipelineStatisticsQueries)
        {
            CHECK(m.withCounts == m.passes);
            CHECK(m.primitives >= 12u); // the cube's twelve triangles, at least once
            CHECK(m.fragments > 0u);
        }
        else
        {
            CHECK(m.withCounts == 0u);
        }
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
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

#if BUILD_PROFILING
TEST_CASE("gpu profile: a frame's GPU passes come back tagged, in order, on the CPU's clock")
{
    ForEachBackend(&Probe);
}
#endif

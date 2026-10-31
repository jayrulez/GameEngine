// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The render graph's half of the profiler, without a GPU: on the Null
// device (no timestamps) a graph's passes show as CPU scopes of the frame and no GPU lane is
// made; the GPU path is probed on Vulkan and WebGPU in Render.Backend.Tests.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.profiler;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::rendergraph;
namespace rhi = foundation::rhi;
namespace profiler = foundation::profiler;

TEST_CASE("rg.profiler: a device without timestamps gets no GPU profiler, and nothing breaks")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    REQUIRE_FALSE(device.features.timestampQueries);
    GraphProfiler gpu(DefaultAllocator());
    CHECK(gpu.Init(device, 2).Code() == ErrorCode::NotSupported);
    rhi::CommandPool* pool = nullptr;
    REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
    rhi::CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    CHECK_FALSE(gpu.BeginFrame(*encoder, 0)); // never initialized: measures nothing
    gpu.BeginPass(*encoder, 0);
    gpu.EndPass(*encoder);
    gpu.EndFrame(*encoder);
    gpu.Destroy();
    device.DestroyCommandPool(pool);
}

TEST_CASE("rg.profiler: pass names are interned once and kept")
{
    ProfileNames names(DefaultAllocator());
    const profiler::NameId shadows = names.NameFor(StringView(u8"shadows"));
    CHECK(names.NameFor(StringView(u8"shadows")) == shadows);
    CHECK(names.NameFor(StringView(u8"forward")) != shadows);
    CHECK(profiler::Profiler::Get().InternName(StringView(u8"shadows")) == shadows);
}

#if BUILD_PROFILING
TEST_CASE("rg.profiler: while armed, each executed pass is a CPU scope of the frame")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::Texture* target = nullptr;
    REQUIRE(device.CreateTexture(rhi::TextureDesc::RenderTarget(rhi::TextureFormat::BGRA8Unorm, 16, 16), target).IsOk());
    rhi::TextureViewDesc vd{};
    vd.format = rhi::TextureFormat::BGRA8Unorm;
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(target, vd, view).IsOk());
    rhi::CommandPool* pool = nullptr;
    REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
    rhi::CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    profiler::Profiler& p = profiler::Profiler::Get();
    REQUIRE_FALSE(p.Armed());
    p.Arm();
    p.BeginFrame();
    {
        RenderGraph graph(DefaultAllocator(), &device);
        graph.SetOutputSize(16, 16);
        graph.BeginFrame(0);
        const RGHandle out = graph.ImportTarget(u8"Out", target, view, rhi::ResourceState::Present);
        graph.AddRenderPass(u8"rg.profiler.pass",
                            [&](PassBuilder& b)
                            {
                                b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                                b.NeverCull();
                                b.SetExecute([](rhi::RenderPassEncoder&) {});
                            });
        CHECK(graph.Execute(encoder).IsOk());
        graph.EndFrame();
    }
    p.EndFrame();
    p.Disarm();

    const profiler::FrameRecord* frame = p.LatestFrame();
    REQUIRE(frame != nullptr);
    usize found = 0;
    for (const profiler::ThreadFrame& thread : frame->threads)
    {
        for (const profiler::ScopeEvent& scope : thread.scopes)
        {
            found += p.NameOf(scope.name) == StringView(u8"rg.profiler.pass") ? 1u : 0u;
        }
    }
    CHECK(found == 1u);
    CHECK_FALSE(frame->gpuValid); // no timestamps on this device: no GPU lane

    device.DestroyCommandPool(pool);
    device.DestroyTextureView(view);
    device.DestroyTexture(target);
}
#endif

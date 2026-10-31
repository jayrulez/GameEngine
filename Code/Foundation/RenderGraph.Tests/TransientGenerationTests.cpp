// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Transient texture generation: each transient carries a stable id for its backing physical texture,
// surfaced via GetTextureGeneration. A bind-group cache over a transient's view keys on this (not the
// raw pointer) so a reused-address view cannot alias a stale, destroyed texture across a resize.
// Driven on the Null RHI so Execute actually allocates/returns the transient.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::rendergraph;
namespace rhi = foundation::rhi;

namespace
{
    struct Harness
    {
        rhi::null::NullDevice device{DefaultAllocator()};
        rhi::Texture* bb = nullptr;
        rhi::TextureView* bbView = nullptr;
        rhi::CommandPool* pool = nullptr;
        rhi::CommandEncoder* enc = nullptr;

        bool Init()
        {
            if (!device
                     .CreateTexture(
                         rhi::TextureDesc::RenderTarget(rhi::TextureFormat::BGRA8Unorm, 64, 64), bb)
                     .IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::BGRA8Unorm;
            if (!device.CreateTextureView(bb, vd, bbView).IsOk())
            {
                return false;
            }
            if (!device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk())
            {
                return false;
            }
            return pool->CreateEncoder(enc).IsOk();
        }
        ~Harness()
        {
            if (pool)
            {
                device.DestroyCommandPool(pool);
            }
            if (bbView)
            {
                device.DestroyTextureView(bbView);
            }
            if (bb)
            {
                device.DestroyTexture(bb);
            }
        }
    };

    // One frame: a transient HDR target written by one pass and read by a backbuffer pass (so it is
    // not culled). Captures the transient's generation and view inside the writing pass's execute.
    void RunFrame(RenderGraph& graph, Harness& h, i32 frameIndex, u64& outGen,
                  rhi::TextureView*& outView)
    {
        graph.SetOutputSize(64, 64);
        graph.BeginFrame(frameIndex);
        const RGHandle bbH =
            graph.ImportTarget(u8"BB", h.bb, h.bbView, rhi::ResourceState::Present);
        const RGHandle hdr =
            graph.CreateTransient(u8"HDR", RGTextureDesc(rhi::TextureFormat::RGBA16Float, 64, 64));
        graph.AddRenderPass(u8"WriteHDR",
                            [&](PassBuilder& b)
                            {
                                b.SetColorTarget(0, hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                                b.NeverCull();
                                b.SetExecute(
                                    [&](rhi::RenderPassEncoder&)
                                    {
                                        outGen = graph.GetTextureGeneration(hdr);
                                        outView = graph.GetTextureView(hdr);
                                    });
                            });
        graph.AddRenderPass(u8"ReadHDR",
                            [&](PassBuilder& b)
                            {
                                b.SetColorTarget(0, bbH, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                                b.ReadTexture(hdr);
                                b.NeverCull();
                                b.SetExecute([](rhi::RenderPassEncoder&) {});
                            });
        CHECK(graph.Execute(h.enc).IsOk());
        graph.EndFrame();
    }
}

TEST_CASE("rg.transient: generation is non-zero and stable across pool reuse")
{
    Harness h;
    REQUIRE(h.Init());
    RenderGraph graph(DefaultAllocator(), &h.device);

    u64 gen0 = 0, gen1 = 0;
    rhi::TextureView* v0 = nullptr;
    rhi::TextureView* v1 = nullptr;
    RunFrame(graph, h, 0, gen0, v0);
    RunFrame(graph, h, 1, gen1, v1);

    CHECK(gen0 != 0); // a freshly allocated transient gets a real id
    CHECK(v0 != nullptr);
    CHECK(gen1 == gen0); // same desc, so pool reuse, the same physical texture and a stable generation
    CHECK(v1 == v0);     // the same pooled view comes back (the case the cache optimizes for)
}

TEST_CASE("rg.transient: distinct transients get distinct generations")
{
    Harness h;
    REQUIRE(h.Init());
    RenderGraph graph(DefaultAllocator(), &h.device);
    graph.SetOutputSize(64, 64);
    graph.BeginFrame(0);

    const RGHandle bbH = graph.ImportTarget(u8"BB", h.bb, h.bbView, rhi::ResourceState::Present);
    const RGHandle a =
        graph.CreateTransient(u8"A", RGTextureDesc(rhi::TextureFormat::RGBA16Float, 64, 64));
    const RGHandle b =
        graph.CreateTransient(u8"B", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm, 32, 32));

    u64 genA = 0, genB = 0;
    graph.AddRenderPass(u8"PA",
                        [&](PassBuilder& pb)
                        {
                            pb.SetColorTarget(0, a, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            pb.NeverCull();
                            pb.SetExecute([&](rhi::RenderPassEncoder&)
                                          { genA = graph.GetTextureGeneration(a); });
                        });
    graph.AddRenderPass(u8"PB",
                        [&](PassBuilder& pb)
                        {
                            pb.SetColorTarget(0, b, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            pb.NeverCull();
                            pb.SetExecute([&](rhi::RenderPassEncoder&)
                                          { genB = graph.GetTextureGeneration(b); });
                        });
    graph.AddRenderPass(u8"Sink",
                        [&](PassBuilder& pb)
                        {
                            pb.SetColorTarget(0, bbH, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            pb.ReadTexture(a);
                            pb.ReadTexture(b);
                            pb.NeverCull();
                            pb.SetExecute([](rhi::RenderPassEncoder&) {});
                        });

    CHECK(graph.Execute(h.enc).IsOk());
    CHECK(genA != 0);
    CHECK(genB != 0);
    CHECK(genA != genB); // two distinct physical allocations, two distinct ids
}

namespace
{
    // The null encoder, logging each barrier into the same list the passes log their runs into.
    class OrderEncoder final : public rhi::null::NullCommandEncoder
    {
    public:
        explicit OrderEncoder(Array<String>& log) : m_log(&log) {}
        void Barrier(const rhi::BarrierGroup& group) override
        {
            for (usize i = 0; i < group.textureBarriers.Size(); ++i)
            {
                const rhi::TextureBarrier& b = group.textureBarriers[i];
                if (b.newState == rhi::ResourceState::ShaderRead &&
                    b.oldState == rhi::ResourceState::RenderTarget)
                {
                    m_log->PushBack(String(u8"target->shader-read"));
                }
            }
        }

    private:
        Array<String>* m_log;
    };
}

TEST_CASE("rg.graph: an imported target takes its final state right after its last pass")
{
    // A camera's render texture: written by one view, then sampled later in the same frame by a
    // pass that does not declare it (a sprite, a UI image). It must be shader-readable by then,
    // not only at the end of the graph.
    Harness h;
    REQUIRE(h.Init());
    rhi::Texture* rt = nullptr;
    rhi::TextureView* rtView = nullptr;
    REQUIRE(h.device
                .CreateTexture(rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8Unorm, 32, 32),
                               rt)
                .IsOk());
    REQUIRE(h.device.CreateTextureView(rt, rhi::TextureViewDesc{}, rtView).IsOk());

    Array<String> log;
    OrderEncoder encoder(log);
    RenderGraph graph(DefaultAllocator(), &h.device);
    graph.SetOutputSize(64, 64);
    graph.BeginFrame(0);
    const RGHandle target = graph.ImportTarget(u8"Minimap", rt, rtView,
                                               rhi::ResourceState::ShaderRead,
                                               rhi::ResourceState::ShaderRead);
    const RGHandle bb = graph.ImportTarget(u8"BB", h.bb, h.bbView, rhi::ResourceState::Present);
    graph.AddRenderPass(u8"DrawMinimap",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, target, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.SetExecute([&](rhi::RenderPassEncoder&)
                                         { log.PushBack(String(u8"draw minimap")); });
                        });
    graph.AddRenderPass(u8"DrawWorld",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, bb, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.SetExecute([&](rhi::RenderPassEncoder&)
                                         { log.PushBack(String(u8"draw world")); });
                        });
    CHECK(graph.Execute(&encoder).IsOk());
    graph.EndFrame();

    REQUIRE(log.Size() == 3u);
    CHECK(log[0] == StringView(u8"draw minimap"));
    CHECK(log[1] == StringView(u8"target->shader-read")); // before the next view samples it
    CHECK(log[2] == StringView(u8"draw world"));

    h.device.DestroyTextureView(rtView);
    h.device.DestroyTexture(rt);
}

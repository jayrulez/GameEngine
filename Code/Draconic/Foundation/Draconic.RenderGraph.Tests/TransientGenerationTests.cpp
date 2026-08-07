// Transient texture generation: each transient carries a stable id for its backing physical texture,
// surfaced via GetTextureGeneration. A bind-group cache over a transient's view keys on this (not the
// raw pointer) so a reused-address view can't alias a stale, destroyed texture across a resize.
// Driven on the Null RHI so Execute actually allocates/returns the transient.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

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

    // One frame: a transient HDR target written by one pass and read by a backbuffer pass (so it isn't
    // culled). Captures the transient's generation + view inside the writing pass's execute.
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
    RenderGraph graph(&h.device);

    u64 gen0 = 0, gen1 = 0;
    rhi::TextureView* v0 = nullptr;
    rhi::TextureView* v1 = nullptr;
    RunFrame(graph, h, 0, gen0, v0);
    RunFrame(graph, h, 1, gen1, v1);

    CHECK(gen0 != 0); // a freshly allocated transient gets a real id
    CHECK(v0 != nullptr);
    CHECK(gen1 == gen0); // same desc -> pool reuse -> SAME physical texture -> stable generation
    CHECK(v1 == v0);     // the same pooled view comes back (the case the cache optimizes for)
}

TEST_CASE("rg.transient: distinct transients get distinct generations")
{
    Harness h;
    REQUIRE(h.Init());
    RenderGraph graph(&h.device);
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
    CHECK(genA != genB); // two distinct physical allocations -> distinct ids
}

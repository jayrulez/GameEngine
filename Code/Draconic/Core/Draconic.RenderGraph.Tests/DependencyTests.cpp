// Ported from Sedulous.RenderGraph.Tests/DependencyTests.bf
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

namespace
{
    StringView PassName(RenderGraph& g, i32 orderSlot)
    {
        return g.Passes()[static_cast<usize>(g.ExecutionOrder()[static_cast<usize>(orderSlot)])]
            ->name.AsView();
    }
}

TEST_CASE("rg.dep: reader depends on writer")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Writer",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Reader",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 2u);
    CHECK(PassName(graph, 0) == u8"Writer");
}

TEST_CASE("rg.dep: multiple readers fan out, writer first")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Writer",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"ReaderA",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"ReaderB",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 3u);
    CHECK(PassName(graph, 0) == u8"Writer");
}

TEST_CASE("rg.dep: subresource writes are independent, reader last")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    RGTextureDesc atlasDesc(rhi::TextureFormat::Depth32Float);
    atlasDesc.arrayLayerCount = 4;
    const RGHandle atlas = graph.CreateTransient(u8"ShadowAtlas", atlasDesc);

    graph.AddRenderPass(u8"Cascade0",
                        [&](PassBuilder& b)
                        {
                            b.SetDepthTarget(atlas, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f,
                                             RGSubresourceRange{0, 1, 0, 1});
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Cascade1",
                        [&](PassBuilder& b)
                        {
                            b.SetDepthTarget(atlas, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f,
                                             RGSubresourceRange{0, 1, 1, 1});
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Forward",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(atlas);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 3u);
    CHECK(PassName(graph, 2) == u8"Forward");
}

TEST_CASE("rg.dep: LoadOp on a color target creates a dependency on the writer")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"SceneColor", RGTextureDesc(rhi::TextureFormat::RGBA16Float));

    graph.AddRenderPass(u8"ForwardOpaque",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Terrain",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Load, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 2u);
    CHECK(PassName(graph, 0) == u8"ForwardOpaque");
    CHECK(PassName(graph, 1) == u8"Terrain");
}

TEST_CASE("rg.dep: LoadOp on a depth target creates a dependency on the writer")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle depth =
        graph.CreateTransient(u8"Depth", RGTextureDesc(rhi::TextureFormat::Depth32Float));

    graph.AddRenderPass(u8"DepthPrepass",
                        [&](PassBuilder& b)
                        {
                            b.SetDepthTarget(depth, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"ForwardOpaque",
                        [&](PassBuilder& b)
                        {
                            b.SetDepthTarget(depth, rhi::LoadOp::Load, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 2u);
    CHECK(PassName(graph, 0) == u8"DepthPrepass");
    CHECK(PassName(graph, 1) == u8"ForwardOpaque");
}

TEST_CASE("rg.dep: writer chain orders correctly")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Write1",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddComputePass(u8"Process",
                         [&](PassBuilder& b)
                         {
                             b.ReadTexture(tex);
                             b.WriteStorage(tex);
                             b.NeverCull();
                         });
    graph.AddRenderPass(u8"FinalRead",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 3u);
    CHECK(PassName(graph, 0) == u8"Write1");
    CHECK(PassName(graph, 1) == u8"Process");
    CHECK(PassName(graph, 2) == u8"FinalRead");
}

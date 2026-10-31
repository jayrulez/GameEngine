// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.RenderGraph.Tests/DependencyTests.bf
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::rendergraph;
namespace rhi = foundation::rhi;

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
    RenderGraph graph(DefaultAllocator(), nullptr);
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
    RenderGraph graph(DefaultAllocator(), nullptr);
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
    RenderGraph graph(DefaultAllocator(), nullptr);
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
    RenderGraph graph(DefaultAllocator(), nullptr);
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
    RenderGraph graph(DefaultAllocator(), nullptr);
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
    RenderGraph graph(DefaultAllocator(), nullptr);
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

// A pass that overwrites a resource must come after the earlier passes that read it (write after
// read), even when it has no inputs of its own and the reader waits on something else: here the
// reader waits on "Lit" while "Overwrite" is free to run at once.
TEST_CASE("rg.dep: a later overwrite waits for an earlier reader (write after read)")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle lit =
        graph.CreateTransient(u8"Lit", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    const RGHandle history = graph.ImportTarget(u8"History", nullptr, nullptr);
    const RGHandle out =
        graph.CreateTransient(u8"Out", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Light",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, lit, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Resolve",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(lit);
                            b.ReadTexture(history); // last frame's content
                            b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"Overwrite",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, history, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 3u);
    CHECK(PassName(graph, 0) == u8"Light");
    CHECK(PassName(graph, 1) == u8"Resolve");
    CHECK(PassName(graph, 2) == u8"Overwrite");
}

// Two passes that overwrite one resource keep their declared order (write after write), so the
// later declaration's content is what follows: the first writer waits on "Input", the second does
// not.
TEST_CASE("rg.dep: two overwrites of one resource keep their declared order (write after write)")
{
    RenderGraph graph(DefaultAllocator(), nullptr);
    graph.BeginFrame(0);
    const RGHandle input =
        graph.CreateTransient(u8"Input", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    const RGHandle target = graph.ImportTarget(u8"Target", nullptr, nullptr);

    graph.AddRenderPass(u8"MakeInput",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, input, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"FirstWrite",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(input);
                            b.SetColorTarget(0, target, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddRenderPass(u8"SecondWrite",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, target, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Compile().IsOk());
    REQUIRE(graph.ExecutionOrder().Size() == 3u);
    CHECK(PassName(graph, 0) == u8"MakeInput");
    CHECK(PassName(graph, 1) == u8"FirstWrite");
    CHECK(PassName(graph, 2) == u8"SecondWrite");
}

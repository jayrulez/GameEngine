// Ported from Sedulous.RenderGraph.Tests/GraphCoreTests.bf
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

TEST_CASE("rg.graph: create transient returns a valid handle")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle handle = graph.CreateTransient(
        u8"Test", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm, SizeMode::FullSize));
    CHECK(handle.IsValid());
    CHECK(graph.ResourceCount() == 1u);
}

TEST_CASE("rg.graph: multiple resources get unique handles")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle h1 = graph.CreateTransient(u8"A", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    const RGHandle h2 =
        graph.CreateTransient(u8"B", RGTextureDesc(rhi::TextureFormat::Depth32Float));
    CHECK(h1 != h2);
    CHECK(graph.ResourceCount() == 2u);
}

TEST_CASE("rg.graph: get resource by name")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle h1 =
        graph.CreateTransient(u8"SceneColor", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    CHECK(graph.GetResource(u8"SceneColor") == h1);
    CHECK_FALSE(graph.GetResource(u8"NonExistent").IsValid());
}

TEST_CASE("rg.graph: pass count is correct")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"Pass1",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    graph.AddComputePass(u8"Pass2", [](PassBuilder& b) { b.HasSideEffects(); });

    CHECK(graph.PassCount() == 2u);
}

TEST_CASE("rg.graph: set output size affects resolution")
{
    RenderGraph graph(nullptr);
    graph.SetOutputSize(1920, 1080);
    CHECK(graph.OutputWidth() == 1920u);
    CHECK(graph.OutputHeight() == 1080u);
}

TEST_CASE("rg.graph: import target with final state")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle handle =
        graph.ImportTarget(u8"Backbuffer", nullptr, nullptr, rhi::ResourceState::Present);
    CHECK(handle.IsValid());
}

TEST_CASE("rg.graph: reset keeps persistent, drops transient")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    graph.RegisterPersistent(u8"Shadow", nullptr, nullptr);
    graph.CreateTransient(u8"Temp", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.Reset();

    CHECK(graph.GetResource(u8"Shadow").IsValid());
    CHECK_FALSE(graph.GetResource(u8"Temp").IsValid());
}

TEST_CASE("rg.graph: SetViewport records a per-pass viewport override")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"ViewportPass",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.SetViewport(10, 20, 100, 200);
                            b.NeverCull();
                        });

    REQUIRE(graph.Passes().Size() == 1u);
    const RenderGraphPass* pass = graph.Passes()[0];
    CHECK(pass->hasViewport);
    CHECK(pass->viewportX == 10);
    CHECK(pass->viewportY == 20);
    CHECK(pass->viewportW == 100u);
    CHECK(pass->viewportH == 200u);
}

TEST_CASE("rg.graph: a pass without SetViewport has no viewport override (full attachment)")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));

    graph.AddRenderPass(u8"FullPass",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    REQUIRE(graph.Passes().Size() == 1u);
    CHECK_FALSE(graph.Passes()[0]->hasViewport);
}

// Ported from Sedulous.RenderGraph.Tests/DebugTests.bf
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
    bool Contains(StringView hay, StringView needle)
    {
        if (needle.Size() > hay.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (hay.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("rg.debug: ExportDOT produces valid syntax")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"SceneColor", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
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
                            b.ReadTexture(depth);
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    String dot;
    GraphDebug::ExportDOT(graph, dot);
    const StringView v = dot.AsView();
    CHECK(Contains(v, u8"digraph"));
    CHECK(Contains(v, u8"DepthPrepass"));
    CHECK(Contains(v, u8"ForwardOpaque"));
    CHECK(Contains(v, u8"SceneColor"));
    CHECK(Contains(v, u8"}"));
}

TEST_CASE("rg.debug: ExportSummary includes counts")
{
    RenderGraph graph(nullptr);
    graph.SetOutputSize(1920, 1080);
    graph.BeginFrame(0);
    const RGHandle color =
        graph.CreateTransient(u8"Color", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.AddRenderPass(u8"Pass1",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });
    REQUIRE(graph.Compile().IsOk());

    String summary;
    GraphDebug::ExportSummary(graph, summary);
    const StringView v = summary.AsView();
    CHECK(Contains(v, u8"1920x1080"));
    CHECK(Contains(v, u8"Pass1"));
    CHECK(Contains(v, u8"Render"));
}

TEST_CASE("rg.debug: DOT marks culled passes dashed")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.AddRenderPass(u8"Culled", [&](PassBuilder& b)
                        { b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store); });
    REQUIRE(graph.Compile().IsOk());

    String dot;
    GraphDebug::ExportDOT(graph, dot);
    CHECK(Contains(dot.AsView(), u8"dashed"));
}

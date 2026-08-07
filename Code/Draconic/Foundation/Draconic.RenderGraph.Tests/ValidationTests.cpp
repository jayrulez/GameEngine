// Ported from Sedulous.RenderGraph.Tests/ValidationTests.bf
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

    bool HasSeverity(const Array<ValidationMessage>& messages, ValidationSeverity severity)
    {
        for (const ValidationMessage& m : messages)
        {
            if (m.severity == severity)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("rg.validation: uninitialized read is an error")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.AddRenderPass(u8"BadPass",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });

    Array<ValidationMessage> messages;
    GraphValidator::Validate(graph, messages);
    CHECK(HasSeverity(messages, ValidationSeverity::Error));
}

TEST_CASE("rg.validation: reading an imported resource is fine")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle imported = graph.ImportTarget(u8"External", nullptr, nullptr);
    graph.AddRenderPass(u8"ReadImported",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(imported);
                            b.NeverCull();
                        });

    Array<ValidationMessage> messages;
    GraphValidator::Validate(graph, messages);
    CHECK_FALSE(HasSeverity(messages, ValidationSeverity::Error));
}

TEST_CASE("rg.validation: empty pass is a warning")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    graph.AddRenderPass(u8"Empty", [](PassBuilder& b) { b.NeverCull(); });

    Array<ValidationMessage> messages;
    GraphValidator::Validate(graph, messages);
    CHECK(HasSeverity(messages, ValidationSeverity::Warning));
}

TEST_CASE("rg.validation: redundant write is a warning")
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
    graph.AddRenderPass(u8"Write2",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                        });

    Array<ValidationMessage> messages;
    GraphValidator::Validate(graph, messages);
    CHECK(HasSeverity(messages, ValidationSeverity::Warning));
}

TEST_CASE("rg.validation: a clean graph produces no messages")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.AddRenderPass(u8"Write",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, tex, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                            b.SetExecute([](rhi::RenderPassEncoder&) {});
                        });
    graph.AddRenderPass(u8"Read",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                            b.SetExecute([](rhi::RenderPassEncoder&) {});
                        });

    Array<ValidationMessage> messages;
    GraphValidator::Validate(graph, messages);
    CHECK(messages.Size() == 0u);
}

TEST_CASE("rg.validation: ValidateToString formats output")
{
    RenderGraph graph(nullptr);
    graph.BeginFrame(0);
    const RGHandle tex =
        graph.CreateTransient(u8"Tex", RGTextureDesc(rhi::TextureFormat::RGBA8Unorm));
    graph.AddRenderPass(u8"BadRead",
                        [&](PassBuilder& b)
                        {
                            b.ReadTexture(tex);
                            b.NeverCull();
                        });

    String result;
    GraphValidator::ValidateToString(graph, result);
    CHECK(Contains(result.AsView(), u8"issue"));
}

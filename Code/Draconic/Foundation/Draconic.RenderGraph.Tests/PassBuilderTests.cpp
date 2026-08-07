// Ported from Sedulous.RenderGraph.Tests/PassBuilderTests.bf
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

TEST_CASE("rg.builder: ReadTexture adds an access")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    const RGHandle handle{0, 1};
    builder.ReadTexture(handle);

    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].handle == handle);
    CHECK(pass.accesses[0].type == RGAccessType::ReadTexture);
    CHECK(pass.accesses[0].subresource.IsAll());
}

TEST_CASE("rg.builder: ReadTexture with subresource")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.ReadTexture(RGHandle{0, 1}, RGSubresourceRange{0, 1, 2, 1});

    CHECK(pass.accesses[0].subresource.baseArrayLayer == 2u);
    CHECK(pass.accesses[0].subresource.arrayLayerCount == 1u);
}

TEST_CASE("rg.builder: SetColorTarget adds access + attachment")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    const RGHandle handle{0, 1};
    builder.SetColorTarget(0, handle, rhi::LoadOp::Clear, rhi::StoreOp::Store);

    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].type == RGAccessType::WriteColorTarget);
    REQUIRE(pass.colorTargets.Size() == 1u);
    CHECK(pass.colorTargets[0].handle == handle);
    CHECK(pass.colorTargets[0].loadOp == rhi::LoadOp::Clear);
}

TEST_CASE("rg.builder: SetDepthTarget adds access + attachment")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    const RGHandle handle{0, 1};
    builder.SetDepthTarget(handle, rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f);

    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].type == RGAccessType::WriteDepthTarget);
    REQUIRE(pass.depthTarget.HasValue());
    CHECK(pass.depthTarget.Value().handle == handle);
    CHECK(pass.depthTarget.Value().depthClearValue == 1.0f);
}

TEST_CASE("rg.builder: SetDepthTarget stencil ops default to DontCare")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.SetDepthTarget(RGHandle{0, 1});

    REQUIRE(pass.depthTarget.HasValue());
    CHECK(pass.depthTarget.Value().stencilLoadOp == rhi::LoadOp::DontCare);
    CHECK(pass.depthTarget.Value().stencilStoreOp == rhi::StoreOp::DontCare);
}

TEST_CASE("rg.builder: SetDepthTarget carries explicit stencil ops + clear")
{
    // The scene-overlay pass shape: depth unused, stencil cleared to 0 for
    // stencil-then-cover UI fills.
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.SetDepthTarget(RGHandle{0, 1}, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, {},
                           rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 0);

    REQUIRE(pass.depthTarget.HasValue());
    const RGDepthTarget& dt = pass.depthTarget.Value();
    CHECK(dt.stencilLoadOp == rhi::LoadOp::Clear);
    CHECK(dt.stencilStoreOp == rhi::StoreOp::DontCare);
    CHECK(dt.stencilClearValue == 0u);
    CHECK(dt.depthStoreOp == rhi::StoreOp::DontCare);
}

TEST_CASE("rg.builder: write-only depth (StoreOp::DontCare) still records the access")
{
    // The overlay-stencil regression: a scratch DS used only within the pass (Clear +
    // DontCare) must still count as a WRITE access - the access keeps the transient
    // referenced (allocated) and drives its layout transition. Without it the resource
    // ref-counts to zero and ExecuteRenderPass silently drops the whole pass.
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.SetDepthTarget(RGHandle{0, 1}, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, {},
                           rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 0);

    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].type == RGAccessType::WriteDepthTarget);
}

TEST_CASE("rg.builder: ReadDepth sets read-only")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.ReadDepth(RGHandle{0, 1});

    REQUIRE(pass.depthTarget.HasValue());
    CHECK(pass.depthTarget.Value().readOnly);
    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].type == RGAccessType::ReadDepthStencil);
}

TEST_CASE("rg.builder: SampleDepth reads without an attachment")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.SampleDepth(RGHandle{0, 1});

    // Unlike ReadDepth, sampling a depth texture in a shader is NOT a depth attachment:
    // it adds a read access only, leaving depthTarget unset.
    CHECK_FALSE(pass.depthTarget.HasValue());
    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].type == RGAccessType::SampleDepthStencil);
    CHECK(pass.accesses[0].IsRead());
    CHECK_FALSE(pass.accesses[0].IsWrite());
}

TEST_CASE("rg.builder: SampleDepth with subresource")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder builder(pass);
    builder.SampleDepth(RGHandle{0, 1}, RGSubresourceRange{0, 1, 3, 2});

    REQUIRE(pass.accesses.Size() == 1u);
    CHECK(pass.accesses[0].subresource.baseArrayLayer == 3u);
    CHECK(pass.accesses[0].subresource.arrayLayerCount == 2u);
}

TEST_CASE("rg.builder: NeverCull / HasSideEffects flags")
{
    {
        RenderGraphPass pass(u8"Test", RGPassType::Render);
        PassBuilder(pass).NeverCull();
        CHECK(pass.neverCull);
        CHECK(pass.ShouldSurviveCulling());
    }
    {
        RenderGraphPass pass(u8"Test", RGPassType::Render);
        PassBuilder(pass).HasSideEffects();
        CHECK(pass.hasSideEffects);
        CHECK(pass.ShouldSurviveCulling());
    }
}

TEST_CASE("rg.builder: EnableIf stores a runtime condition")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder(pass).EnableIf([]() { return true; });
    REQUIRE(static_cast<bool>(pass.condition));
    CHECK(pass.condition());
}

TEST_CASE("rg.builder: storage + copy accesses")
{
    {
        RenderGraphPass pass(u8"Test", RGPassType::Compute);
        PassBuilder(pass).WriteStorage(RGHandle{0, 1});
        REQUIRE(pass.accesses.Size() == 1u);
        CHECK(pass.accesses[0].type == RGAccessType::WriteStorage);
    }
    {
        RenderGraphPass pass(u8"Test", RGPassType::Compute);
        PassBuilder(pass).ReadWriteStorage(RGHandle{0, 1});
        REQUIRE(pass.accesses.Size() == 1u);
        CHECK(pass.accesses[0].type == RGAccessType::ReadWriteStorage);
        CHECK(pass.accesses[0].IsRead());
        CHECK(pass.accesses[0].IsWrite());
    }
    {
        RenderGraphPass pass(u8"Test", RGPassType::Copy);
        PassBuilder(pass).CopySrc(RGHandle{0, 1}).CopyDst(RGHandle{1, 1});
        REQUIRE(pass.accesses.Size() == 2u);
        CHECK(pass.accesses[0].type == RGAccessType::ReadCopySrc);
        CHECK(pass.accesses[1].type == RGAccessType::WriteCopyDst);
    }
}

TEST_CASE("rg.builder: fluent chaining")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    PassBuilder(pass)
        .ReadTexture(RGHandle{2, 1})
        .SetColorTarget(0, RGHandle{0, 1}, rhi::LoadOp::Clear, rhi::StoreOp::Store)
        .SetDepthTarget(RGHandle{1, 1}, rhi::LoadOp::Load, rhi::StoreOp::Store)
        .NeverCull();

    CHECK(pass.accesses.Size() == 3u); // read + write-color + readwrite-depth (Load+Store)
    CHECK(pass.colorTargets.Size() == 1u);
    CHECK(pass.depthTarget.HasValue());
    CHECK(pass.neverCull);
}

TEST_CASE("rg.builder: GetInputs folds LoadOp into a read")
{
    RenderGraphPass pass(u8"Test", RGPassType::Render);
    const RGHandle handle{0, 1};
    PassBuilder(pass).SetColorTarget(0, handle, rhi::LoadOp::Load, rhi::StoreOp::Store);

    Array<RGResourceAccess> inputs;
    pass.GetInputs(inputs);
    bool hasRead = false;
    for (const RGResourceAccess& input : inputs)
    {
        if (input.handle == handle && input.IsRead())
        {
            hasRead = true;
        }
    }
    CHECK(hasRead);
}

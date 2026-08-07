// Ported from Sedulous.RenderGraph.Tests/DescriptorTests.bf
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

TEST_CASE("rg.descriptor: full size resolves")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, SizeMode::FullSize);
    desc.Resolve(1920, 1080);
    CHECK(desc.width == 1920u);
    CHECK(desc.height == 1080u);
}

TEST_CASE("rg.descriptor: half size resolves")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, SizeMode::HalfSize);
    desc.Resolve(1920, 1080);
    CHECK(desc.width == 960u);
    CHECK(desc.height == 540u);
}

TEST_CASE("rg.descriptor: quarter size resolves")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, SizeMode::QuarterSize);
    desc.Resolve(1920, 1080);
    CHECK(desc.width == 480u);
    CHECK(desc.height == 270u);
}

TEST_CASE("rg.descriptor: custom is not resolved")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, 256, 256);
    desc.Resolve(1920, 1080);
    CHECK(desc.width == 256u);
    CHECK(desc.height == 256u);
}

TEST_CASE("rg.descriptor: custom size clamps to at least one")
{
    // A degenerate viewport size (0) must not produce a zero-extent transient - that reaches
    // CreateTexture / vkCmdBeginRendering and trips the resize-corruption cascade.
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, 0, 0);
    desc.Resolve(1920, 1080);
    CHECK(desc.width >= 1u);
    CHECK(desc.height >= 1u);
}

TEST_CASE("rg.descriptor: full size clamps to at least one")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, SizeMode::FullSize);
    desc.Resolve(0, 0);
    CHECK(desc.width >= 1u);
    CHECK(desc.height >= 1u);
}

TEST_CASE("rg.descriptor: half size clamps to at least one")
{
    RGTextureDesc desc(rhi::TextureFormat::RGBA8Unorm, SizeMode::HalfSize);
    desc.Resolve(1, 1);
    CHECK(desc.width >= 1u);
    CHECK(desc.height >= 1u);
}

TEST_CASE("rg.descriptor: color target defaults")
{
    RGColorTarget target{};
    target.handle = RGHandle{0, 1};
    CHECK(target.loadOp == rhi::LoadOp::Clear);
    CHECK(target.storeOp == rhi::StoreOp::Store);
}

TEST_CASE("rg.descriptor: depth target defaults")
{
    RGDepthTarget target{};
    target.handle = RGHandle{0, 1};
    CHECK(target.depthLoadOp == rhi::LoadOp::Clear);
    CHECK(target.depthStoreOp == rhi::StoreOp::Store);
    CHECK(target.depthClearValue == 1.0f);
    CHECK_FALSE(target.readOnly);
}

TEST_CASE("rg.descriptor: config defaults")
{
    RenderGraphConfig config{};
    CHECK(config.frameBufferCount == 2);
}

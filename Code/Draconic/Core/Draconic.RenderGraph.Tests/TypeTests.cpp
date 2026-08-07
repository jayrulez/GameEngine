// Ported from Sedulous.RenderGraph.Tests/TypeTests.bf
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

TEST_CASE("rg.type: handle equality")
{
    CHECK(RGHandle{1, 1} == RGHandle{1, 1});
    CHECK(RGHandle{1, 1} != RGHandle{2, 1});
    CHECK(RGHandle{1, 1} != RGHandle{1, 2}); // generation matters
}

TEST_CASE("rg.type: handle validity")
{
    CHECK_FALSE(RGHandle::Invalid().IsValid());
    CHECK(RGHandle{0, 1}.IsValid());
    CHECK_FALSE(PassHandle::Invalid().IsValid());
    CHECK(PassHandle{0}.IsValid());
}

TEST_CASE("rg.type: handle works as a hash-map key")
{
    HashMap<RGHandle, i32> map;
    map.InsertOrAssign(RGHandle{1, 1}, 7);
    const i32* found = map.Find(RGHandle{1, 1}); // equal handle
    REQUIRE(found != nullptr);
    CHECK(*found == 7);
    CHECK(map.Find(RGHandle{2, 1}) == nullptr);
}

TEST_CASE("rg.type: access IsRead / IsWrite")
{
    CHECK(IsRead(RGAccessType::ReadTexture));
    CHECK(IsRead(RGAccessType::ReadBuffer));
    CHECK(IsRead(RGAccessType::ReadDepthStencil));
    CHECK(IsRead(RGAccessType::SampleDepthStencil));
    CHECK(IsRead(RGAccessType::ReadCopySrc));
    CHECK(IsRead(RGAccessType::ReadWriteStorage));
    CHECK_FALSE(IsRead(RGAccessType::WriteColorTarget));
    CHECK_FALSE(IsRead(RGAccessType::WriteStorage));

    CHECK(IsWrite(RGAccessType::WriteColorTarget));
    CHECK(IsWrite(RGAccessType::WriteDepthTarget));
    CHECK(IsWrite(RGAccessType::WriteStorage));
    CHECK(IsWrite(RGAccessType::WriteCopyDst));
    CHECK(IsWrite(RGAccessType::ReadWriteStorage));
    CHECK_FALSE(IsWrite(RGAccessType::ReadTexture));
    CHECK_FALSE(IsWrite(RGAccessType::ReadBuffer));
    CHECK_FALSE(IsWrite(RGAccessType::SampleDepthStencil));
}

TEST_CASE("rg.type: access -> resource state")
{
    using RS = rhi::ResourceState;
    CHECK(ToResourceState(RGAccessType::ReadTexture) == RS::ShaderRead);
    CHECK(ToResourceState(RGAccessType::WriteColorTarget) == RS::RenderTarget);
    CHECK(ToResourceState(RGAccessType::WriteDepthTarget) == RS::DepthStencilWrite);
    CHECK(ToResourceState(RGAccessType::ReadDepthStencil) == RS::DepthStencilRead);
    // Sampling a depth texture in a shader uses the same read-only depth layout as a
    // read-only depth attachment (DEPTH_STENCIL_READ_ONLY_OPTIMAL), not ShaderRead.
    CHECK(ToResourceState(RGAccessType::SampleDepthStencil) == RS::DepthStencilRead);
    CHECK(ToResourceState(RGAccessType::ReadCopySrc) == RS::CopySrc);
    CHECK(ToResourceState(RGAccessType::WriteCopyDst) == RS::CopyDst);
    CHECK(ToResourceState(RGAccessType::WriteStorage) == RS::ShaderWrite);
}

TEST_CASE("rg.type: subresource All + overlap")
{
    const RGSubresourceRange all = RGSubresourceRange::All();
    CHECK(all.IsAll());
    CHECK(all.baseMipLevel == 0u);
    CHECK(all.mipLevelCount == 0u);

    const RGSubresourceRange layer0{0, 1, 0, 1};
    const RGSubresourceRange layer1{0, 1, 1, 1};
    CHECK_FALSE(layer0.Overlaps(layer1, 1, 4));
    CHECK(all.Overlaps(layer0, 1, 4));
    CHECK(all.Overlaps(layer1, 1, 4));
    CHECK(layer0.Overlaps(layer0, 1, 4));

    const RGSubresourceRange mip0{0, 1, 0, 0};
    const RGSubresourceRange mip1{1, 1, 0, 0};
    CHECK_FALSE(mip0.Overlaps(mip1, 4, 1));
    CHECK(mip0.Overlaps(mip0, 4, 1));
}

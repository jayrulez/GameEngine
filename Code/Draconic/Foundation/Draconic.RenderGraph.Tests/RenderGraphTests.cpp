// Direct unit tests for pieces without a dedicated Sedulous test file
// (SubresourceStateTracker, PersistentResource ping-pong, resource tracking).
// The Sedulous suite is ported in Type/Descriptor/PassBuilder/Dependency/
// Culling/GraphCore/Barrier Tests.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

TEST_CASE("rg.persistent: ping-pong swap")
{
    auto* a = reinterpret_cast<rhi::Texture*>(0x10);
    auto* b = reinterpret_cast<rhi::Texture*>(0x20);
    auto* va = reinterpret_cast<rhi::TextureView*>(0x30);
    auto* vb = reinterpret_cast<rhi::TextureView*>(0x40);

    PersistentResource single(a, va);
    CHECK_FALSE(single.IsPingPong());
    CHECK(single.CurrentTexture() == a);
    CHECK(single.PreviousTexture() == a);
    single.Swap();
    CHECK(single.CurrentTexture() == a);

    PersistentResource pp(a, b, va, vb);
    CHECK(pp.IsPingPong());
    CHECK(pp.CurrentTexture() == a);
    CHECK(pp.PreviousTexture() == b);
    pp.Swap();
    CHECK(pp.CurrentTexture() == b);
    CHECK(pp.PreviousTexture() == a);
}

TEST_CASE("rg.resource: tracking + totals from descriptor")
{
    RenderGraphResource res(u8"gbuffer", RGResourceType::Texture, RGResourceLifetime::Transient);
    res.textureDesc.mipLevelCount = 4;
    res.textureDesc.arrayLayerCount = 6;

    CHECK(res.TotalMipLevels() == 4u); // no GPU texture -> from descriptor
    CHECK(res.TotalArrayLayers() == 6u);

    res.refCount = 3;
    res.firstUsePass = 2;
    res.ResetTracking();
    CHECK(res.refCount == 0);
    CHECK(res.firstUsePass == -1);
    CHECK_FALSE(res.firstWriter.IsValid());
    CHECK_FALSE(res.finalState.HasValue());
}

TEST_CASE("rg.state_tracker: uniform fast path, divergence, collapse")
{
    using RS = rhi::ResourceState;
    SubresourceStateTracker t(4, 2, RS::Undefined);
    CHECK(t.IsUniform());
    CHECK(t.GetState(0, 0) == RS::Undefined);

    t.SetState(RGSubresourceRange::All(), RS::ShaderRead);
    CHECK(t.IsUniform());
    CHECK(t.GetState(3, 1) == RS::ShaderRead);

    t.SetState(0, 1, 0, 1, RS::RenderTarget);
    CHECK_FALSE(t.IsUniform());
    CHECK(t.GetState(0, 0) == RS::RenderTarget);
    CHECK(t.GetState(1, 0) == RS::ShaderRead);

    Array<RS> snapshot = t.CopyStates();
    CHECK(snapshot.Size() == 8u);

    t.SetAll(RS::ShaderRead);
    CHECK(t.IsUniform());

    SubresourceStateTracker restored(4, 2, RS::Undefined);
    restored.InitFromStates(snapshot, RS::Undefined);
    CHECK_FALSE(restored.IsUniform());
    CHECK(restored.GetState(0, 0) == RS::RenderTarget);

    restored.SetState(0, 1, 0, 1, RS::ShaderRead);
    CHECK(restored.IsUniform());
}

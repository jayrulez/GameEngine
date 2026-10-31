// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GPU picking, the device-free half: the entity tag layout, the rect clamp, the projection crop
// (a pixel of the full view maps to the centre of the cropped clip space, which is what lets a
// click render a 1x1 target), the readback decode (unique hits, background skipped, padded
// rows), and the PickSystem's request state machine on the Null device: a request is declared,
// retired on the ring's round trip and taken once; it expires when no view renders; it can be
// cancelled.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.rendergraph;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace rendergraph = foundation::rendergraph;

namespace
{
    // The pixel a world point lands on through `view * projection` in a w x h viewport (y down).
    void PixelOf(const Float4x4& viewProj, Float3 world, u32 w, u32 h, f32& px, f32& py)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * viewProj;
        const f32 ndcX = clip.x / clip.w;
        const f32 ndcY = clip.y / clip.w;
        px = (ndcX * 0.5f + 0.5f) * static_cast<f32>(w);
        py = (0.5f - ndcY * 0.5f) * static_cast<f32>(h);
    }
}

TEST_CASE("picking: EntityTag packs index low / generation high and round-trips")
{
    const u64 tag = EntityTag::Pack(1234u, 77u);
    CHECK(EntityTag::Index(tag) == 1234u);
    CHECK(EntityTag::Generation(tag) == 77u);
    CHECK(EntityTag::Pack(0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(EntityTag::Index(EntityTag::Pack(0u, 5u)) == 0u);
    CHECK(EntityTag::Generation(EntityTag::Pack(9u, 0u)) == 0u);
}

TEST_CASE("picking: ClampPickRect keeps the inside part and rejects the rest")
{
    PickRect inside{10, 20, 5, 6};
    REQUIRE(ClampPickRect(inside, 100, 80));
    CHECK(inside.x == 10);
    CHECK(inside.y == 20);
    CHECK(inside.width == 5);
    CHECK(inside.height == 6);

    PickRect straddling{-3, 75, 10, 10}; // over the left edge and the bottom edge
    REQUIRE(ClampPickRect(straddling, 100, 80));
    CHECK(straddling.x == 0);
    CHECK(straddling.y == 75);
    CHECK(straddling.width == 7);
    CHECK(straddling.height == 5);

    PickRect outside{100, 0, 1, 1}; // one past the right edge
    CHECK_FALSE(ClampPickRect(outside, 100, 80));
    PickRect negative{-5, -5, 5, 5};
    CHECK_FALSE(ClampPickRect(negative, 100, 80));
    PickRect empty{10, 10, 0, 4};
    CHECK_FALSE(ClampPickRect(empty, 100, 80));
    PickRect noView{0, 0, 1, 1};
    CHECK_FALSE(ClampPickRect(noView, 0, 0));
}

TEST_CASE("picking: CropProjectionToRect maps the rect to the whole clip space, depth untouched")
{
    constexpr u32 kW = 640, kH = 360;
    const Float4x4 view =
        Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0472f, 640.0f / 360.0f, 0.1f, 100.0f);
    const Float4x4 full = view * proj;

    // A world point off-centre; find its pixel in the full view.
    const Float3 world{1.3f, -0.7f, -6.0f};
    f32 px = 0.0f, py = 0.0f;
    PixelOf(full, world, kW, kH, px, py);
    REQUIRE(px > 0.0f);
    REQUIRE(py > 0.0f);
    REQUIRE(px < static_cast<f32>(kW));
    REQUIRE(py < static_cast<f32>(kH));

    // Crop to the 1x1 rect of that pixel: the point lands at the centre of the 1x1 target
    // (NDC (0,0) up to sub-pixel: the pixel's centre against the point's exact position).
    const PickRect one{static_cast<i32>(px), static_cast<i32>(py), 1, 1};
    const Float4x4 cropped = view * CropProjectionToRect(proj, one, kW, kH);
    f32 cx = 0.0f, cy = 0.0f;
    PixelOf(cropped, world, 1, 1, cx, cy);
    CHECK(cx >= 0.0f);
    CHECK(cx < 1.0f);
    CHECK(cy >= 0.0f);
    CHECK(cy < 1.0f);

    // A wider rect: the point keeps its offset inside the rect (pixel-exact remap).
    const PickRect rect{static_cast<i32>(px) - 7, static_cast<i32>(py) - 3, 20, 12};
    const Float4x4 croppedRect = view * CropProjectionToRect(proj, rect, kW, kH);
    f32 rx = 0.0f, ry = 0.0f;
    PixelOf(croppedRect, world, rect.width, rect.height, rx, ry);
    CHECK(rx == doctest::Approx(px - static_cast<f32>(rect.x)).epsilon(0.01));
    CHECK(ry == doctest::Approx(py - static_cast<f32>(rect.y)).epsilon(0.01));

    // Depth is the same: z/w through both projections agree (the pick's own depth test ranks
    // surfaces exactly as the view would).
    const Float4 clipFull = Float4{world.x, world.y, world.z, 1.0f} * full;
    const Float4 clipCrop = Float4{world.x, world.y, world.z, 1.0f} * croppedRect;
    CHECK(clipCrop.z / clipCrop.w == doctest::Approx(clipFull.z / clipFull.w).epsilon(1e-5));

    // The whole viewport as the rect is the identity crop.
    const PickRect all{0, 0, kW, kH};
    const Float4x4 same = CropProjectionToRect(proj, all, kW, kH);
    for (usize r = 0; r < 4; ++r)
    {
        for (usize c = 0; c < 4; ++c)
        {
            CHECK(same(r, c) == doctest::Approx(proj(r, c)).epsilon(1e-6));
        }
    }
}

TEST_CASE("picking: DecodePickTexels dedupes, skips background, honours the row stride")
{
    // 3x2 texels in 256-byte rows: (index+1, generation) pairs.
    constexpr u32 kW = 3, kH = 2;
    const u32 stride = PickRowStride(kW);
    CHECK(stride == 256u);
    CHECK(PickRowStride(32) == 256u);  // 32 * 8 = 256 exactly
    CHECK(PickRowStride(33) == 512u);
    Array<u8> bytes;
    bytes.Resize(static_cast<usize>(stride) * kH, 0u);
    const auto put = [&](u32 x, u32 y, u32 indexPlusOne, u32 gen)
    {
        u32 texel[2] = {indexPlusOne, gen};
        MemCopy(bytes.Data() + static_cast<usize>(y) * stride + static_cast<usize>(x) * 8u, texel,
                sizeof(texel));
    };
    put(0, 0, 5, 1);  // entity 4 gen 1
    put(1, 0, 0, 9);  // background (index 0), whatever y says
    put(2, 0, 5, 1);  // entity 4 again -> deduped
    put(0, 1, 1, 0);  // entity 0 gen 0
    put(1, 1, 5, 2);  // entity 4 but a different generation -> distinct hit
    put(2, 1, 0, 0);

    Array<PickHit> hits;
    DecodePickTexels(bytes.Data(), kW, kH, stride, hits);
    REQUIRE(hits.Size() == 3);
    CHECK(hits[0].entityIndex == 4u);
    CHECK(hits[0].generation == 1u);
    CHECK(hits[1].entityIndex == 0u);
    CHECK(hits[1].generation == 0u);
    CHECK(hits[2].entityIndex == 4u);
    CHECK(hits[2].generation == 2u);

    DecodePickTexels(nullptr, kW, kH, stride, hits);
    CHECK(hits.IsEmpty());
}

TEST_CASE("picking: PickSystem request -> declared -> retired on the ring round trip -> taken once")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    PickSystem pick(DefaultAllocator(), device, /*framesInFlight*/ 2);
    int viewportKey = 0;
    int otherKey = 0;

    const PickRequestId id = pick.Request(&viewportKey, PickRect{10, 10, 1, 1});
    REQUIRE(id != kInvalidPickRequest);
    CHECK(pick.IsPending(id));
    CHECK(pick.PendingCount(&viewportKey) == 1u);
    CHECK(pick.PendingCount(&otherKey) == 0u);
    CHECK(pick.PendingTotal() == 1u);
    PickResult result;
    CHECK_FALSE(pick.TryTakeResult(id, result));

    // Frame 0: the view declares a pick pass and a copy pass in the graph; slot AwaitReadback.
    rendergraph::RenderGraph graph(DefaultAllocator(), &device);
    graph.BeginFrame(0);
    u32 recorded = 0;
    const PickRecordFn record = [](void* context, rhi::RenderPassEncoder&, const Float4x4&,
                                   const PickRect&, const void*)
    { ++*static_cast<u32*>(context); };
    const Float4x4 view = Float4x4::Identity();
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 0.1f, 100.0f);
    pick.BeginFrame(0);
    // A view with another key answers nothing.
    CHECK(pick.DeclarePasses(graph, &otherKey, view, proj, 100, 100, rhi::TextureFormat::Depth32Float,
                             0, record, &recorded, nullptr) == 0u);
    CHECK(pick.PendingCount(&viewportKey) == 1u);
    CHECK(pick.DeclarePasses(graph, &viewportKey, view, proj, 100, 100,
                             rhi::TextureFormat::Depth32Float, 0, record, &recorded, nullptr) == 1u);
    CHECK(recorded == 0u); // declared, not executed (the graph never ran)
    CHECK(pick.PendingCount(&viewportKey) == 0u); // not waiting for a render
    CHECK(pick.IsPending(id));                    // ...but not answered yet either
    CHECK(graph.PassCount() >= 2u);               // the id pass and the readback copy
    graph.EndFrame();

    // Frame 1 (the other ring slot): still in flight. Frame 0 again: retired and decoded (the Null
    // device's buffer maps to zeros, so no hits), rendered = true.
    pick.BeginFrame(1);
    CHECK(pick.IsPending(id));
    CHECK_FALSE(pick.TryTakeResult(id, result));
    pick.BeginFrame(0);
    CHECK_FALSE(pick.IsPending(id));
    REQUIRE(pick.TryTakeResult(id, result));
    CHECK(result.id == id);
    CHECK(result.rendered);
    CHECK(result.hits.IsEmpty());
    CHECK_FALSE(pick.TryTakeResult(id, result)); // taken once
    CHECK_FALSE(pick.IsPending(id));
}

TEST_CASE("picking: a rect fully outside the view answers at once with no pass; expiry; cancel")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    PickSystem pick(DefaultAllocator(), device, 2);
    int viewportKey = 0;
    rendergraph::RenderGraph graph(DefaultAllocator(), &device);
    const PickRecordFn record =
        [](void*, rhi::RenderPassEncoder&, const Float4x4&, const PickRect&, const void*) {};
    const Float4x4 view = Float4x4::Identity();
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 0.1f, 100.0f);

    // Outside: no pass, ready immediately, rendered (the view did consider it).
    const PickRequestId outside = pick.Request(&viewportKey, PickRect{500, 500, 1, 1});
    graph.BeginFrame(0);
    pick.BeginFrame(0);
    CHECK(pick.DeclarePasses(graph, &viewportKey, view, proj, 100, 100,
                             rhi::TextureFormat::Depth32Float, 0, record, nullptr, nullptr) == 0u);
    graph.EndFrame();
    PickResult result;
    REQUIRE(pick.TryTakeResult(outside, result));
    CHECK(result.rendered);
    CHECK(result.hits.IsEmpty());

    // Expiry: a request whose view never renders completes as not rendered after the window.
    const PickRequestId orphan = pick.Request(&viewportKey, PickRect{1, 1, 1, 1});
    for (u32 f = 0; f <= kPickSystemExpireFrames; ++f)
    {
        CHECK_FALSE(pick.TryTakeResult(orphan, result));
        pick.BeginFrame(f % 2);
    }
    REQUIRE(pick.TryTakeResult(orphan, result));
    CHECK_FALSE(result.rendered);
    CHECK(result.hits.IsEmpty());

    // Cancel: a waiting request vanishes; ids never answer.
    const PickRequestId a = pick.Request(&viewportKey, PickRect{1, 1, 1, 1});
    const PickRequestId b = pick.Request(&viewportKey, PickRect{2, 2, 3, 3});
    CHECK(pick.PendingCount(&viewportKey) == 2u);
    pick.Cancel(&viewportKey);
    CHECK(pick.PendingCount(&viewportKey) == 0u);
    CHECK_FALSE(pick.IsPending(a));
    CHECK_FALSE(pick.IsPending(b));
    CHECK_FALSE(pick.TryTakeResult(a, result));
    CHECK_FALSE(pick.TryTakeResult(b, result));

    // Cancel of an in-flight readback: it still retires, its result is discarded.
    const PickRequestId c = pick.Request(&viewportKey, PickRect{1, 1, 1, 1});
    graph.BeginFrame(0);
    pick.BeginFrame(0);
    CHECK(pick.DeclarePasses(graph, &viewportKey, view, proj, 100, 100,
                             rhi::TextureFormat::Depth32Float, 0, record, nullptr, nullptr) == 1u);
    graph.EndFrame();
    pick.Cancel(&viewportKey);
    CHECK_FALSE(pick.IsPending(c));
    pick.BeginFrame(1);
    pick.BeginFrame(0);
    CHECK_FALSE(pick.TryTakeResult(c, result));
    CHECK(pick.PendingTotal() == 0u);
    device.WaitIdle();
}

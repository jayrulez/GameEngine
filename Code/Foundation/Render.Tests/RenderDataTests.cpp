// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene-agnostic render-data core (no GPU): the frame arena, ExtractedScene snapshot,
// sort keys + radix sort, the RenderView draw-list build (cull/sort), and the view pool.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;

TEST_CASE("FrameArena: allocations are distinct, aligned, and reset reuses chunks")
{
    FrameArena arena{DefaultAllocator()};
    MeshRenderData* a = arena.New<MeshRenderData>();
    MeshRenderData* b = arena.New<MeshRenderData>();
    MeshRenderData* c = arena.New<MeshRenderData>();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(reinterpret_cast<usize>(a) % alignof(MeshRenderData) == 0);
    CHECK(reinterpret_cast<usize>(b) % alignof(MeshRenderData) == 0);

    const usize chunksAfterFirst = arena.ChunkCount();
    arena.Reset();
    for (int i = 0; i < 3; ++i)
    {
        CHECK(arena.New<MeshRenderData>() != nullptr);
    }
    CHECK(arena.ChunkCount() == chunksAfterFirst); // same load reuses chunks, no growth
}

TEST_CASE("ExtractedScene: Add registers items; Reset empties without freeing chunks")
{
    ExtractedScene scene{DefaultAllocator()};
    CHECK(scene.IsEmpty());
    for (int i = 0; i < 10; ++i)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        REQUIRE(rd != nullptr);
        rd->entityId = static_cast<u64>(i);
    }
    REQUIRE(scene.Size() == 10);
    CHECK(scene.Items()[5]->category == RenderCategories::Opaque);

    scene.Reset();
    CHECK(scene.IsEmpty());
    CHECK(scene.Size() == 0);

    // The scene a snapshot came from is stamped each frame; a reset drops it (a pooled snapshot
    // may serve another scene next).
    scene.SetSceneSerial(42);
    CHECK(scene.SceneSerial() == 42u);
    scene.Reset();
    CHECK(scene.SceneSerial() == 0u);
}

TEST_CASE("sort keys: category dominates, then state, then depth")
{
    // Category is most significant: any opaque key < any transparent key.
    const u64 opaque = MakeSortKey(RenderCategories::Opaque, 0xFFFFFF, 0xFFFFFF);
    const u64 transparent = MakeSortKey(RenderCategories::Transparent, 0, 0);
    CHECK(opaque < transparent);

    // Within a category, smaller depth sorts first (front-to-back for opaque).
    const u64 near =
        MakeSortKey(RenderCategories::Opaque, 7, QuantizeDepth(0.10f, /*invert*/ false));
    const u64 far =
        MakeSortKey(RenderCategories::Opaque, 7, QuantizeDepth(0.90f, /*invert*/ false));
    CHECK(near < far);

    // Inverted depth (transparent) reverses it (back-to-front).
    const u64 tNear =
        MakeSortKey(RenderCategories::Transparent, 0, QuantizeDepth(0.10f, /*invert*/ true));
    const u64 tFar =
        MakeSortKey(RenderCategories::Transparent, 0, QuantizeDepth(0.90f, /*invert*/ true));
    CHECK(tFar < tNear);
}

TEST_CASE("RadixSortDrawItems sorts ascending by key")
{
    Array<DrawItem> items;
    const u64 keys[] = {50, 3, 9999, 0, 42, 7, 0x00FF00FF00FF00FFull, 1, 256, 255};
    for (u64 k : keys)
    {
        items.PushBack(DrawItem{k, nullptr});
    }

    Array<DrawItem> scratch;
    RadixSortDrawItems(items, scratch);

    REQUIRE(items.Size() == 10);
    for (usize i = 1; i < items.Size(); ++i)
    {
        CHECK(items[i - 1].key <= items[i].key);
    }
    CHECK(items[0].key == 0);
    CHECK(items[items.Size() - 1].key == 0x00FF00FF00FF00FFull);
}

TEST_CASE("RenderView::BuildDrawList frustum-culls a sphere entirely outside the view, and a "
          "view whose settings opt out of culling keeps everything")
{
    ExtractedScene scene{DefaultAllocator()};
    auto add = [&](Float3 center, f32 radius, u64 tag)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->worldCenter = center;
        rd->worldRadius = radius;
        rd->entityId = tag;
        rd->category = RenderCategories::Opaque;
    };
    add(Float3{0, 0, -5}, 1.0f, 1);    // in front of the camera: kept
    add(Float3{0, 0, 50}, 1.0f, 2);    // behind it: culled
    add(Float3{500, 0, -5}, 1.0f, 3);  // far off to the side: culled
    add(Float3{0, 0, -20}, 1.0f, 4);   // farther in front: kept

    // A real perspective (the default ViewCamera's identity projection makes the frustum the
    // reverse-Z unit box, which culls everything at a distance - the sort tests above never cull).
    ViewCamera camera;
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;
    Array<DrawItem> scratch;

    RenderView culled;
    culled.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    culled.BuildDrawList(scratch, /*cull*/ true);
    REQUIRE(culled.DrawList().Size() == 2);
    CHECK(culled.CulledCount() == 2);
    for (const DrawItem& item : culled.DrawList())
    {
        const u64 id = static_cast<const MeshRenderData*>(item.data)->entityId;
        CHECK((id == 1 || id == 4));
    }

    // Culling off (the frame's switch off, or a view's frustumCull cleared by the
    // "draw everything" override): the behind-camera sphere is in the list too.
    RenderView all;
    all.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    all.BuildDrawList(scratch, /*cull*/ false);
    CHECK(all.DrawList().Size() == 4);
    CHECK(all.CulledCount() == 0);

    // The frame gates by BOTH: its global switch (on by default) and the view's setting.
    rhi::null::NullDevice device{DefaultAllocator()};
    RendererRegistry registry; // no renderer needed: AddView only builds the draw list
    RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2);
    CHECK(frame.ViewCulling()); // the default since 2026-09-24
    frame.SetViewCulling(true);
    ViewSettings optOut;
    optOut.frustumCull = false;
    RenderView* framed = frame.AddView(scene, camera, optOut, nullptr, rhi::TextureFormat::BGRA8Unorm,
                                       64, 64, nullptr, nullptr, nullptr);
    REQUIRE(framed != nullptr);
    CHECK(framed->DrawList().Size() == 4);
    RenderView* framedCulled = frame.AddView(scene, camera, settings, nullptr,
                                             rhi::TextureFormat::BGRA8Unorm, 64, 64, nullptr,
                                             nullptr, nullptr);
    REQUIRE(framedCulled != nullptr);
    CHECK(framedCulled->DrawList().Size() == 2);
}

// Sedulous ebbf7a3b / 4ed0922d: a scene size of its own runs the chain at it and remembers the
// output rectangle; the viewport's own size is no scaling at all, unless a crop shows a slice.
TEST_CASE("RenderView: a scene size splits the scene from the output")
{
    ExtractedScene scene{DefaultAllocator()};
    RenderView view;
    ViewSettings settings;
    settings.viewportX = 160;
    settings.viewportWidth = 960;
    settings.viewportHeight = 540;
    settings.scene = SceneSize(1280, 720);
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK(view.IsScaled());
    CHECK(view.Width() == 1280u); // the chain runs at the scene size
    CHECK(view.Height() == 720u);
    CHECK(view.ViewportX() == 0);
    CHECK(view.ViewportWidth() == 1280u);
    CHECK(view.ViewportHeight() == 720u);
    CHECK(view.OutputWidth() == 1280u);
    CHECK(view.OutputHeight() == 540u);
    CHECK(view.OutputViewportX() == 160);
    CHECK(view.OutputViewportWidth() == 960u);
    CHECK(view.OutputViewportHeight() == 540u);

    settings.scene = SceneSize(960, 540);
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK_FALSE(view.IsScaled()); // the viewport's own size is not a scale
    CHECK(view.Width() == 1280u);
    CHECK(view.ViewportX() == 160);

    settings.scene.sourceHeight = 0.75f; // the same size, but a crop's slice of it shows
    view.Bind(scene, ViewCamera{}, settings, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 1280, 540);
    CHECK(view.IsScaled());
}

TEST_CASE("SceneSize: a fit gives the scene size and the slice it shows")
{
    const SceneSize letterbox =
        SceneSize::FromFit(ContentFit{Rectangle{0, 0, 1920, 1080}, Float2{1280, 960}, FitMode::Letterbox});
    CHECK(letterbox.width == 1280u);
    CHECK(letterbox.height == 960u);
    CHECK(letterbox.sourceX == doctest::Approx(0.0f));
    CHECK(letterbox.sourceWidth == doctest::Approx(1.0f));
    CHECK(letterbox.sourceHeight == doctest::Approx(1.0f));
    CHECK_FALSE(letterbox.IsCropped());
    // 4:3 cropped to fill 16:9: the full width, the middle three quarters of the height.
    const SceneSize crop =
        SceneSize::FromFit(ContentFit{Rectangle{0, 0, 1920, 1080}, Float2{1280, 960}, FitMode::Crop});
    CHECK(crop.sourceX == doctest::Approx(0.0f));
    CHECK(crop.sourceWidth == doctest::Approx(1.0f));
    CHECK(crop.sourceHeight == doctest::Approx(0.75f));
    CHECK(crop.sourceY == doctest::Approx(0.125f));
    CHECK(crop.IsCropped());
}

TEST_CASE("RenderView::BuildDrawList sorts opaque front-to-back, transparent back-to-front")
{
    // Three meshes in front of an identity camera at view-space depths 2, 5, 8.
    ExtractedScene scene{DefaultAllocator()};
    auto add = [&](f32 z, u64 tag, RenderCategory cat)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->worldCenter = Float3{0, 0, z}; // camera looks down -z, so z<0 is in front
        rd->entityId = tag;
        rd->category = cat;
    };

    ViewCamera camera; // identity view, farZ 1000
    ViewSettings settings;
    Array<DrawItem> scratch;

    SUBCASE("opaque: nearest first")
    {
        add(-2.0f, /*tag*/ 2, RenderCategories::Opaque);
        add(-8.0f, /*tag*/ 8, RenderCategories::Opaque);
        add(-5.0f, /*tag*/ 5, RenderCategories::Opaque);

        RenderView view;
        view.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
        view.BuildDrawList(scratch);

        const Span<const DrawItem> dl = view.DrawList();
        REQUIRE(dl.Size() == 3);
        CHECK(static_cast<const MeshRenderData*>(dl[0].data)->entityId == 2); // nearest
        CHECK(static_cast<const MeshRenderData*>(dl[1].data)->entityId == 5);
        CHECK(static_cast<const MeshRenderData*>(dl[2].data)->entityId == 8); // farthest
    }

    SUBCASE("transparent: farthest first")
    {
        add(-2.0f, 2, RenderCategories::Transparent);
        add(-8.0f, 8, RenderCategories::Transparent);
        add(-5.0f, 5, RenderCategories::Transparent);

        RenderView view;
        view.Bind(scene, camera, settings, nullptr, rhi::TextureFormat::BGRA8Unorm, 64, 64);
        view.BuildDrawList(scratch);

        const Span<const DrawItem> dl = view.DrawList();
        REQUIRE(dl.Size() == 3);
        CHECK(static_cast<const MeshRenderData*>(dl[0].data)->entityId == 8); // farthest
        CHECK(static_cast<const MeshRenderData*>(dl[2].data)->entityId == 2); // nearest
    }
}

TEST_CASE("RenderViewPool: Acquire hands out stable views; Begin rewinds")
{
    RenderViewPool pool{DefaultAllocator()};
    pool.Begin();
    RenderView* a = pool.Acquire();
    RenderView* b = pool.Acquire();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a != b);
    CHECK(pool.ActiveCount() == 2);

    pool.Begin();
    CHECK(pool.ActiveCount() == 0);
    RenderView* a2 = pool.Acquire();
    CHECK(a2 == a); // pooled storage reused, addresses stable
}

TEST_CASE("DynamicUniformRing: per-frame regions are disjoint; exhaustion + grow")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    const u32 framesInFlight = 3;
    DynamicUniformRing ring(device, framesInFlight, /*slotSize*/ 256);
    REQUIRE(ring.Reserve(4)); // 4 slots per frame region
    const u32 gen0 = ring.Generation();

    // Each frame's first slot lands in a distinct region (frameIndex * slotsPerFrame * 256).
    u32 firstOffset[3] = {};
    for (u32 f = 0; f < framesInFlight; ++f)
    {
        ring.BeginFrame(f);
        DynamicUniformRing::Range s = ring.Allocate();
        REQUIRE(s.ok);
        firstOffset[f] = s.byteOffset;
        CHECK(s.slotIndex == f * 4u); // absolute slot index = region base
        ring.EndFrame();
    }
    CHECK(firstOffset[0] == 0u);
    CHECK(firstOffset[1] == 4u * 256u); // region size = slotsPerFrame * slotSize
    CHECK(firstOffset[2] == 8u * 256u);

    // A region holds exactly slotsPerFrame slots; the next Allocate fails (no silent grow).
    ring.BeginFrame(0);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(ring.Allocate().ok);
    }
    CHECK_FALSE(ring.Allocate().ok);
    ring.EndFrame();

    // AllocateRange hands out contiguous runs and also fails past the region.
    ring.BeginFrame(1);
    DynamicUniformRing::Range r = ring.AllocateRange(3);
    REQUIRE(r.ok);
    CHECK(r.slotIndex == 1u * 4u);         // region 1 base
    CHECK_FALSE(ring.AllocateRange(2).ok); // only 1 slot left in the region
    ring.EndFrame();

    // Lazy mapping: a frame that allocates nothing never maps (no flush, no compare on an
    // emulated mapping); the first allocation maps; EndFrame reports the written slot count.
    ring.BeginFrame(2);
    CHECK_FALSE(ring.IsMappedThisFrame());
    CHECK(ring.FrameAllocatedSlots() == 0u);
    ring.EndFrame(); // nothing to flush
    ring.BeginFrame(2);
    CHECK(ring.AllocateRange(2).ok);
    CHECK(ring.IsMappedThisFrame());
    CHECK(ring.FrameAllocatedSlots() == 2u);
    ring.EndFrame();
    CHECK_FALSE(ring.IsMappedThisFrame());
    // Outside a frame nothing allocates (the region base is undefined).
    CHECK_FALSE(ring.Allocate().ok);
    CHECK(ring.SlotsPerFrame() == 4u);

    // Reserving more grows (new generation); a smaller reserve does not.
    REQUIRE(ring.Reserve(16));
    CHECK(ring.Generation() != gen0);
    const u32 gen1 = ring.Generation();
    REQUIRE(ring.Reserve(4));
    CHECK(ring.Generation() == gen1); // no shrink, no realloc
}

TEST_CASE("GpuBufferPool sub-allocates within a chunk and grows by adding chunks")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuBufferPool pool(device, rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst, /*chunk*/ 1024,
                       u8"test");

    GpuBufferPool::Alloc a = pool.Allocate(100, 16);
    GpuBufferPool::Alloc b = pool.Allocate(100, 16);
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    CHECK(a.offset == 0u);
    CHECK(b.offset == 112u);     // 100 rounded up to the next multiple of 16
    CHECK(a.buffer == b.buffer); // same chunk
    CHECK(pool.ChunkCount() == 1u);

    // An allocation that doesn't fit the remaining chunk space opens a new chunk.
    GpuBufferPool::Alloc big = pool.Allocate(2000, 16);
    REQUIRE(big.ok);
    CHECK(big.offset == 0u);
    CHECK(big.buffer != a.buffer);
    CHECK(pool.ChunkCount() == 2u);

    pool.Clear();
    CHECK(pool.ChunkCount() == 0u);
}

TEST_CASE("RendererRegistry routes categories to renderers")
{
    struct FakeRenderer final : Renderer
    {
        Span<const RenderCategory> SupportedCategories() const override
        {
            static constexpr RenderCategory cats[] = {RenderCategories::Opaque,
                                                      RenderCategories::Masked};
            return Span<const RenderCategory>{cats, 2};
        }
        void Resolve(const RenderRecordContext&, Span<const DrawItem>,
                     Array<ResolvedDraw>&) override
        {
        }
    };

    FakeRenderer r;
    RendererRegistry registry;
    registry.Register(&r);

    CHECK(registry.ById(r.RendererId()) == &r);
    CHECK(registry.ById(999) == nullptr); // unregistered id
    CHECK(registry.Unique().Size() == 1);
}

// ---- overlay registries (the two-tier overlay coordination model) ----

namespace
{
    struct FakeSceneOverlay final : foundation::render::ISceneOverlay
    {
        i32 order = 0;
        explicit FakeSceneOverlay(i32 o) : order(o) {}
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return order; }
        void Render(rhi::RenderPassEncoder&, const foundation::render::SceneOverlayView&) override {}
    };
}

TEST_CASE("overlay views: color-only by default (depthStencilFormat Undefined)")
{
    // Sources must treat Undefined as "no stencil in this pass" and fall back to
    // tessellated fills; a pass that attaches a DS advertises its exact format.
    foundation::render::SceneOverlayView sceneView;
    CHECK(sceneView.depthStencilFormat == foundation::rhi::TextureFormat::Undefined);
    foundation::render::ScreenOverlayView screenView;
    CHECK(screenView.depthStencilFormat == foundation::rhi::TextureFormat::Undefined);
}

TEST_CASE("overlay registry: sorted by order, stable ties, idempotent, removable")
{
    foundation::render::OverlayRegistry<foundation::render::ISceneOverlay> registry;
    CHECK(registry.IsEmpty());

    FakeSceneOverlay foreground{10};
    FakeSceneOverlay backgroundA{0};
    FakeSceneOverlay backgroundB{0}; // ties keep registration order
    FakeSceneOverlay middle{5};

    registry.Add(&foreground);
    registry.Add(&backgroundA);
    registry.Add(&backgroundB);
    registry.Add(&middle);
    registry.Add(&middle); // idempotent re-register
    registry.Add(nullptr); // ignored

    REQUIRE(registry.Items().Size() == 4u);
    CHECK(registry.Items()[0] == &backgroundA);
    CHECK(registry.Items()[1] == &backgroundB);
    CHECK(registry.Items()[2] == &middle);
    CHECK(registry.Items()[3] == &foreground);

    registry.Remove(&backgroundB);
    registry.Remove(&backgroundB); // no-op
    REQUIRE(registry.Items().Size() == 3u);
    CHECK(registry.Items()[0] == &backgroundA);
    CHECK(registry.Items()[1] == &middle);
    CHECK(registry.Items()[2] == &foreground);
    CHECK_FALSE(registry.Contains(&backgroundB));
    CHECK(registry.Contains(&middle));
}

TEST_CASE("LightFalloff matches the forward shader's range window and spot cone")
{
    GpuLight point;
    point.type = 1.0f;
    point.positionWS = Float3{0.0f, 2.0f, 0.0f};
    point.range = 10.0f;
    // d = 0.2 of the range: window (1 - d^4)^2 = 0.99680256, over dist^2 (+1e-4 as in the shader).
    CHECK(LightFalloff(point, Float3{0, 0, 0}) == doctest::Approx(0.99680256f / 4.0001f));
    CHECK(LightFalloff(point, Float3{0, 12.0f, 0}) == doctest::Approx(0.0f)); // at the range: none
    point.range = 0.0f; // no range: the shader returns one, no inverse square either
    CHECK(LightFalloff(point, Float3{0, 0, 0}) == doctest::Approx(1.0f));

    GpuLight spot;
    spot.type = 2.0f;
    spot.directionWS = Float3{0.0f, -1.0f, 0.0f};
    spot.innerCos = Cos(0.3f);
    spot.outerCos = Cos(0.5f);
    CHECK(LightFalloff(spot, Float3{0, -2.0f, 0}) == doctest::Approx(1.0f));              // the axis
    CHECK(LightFalloff(spot, Float3{2.0f * Tan(0.7f), -2.0f, 0}) == doctest::Approx(0.0f)); // outside

    GpuLight sun; // directional: everywhere alike
    CHECK(LightFalloff(sun, Float3{5, 5, 5}) == doctest::Approx(1.0f));
}

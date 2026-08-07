// The scene-agnostic render-data core (no GPU): the frame arena, ExtractedScene snapshot,
// sort keys + radix sort, the RenderView draw-list build (cull/sort), and the view pool.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.render;

using namespace draconic::foundation;
using namespace draconic::render;
namespace rhi = draconic::rhi;

TEST_CASE("FrameArena: allocations are distinct, aligned, and reset reuses chunks")
{
    FrameArena arena;
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
    ExtractedScene scene;
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

TEST_CASE("RenderView::BuildDrawList sorts opaque front-to-back, transparent back-to-front")
{
    // Three meshes in front of an identity camera at view-space depths 2, 5, 8.
    ExtractedScene scene;
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
    RenderViewPool pool;
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
    struct FakeSceneOverlay final : draconic::render::ISceneOverlay
    {
        i32 order = 0;
        explicit FakeSceneOverlay(i32 o) : order(o) {}
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return order; }
        void Render(rhi::RenderPassEncoder&, const draconic::render::SceneOverlayView&) override {}
    };
}

TEST_CASE("overlay views: color-only by default (depthStencilFormat Undefined)")
{
    // Sources must treat Undefined as "no stencil in this pass" and fall back to
    // tessellated fills; a pass that attaches a DS advertises its exact format.
    draconic::render::SceneOverlayView sceneView;
    CHECK(sceneView.depthStencilFormat == draconic::rhi::TextureFormat::Undefined);
    draconic::render::ScreenOverlayView screenView;
    CHECK(screenView.depthStencilFormat == draconic::rhi::TextureFormat::Undefined);
}

TEST_CASE("overlay registry: sorted by order, stable ties, idempotent, removable")
{
    draconic::render::OverlayRegistry<draconic::render::ISceneOverlay> registry;
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

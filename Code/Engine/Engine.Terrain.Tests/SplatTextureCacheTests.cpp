// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The GPU splat caches (engine.terrain :splattexture, top-K model): the weight and index texture
// pair per SplatWeights, keyed by uid (two rasters at the same address must not alias); a paint's
// version bump retires the old pair through the queue (never a direct in-flight destroy); the
// palette Texture2DArray and tileScale-buffer cache keyed by TerrainPaletteData::uid and the scale
// hash; Clear frees at shutdown; and the manager's extract derives both from a weights-bearing
// terrain.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render;
import foundation.scene;
import foundation.heightfield;
import foundation.terrain.resource;
import engine.terrain;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace render = foundation::render;
namespace scene = foundation::scene;
namespace terrain = foundation::terrain;

namespace
{
    // A minimal valid palette blob: `slices` slices of a 4x4 RGBA8 single-mip chain. A mipCount of
    // 3 exercises the chain math (4x4 + 2x2 + 1x1).
    RefPtr<terrain::TerrainPaletteData> MakePalette(u32 slices)
    {
        auto data = MakeRef<terrain::TerrainPaletteData>(DefaultAllocator());
        data->sliceSize = 4;
        data->mipCount = 3;
        data->sliceCount = slices;
        data->texels.Resize(terrain::TerrainPaletteData::SliceBytes(4, 3) * slices, u8{200});
        return data;
    }
}

TEST_CASE("splat cache: GetOrCreate caches the PAIR by uid+version; a bump retires + rebuilds")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    render::GpuRetireQueue retire;
    retire.Initialize(&device, 2);

    engine::terrain::TerrainSplatTextureCache cache;
    cache.SetRetireQueue(&retire);

    RefPtr<terrain::SplatWeights> sw = MakeRef<terrain::SplatWeights>(DefaultAllocator(), 8, 8);
    (void)terrain::PaintTopK(*sw, 0.5f, 0.5f, 0.5f, 0.5f, 3u, 1.0f);

    const engine::terrain::SplatTextureViews v1 = cache.GetOrCreate(device, *sw, sw->Version());
    REQUIRE(v1.weightView != nullptr);
    REQUIRE(v1.indexView != nullptr);
    CHECK(v1.weightView != v1.indexView);
    CHECK(cache.Size() == 1u);
    // The same uid and version return the same views (no rebuild).
    const engine::terrain::SplatTextureViews again = cache.GetOrCreate(device, *sw, sw->Version());
    CHECK(again.weightView == v1.weightView);
    CHECK(again.indexView == v1.indexView);
    CHECK(cache.Size() == 1u);
    CHECK(retire.PendingCount() == 0u);

    // A paint bumps the version: the entry rebuilds in place and both old textures and views are
    // retired (not destroyed), since a submitted frame still samples them through the set-3 group.
    sw->BumpVersion();
    const engine::terrain::SplatTextureViews v2 = cache.GetOrCreate(device, *sw, sw->Version());
    REQUIRE(v2.weightView != nullptr);
    CHECK(v2.weightView != v1.weightView);
    CHECK(v2.indexView != v1.indexView);
    CHECK(cache.Size() == 1u);          // still one entry (rebuilt in place)
    CHECK(retire.PendingCount() == 4u); // 2 textures + 2 views queued, not freed

    // Clear with the queue wired retires the live pair too (a scene destroy mid frame is the same
    // in-flight hazard as the rebuild); the drain frees everything (ASAN).
    cache.Clear(device);
    CHECK(retire.PendingCount() == 8u); // the rebuilt pair joins the retired pair
    retire.Flush();
    retire.Flush();
}

TEST_CASE("splat cache: keyed by uid, so two distinct rasters never alias")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    engine::terrain::TerrainSplatTextureCache cache;

    RefPtr<terrain::SplatWeights> a = MakeRef<terrain::SplatWeights>(DefaultAllocator(), 4, 4);
    RefPtr<terrain::SplatWeights> b = MakeRef<terrain::SplatWeights>(DefaultAllocator(), 4, 4);
    CHECK(a->uid != b->uid);

    const engine::terrain::SplatTextureViews va = cache.GetOrCreate(device, *a, a->Version());
    const engine::terrain::SplatTextureViews vb = cache.GetOrCreate(device, *b, b->Version());
    REQUIRE(va.weightView != nullptr);
    REQUIRE(vb.weightView != nullptr);
    CHECK(va.weightView != vb.weightView);
    CHECK(va.indexView != vb.indexView);
    CHECK(cache.Size() == 2u);

    cache.Clear(device);
    CHECK(cache.Size() == 0u);
}

TEST_CASE("splat cache: an empty raster yields no textures")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    engine::terrain::TerrainSplatTextureCache cache;
    terrain::SplatWeights empty; // default = 0x0
    const engine::terrain::SplatTextureViews views =
        cache.GetOrCreate(device, empty, empty.Version());
    CHECK(views.weightView == nullptr);
    CHECK(views.indexView == nullptr);
    CHECK(cache.Size() == 0u);
}

TEST_CASE("palette cache: keyed by data uid + the tile-scale hash; a scale edit rebuilds")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    render::GpuRetireQueue retire;
    retire.Initialize(&device, 2);

    engine::terrain::TerrainPaletteTextureCache cache;
    cache.SetRetireQueue(&retire);

    RefPtr<terrain::TerrainPaletteData> data = MakePalette(3);
    Array<f32> scales;
    scales.PushBack(4.0f);
    scales.PushBack(8.0f);
    scales.PushBack(2.0f);

    const engine::terrain::PaletteGpu p1 =
        cache.GetOrCreate(device, *data, Span<const f32>{scales.Data(), scales.Size()});
    REQUIRE(p1.arrayView != nullptr);
    REQUIRE(p1.tileScaleBuffer != nullptr);
    CHECK(p1.generation == 1u);
    CHECK(cache.Size() == 1u);
    // The same uid and scales: the same objects (no rebuild).
    const engine::terrain::PaletteGpu same =
        cache.GetOrCreate(device, *data, Span<const f32>{scales.Data(), scales.Size()});
    CHECK(same.arrayView == p1.arrayView);
    CHECK(same.tileScaleBuffer == p1.tileScaleBuffer);
    CHECK(same.generation == p1.generation);
    CHECK(retire.PendingCount() == 0u);

    // A per-layer tile-scale edit (no re-cook: same uid) rebuilds with the old set retired and a
    // bumped generation, which the set-3 bind cache keys on.
    scales[1] = 16.0f;
    const engine::terrain::PaletteGpu p2 =
        cache.GetOrCreate(device, *data, Span<const f32>{scales.Data(), scales.Size()});
    REQUIRE(p2.arrayView != nullptr);
    CHECK(p2.generation == 2u);
    CHECK(cache.Size() == 1u);
    CHECK(retire.PendingCount() == 3u); // old array texture, view and tileScale buffer

    // A palette re-cook makes a new TerrainPaletteData (a new uid), so a second entry.
    RefPtr<terrain::TerrainPaletteData> recooked = MakePalette(3);
    CHECK(recooked->uid != data->uid);
    const engine::terrain::PaletteGpu p3 =
        cache.GetOrCreate(device, *recooked, Span<const f32>{scales.Data(), scales.Size()});
    REQUIRE(p3.arrayView != nullptr);
    CHECK(p3.arrayView != p2.arrayView);
    CHECK(cache.Size() == 2u);

    // Clear with the queue wired retires both live entries (3 objects each) behind the 3 already
    // aging: a scene destroy mid frame never frees a bound array texture.
    cache.Clear(device);
    CHECK(cache.Size() == 0u);
    CHECK(retire.PendingCount() == 9u);
    retire.Flush();
}

TEST_CASE("palette cache: invalid palette data yields nothing")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    engine::terrain::TerrainPaletteTextureCache cache;
    terrain::TerrainPaletteData bad; // zero-sized = invalid
    const engine::terrain::PaletteGpu p = cache.GetOrCreate(device, bad, Span<const f32>{});
    CHECK(p.arrayView == nullptr);
    CHECK(p.tileScaleBuffer == nullptr);
    CHECK(cache.Size() == 0u);
}

TEST_CASE("engine.terrain: a weights-bearing terrain derives the splat pair + palette on extract")
{
    rhi::null::NullDevice device{DefaultAllocator()};

    scene::Scene sceneObj{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(sceneObj);
    auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
    REQUIRE(mgr != nullptr);
    mgr->SetRenderContext(&device, 7, nullptr);

    // In-memory terrain: heightfield (for the draw), CPU weights and a cook-shaped palette.
    RefPtr<foundation::heightfield::Heightfield> grid =
        MakeRef<foundation::heightfield::Heightfield>(DefaultAllocator(), 65,
                                                      Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    RefPtr<terrain::SplatWeights> weights =
        MakeRef<terrain::SplatWeights>(DefaultAllocator(), 32, 32);
    (void)terrain::PaintTopK(*weights, 0.5f, 0.5f, 0.4f, 0.4f, 0u, 1.0f);
    auto res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get();
    res->weights = weights.Get();
    res->palette.PushBack(terrain::TerrainResource::Layer{});
    res->paletteData = MakePalette(1);

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"terrain");
    engine::terrain::TerrainComponent& c = mgr->Add(e);
    c.terrain = res.Get();
    sceneObj.Start();

    foundation::render::ExtractedScene snapshot{DefaultAllocator()};
    mgr->ExtractRenderData(snapshot);
    CHECK(mgr->SplatTextureCount() == 1u);   // the extract built and cached the GPU pair
    CHECK(mgr->PaletteTextureCount() == 1u); // and the palette array and tileScale buffer

    mgr->ClearGpu();
    CHECK(mgr->SplatTextureCount() == 0u); // freed through the live device
    CHECK(mgr->PaletteTextureCount() == 0u);
}

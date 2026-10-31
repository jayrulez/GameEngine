// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SplatWeights (top-K splat model): the pure brush cores and the fixed-layer import.
// Headless (no RHI): PaintTopK convexity (sum + base == 1 within quantum), slot selection
// (existing, then free, then evict the minimum), full-paint one-hot convergence, EraseTopK
// revealing base, dirty-rect bounds, version bumps, and the fixed-layer raster converter
// (renormalized by the texel sum).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.terrain.resource;

using namespace foundation::core;
using namespace foundation::terrain;

namespace
{
    // Sum of the four slot weights at a texel (0..255 domain).
    [[nodiscard]] i32 SlotSum(const SplatWeights& sw, i32 x, i32 y)
    {
        i32 sum = 0;
        for (u32 k = 0; k < kSplatSlotCount; ++k)
        {
            sum += sw.SlotWeight(x, y, k);
        }
        return sum;
    }
}

TEST_CASE("splat weights: an all-zero raster is pure base by construction")
{
    SplatWeights sw(8, 8);
    CHECK(!sw.IsEmpty());
    CHECK(sw.Version() == 1u);
    CHECK(sw.BaseWeight(4, 4) == 255);
    CHECK(SlotSum(sw, 4, 4) == 0);
}

TEST_CASE("splat weights: painting raises the selected PALETTE layer and stays convex")
{
    SplatWeights sw(32, 32);
    const u64 v0 = sw.Version();
    const SplatRegion r = PaintTopK(sw, 0.5f, 0.5f, 0.25f, 0.25f, /*palette*/ 7, 0.6f);
    REQUIRE(!r.IsEmpty());
    CHECK(sw.Version() > v0);

    // The centre gained layer 7; base fell by exactly the gained amount (convex: sum <= 255).
    CHECK(sw.WeightOfLayer(16, 16, 7) > 100);
    CHECK(sw.BaseWeight(16, 16) == 255 - SlotSum(sw, 16, 16));
    for (i32 y = r.minY; y <= r.maxY; ++y)
    {
        for (i32 x = r.minX; x <= r.maxX; ++x)
        {
            CHECK(SlotSum(sw, x, y) <= 255);
        }
    }
    // Outside the dirty rect nothing changed.
    CHECK(SlotSum(sw, 0, 0) == 0);
}

TEST_CASE("splat weights: repeated full-strength painting converges to one-hot")
{
    SplatWeights sw(8, 8);
    for (i32 i = 0; i < 24; ++i)
    {
        (void)PaintTopK(sw, 0.5f, 0.5f, 0.9f, 0.9f, 3, 1.0f);
    }
    CHECK(sw.WeightOfLayer(4, 4, 3) == 255); // pure layer 3
    CHECK(sw.BaseWeight(4, 4) == 0);
}

TEST_CASE("splat weights: painting a second layer fades the first (lerp-to-one-hot)")
{
    SplatWeights sw(8, 8);
    for (i32 i = 0; i < 24; ++i)
    {
        (void)PaintTopK(sw, 0.5f, 0.5f, 0.9f, 0.9f, 1, 1.0f);
    }
    const u8 before = sw.WeightOfLayer(4, 4, 1);
    REQUIRE(before == 255);
    (void)PaintTopK(sw, 0.5f, 0.5f, 0.9f, 0.9f, 2, 0.5f);
    CHECK(sw.WeightOfLayer(4, 4, 1) < before); // layer 1 faded
    CHECK(sw.WeightOfLayer(4, 4, 2) > 0);      // layer 2 rose
    CHECK(SlotSum(sw, 4, 4) <= 255);
}

TEST_CASE("splat weights: a fifth layer evicts the minimum-weight slot (R8)")
{
    SplatWeights sw(4, 4);
    // Fill all four slots with distinct layers at descending strengths.
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 10, 0.50f);
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 11, 0.40f);
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 12, 0.30f);
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 13, 0.20f);
    REQUIRE(sw.WeightOfLayer(2, 2, 13) > 0);
    // Find the minimum-weight layer before the eviction paint.
    u32 minLayer = 10;
    u8 minW = 255;
    for (u32 layer = 10; layer <= 13; ++layer)
    {
        const u8 w = sw.WeightOfLayer(2, 2, layer);
        if (w < minW)
        {
            minW = w;
            minLayer = layer;
        }
    }
    // Painting a fifth layer evicts exactly the min slot.
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 42, 0.6f);
    CHECK(sw.WeightOfLayer(2, 2, 42) > 0);       // the new layer took a slot
    CHECK(sw.WeightOfLayer(2, 2, minLayer) == 0); // the weakest layer was evicted
    CHECK(SlotSum(sw, 2, 2) <= 255);
}

TEST_CASE("splat weights: the eraser fades all layers toward base")
{
    SplatWeights sw(8, 8);
    for (i32 i = 0; i < 10; ++i)
    {
        (void)PaintTopK(sw, 0.5f, 0.5f, 0.9f, 0.9f, 5, 1.0f);
    }
    REQUIRE(sw.BaseWeight(4, 4) < 40);
    for (i32 i = 0; i < 24; ++i)
    {
        (void)EraseTopK(sw, 0.5f, 0.5f, 0.9f, 0.9f, 1.0f);
    }
    CHECK(sw.BaseWeight(4, 4) == 255); // fully erased: pure base again
    CHECK(SlotSum(sw, 4, 4) == 0);
    // Freed slots cleared their index (top-K stays meaningful).
    for (u32 k = 0; k < kSplatSlotCount; ++k)
    {
        CHECK(sw.SlotIndex(4, 4, k) == 0);
    }
}

TEST_CASE("splat weights: palette-remove remap frees the removed layer and shifts the rest (R6)")
{
    SplatWeights sw(8, 8);
    // Three distinct layers in the slots.
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 3, 0.5f);
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 5, 0.3f);
    (void)PaintTopK(sw, 0.5f, 0.5f, 2.0f, 2.0f, 9, 0.2f);
    const u8 w3 = sw.WeightOfLayer(4, 4, 3);
    const u8 w9 = sw.WeightOfLayer(4, 4, 9);
    REQUIRE(sw.WeightOfLayer(4, 4, 5) > 0);
    const u64 v0 = sw.Version();

    // Remove palette layer 5: its weight falls to base; layers above it shift down by one.
    CHECK(RemapOnPaletteRemove(sw, 5));
    CHECK(sw.Version() > v0);
    CHECK(sw.WeightOfLayer(4, 4, 3) == w3); // below the removed index: untouched
    CHECK(sw.WeightOfLayer(4, 4, 5) == 0);  // the removed layer is gone
    CHECK(sw.WeightOfLayer(4, 4, 8) == w9); // 9 became 8
    CHECK(sw.WeightOfLayer(4, 4, 9) == 0);

    // A raster that never referenced the removed layer reports no change.
    SplatWeights clean(4, 4);
    CHECK(!RemapOnPaletteRemove(clean, 5));
}

TEST_CASE("splat weights: the fixed-layer raster import renormalizes by the texel sum (R2)")
{
    // An imported texel whose channel sum drifted from 255: (R,G,B,A) = (100, 60, 40, 0),
    // sum 200. Shares: base = 0.5, palette 0 = 0.3, palette 1 = 0.2.
    Array<u8> legacy;
    legacy.Resize(4 * 4 * 4, u8{0});
    for (usize t = 0; t < 16; ++t)
    {
        legacy[t * 4 + 0] = 100;
        legacy[t * 4 + 1] = 60;
        legacy[t * 4 + 2] = 40;
        legacy[t * 4 + 3] = 0;
    }
    RefPtr<SplatWeights> sw =
        SplatWeightsFromFixedLayerRaster(Span<const u8>{legacy.Data(), legacy.Size()}, 4, 4,
                                         DefaultAllocator());
    REQUIRE(sw.Get() != nullptr);
    REQUIRE(!sw->IsEmpty());
    // G is palette 0 at 0.3 of 255 = 77; B is palette 1 at 0.2 = 51.
    CHECK(sw->WeightOfLayer(1, 1, 0) == 77);
    CHECK(sw->WeightOfLayer(1, 1, 1) == 51);
    CHECK(sw->WeightOfLayer(1, 1, 2) == 0);
    // Base is the normalized R share: 0.5, so 127 or 128 within quantum.
    const u8 base = sw->BaseWeight(1, 1);
    CHECK(base >= 126);
    CHECK(base <= 128);
}

TEST_CASE("splat weights: the fixed-layer raster import maps a solid-R raster to pure base")
{
    // (255,0,0,0) = the full base share everywhere.
    Array<u8> legacy;
    legacy.Resize(4 * 4 * 4, u8{0});
    for (usize t = 0; t < 16; ++t)
    {
        legacy[t * 4 + 0] = 255;
    }
    RefPtr<SplatWeights> sw =
        SplatWeightsFromFixedLayerRaster(Span<const u8>{legacy.Data(), legacy.Size()}, 4, 4,
                                         DefaultAllocator());
    REQUIRE(sw.Get() != nullptr);
    CHECK(sw->BaseWeight(2, 2) == 255);
    CHECK(SlotSum(*sw, 2, 2) == 0);
}

TEST_CASE("splat weights: cooked source round-trips both rasters")
{
    SplatWeights sw(8, 8);
    (void)PaintTopK(sw, 0.5f, 0.5f, 0.5f, 0.5f, 9, 1.0f);

    SplatWeightsSource src;
    SplatWeightsSource::FromWeights(sw, src);
    CHECK(src.width == 8);
    CHECK(src.height == 8);

    RefPtr<SplatWeights> restored =
        src.Build(SplatWeightsSource::IndexBlob(sw), SplatWeightsSource::WeightBlob(sw),
                  DefaultAllocator());
    REQUIRE(restored.Get() != nullptr);
    CHECK(restored->WeightOfLayer(4, 4, 9) == sw.WeightOfLayer(4, 4, 9));
    CHECK(restored->SlotIndex(4, 4, 0) == sw.SlotIndex(4, 4, 0));

    // A blob-size mismatch yields an empty raster, never a malformed one.
    RefPtr<SplatWeights> bad = src.Build(Span<const byte>{}, SplatWeightsSource::WeightBlob(sw), DefaultAllocator());
    REQUIRE(bad.Get() != nullptr);
    CHECK(bad->IsEmpty());
}

TEST_CASE("splat weights: the brush core scales - small cores grade, the default core is flat")
{
    // Soft blending: at low stamp amounts the tool shrinks the flat core, so coverage grades
    // across the radius instead of converging a flat half-radius core against a thin skirt.
    SplatWeights hard(32, 32);
    (void)PaintTopK(hard, 0.5f, 0.5f, 0.25f, 0.25f, 1, 1.0f); // default core 0.5
    SplatWeights soft(32, 32);
    (void)PaintTopK(soft, 0.5f, 0.5f, 0.25f, 0.25f, 1, 1.0f, /*coreFraction*/ 0.0f);

    // Centre: both full.
    CHECK(hard.WeightOfLayer(16, 16, 1) == 255);
    CHECK(soft.WeightOfLayer(16, 16, 1) >= 250); // cosine ~1 at the centre texel
    // Inside the default core (about 35% of the radius): hard is flat full, soft already graded.
    const u8 hardMid = hard.WeightOfLayer(19, 16, 1);
    const u8 softMid = soft.WeightOfLayer(19, 16, 1);
    CHECK(hardMid == 255);
    CHECK(softMid < 220);
    CHECK(softMid > 60); // graded, not culled
}

namespace
{
    // Author a one-hot texel directly (slot 0 = `layer` at full weight).
    void SetOneHot(SplatWeights& sw, i32 x, i32 y, u8 layer)
    {
        const usize at = sw.TexelOffset(x, y);
        sw.Indices()[at] = layer;
        sw.Weights()[at] = 255;
    }
}

TEST_CASE("splat weights: smoothing a uniform region is a no-op")
{
    SplatWeights sw(16, 16);
    for (i32 y = 0; y < 16; ++y)
    {
        for (i32 x = 0; x < 16; ++x)
        {
            SetOneHot(sw, x, y, 3);
        }
    }
    const u64 v0 = sw.Version();
    const SplatRegion r = SmoothTopK(sw, 0.5f, 0.5f, 0.4f, 0.4f, 1.0f);
    CHECK(r.IsEmpty());
    CHECK(sw.Version() == v0);
    CHECK(sw.WeightOfLayer(8, 8, 3) == 255);
}

TEST_CASE("splat weights: smoothing feathers a hard paint-vs-base edge both ways")
{
    SplatWeights sw(16, 16);
    for (i32 y = 0; y < 16; ++y)
    {
        for (i32 x = 0; x < 8; ++x)
        {
            SetOneHot(sw, x, y, 2); // left half = one-hot layer 2, right half = pure base
        }
    }
    const u64 v0 = sw.Version();
    const SplatRegion r = SmoothTopK(sw, 0.5f, 0.5f, 0.45f, 0.45f, 1.0f);
    REQUIRE(!r.IsEmpty());
    CHECK(sw.Version() > v0);

    // The boundary column faded (its 3x3 sees base), the first base column gained paint
    // (its 3x3 sees the layer), and the deep interior is untouched.
    const u8 wEdge = sw.WeightOfLayer(7, 8, 2);
    const u8 wBled = sw.WeightOfLayer(8, 8, 2);
    CHECK(wEdge < 255);
    CHECK(wEdge > 0);
    CHECK(wBled > 0);
    CHECK(wBled < wEdge);
    CHECK(sw.WeightOfLayer(5, 8, 2) == 255); // fully inside: 3x3 all painted
    CHECK(sw.WeightOfLayer(11, 8, 2) == 0);  // fully outside: 3x3 all base

    // Convexity holds everywhere the brush touched.
    for (i32 y = r.minY; y <= r.maxY; ++y)
    {
        for (i32 x = r.minX; x <= r.maxX; ++x)
        {
            CHECK(SlotSum(sw, x, y) <= 255);
        }
    }
}

TEST_CASE("splat weights: smoothing across many layers keeps top-K and convexity")
{
    SplatWeights sw(8, 8);
    // A 3x3 block of nine distinct one-hot layers: every neighborhood union exceeds K = 4.
    u8 layer = 10;
    for (i32 y = 3; y <= 5; ++y)
    {
        for (i32 x = 3; x <= 5; ++x)
        {
            SetOneHot(sw, x, y, layer++);
        }
    }
    const SplatRegion r = SmoothTopK(sw, 0.5625f, 0.5625f, 0.3f, 0.3f, 1.0f);
    REQUIRE(!r.IsEmpty());
    for (i32 y = r.minY; y <= r.maxY; ++y)
    {
        for (i32 x = r.minX; x <= r.maxX; ++x)
        {
            CHECK(SlotSum(sw, x, y) <= 255);
            // Every occupied slot names a real layer; freed slots are fully cleared.
            for (u32 k = 0; k < kSplatSlotCount; ++k)
            {
                if (sw.SlotWeight(x, y, k) == 0)
                {
                    CHECK(sw.SlotIndex(x, y, k) == 0);
                }
            }
        }
    }
    // The centre texel kept at most K layers of its 9-layer union, and gave weight to base.
    CHECK(SlotSum(sw, 4, 4) < 255);
    CHECK(SlotSum(sw, 4, 4) > 0);
}

TEST_CASE("splat weights: smoothing outside the brush leaves texels untouched")
{
    SplatWeights sw(32, 32);
    for (i32 y = 0; y < 32; ++y)
    {
        for (i32 x = 0; x < 16; ++x)
        {
            SetOneHot(sw, x, y, 1);
        }
    }
    const SplatRegion r = SmoothTopK(sw, 0.5f, 0.5f, 0.15f, 0.15f, 1.0f);
    REQUIRE(!r.IsEmpty());
    CHECK(r.minX >= 10);
    CHECK(r.maxX <= 21);
    CHECK(sw.WeightOfLayer(15, 2, 1) == 255); // same edge, far row: outside the disc
    CHECK(sw.WeightOfLayer(16, 2, 1) == 0);
}

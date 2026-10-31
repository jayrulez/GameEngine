// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation.resource: the mask raster: planes start empty, paint raises toward 255
// and converges, erase fades to 0, smooth feathers a seam, planes are independent, a brush op
// reports its region and bumps the version, and the cooked source round-trips the blob.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vegetation.resource;

using namespace foundation::core;
using namespace foundation::vegetation;

TEST_CASE("vegetation mask: a fresh mask is empty everywhere, planes and texels bounds-checked")
{
    VegetationMask mask(16, 8, 3);
    CHECK(!mask.IsEmpty());
    CHECK(mask.Width() == 16);
    CHECK(mask.Height() == 8);
    CHECK(mask.PlaneCount() == 3u);
    CHECK(mask.Densities().Size() == 16u * 8u * 3u);
    CHECK(mask.Plane(2).Size() == 128u);
    CHECK(mask.Plane(3).IsEmpty()); // out of range
    for (u32 p = 0; p < 3; ++p)
    {
        CHECK(mask.DensityAt(p, 5, 5) == 0);
        CHECK(mask.ShareAt(p, 0.3f, 0.7f) == 0.0f);
    }
    CHECK(mask.DensityAt(9, 0, 0) == 0);
    CHECK(mask.DensityAt(0, -1, 0) == 0);
    CHECK(mask.DensityAt(0, 16, 0) == 0);
    mask.SetDensity(1, 3, 2, 200);
    CHECK(mask.DensityAt(1, 3, 2) == 200);
    CHECK(mask.DensityAt(0, 3, 2) == 0); // another plane untouched
    mask.SetDensity(7, 0, 0, 9);          // out of range: ignored
    CHECK(mask.ShareAt(1, 3.0f / 15.0f, 2.0f / 7.0f) == doctest::Approx(200.0f / 255.0f));

    VegetationMask none;
    CHECK(none.IsEmpty());
    CHECK(none.Plane(0).IsEmpty());
    // Plane count is clamped to the cap; zero means one.
    CHECK(VegetationMask(2, 2, 0).PlaneCount() == 1u);
    CHECK(VegetationMask(2, 2, 99).PlaneCount() == kMaxMaskPlanes);
}

TEST_CASE("vegetation mask: painting raises the plane inside the disc, converges to 255, reports the region")
{
    VegetationMask mask(32, 32, 2);
    const u64 v0 = mask.Version();
    const MaskRegion r = PaintMask(mask, 0, 0.5f, 0.5f, 0.25f, 0.25f, 0.5f);
    REQUIRE(!r.IsEmpty());
    CHECK(mask.Version() == v0 + 1);
    CHECK(r.minX >= 7);
    CHECK(r.maxX <= 24);
    CHECK(r.minY >= 7);
    CHECK(r.maxY <= 24);
    const u8 centre = mask.DensityAt(0, 16, 16);
    CHECK(centre >= 127); // half strength at the flat core: 0 + 0.5 * 255
    CHECK(centre <= 129);
    CHECK(mask.DensityAt(0, 2, 2) == 0);   // outside the disc
    CHECK(mask.DensityAt(1, 16, 16) == 0); // the other plane
    CHECK(mask.DensityAt(0, 16, 9) < centre); // the cosine skirt grades toward the rim
    CHECK(mask.DensityAt(0, 16, 9) > 0);

    // Repeated full-strength painting converges to 255 at the core.
    for (i32 i = 0; i < 4; ++i)
    {
        (void)PaintMask(mask, 0, 0.5f, 0.5f, 0.25f, 0.25f, 1.0f);
    }
    CHECK(mask.DensityAt(0, 16, 16) == 255);
    // A saturated stamp changes nothing: empty region, no version bump.
    const u64 v1 = mask.Version();
    const MaskRegion again = PaintMask(mask, 0, 0.5f, 0.5f, 0.02f, 0.02f, 1.0f);
    CHECK(again.IsEmpty());
    CHECK(mask.Version() == v1);
    // A plane out of range, a zero amount or a zero radius are no-ops.
    CHECK(PaintMask(mask, 5, 0.5f, 0.5f, 0.25f, 0.25f, 1.0f).IsEmpty());
    CHECK(PaintMask(mask, 1, 0.5f, 0.5f, 0.25f, 0.25f, 0.0f).IsEmpty());
    CHECK(PaintMask(mask, 1, 0.5f, 0.5f, 0.0f, 0.25f, 1.0f).IsEmpty());
    CHECK(mask.DensityAt(1, 16, 16) == 0);
}

TEST_CASE("vegetation mask: the eraser fades to 0 and smoothing feathers a hard edge both ways")
{
    VegetationMask mask(32, 32, 1);
    // A hard half: the left 16 columns at 255.
    for (i32 y = 0; y < 32; ++y)
    {
        for (i32 x = 0; x < 16; ++x)
        {
            mask.SetDensity(0, x, y, 255);
        }
    }
    const u64 v0 = mask.Version();
    const MaskRegion s = SmoothMask(mask, 0, 0.5f, 0.5f, 0.2f, 0.2f, 1.0f, 0.95f);
    REQUIRE(!s.IsEmpty());
    CHECK(mask.Version() == v0 + 1);
    CHECK(mask.DensityAt(0, 15, 16) < 255); // the painted side softened
    CHECK(mask.DensityAt(0, 16, 16) > 0);   // the empty side gained
    CHECK(mask.DensityAt(0, 15, 16) > mask.DensityAt(0, 16, 16)); // still ordered
    CHECK(mask.DensityAt(0, 2, 16) == 255); // far from the brush: untouched
    CHECK(mask.DensityAt(0, 30, 16) == 0);
    // Smoothing a uniform region is a no-op.
    CHECK(SmoothMask(mask, 0, 0.1f, 0.1f, 0.05f, 0.05f, 1.0f).IsEmpty());

    // Erase: half strength halves, full strength reaches exactly 0.
    (void)EraseMask(mask, 0, 0.25f, 0.5f, 0.1f, 0.1f, 0.5f, 0.95f);
    CHECK(mask.DensityAt(0, 8, 16) == 128);
    for (i32 i = 0; i < 3; ++i)
    {
        (void)EraseMask(mask, 0, 0.25f, 0.5f, 0.1f, 0.1f, 1.0f, 0.95f);
    }
    CHECK(mask.DensityAt(0, 8, 16) == 0);
    CHECK(EraseMask(mask, 3, 0.25f, 0.5f, 0.1f, 0.1f, 1.0f).IsEmpty()); // plane out of range
}

TEST_CASE("vegetation mask: the cooked source round-trips every plane; a bad blob yields an empty mask")
{
    VegetationMask authored(8, 4, 3);
    (void)PaintMask(authored, 1, 0.5f, 0.5f, 0.4f, 0.4f, 1.0f);
    (void)PaintMask(authored, 2, 0.2f, 0.5f, 0.2f, 0.4f, 0.5f);
    VegetationMaskSource src;
    VegetationMaskSource::FromMask(authored, src);
    CHECK(src.width == 8);
    CHECK(src.height == 4);
    CHECK(src.planeCount == 3u);
    const Span<const byte> blob = VegetationMaskSource::DensityBlob(authored);
    CHECK(blob.Size() == 8u * 4u * 3u);

    RefPtr<VegetationMask> loaded = src.Build(blob, DefaultAllocator());
    REQUIRE(loaded.Get() != nullptr);
    CHECK(loaded->PlaneCount() == 3u);
    for (u32 p = 0; p < 3; ++p)
    {
        for (i32 y = 0; y < 4; ++y)
        {
            for (i32 x = 0; x < 8; ++x)
            {
                CHECK(loaded->DensityAt(p, x, y) == authored.DensityAt(p, x, y));
            }
        }
    }
    CHECK(loaded->uid != authored.uid); // a fresh object

    // A size mismatch or bad dims give an empty mask, never a malformed one.
    CHECK(src.Build(Span<const byte>{blob.Data(), blob.Size() - 1}, DefaultAllocator())->IsEmpty());
    VegetationMaskSource bad;
    VegetationMaskSource::FromMask(authored, bad);
    bad.planeCount = 0;
    CHECK(bad.Build(blob, DefaultAllocator())->IsEmpty());
    VegetationMaskSource::FromMask(authored, bad);
    bad.width = -1;
    CHECK(bad.Build(blob, DefaultAllocator())->IsEmpty());
    // The stream name is the sidecar contract the builder and the editor persist share.
    CHECK(kVegetationMaskStream == u8"densities");
}

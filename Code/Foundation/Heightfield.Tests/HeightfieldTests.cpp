// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.heightfield: the pure grid and sampling math. Covers the size contract, exact
// grid-point and bilinear-midpoint sampling, edge clamp, the world-to-grid round trip, normals, cell
// bounds, and the ray-march query (the height source the editor brushes and physics share).
// Heightfield is an Object (a resource product), so it is held through a RefPtr.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.heightfield;

using namespace foundation::core;
using namespace foundation::heightfield;

namespace
{
    // A flat-then-ramped fixture: a 65-grid over a 64x64 world, Y range [0, 10], with sample values
    // that increase linearly along +X (so world height is a known ramp).
    RefPtr<Heightfield> MakeRampX()
    {
        RefPtr<Heightfield> hf = MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f},
                                                      0.0f, 10.0f);
        for (i32 z = 0; z < 65; ++z)
        {
            for (i32 x = 0; x < 65; ++x)
            {
                // sample increases with x: 0 at x=0, 65535 at x=64.
                const f32 t = static_cast<f32>(x) / 64.0f;
                hf->SetSample(x, z, static_cast<Height>(t * 65535.0f + 0.5f));
            }
        }
        return hf;
    }

    bool Near(f32 a, f32 b, f32 eps = 1.0e-2f) { return Abs(a - b) <= eps; }
}

TEST_CASE("heightfield: size contract - square 64k+1 only")
{
    CHECK(IsValidSize(65));
    CHECK(IsValidSize(129));
    CHECK(IsValidSize(257));
    CHECK(IsValidSize(1025));
    CHECK_FALSE(IsValidSize(64));  // one short
    CHECK_FALSE(IsValidSize(66));  // not 64k+1
    CHECK_FALSE(IsValidSize(1));   // too small
    CHECK_FALSE(IsValidSize(129 - 1));

    CHECK(NextValidSize(1) == 65);
    CHECK(NextValidSize(65) == 65);
    CHECK(NextValidSize(66) == 129);
    CHECK(NextValidSize(129) == 129);
    CHECK(NextValidSize(200) == 257);
}

TEST_CASE("heightfield: empty default is inert")
{
    RefPtr<Heightfield> owner = MakeRef<Heightfield>(DefaultAllocator());
    const Heightfield& hf = *owner;
    CHECK(hf.IsEmpty());
    CHECK(hf.Size() == 0);
    CHECK(hf.GetHeightAt(0.0f, 0.0f) == 0.0f);
}

TEST_CASE("heightfield: construction + raw sample round-trip")
{
    RefPtr<Heightfield> owner =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, -5.0f, 5.0f);
    Heightfield& hf = *owner;
    CHECK_FALSE(hf.IsEmpty());
    CHECK(hf.Size() == 65);
    CHECK(hf.Samples().Size() == 65u * 65u);
    CHECK(hf.MinY() == -5.0f);
    CHECK(hf.MaxY() == 5.0f);

    hf.SetSample(3, 7, 40000);
    CHECK(hf.GetSample(3, 7) == 40000);
    // Untouched neighbours stay zero.
    CHECK(hf.GetSample(4, 7) == 0);
}

TEST_CASE("heightfield: quantization maps u16 across the world Y range")
{
    RefPtr<Heightfield> owner =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    const Heightfield& hf = *owner;
    CHECK(Near(hf.SampleToWorldY(0.0f), 0.0f));
    CHECK(Near(hf.SampleToWorldY(65535.0f), 10.0f));
    CHECK(Near(hf.SampleToWorldY(32767.5f), 5.0f));

    CHECK(hf.WorldYToSample(0.0f) == 0);
    CHECK(hf.WorldYToSample(10.0f) == 65535);
    // Clamps out-of-range.
    CHECK(hf.WorldYToSample(-1.0f) == 0);
    CHECK(hf.WorldYToSample(99.0f) == 65535);
}

TEST_CASE("heightfield: exact grid-point heights")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    // At x=0 (world -32), height 0; at x=64 (world +32), height 10.
    CHECK(Near(hf.GetHeightAtGrid(0, 0), 0.0f));
    CHECK(Near(hf.GetHeightAtGrid(64, 0), 10.0f));
    CHECK(Near(hf.GetHeightAtGrid(32, 0), 5.0f));
}

TEST_CASE("heightfield: bilinear midpoint sampling")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    // World x=0 is the grid center (x=32) -> ~5. A midpoint between two columns interpolates.
    CHECK(Near(hf.GetHeightAt(0.0f, 0.0f), 5.0f));
    CHECK(Near(hf.GetHeightAt(-32.0f, 0.0f), 0.0f));
    CHECK(Near(hf.GetHeightAt(32.0f, 0.0f), 10.0f));
    // Halfway between world x=-32 and x=0 is x=-16 -> ~2.5.
    CHECK(Near(hf.GetHeightAt(-16.0f, 0.0f), 2.5f));
}

TEST_CASE("heightfield: sampling clamps at the edges")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    // Far outside the footprint clamps to the nearest edge column.
    CHECK(Near(hf.GetHeightAt(-1000.0f, 0.0f), 0.0f));
    CHECK(Near(hf.GetHeightAt(1000.0f, 0.0f), 10.0f));
}

TEST_CASE("heightfield: world<->grid round-trip")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    const f32 samples[] = {-32.0f, -10.0f, 0.0f, 15.5f, 32.0f};
    for (f32 wx : samples)
    {
        const Float2 g = hf.WorldToGrid(wx, wx);
        const Float2 w = hf.GridToWorld(g.x, g.y);
        CHECK(Near(w.x, wx, 1.0e-3f));
        CHECK(Near(w.y, wx, 1.0e-3f));
    }
    // Corners map to grid extremes.
    const Float2 g0 = hf.WorldToGrid(-32.0f, -32.0f);
    CHECK(Near(g0.x, 0.0f));
    CHECK(Near(g0.y, 0.0f));
    const Float2 g1 = hf.WorldToGrid(32.0f, 32.0f);
    CHECK(Near(g1.x, 64.0f));
    CHECK(Near(g1.y, 64.0f));
}

TEST_CASE("heightfield: flat field has an up normal, a ramp tilts")
{
    RefPtr<Heightfield> flatOwner =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    Heightfield& flat = *flatOwner;
    for (i32 i = 0; i < flat.Size() * flat.Size(); ++i)
    {
        flat.Samples()[static_cast<usize>(i)] = 30000;
    }
    const Float3 up = flat.GetNormalAt(0.0f, 0.0f);
    CHECK(Near(up.x, 0.0f));
    CHECK(Near(up.y, 1.0f));
    CHECK(Near(up.z, 0.0f));

    RefPtr<Heightfield> rampOwner = MakeRampX();
    const Float3 n = rampOwner->GetNormalAt(0.0f, 0.0f);
    CHECK(n.x < 0.0f);        // surface rises toward +X, so the normal tilts toward -X
    CHECK(n.y > 0.0f);
    CHECK(Near(n.z, 0.0f));
}

TEST_CASE("heightfield: cell bounds report the min/max over a block")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    f32 lo = 0.0f;
    f32 hi = 0.0f;
    hf.CellBounds(0, 0, 64, 64, lo, hi); // whole grid
    CHECK(Near(lo, 0.0f));
    CHECK(Near(hi, 10.0f));

    hf.CellBounds(0, 0, 0, 64, lo, hi); // first column only -> all 0
    CHECK(Near(lo, 0.0f));
    CHECK(Near(hi, 0.0f));

    hf.CellBounds(64, 0, 64, 64, lo, hi); // last column only -> all 10
    CHECK(Near(lo, 10.0f));
    CHECK(Near(hi, 10.0f));
}

TEST_CASE("heightfield: ray march - straight-down ray hits the surface height")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    // Drop from high above world (0,0): surface there is ~5.
    f32 t = 0.0f;
    REQUIRE(hf.QueryRay(Float3{0.0f, 100.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    const f32 hitY = 100.0f - t; // dir is -Y unit
    CHECK(Near(hitY, 5.0f, 5.0e-2f));

    // Over the tall edge (world +32) the surface is ~10.
    REQUIRE(hf.QueryRay(Float3{31.9f, 100.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    CHECK(Near(100.0f - t, 10.0f, 1.0e-1f));
}

TEST_CASE("heightfield: ray march - a ray that misses the footprint returns false")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    f32 t = 0.0f;
    // Straight down but far outside the XZ footprint.
    CHECK_FALSE(hf.QueryRay(Float3{1000.0f, 100.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    // Just outside the authored extent (footprint is [-32, 32]) also misses.
    CHECK_FALSE(hf.QueryRay(Float3{32.5f, 100.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    // Pointing up, away from the surface, from above.
    CHECK_FALSE(hf.QueryRay(Float3{0.0f, 100.0f, 0.0f}, Float3{0.0f, 1.0f, 0.0f}, t));
}

TEST_CASE("heightfield: ray march - an angled ray descends onto the ramp")
{
    RefPtr<Heightfield> owner = MakeRampX();
    const Heightfield& hf = *owner;
    // From above the low edge, angled toward +X and down; must land on the surface.
    f32 t = 0.0f;
    REQUIRE(hf.QueryRay(Float3{-30.0f, 20.0f, 0.0f}, Float3{1.0f, -1.0f, 0.0f}, t));
    const Float3 hit{-30.0f + 0.70710678f * t, 20.0f - 0.70710678f * t, 0.0f};
    CHECK(Near(hit.y, hf.GetHeightAt(hit.x, hit.z), 5.0e-2f));
}

TEST_CASE("heightfield sculpt: raise lifts the center most, bumps version, reports the region")
{
    // 129 grid over 128x128 world, Y range [0,100], flat at sample 0.
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    const u64 v0 = h->Version();

    // Raise +10 world units within a 20-unit radius at the origin (grid center 64,64).
    const HeightfieldRegion r = SculptRaise(*h, 0.0f, 0.0f, 20.0f, 10.0f);
    CHECK_FALSE(r.IsEmpty());
    CHECK(h->Version() == v0 + 1);                 // one re-upload signal per stroke step
    CHECK(r.minX <= 64);
    CHECK(r.maxX >= 64);

    const f32 center = h->GetHeightAtGrid(64, 64); // ~+10 (full falloff weight at center)
    const f32 edge = h->GetHeightAtGrid(74, 64);   // ~half radius -> partial
    CHECK(center > 9.0f);
    CHECK(center <= 10.01f);
    CHECK(edge > 0.0f);
    CHECK(edge < center);                          // cosine falloff: rim raised less than center
    CHECK(h->GetHeightAtGrid(0, 0) == doctest::Approx(0.0f)); // outside the radius: untouched
}

TEST_CASE("heightfield sculpt: negative strength lowers; clamps at the floor")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    for (i32 z = 0; z < 129; ++z)
        for (i32 x = 0; x < 129; ++x)
            h->SetSample(x, z, static_cast<Height>(30000)); // a mid plateau

    SculptRaise(*h, 0.0f, 0.0f, 20.0f, -1000.0f); // lower hard -> clamps to 0
    CHECK(h->GetSample(64, 64) == 0);
}

TEST_CASE("heightfield sculpt: flatten pulls toward the target height")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    // start high, flatten toward 25 with full amount
    for (i32 z = 0; z < 129; ++z)
        for (i32 x = 0; x < 129; ++x)
            h->SetSample(x, z, h->WorldYToSample(80.0f));

    SculptFlatten(*h, 0.0f, 0.0f, 20.0f, 1.0f, 25.0f);
    CHECK(h->GetHeightAtGrid(64, 64) == doctest::Approx(25.0f).epsilon(0.02)); // center reaches target
    CHECK(h->GetHeightAtGrid(64, 64) < 80.0f);
}

TEST_CASE("heightfield sculpt: smooth reduces a one-cell spike")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    h->SetSample(64, 64, h->WorldYToSample(90.0f)); // a lone spike on a flat field
    const f32 before = h->GetHeightAtGrid(64, 64);

    SculptSmooth(*h, 0.0f, 0.0f, 20.0f, 1.0f);
    const f32 after = h->GetHeightAtGrid(64, 64);
    CHECK(after < before);      // the spike is pulled toward its (0) neighbourhood
    CHECK(after >= 0.0f);
}

TEST_CASE("heightfield sculpt: a brush entirely off-grid touches nothing")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    const u64 v0 = h->Version();
    const HeightfieldRegion r = SculptRaise(*h, 100000.0f, 100000.0f, 5.0f, 10.0f);
    CHECK(r.IsEmpty());
    CHECK(h->Version() == v0); // no change -> no re-upload signal
}

// ---- holes: a per-sample cut; a triangle with a hole vertex is gone ----

TEST_CASE("heightfield holes: a fresh grid is solid; SetHole counts, SetHoles replaces and refuses a wrong size")
{
    RefPtr<Heightfield> h = MakeRampX();
    CHECK_FALSE(h->HasHoles());
    CHECK(h->HoleCount() == 0u);
    CHECK(h->Holes().Size() == 65u * 65u);
    h->SetHole(3, 3, true);
    h->SetHole(3, 3, true); // idempotent: counted once
    CHECK(h->HoleCount() == 1u);
    CHECK(h->IsHole(3, 3));
    CHECK(h->Holes()[3 + 3 * 65] == 255);
    h->SetHole(3, 3, false);
    CHECK_FALSE(h->HasHoles());
    Array<u8> plane;
    plane.Resize(65u * 65u);
    plane[10] = 1; // any non-zero byte is a cut, normalised to 255
    CHECK(h->SetHoles(Span<const u8>(plane.Data(), plane.Size())));
    CHECK(h->HoleCount() == 1u);
    CHECK(h->Holes()[10] == 255);
    Array<u8> wrong;
    wrong.Resize(9);
    CHECK_FALSE(h->SetHoles(Span<const u8>(wrong.Data(), wrong.Size())));
    CHECK(h->HoleCount() == 1u); // untouched by the refusal
}

TEST_CASE("heightfield holes: CellHasHole is any corner, BlockHasHole any sample in the block, both clamped")
{
    RefPtr<Heightfield> h = MakeRampX();
    h->SetHole(10, 10, true);
    // The four cells sharing sample (10,10) lose their triangles; a cell one away does not.
    CHECK(h->CellHasHole(9, 9));
    CHECK(h->CellHasHole(10, 10));
    CHECK(h->CellHasHole(9, 10));
    CHECK(h->CellHasHole(10, 9));
    CHECK_FALSE(h->CellHasHole(11, 11));
    CHECK_FALSE(h->CellHasHole(8, 8));
    // A coarse block sees a cut anywhere inside it, including its interior.
    CHECK(h->BlockHasHole(8, 8, 12, 12));
    CHECK(h->BlockHasHole(10, 10, 10, 10));
    CHECK_FALSE(h->BlockHasHole(11, 11, 20, 20));
    CHECK_FALSE(h->BlockHasHole(0, 0, 9, 9));
    // Past the grid the indices clamp: a cut on the last sample is seen from beyond it.
    h->SetHole(64, 64, true);
    CHECK(h->CellHasHole(64, 64));
    CHECK(h->BlockHasHole(70, 70, 90, 90));
    i32 cx = 0, cz = 0;
    h->CellOfLocal(100.0f, 100.0f, cx, cz); // far outside: the last cell
    CHECK(cx == 63);
    CHECK(cz == 63);
    h->CellOfLocal(-31.5f, -31.5f, cx, cz); // world (-32,-32) is sample (0,0): the first cell
    CHECK(cx == 0);
    CHECK(cz == 0);
}

TEST_CASE("heightfield holes: the cut brush has a hard edge, bumps the version once, reports the region; fill restores")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 100.0f);
    const u64 v0 = h->Version();
    const HeightfieldRegion region = CutHoles(*h, 0.0f, 0.0f, 3.0f); // 1 m cells: radius 3
    CHECK_FALSE(region.IsEmpty());
    CHECK(h->Version() == v0 + 1);
    CHECK(h->HasHoles());
    // The centre and a sample 2 m out are cut (inside); a sample 3 m out is on the rim: not.
    CHECK(h->IsHole(64, 64));
    CHECK(h->IsHole(66, 64));
    CHECK_FALSE(h->IsHole(67, 64));
    CHECK_FALSE(h->IsHole(64, 68));
    // Every cut sample lies inside the reported rect.
    for (i32 z = 0; z < 129; ++z)
    {
        for (i32 x = 0; x < 129; ++x)
        {
            if (h->IsHole(x, z))
            {
                CHECK(x >= region.minX);
                CHECK(x <= region.maxX);
                CHECK(z >= region.minZ);
                CHECK(z <= region.maxZ);
            }
        }
    }
    // Cutting the same disc again changes nothing and bumps nothing.
    (void)CutHoles(*h, 0.0f, 0.0f, 3.0f);
    CHECK(h->Version() == v0 + 1);
    // Fill restores and bumps once; a fill over solid ground bumps nothing.
    (void)FillHoles(*h, 0.0f, 0.0f, 3.0f);
    CHECK_FALSE(h->HasHoles());
    CHECK(h->Version() == v0 + 2);
    (void)FillHoles(*h, 0.0f, 0.0f, 3.0f);
    CHECK(h->Version() == v0 + 2);
    // A disc past the edge marks the edge samples and never wraps.
    (void)CutHoles(*h, 64.0f, 64.0f, 2.0f);
    CHECK(h->IsHole(128, 128));
    CHECK_FALSE(h->IsHole(0, 0));
}

TEST_CASE("heightfield holes: a ray through a cut cell misses and one beside it hits; an entry under a cut is no hit")
{
    RefPtr<Heightfield> h =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    for (i32 z = 0; z < 65; ++z)
    {
        for (i32 x = 0; x < 65; ++x)
        {
            h->SetSample(x, z, 32768); // a flat field at ~5 m
        }
    }
    f32 t = 0.0f;
    REQUIRE(h->QueryRay(Float3{0.0f, 20.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    (void)CutHoles(*h, 0.0f, 0.0f, 2.5f); // a hole around the origin (1 m cells)
    CHECK_FALSE(h->QueryRay(Float3{0.0f, 20.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t)); // through it
    REQUIRE(h->QueryRay(Float3{10.0f, 20.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));   // beside it
    CHECK(Near(20.0f - t, 5.0f, 5.0e-2f));
    // An angled ray that crosses the surface inside the cut goes on and lands where the field
    // is solid again (past the hole's rim on the far side).
    REQUIRE(h->QueryRay(Float3{-6.0f, 5.5f, 0.0f}, Float3{1.0f, -0.05f, 0.0f}, t));
    const f32 hitX = -6.0f + t * (1.0f / Sqrt(1.0f + 0.05f * 0.05f));
    CHECK(hitX > 2.5f); // not inside the cut
    // Starting under the surface inside the cut (a cave) is no hit at the entry.
    CHECK_FALSE(h->QueryRay(Float3{0.0f, 2.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    // The brushes' pick treats the cut as surface: the same ray lands on the plane at 5 m.
    REQUIRE(h->QueryRayIgnoringHoles(Float3{0.0f, 20.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, t));
    CHECK(Near(20.0f - t, 5.0f, 5.0e-2f));
    // An angled ray whose crossing lies inside the cut (x ~ -1): the surface rule passes through
    // and never comes back above the field (no hit); the brushes' rule lands on the plane there.
    CHECK_FALSE(h->QueryRay(Float3{-6.0f, 5.25f, 0.0f}, Float3{1.0f, -0.05f, 0.0f}, t));
    REQUIRE(h->QueryRayIgnoringHoles(Float3{-6.0f, 5.25f, 0.0f}, Float3{1.0f, -0.05f, 0.0f}, t));
    const f32 insideX = -6.0f + t * (1.0f / Sqrt(1.0f + 0.05f * 0.05f));
    CHECK(insideX > -2.5f);
    CHECK(insideX < 2.5f);
}


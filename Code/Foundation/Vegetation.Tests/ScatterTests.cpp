// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation: the scatter's contract: determinism per seed, density to count, the
// splat / slope / height rules, normal alignment, the fade math, chunk touch mapping, the cap.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <initializer_list>

import foundation.core;
import foundation.heightfield;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import foundation.vegetation;

using namespace foundation::core;
namespace hf = foundation::heightfield;
namespace tmodel = foundation::terrain;
namespace veg = foundation::vegetation;

namespace
{
    constexpr i32 kGrid = 65;     // one 64-quad chunk
    constexpr f32 kWorld = 64.0f; // 1 m per quad: the chunk is 64 x 64 m = 4096 m^2

    // A flat field at `height` (world Y range 0..40).
    RefPtr<hf::Heightfield> MakeFlat(f32 height)
    {
        auto grid = MakeRef<hf::Heightfield>(DefaultAllocator(), kGrid, Float2{kWorld, kWorld},
                                             0.0f, 40.0f);
        const hf::Height sample = grid->WorldYToSample(height);
        for (i32 z = 0; z < kGrid; ++z)
        {
            for (i32 x = 0; x < kGrid; ++x)
            {
                grid->SetSample(x, z, sample);
            }
        }
        return grid;
    }

    // A plane rising with x: 0 at the -x edge, `rise` at the +x edge (a constant slope).
    RefPtr<hf::Heightfield> MakeRamp(f32 rise)
    {
        auto grid = MakeRef<hf::Heightfield>(DefaultAllocator(), kGrid, Float2{kWorld, kWorld},
                                             0.0f, 40.0f);
        for (i32 z = 0; z < kGrid; ++z)
        {
            for (i32 x = 0; x < kGrid; ++x)
            {
                const f32 t = static_cast<f32>(x) / static_cast<f32>(kGrid - 1);
                grid->SetSample(x, z, grid->WorldYToSample(t * rise));
            }
        }
        return grid;
    }

    // The single chunk of a 65-sample grid.
    tmodel::TerrainChunk ChunkOf(const hf::Heightfield& grid)
    {
        Array<tmodel::TerrainChunk> chunks;
        tmodel::BuildChunks(grid, chunks);
        REQUIRE(chunks.Size() == 1u);
        return chunks[0];
    }

    // A splat raster: palette layer 0 one-hot on the left half (x < 0), base on the right.
    RefPtr<tmodel::SplatWeights> MakeHalfSplat()
    {
        constexpr i32 n = 32;
        auto sw = MakeRef<tmodel::SplatWeights>(DefaultAllocator(), n, n);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        for (i32 y = 0; y < n; ++y)
        {
            for (i32 x = 0; x < n / 2; ++x)
            {
                const usize at = sw->TexelOffset(x, y);
                idx[at + 0] = 0;
                wts[at + 0] = 255;
            }
        }
        sw->BumpVersion();
        return sw;
    }

    veg::ScatterLayer Uniform(f32 density)
    {
        veg::ScatterLayer layer;
        layer.placement = veg::VegetationPlacement::Uniform;
        layer.density = density;
        layer.maxSlopeDegrees = 90.0f;
        return layer;
    }

    bool SameTransforms(const Array<Float4x4>& a, const Array<Float4x4>& b)
    {
        if (a.Size() != b.Size())
        {
            return false;
        }
        for (usize i = 0; i < a.Size(); ++i)
        {
            if (MemCompare(&a[i], &b[i], sizeof(Float4x4)) != 0)
            {
                return false;
            }
        }
        return true;
    }

    Float3 Row(const Float4x4& m, u32 r)
    {
        return Float3{m.m[r][0], m.m[r][1], m.m[r][2]};
    }
}

TEST_CASE("vegetation scatter: a seed gives byte-identical instances; another chunk or layer differs")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(5.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*grid);
    const veg::ScatterLayer layer = Uniform(0.25f);
    const Guid layerId{0x1234u, 0x5678u};
    const u64 seed = veg::ChunkSeed(layerId, 0, 0, 0);

    veg::ScatterResult a;
    veg::ScatterResult b;
    veg::ScatterChunk(seed, chunk, *grid, nullptr, nullptr, layer, AABB::Empty(), a);
    veg::ScatterChunk(seed, chunk, *grid, nullptr, nullptr, layer, AABB::Empty(), b);
    REQUIRE(!a.transforms.IsEmpty());
    CHECK(SameTransforms(a.transforms, b.transforms));
    // Every instance sits on the surface, inside the chunk footprint.
    for (const Float4x4& m : a.transforms)
    {
        CHECK(m.m[3][1] == doctest::Approx(5.0f).epsilon(0.01));
        CHECK(m.m[3][0] >= chunk.bounds.min.x);
        CHECK(m.m[3][0] <= chunk.bounds.max.x);
        CHECK(m.m[3][2] >= chunk.bounds.min.z);
        CHECK(m.m[3][2] <= chunk.bounds.max.z);
    }

    // The seed is the identity: a neighbouring chunk, another layer index or another owner
    // scatters differently.
    CHECK(veg::ChunkSeed(layerId, 0, 1, 0) != seed);
    CHECK(veg::ChunkSeed(layerId, 0, 0, 1) != seed);
    CHECK(veg::ChunkSeed(layerId, 1, 0, 0) != seed);
    CHECK(veg::ChunkSeed(Guid{0x1234u, 0x5679u}, 0, 0, 0) != seed);
    veg::ScatterResult c;
    veg::ScatterChunk(veg::ChunkSeed(layerId, 0, 1, 0), chunk, *grid, nullptr, nullptr, layer,
                      AABB::Empty(), c);
    CHECK(!SameTransforms(a.transforms, c.transforms));
}

TEST_CASE("vegetation scatter: density scales the candidate count; the per-chunk cap stops placing when the chunk is full")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(1.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*grid);

    veg::ScatterResult one;
    veg::ScatterChunk(7, chunk, *grid, nullptr, nullptr, Uniform(0.25f), AABB::Empty(), one);
    veg::ScatterResult two;
    veg::ScatterChunk(7, chunk, *grid, nullptr, nullptr, Uniform(0.5f), AABB::Empty(), two);
    CHECK(one.candidateCount == 1024u); // 0.25 / m^2 x 4096 m^2
    CHECK(two.candidateCount == 2048u);
    CHECK(one.transforms.Size() == 1024u); // Uniform on a flat field keeps every candidate
    CHECK(two.transforms.Size() == 2048u);
    CHECK(!one.densityClamped);
    CHECK(one.effectiveDensity == doctest::Approx(0.25f));

    // Over budget: the cap wins and the density reports what was actually used.
    veg::ScatterLayer dense = Uniform(10.0f); // 40960 wanted
    dense.maxInstancesPerChunk = 4096;
    veg::ScatterResult capped;
    veg::ScatterChunk(7, chunk, *grid, nullptr, nullptr, dense, AABB::Empty(), capped);
    CHECK(capped.candidateCount == 4096u); // every candidate placed: the loop stopped at the cap
    CHECK(capped.densityClamped);
    CHECK(capped.effectiveDensity == doctest::Approx(1.0f));
    CHECK(capped.transforms.Size() == 4096u);
    // The placed set is a prefix of the uncapped one (the candidate stream is stable).
    veg::ScatterLayer roomy = Uniform(10.0f);
    roomy.maxInstancesPerChunk = 1u << 20;
    veg::ScatterResult full;
    veg::ScatterChunk(7, chunk, *grid, nullptr, nullptr, roomy, AABB::Empty(), full);
    REQUIRE(full.transforms.Size() > 4096u);
    CHECK(MemCompare(full.transforms.Data(), capped.transforms.Data(), 4096u * sizeof(Float4x4)) == 0);

    // The cap counts placed instances, not candidates: a mask patch on a quarter of the chunk
    // grows at the layer's full density (2 / m^2 x 1024 m^2 ~ 2048), where scaling 2 x 4096
    // candidates over the whole chunk down to the cap would halve it to 1 / m^2.
    auto quarter = MakeRef<veg::VegetationMask>(DefaultAllocator(), 32, 32, 1);
    for (i32 y = 0; y < 16; ++y)
    {
        for (i32 x = 0; x < 16; ++x)
        {
            quarter->SetDensity(0, x, y, 255);
        }
    }
    veg::ScatterLayer patch;
    patch.placement = veg::VegetationPlacement::Mask;
    patch.maskPlane = 0;
    patch.density = 2.0f;
    patch.maxSlopeDegrees = 90.0f;
    patch.maxInstancesPerChunk = 4096;
    veg::ScatterResult grown;
    veg::ScatterChunk(7, chunk, *grid, nullptr, quarter.Get(), patch, AABB::Empty(), grown);
    CHECK(grown.candidateCount == 8192u);
    CHECK(!grown.densityClamped);
    CHECK(grown.effectiveDensity == doctest::Approx(2.0f));
    CHECK(grown.transforms.Size() > 1800u);
    CHECK(grown.transforms.Size() < 2300u);
    // ...and a cap below that fills the patch to exactly the cap, all inside it.
    patch.maxInstancesPerChunk = 512;
    veg::ScatterChunk(7, chunk, *grid, nullptr, quarter.Get(), patch, AABB::Empty(), grown);
    CHECK(grown.transforms.Size() == 512u);
    CHECK(grown.densityClamped);
    CHECK(grown.candidateCount < 8192u);
    for (const Float4x4& m : grown.transforms)
    {
        CHECK(m.m[3][0] < 0.0f);
        CHECK(m.m[3][2] < 0.0f);
    }

    // Nothing to do: zero density.
    veg::ScatterResult none;
    veg::ScatterChunk(7, chunk, *grid, nullptr, nullptr, Uniform(0.0f), AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
}

TEST_CASE("vegetation scatter: the splat rule grows only where the layer's share clears the threshold")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(2.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*grid);
    RefPtr<tmodel::SplatWeights> splat = MakeHalfSplat();

    veg::ScatterLayer grass;
    grass.placement = veg::VegetationPlacement::Splat;
    grass.splatLayer = 0;
    grass.splatThreshold = 0.25f;
    grass.density = 0.5f;
    grass.maxSlopeDegrees = 90.0f;

    veg::ScatterResult painted;
    veg::ScatterChunk(3, chunk, *grid, splat.Get(), nullptr, grass, AABB::Empty(), painted);
    REQUIRE(!painted.transforms.IsEmpty());
    // Roughly half the candidates (the painted half at share 1 keeps every one of them).
    CHECK(painted.transforms.Size() > painted.candidateCount / 3);
    CHECK(painted.transforms.Size() < painted.candidateCount * 2 / 3);
    for (const Float4x4& m : painted.transforms)
    {
        CHECK(m.m[3][0] < 0.0f); // only on the painted (x < 0) half
    }

    // The base layer is the complement.
    veg::ScatterLayer base = grass;
    base.splatLayer = veg::kSplatBaseLayer;
    veg::ScatterResult unpainted;
    veg::ScatterChunk(3, chunk, *grid, splat.Get(), nullptr, base, AABB::Empty(), unpainted);
    REQUIRE(!unpainted.transforms.IsEmpty());
    for (const Float4x4& m : unpainted.transforms)
    {
        CHECK(m.m[3][0] > 0.0f);
    }

    // A layer nobody painted, or a threshold above every share, grows nothing; nor does a
    // Splat layer with no splat at all.
    veg::ScatterLayer rocks = grass;
    rocks.splatLayer = 3;
    veg::ScatterResult none;
    veg::ScatterChunk(3, chunk, *grid, splat.Get(), nullptr, rocks, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
    veg::ScatterLayer strict = grass;
    strict.splatThreshold = 1.5f;
    veg::ScatterChunk(3, chunk, *grid, splat.Get(), nullptr, strict, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
    veg::ScatterChunk(3, chunk, *grid, nullptr, nullptr, grass, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());

    // The share is the sampled weight: a half-weight paint keeps about half the candidates.
    CHECK(veg::PlacementShareAt(grass, *grid, splat.Get(), nullptr, -16.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(veg::PlacementShareAt(grass, *grid, splat.Get(), nullptr, 16.0f, 0.0f) == doctest::Approx(0.0f));
    CHECK(veg::PlacementShareAt(base, *grid, splat.Get(), nullptr, 16.0f, 0.0f) == doctest::Approx(1.0f));
}

TEST_CASE("vegetation scatter: the slope limit and the height window reject")
{
    // A ramp rising 32 m over 64 m: slope atan(0.5) = 26.6 degrees everywhere.
    RefPtr<hf::Heightfield> ramp = MakeRamp(32.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*ramp);

    veg::ScatterLayer gentle = Uniform(0.25f);
    // The limit sits under the ramp's slope everywhere: the central difference halves at the
    // clamped x edges (13.3 degrees there), so 10 keeps rejecting at the rim too.
    gentle.maxSlopeDegrees = 10.0f;
    veg::ScatterResult none;
    veg::ScatterChunk(11, chunk, *ramp, nullptr, nullptr, gentle, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());

    veg::ScatterLayer steep = Uniform(0.25f);
    steep.maxSlopeDegrees = 35.0f; // allowed
    veg::ScatterResult all;
    veg::ScatterChunk(11, chunk, *ramp, nullptr, nullptr, steep, AABB::Empty(), all);
    CHECK(all.transforms.Size() == all.candidateCount);

    // The height window: only the band 8..16 m up the ramp (x in the middle quarter).
    veg::ScatterLayer band = steep;
    band.heightRange = Float2{8.0f, 16.0f};
    veg::ScatterResult banded;
    veg::ScatterChunk(11, chunk, *ramp, nullptr, nullptr, band, AABB::Empty(), banded);
    REQUIRE(!banded.transforms.IsEmpty());
    CHECK(banded.transforms.Size() < all.transforms.Size());
    for (const Float4x4& m : banded.transforms)
    {
        CHECK(m.m[3][1] >= 8.0f - 0.05f);
        CHECK(m.m[3][1] <= 16.0f + 0.05f);
    }
}

TEST_CASE("vegetation scatter: alignToNormal tilts each instance onto the sampled normal; scale is uniform")
{
    RefPtr<hf::Heightfield> ramp = MakeRamp(32.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*ramp);
    const Float3 expected = Normalized(Float3{-0.5f, 1.0f, 0.0f}); // the ramp's normal

    veg::ScatterLayer flat = Uniform(0.05f);
    flat.scaleRange = Float2{2.0f, 2.0f};
    veg::ScatterResult upright;
    veg::ScatterChunk(5, chunk, *ramp, nullptr, nullptr, flat, AABB::Empty(), upright);
    REQUIRE(!upright.transforms.IsEmpty());
    for (const Float4x4& m : upright.transforms)
    {
        const Float3 up = Row(m, 1);
        CHECK(Length(up) == doctest::Approx(2.0f).epsilon(0.001)); // the uniform scale
        CHECK(Normalized(up).y == doctest::Approx(1.0f).epsilon(0.001));
    }

    veg::ScatterLayer aligned = flat;
    aligned.alignToNormal = true;
    veg::ScatterResult tilted;
    veg::ScatterChunk(5, chunk, *ramp, nullptr, nullptr, aligned, AABB::Empty(), tilted);
    REQUIRE(tilted.transforms.Size() == upright.transforms.Size()); // same candidates, same keeps
    for (const Float4x4& m : tilted.transforms)
    {
        const Float3 up = Normalized(Row(m, 1));
        if (Abs(m.m[3][0]) < 31.0f) // inside the rim the central difference is the true slope
        {
            CHECK(Dot(up, expected) == doctest::Approx(1.0f).epsilon(0.01));
        }
        // Still a right-handed orthonormal frame at scale 2.
        const Float3 x = Row(m, 0);
        const Float3 z = Row(m, 2);
        CHECK(Length(x) == doctest::Approx(2.0f).epsilon(0.001));
        CHECK(Dot(Normalized(x), up) == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(Dot(Normalized(z), up) == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(Dot(Cross(Normalized(x), up), Normalized(z)) == doctest::Approx(1.0f).epsilon(0.01));
    }
}

TEST_CASE("vegetation scatter: the mesh extent grows the chunk bounds by the largest scale")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(3.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*grid);
    veg::ScatterLayer layer = Uniform(0.1f);
    layer.scaleRange = Float2{1.0f, 3.0f};
    const AABB blade = AABB::FromCenterExtents(Float3{0.0f, 0.5f, 0.0f}, Float3{0.1f, 0.5f, 0.1f});

    veg::ScatterResult r;
    veg::ScatterChunk(9, chunk, *grid, nullptr, nullptr, layer, blade, r);
    REQUIRE(!r.transforms.IsEmpty());
    // reach = |extents| + |center| = sqrt(0.01 + 0.25 + 0.01) + 0.5 ~ 1.0198; x 3 = 3.06
    const f32 grow = (Length(Float3{0.1f, 0.5f, 0.1f}) + 0.5f) * 3.0f;
    CHECK(r.localBounds.min.x == doctest::Approx(chunk.bounds.min.x - grow));
    CHECK(r.localBounds.max.y == doctest::Approx(chunk.bounds.max.y + grow));

    veg::ScatterResult bare;
    veg::ScatterChunk(9, chunk, *grid, nullptr, nullptr, layer, AABB::Empty(), bare);
    CHECK(bare.localBounds.min.x == doctest::Approx(chunk.bounds.min.x));
}

TEST_CASE("vegetation fade: full inside fadeStart, zero beyond fadeEnd, monotone between; the prefix is a count")
{
    CHECK(veg::DensityAtDistance(0.0f, 40.0f, 80.0f) == 1.0f);
    CHECK(veg::DensityAtDistance(40.0f, 40.0f, 80.0f) == 1.0f);
    CHECK(veg::DensityAtDistance(80.0f, 40.0f, 80.0f) == 0.0f);
    CHECK(veg::DensityAtDistance(500.0f, 40.0f, 80.0f) == 0.0f);
    CHECK(veg::DensityAtDistance(60.0f, 40.0f, 80.0f) == doctest::Approx(0.5f));
    f32 previous = 1.0f;
    for (f32 d = 40.0f; d <= 80.0f; d += 1.0f)
    {
        const f32 now = veg::DensityAtDistance(d, 40.0f, 80.0f);
        CHECK(now <= previous);
        previous = now;
    }
    // A degenerate window is a hard cut at fadeEnd.
    CHECK(veg::DensityAtDistance(79.0f, 80.0f, 80.0f) == 1.0f);
    CHECK(veg::DensityAtDistance(80.0f, 80.0f, 80.0f) == 0.0f);

    CHECK(veg::FadePrefix(1000, 1.0f) == 1000u);
    CHECK(veg::FadePrefix(1000, 0.0f) == 0u);
    CHECK(veg::FadePrefix(1000, 0.5f) == 500u);
    CHECK(veg::FadePrefix(1000, 2.0f) == 1000u);
    CHECK(veg::FadePrefix(3, 0.5f) == 2u); // rounds
    CHECK(veg::FadePrefix(0, 0.5f) == 0u);
}

TEST_CASE("vegetation: ChunksTouchedBy maps a grid region to the chunks that share its samples")
{
    Array<u32> touched;
    hf::HeightfieldRegion inside;
    inside.minX = 10;
    inside.maxX = 20;
    inside.minZ = 70;
    inside.maxZ = 80;
    veg::ChunksTouchedBy(inside, 4, touched); // a 257-sample grid: 4 x 4 chunks
    REQUIRE(touched.Size() == 1u);
    CHECK(touched[0] == 4u); // chunk (0, 1)

    // A region ending ON the shared boundary sample 64 touches chunk 0 and chunk 1.
    touched.Clear();
    hf::HeightfieldRegion edge;
    edge.minX = 60;
    edge.maxX = 64;
    edge.minZ = 5;
    edge.maxZ = 6;
    veg::ChunksTouchedBy(edge, 4, touched);
    REQUIRE(touched.Size() == 2u);
    CHECK(touched[0] == 0u);
    CHECK(touched[1] == 1u);

    // A region starting on a boundary sample touches the chunk before it too.
    touched.Clear();
    hf::HeightfieldRegion corner;
    corner.minX = 64;
    corner.maxX = 64;
    corner.minZ = 64;
    corner.maxZ = 64;
    veg::ChunksTouchedBy(corner, 4, touched);
    CHECK(touched.Size() == 4u);

    // Appends unique: a second call over the same region adds nothing; an empty region nothing.
    veg::ChunksTouchedBy(corner, 4, touched);
    CHECK(touched.Size() == 4u);
    veg::ChunksTouchedBy(hf::HeightfieldRegion{}, 4, touched);
    CHECK(touched.Size() == 4u);

    // Clamped to the grid.
    touched.Clear();
    hf::HeightfieldRegion beyond;
    beyond.minX = 250;
    beyond.maxX = 400;
    beyond.minZ = 0;
    beyond.maxZ = 0;
    veg::ChunksTouchedBy(beyond, 4, touched);
    REQUIRE(touched.Size() == 1u);
    CHECK(touched[0] == 3u);
}

TEST_CASE("vegetation layer: the scatter hash covers the scatter parameters, not the per-frame draw state")
{
    veg::ScatterLayer a;
    veg::ScatterLayer b = a;
    CHECK(veg::LayerScatterHash(a) == veg::LayerScatterHash(b));
    b.density = 3.0f;
    CHECK(veg::LayerScatterHash(a) != veg::LayerScatterHash(b));
    b = a;
    b.fadeEnd = 200.0f; // fade and shadows are draw-time: no regrow
    b.castShadows = true;
    CHECK(veg::LayerScatterHash(a) == veg::LayerScatterHash(b));
    for (const veg::VegetationPlacement p :
         {veg::VegetationPlacement::Uniform, veg::VegetationPlacement::Mask})
    {
        b = a;
        b.placement = p;
        CHECK(veg::LayerScatterHash(a) != veg::LayerScatterHash(b));
    }
}

TEST_CASE("vegetation scatter: a Mask layer grows where its plane is painted; SplatTimesMask multiplies the two")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(2.0f);
    const tmodel::TerrainChunk chunk = ChunkOf(*grid);
    RefPtr<tmodel::SplatWeights> splat = MakeHalfSplat(); // palette 0 on x < 0
    // A two-plane mask: plane 1 painted full on the top half (z < 0); plane 0 empty.
    auto mask = MakeRef<veg::VegetationMask>(DefaultAllocator(), 32, 32, 2);
    for (i32 y = 0; y < 16; ++y)
    {
        for (i32 x = 0; x < 32; ++x)
        {
            mask->SetDensity(1, x, y, 255);
        }
    }

    veg::ScatterLayer flowers;
    flowers.placement = veg::VegetationPlacement::Mask;
    flowers.maskPlane = 1;
    flowers.density = 0.5f;
    flowers.maxSlopeDegrees = 90.0f;
    veg::ScatterResult painted;
    veg::ScatterChunk(21, chunk, *grid, splat.Get(), mask.Get(), flowers, AABB::Empty(), painted);
    REQUIRE(!painted.transforms.IsEmpty());
    CHECK(painted.transforms.Size() > painted.candidateCount / 3);
    CHECK(painted.transforms.Size() < painted.candidateCount * 2 / 3);
    for (const Float4x4& m : painted.transforms)
    {
        CHECK(m.m[3][2] < 0.0f); // the painted (z < 0) half only; x is free
    }
    // The empty plane, a plane out of range, or no mask at all grow nothing; the splat
    // threshold does not apply to a mask density.
    veg::ScatterLayer empty = flowers;
    empty.maskPlane = 0;
    veg::ScatterResult none;
    veg::ScatterChunk(21, chunk, *grid, splat.Get(), mask.Get(), empty, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
    empty.maskPlane = 7;
    veg::ScatterChunk(21, chunk, *grid, splat.Get(), mask.Get(), empty, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
    veg::ScatterChunk(21, chunk, *grid, splat.Get(), nullptr, flowers, AABB::Empty(), none);
    CHECK(none.transforms.IsEmpty());
    mask->SetDensity(1, 8, 8, 40); // a faint texel (0.16 < the 0.25 splat threshold) still grows
    CHECK(veg::PlacementShareAt(flowers, *grid, splat.Get(), mask.Get(), -15.0f, -15.0f) ==
          doctest::Approx(40.0f / 255.0f));

    // SplatTimesMask: the painted splat half (x < 0) and the painted mask half (z < 0): one
    // quadrant.
    veg::ScatterLayer carved = flowers;
    carved.placement = veg::VegetationPlacement::SplatTimesMask;
    carved.splatLayer = 0;
    carved.splatThreshold = 0.25f;
    mask->SetDensity(1, 8, 8, 255);
    veg::ScatterResult quadrant;
    veg::ScatterChunk(21, chunk, *grid, splat.Get(), mask.Get(), carved, AABB::Empty(), quadrant);
    REQUIRE(!quadrant.transforms.IsEmpty());
    CHECK(quadrant.transforms.Size() < painted.transforms.Size());
    for (const Float4x4& m : quadrant.transforms)
    {
        CHECK(m.m[3][0] < 0.0f);
        CHECK(m.m[3][2] < 0.0f);
    }
    CHECK(veg::PlacementShareAt(carved, *grid, splat.Get(), mask.Get(), -16.0f, -16.0f) ==
          doctest::Approx(1.0f));
    CHECK(veg::PlacementShareAt(carved, *grid, splat.Get(), mask.Get(), -16.0f, 16.0f) ==
          doctest::Approx(0.0f));
    CHECK(veg::PlacementShareAt(carved, *grid, nullptr, mask.Get(), -16.0f, -16.0f) == 0.0f);

    // The hash covers the placement and the plane (a mask edit is a version bump, not a hash).
    veg::ScatterLayer other = flowers;
    other.maskPlane = 0;
    CHECK(veg::LayerScatterHash(other) != veg::LayerScatterHash(flowers));
}

TEST_CASE("vegetation stamp: a seeded stamp places a deterministic count inside the disc; slope, spacing and a blocked query reject; erase removes")
{
    RefPtr<hf::Heightfield> grid = MakeFlat(2.0f);
    veg::ScatterLayer rocks = Uniform(0.0f); // a prop layer's rules: placement is not consulted
    rocks.scaleRange = Float2{1.0f, 1.0f};
    rocks.maxSlopeDegrees = 90.0f;
    const AABB rock = AABB::FromCenterExtents(Float3{0, 0.5f, 0}, Float3{0.5f, 0.5f, 0.5f});

    Array<Float4x4> a;
    const veg::StampResult ra = veg::ScatterStamp(99, *grid, rocks, rock, 4.0f, -3.0f, 6.0f,
                                                  0.5f, 1.0f, 0.0f, {}, {}, a);
    CHECK(ra.candidates == 57u); // 0.5 x pi x 36 = 56.5
    CHECK(ra.placed == 57u);     // flat, no spacing, nothing blocked: every candidate lands
    REQUIRE(a.Size() == 57u);
    for (const Float4x4& m : a)
    {
        const f32 dx = m.m[3][0] - 4.0f;
        const f32 dz = m.m[3][2] + 3.0f;
        CHECK(dx * dx + dz * dz <= 36.0f + 1e-3f);
        CHECK(m.m[3][1] == doctest::Approx(2.0f).epsilon(0.01));
    }
    // The same seed places the same instances; another seed differs; amount scales the count.
    Array<Float4x4> b;
    (void)veg::ScatterStamp(99, *grid, rocks, rock, 4.0f, -3.0f, 6.0f, 0.5f, 1.0f, 0.0f, {}, {}, b);
    CHECK(SameTransforms(a, b));
    Array<Float4x4> c;
    (void)veg::ScatterStamp(100, *grid, rocks, rock, 4.0f, -3.0f, 6.0f, 0.5f, 1.0f, 0.0f, {}, {}, c);
    CHECK(!SameTransforms(a, c));
    Array<Float4x4> half;
    CHECK(veg::ScatterStamp(99, *grid, rocks, rock, 4.0f, -3.0f, 6.0f, 0.5f, 0.5f, 0.0f, {}, {}, half)
              .candidates == 28u);

    // Spacing: a rock of radius ~0.87 with spacing 2 keeps ~1.7 m apart, so far fewer land, and
    // the ones that do are never within reach of each other or of the existing ones.
    Array<Float4x4> spaced;
    const veg::StampResult rs = veg::ScatterStamp(
        99, *grid, rocks, rock, 4.0f, -3.0f, 6.0f, 2.0f, 1.0f, 2.0f,
        Span<const Float4x4>{a.Data(), a.Size()}, {}, spaced);
    CHECK(rs.rejectedSpacing > 0u);
    CHECK(rs.placed + rs.rejectedSpacing == rs.candidates);
    const f32 reach = 2.0f * Length(rock.Extents());
    for (const Float4x4& m : spaced)
    {
        for (const Float4x4& e : a)
        {
            const f32 dx = m.m[3][0] - e.m[3][0];
            const f32 dz = m.m[3][2] - e.m[3][2];
            CHECK(dx * dx + dz * dz >= reach * reach - 1e-3f);
        }
    }

    // A blocked query (the physics world's overlap in the editor) rejects where it says so.
    Array<Float4x4> blocked;
    const veg::StampResult rb = veg::ScatterStamp(
        99, *grid, rocks, rock, 4.0f, -3.0f, 6.0f, 0.5f, 1.0f, 0.0f, {},
        [](Float3 p, f32) { return p.x > 4.0f; }, blocked);
    CHECK(rb.rejectedBlocked > 0u);
    CHECK(rb.placed + rb.rejectedBlocked == rb.candidates);
    for (const Float4x4& m : blocked)
    {
        CHECK(m.m[3][0] <= 4.0f);
    }

    // The slope rule: a steep ramp with a tight limit places nothing.
    RefPtr<hf::Heightfield> ramp = MakeRamp(32.0f);
    veg::ScatterLayer gentle = rocks;
    gentle.maxSlopeDegrees = 10.0f;
    Array<Float4x4> none;
    const veg::StampResult rr =
        veg::ScatterStamp(5, *ramp, gentle, rock, 0.0f, 0.0f, 6.0f, 0.5f, 1.0f, 0.0f, {}, {}, none);
    CHECK(none.IsEmpty());
    CHECK(rr.rejectedRules == rr.candidates);

    // Erase: the instances inside the disc go, the rest keep their order.
    Array<Float4x4> field = a;
    const u32 removed = veg::EraseInstancesInDisc(field, 4.0f, -3.0f, 3.0f);
    CHECK(removed > 0u);
    CHECK(field.Size() + removed == a.Size());
    for (const Float4x4& m : field)
    {
        const f32 dx = m.m[3][0] - 4.0f;
        const f32 dz = m.m[3][2] + 3.0f;
        CHECK(dx * dx + dz * dz > 9.0f);
    }
    CHECK(veg::EraseInstancesInDisc(field, 100.0f, 100.0f, 1.0f) == 0u);
    // Degenerate inputs place nothing.
    CHECK(veg::ScatterStamp(1, *grid, rocks, rock, 0, 0, 0.0f, 1.0f, 1.0f, 0.0f, {}, {}, none).candidates == 0u);
    CHECK(veg::ScatterStamp(1, *grid, rocks, rock, 0, 0, 5.0f, 0.0f, 1.0f, 0.0f, {}, {}, none).candidates == 0u);
}

TEST_CASE("vegetation holes: nothing grows or stands over a cut cell - the scatter and the stamp both reject it")
{
    // A cut cell has no surface. A Uniform layer over a flat field with a
    // disc cut out of its middle places no instance inside the disc, and a prop stamp centred on
    // the cut places none there either.
    RefPtr<hf::Heightfield> grid = MakeFlat(5.0f);
    (void)hf::CutHoles(*grid, 0.0f, 0.0f, 8.0f); // 1 m cells: a 16 m wide cut at the centre
    REQUIRE(grid->HasHoles());
    veg::ScatterLayer layer;
    layer.placement = veg::VegetationPlacement::Uniform;
    layer.density = 1.0f;
    layer.maxSlopeDegrees = 90.0f;
    veg::ScatterResult result;
    veg::ScatterChunk(1234u, ChunkOf(*grid), *grid, nullptr, nullptr, layer,
                      AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 0.5f, 0.5f}), result);
    REQUIRE(result.transforms.Size() > 100u); // the field around the cut still grows
    for (const Float4x4& m : result.transforms)
    {
        const f32 x = m.m[3][0];
        const f32 z = m.m[3][2];
        // Inside the cut, allowing the one-cell rim a corner sample removes.
        CHECK(Sqrt(x * x + z * z) > 8.0f - 1.5f);
    }
    // The share itself reads zero over the cut and one beside it.
    CHECK(veg::PlacementShareAt(layer, *grid, nullptr, nullptr, 0.0f, 0.0f) == 0.0f);
    CHECK(veg::PlacementShareAt(layer, *grid, nullptr, nullptr, 20.0f, 20.0f) == 1.0f);

    // A prop stamp over the cut places nothing; the same stamp beside it does.
    Array<Float4x4> existing;
    Array<Float4x4> placed;
    veg::StampResult stampOver = veg::ScatterStamp(
        7u, *grid, layer, AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 0.5f, 0.5f}), 0.0f,
        0.0f, 3.0f, 2.0f, 1.0f, 0.0f, Span<const Float4x4>{existing.Data(), existing.Size()},
        {}, placed);
    CHECK(placed.IsEmpty());
    CHECK(stampOver.rejectedRules > 0u);
    veg::StampResult stampBeside = veg::ScatterStamp(
        7u, *grid, layer, AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 0.5f, 0.5f}), 20.0f,
        20.0f, 3.0f, 2.0f, 1.0f, 0.0f, Span<const Float4x4>{existing.Data(), existing.Size()},
        {}, placed);
    CHECK(placed.Size() > 0u);
    CHECK(stampBeside.rejectedRules == 0u);
}


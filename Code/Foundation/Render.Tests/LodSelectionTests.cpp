// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The per-view LOD selection math (pure, no GPU): coverage from a camera (perspective divides by
// view depth; ortho is depth-free; bias halves per unit), the descending-threshold pick walk,
// and the 5% hysteresis band either side of a threshold.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.geometry;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace geometry = foundation::geometry;

namespace
{
    // A 3-LOD mesh shell: only lodCount/lodCoverage matter to the pick walk.
    RefPtr<geometry::StaticMesh> ThreeLodMesh()
    {
        auto mesh = MakeRef<geometry::StaticMesh>(DefaultAllocator());
        mesh->lodCount = 3;
        mesh->lodCoverage.PushBack(1.0f);
        mesh->lodCoverage.PushBack(0.25f); // below 25% of half-height -> LOD 1
        mesh->lodCoverage.PushBack(0.05f); // below 5% -> LOD 2
        return mesh;
    }

    ViewCamera PerspectiveCam(f32 fovY)
    {
        ViewCamera cam;
        cam.view = Float4x4::Identity(); // camera at origin looking down -z
        cam.projection = Float4x4::PerspectiveFovRH(fovY, 1.0f, 0.1f, 1000.0f);
        return cam;
    }
}

TEST_CASE("lod select: perspective coverage halves with distance; ortho is depth-free")
{
    const ViewCamera cam = PerspectiveCam(kHalfPi); // 90 deg: proj11 = 1
    const f32 near10 = LodCoverageFor(cam, Float3{0, 0, -10.0f}, 1.0f, 0.0f);
    const f32 far20 = LodCoverageFor(cam, Float3{0, 0, -20.0f}, 1.0f, 0.0f);
    CHECK(near10 == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(far20 == doctest::Approx(near10 * 0.5f).epsilon(0.01));

    // Behind/at the camera clamps depth (never divides by ~zero, never goes negative).
    const f32 behind = LodCoverageFor(cam, Float3{0, 0, 5.0f}, 1.0f, 0.0f);
    CHECK(behind > 0.0f);

    ViewCamera ortho;
    ortho.projection = Float4x4::OrthographicRH(10.0f, 10.0f, 0.1f, 100.0f);
    const f32 a = LodCoverageFor(ortho, Float3{0, 0, -1.0f}, 1.0f, 0.0f);
    const f32 b = LodCoverageFor(ortho, Float3{0, 0, -90.0f}, 1.0f, 0.0f);
    CHECK(a == doctest::Approx(b)); // depth-free
}

TEST_CASE("lod select: bias halves effective coverage per unit (both directions)")
{
    const ViewCamera cam = PerspectiveCam(kHalfPi);
    const f32 unbiased = LodCoverageFor(cam, Float3{0, 0, -10.0f}, 1.0f, 0.0f);
    CHECK(LodCoverageFor(cam, Float3{0, 0, -10.0f}, 1.0f, 1.0f) ==
          doctest::Approx(unbiased * 0.5f));
    CHECK(LodCoverageFor(cam, Float3{0, 0, -10.0f}, 1.0f, -1.0f) ==
          doctest::Approx(unbiased * 2.0f));
}

TEST_CASE("lod select: the pick walk (descending thresholds, clamped, chainless = 0)")
{
    RefPtr<geometry::StaticMesh> mesh = ThreeLodMesh();
    CHECK(PickLodLevel(*mesh, 0.50f) == 0); // big on screen -> finest
    CHECK(PickLodLevel(*mesh, 0.25f) == 0); // exactly at the boundary -> still LOD 0
    CHECK(PickLodLevel(*mesh, 0.20f) == 1);
    CHECK(PickLodLevel(*mesh, 0.04f) == 2);
    CHECK(PickLodLevel(*mesh, 0.0f) == 2);  // vanishing -> coarsest

    geometry::StaticMesh chainless;
    CHECK(PickLodLevel(chainless, 0.0f) == 0);

    // A malformed (non-descending) tail stops the walk instead of over-coarsening.
    RefPtr<geometry::StaticMesh> weird = ThreeLodMesh();
    weird->lodCoverage[2] = 0.9f; // not descending
    CHECK(PickLodLevel(*weird, 0.5f) == 0);
}

TEST_CASE("lod select: hysteresis keeps the previous level near a boundary only")
{
    RefPtr<geometry::StaticMesh> mesh = ThreeLodMesh();
    // Just under the 0.25 boundary: the raw pick says 1, but a previous LOD 0 sits inside the 5%
    // band (0.246 * 1.05 > 0.25), so it sticks and the boundary does not flicker.
    CHECK(PickLodLevel(*mesh, 0.246f) == 1);
    CHECK(ApplyLodHysteresis(*mesh, 0.246f, 1, 0) == 0);
    // Just over the boundary the other way: previous LOD 1 sticks too.
    CHECK(PickLodLevel(*mesh, 0.253f) == 0);
    CHECK(ApplyLodHysteresis(*mesh, 0.253f, 0, 1) == 1);
    // Far from any boundary the previous level is not pickable within the band: the raw selection
    // wins (hysteresis never pins a stale level).
    CHECK(ApplyLodHysteresis(*mesh, 0.8f, 0, 2) == 0);
    CHECK(ApplyLodHysteresis(*mesh, 0.01f, 2, 0) == 2);
    // Matching previous is a no-op.
    CHECK(ApplyLodHysteresis(*mesh, 0.246f, 1, 1) == 1);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Lod - the shared LOD selection math (mesh chains and terrain chunks both
// delegate here; one coverage formula, one walk, one hysteresis rule).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.lod;

using namespace foundation::core;
using namespace foundation::lod;

TEST_CASE("lod math: perspective coverage halves with distance; ortho is depth-free; bias halves")
{
    const Float4x4 view = Float4x4::Identity(); // camera at origin, -z forward
    const Float4x4 proj = Float4x4::PerspectiveFovRH(kHalfPi, 1.0f, 0.1f, 1000.0f);
    const f32 at10 = ProjectedSphereCoverage(view, proj, Float3{0, 0, -10.0f}, 1.0f);
    const f32 at20 = ProjectedSphereCoverage(view, proj, Float3{0, 0, -20.0f}, 1.0f);
    CHECK(at10 == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(at20 == doctest::Approx(at10 * 0.5f).epsilon(0.01));
    CHECK(ProjectedSphereCoverage(view, proj, Float3{0, 0, -10.0f}, 1.0f, 1.0f) ==
          doctest::Approx(at10 * 0.5f));
    CHECK(ProjectedSphereCoverage(view, proj, Float3{0, 0, 5.0f}, 1.0f) > 0.0f); // behind: clamped

    const Float4x4 ortho = Float4x4::OrthographicRH(10.0f, 10.0f, 0.1f, 100.0f);
    CHECK(ProjectedSphereCoverage(view, ortho, Float3{0, 0, -1.0f}, 1.0f) ==
          doctest::Approx(ProjectedSphereCoverage(view, ortho, Float3{0, 0, -90.0f}, 1.0f)));
}

TEST_CASE("lod math: the threshold walk (descending, boundary stays finer, degenerate inputs)")
{
    const f32 thresholds[] = {1.0f, 0.25f, 0.05f};
    const Span<const f32> t{thresholds, 3};
    CHECK(SelectLevelByCoverage(t, 0.5f) == 0);
    CHECK(SelectLevelByCoverage(t, 0.25f) == 0);
    CHECK(SelectLevelByCoverage(t, 0.2f) == 1);
    CHECK(SelectLevelByCoverage(t, 0.01f) == 2);
    CHECK(SelectLevelByCoverage(t, 0.0f) == 2);
    CHECK(SelectLevelByCoverage(Span<const f32>{}, 0.0f) == 0);
    const f32 nonDescending[] = {1.0f, 0.25f, 0.9f};
    CHECK(SelectLevelByCoverage(Span<const f32>{nonDescending, 3}, 0.5f) == 0); // walk stops
}

TEST_CASE("lod math: hysteresis keeps the previous level near a boundary only")
{
    const f32 thresholds[] = {1.0f, 0.25f, 0.05f};
    const Span<const f32> t{thresholds, 3};
    CHECK(ApplyCoverageHysteresis(t, 0.246f, 1, 0) == 0); // sticks inside the band
    CHECK(ApplyCoverageHysteresis(t, 0.253f, 0, 1) == 1);
    CHECK(ApplyCoverageHysteresis(t, 0.8f, 0, 2) == 0);   // far away: raw wins
    CHECK(ApplyCoverageHysteresis(t, 0.01f, 2, 0) == 2);
    CHECK(ApplyCoverageHysteresis(t, 0.246f, 1, 1) == 1); // matching previous is a no-op
}

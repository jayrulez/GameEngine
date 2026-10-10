// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests - the scene grid's pure parts: the planes' axes, the spacing stepping a
// decade at a time with the camera's distance (and the blend across each step), and the reach.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import editor.scene;

using namespace foundation::core;

TEST_CASE("scene grid: each plane's axes are unit, at right angles, and turn into its normal")
{
    const editor::GridPlane planes[3] = {editor::GridPlane::XZ, editor::GridPlane::XY,
                                         editor::GridPlane::YZ};
    const Float3 normals[3] = {Float3{0, 1, 0}, Float3{0, 0, 1}, Float3{1, 0, 0}};
    for (u32 i = 0; i < 3; ++i)
    {
        CAPTURE(i);
        const editor::GridPlaneAxes axes = editor::AxesOf(planes[i]);
        CHECK(Length(axes.u) == doctest::Approx(1.0f));
        CHECK(Length(axes.v) == doctest::Approx(1.0f));
        CHECK(Dot(axes.u, axes.v) == doctest::Approx(0.0f));
        const Float3 n = Cross(axes.u, axes.v);
        CHECK(n.x == doctest::Approx(normals[i].x));
        CHECK(n.y == doctest::Approx(normals[i].y));
        CHECK(n.z == doctest::Approx(normals[i].z));
        CHECK(axes.normal.y == doctest::Approx(normals[i].y));
    }
}

TEST_CASE("scene grid: the spacing steps a decade at a time, blending across each step")
{
    // 10 m up: metre cells, at the start of their decade.
    editor::GridSpacing s = editor::GridSpacingFor(10.0f);
    CHECK(s.spacing == doctest::Approx(1.0f));
    CHECK(s.blend == doctest::Approx(0.0f).epsilon(1e-4));
    // Half way (in decades) to 100 m: still metres, half blended toward ten.
    s = editor::GridSpacingFor(31.6228f);
    CHECK(s.spacing == doctest::Approx(1.0f));
    CHECK(s.blend == doctest::Approx(0.5f).epsilon(1e-3));
    // 100 m up: ten-metre cells; 1 m up: ten-centimetre cells.
    CHECK(editor::GridSpacingFor(100.0f).spacing == doctest::Approx(10.0f));
    CHECK(editor::GridSpacingFor(1.0f).spacing == doctest::Approx(0.1f));
    // On the plane, or below it: never finer than a centimetre.
    CHECK(editor::GridSpacingFor(0.0f).spacing == doctest::Approx(0.01f));
    CHECK(editor::GridSpacingFor(-5.0f).spacing == doctest::Approx(0.01f));

    // The spacing never shrinks as the camera climbs, and the blend stays within 0..1.
    f32 last = 0.0f;
    for (f32 d = 0.05f; d < 5000.0f; d *= 1.3f)
    {
        const editor::GridSpacing step = editor::GridSpacingFor(d);
        CHECK(step.spacing >= last);
        CHECK(step.blend >= 0.0f);
        CHECK(step.blend <= 1.0f);
        last = step.spacing;
    }
}

TEST_CASE("scene grid: it reaches further as the camera climbs, within the far plane")
{
    CHECK(editor::GridFadeDistance(0.0f) == doctest::Approx(60.0f));
    CHECK(editor::GridFadeDistance(10.0f) > editor::GridFadeDistance(1.0f));
    CHECK(editor::GridFadeDistance(1.0e6f) <= 900.0f);
}

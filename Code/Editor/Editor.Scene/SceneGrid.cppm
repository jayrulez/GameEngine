// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_grid partition.
//
// The scene view's grid: which plane it lies on, how far apart its lines are for where the
// camera is, and how far it reaches. One answer for everything that shows the grid - the shader
// grid (DebugDraw::DrawGridPlane), the line grid kept to compare it with, and the zoom readout's
// "grid cell" - so they always agree. Pure, so it is tested without a viewport.

module;
#include "Core/Prelude.h"
#include <cmath>

export module editor.scene:scene_grid;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    /// The plane the grid lies on (through the world origin).
    enum class GridPlane : u8
    {
        XZ, // the ground (the default)
        XY, // facing Z
        YZ, // facing X
    };

    /// A plane's two in-plane axes and its normal (unit, right-handed: u x v = normal).
    struct GridPlaneAxes
    {
        Float3 u{};
        Float3 v{};
        Float3 normal{};
    };

    [[nodiscard]] inline GridPlaneAxes AxesOf(GridPlane plane)
    {
        switch (plane)
        {
        case GridPlane::XY:
            return {Float3{1, 0, 0}, Float3{0, 1, 0}, Float3{0, 0, 1}};
        case GridPlane::YZ:
            return {Float3{0, 1, 0}, Float3{0, 0, 1}, Float3{1, 0, 0}};
        default:
            return {Float3{0, 0, 1}, Float3{1, 0, 0}, Float3{0, 1, 0}};
        }
    }

    /// The finest line spacing for a camera `distance` metres from the plane, a power of ten (10 m
    /// up shows 1 m cells, 100 m up 10 m cells), and `blend`, how far (0..1) the camera is toward
    /// the next decade, across which the finest lines fade out and the next decade's in.
    struct GridSpacing
    {
        f32 spacing = 1.0f;
        f32 blend = 0.0f;
    };

    [[nodiscard]] inline GridSpacing GridSpacingFor(f32 distance)
    {
        constexpr f32 kCellsPerDistance = 0.1f; // the finest cell is a tenth of the distance
        constexpr f32 kFinest = 0.01f;          // a centimetre: no finer, however close
        const f32 target = Max(kFinest, Max(distance, 0.0f) * kCellsPerDistance);
        const f32 decades = std::log10(target);
        const f32 whole = std::floor(decades);
        GridSpacing out;
        out.spacing = std::pow(10.0f, whole);
        out.blend = Clamp(decades - whole, 0.0f, 1.0f);
        return out;
    }

    /// How far from the camera the grid reaches before it has faded out: a broad reach near the
    /// plane (a level seen from a person's height), growing as the camera climbs, inside the
    /// editor camera's far plane.
    [[nodiscard]] inline f32 GridFadeDistance(f32 distance)
    {
        return Clamp(60.0f + 25.0f * Max(distance, 0.0f), 60.0f, 900.0f);
    }
}

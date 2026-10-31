// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :pick partition.
//
// The footprint pick that both vegetation brushes share. It finds the terrain under the cursor
// whose entity (or an ancestor) carries the TerrainVegetationComponent, ray-picks its
// heightfield, and maps the hit to the 0..1 footprint UV (the splat mapping) and to terrain-local
// space.

module;
#include "Core/Prelude.h"

export module editor.vegetation:pick;

import foundation.core;
import foundation.scene;
import foundation.heightfield;         // Heightfield
import foundation.vegetation.resource; // VegetationMask
import engine.vegetation;              // TerrainVegetationComponent, TerrainVegetationComponentManager

using namespace foundation::core;

export namespace editor
{
    struct VegetationPick
    {
        engine::vegetation::TerrainVegetationComponent* component = nullptr;
        engine::vegetation::TerrainVegetationComponentManager* manager = nullptr;
        foundation::scene::EntityHandle owner{};         // the component's entity
        foundation::scene::EntityHandle terrainEntity{}; // the TerrainComponent's entity
        foundation::heightfield::Heightfield* heightfield = nullptr;
        foundation::vegetation::VegetationMask* mask = nullptr; // the component's mask, when resolved
        Guid maskId;                                             // its source asset guid (may be nil)
        i32 gridSize = 0;    // heightfield samples per side (maps footprint regions to the grid)
        f32 uvX = 0.0f;      // hit in the 0..1 footprint UV
        f32 uvY = 0.0f;
        f32 worldSizeX = 1.0f;
        f32 worldSizeY = 1.0f;
        Float3 localHit{};   // terrain-local (the space the instances live in)
        Float3 worldHit{};
        Float3 worldNormal{0.0f, 1.0f, 0.0f};
        Float4x4 terrainWorld = Float4x4::Identity();
        bool valid = false;

        /// The nearest vegetation footprint under the ray. With `requireMask`, only components
        /// whose mask resolves count (the mask brush); otherwise any component over a terrain does
        /// (the prop brush). `ignoreHoles` picks the terrain plane through a cut
        /// (QueryRayIgnoringHoles); the prop brush uses it so props standing over a hole can be
        /// erased, while the mask brush picks only the surface.
        [[nodiscard]] static VegetationPick Resolve(foundation::scene::Scene& scene, Float3 rayOrigin,
                                                    Float3 rayDirection, bool requireMask,
                                                    bool ignoreHoles = false);

        /// Whether any vegetation footprint exists in the scene (used by the brushes' IsAvailable).
        [[nodiscard]] static bool AnyFootprint(foundation::scene::Scene& scene, bool requireMask);
    };
}

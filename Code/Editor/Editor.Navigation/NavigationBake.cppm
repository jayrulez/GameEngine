// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Navigation - the `editor.navigation` module (interface).
//
// The "Bake Navigation" flow: collect the scene's static geometry touching a zone's box (what its
// systems answer through AsStaticGeometrySource: physics' static, solid bodies and terrain's
// surface, never render meshes, which include whatever moves), transform its triangles into
// zone-local space (so the baked navmesh follows the zone entity's transform to any placement
// without a rebake), run the Recast bake, and write the result into the zone's
// NavigationZoneAsset sidecar.
//
// This interface stays lean: the heavy imports (navigation.pipeline) and the bake logic live in
// NavigationBakeImpl.cpp, because heavy interface units bloat GCC's module (gcm) build.
// Signatures use only scene, content and core types. Editor-only; the player never links this.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT (the settings section)

export module editor.navigation;

import foundation.core;
import foundation.scene;
import foundation.content;
import editor.core; // EditorContext (where the domain contributes its settings)

using namespace foundation::core;

export namespace editor::navigation
{
    namespace scene = foundation::scene;

    // The navigation domain's per-user editor settings section. It lives in the user store the
    // app owns; the domain owns its shape.
    class NavigationEditorSettings final : public ISerializable
    {
        RTTI_OBJECT(NavigationEditorSettings, ISerializable)
    public:
        bool parallelBake = true; // bake tiles across workers (output is byte-identical)

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "parallelBake", parallelBake);
        }
    };

    // Type registration. The composition root calls this before the app is constructed, because
    // the user settings file loads during app boot and an unregistered section would deserialize
    // untyped and read as defaults.
    void RegisterNavigationEditorSettingsTypes();
    // The Preferences contribution (needs the live context), called from registerEditors.
    void RegisterNavigationEditorSettings(editor::EditorContext& context);

    // The live toggle value (defaults apply when no store is wired, as when headless or in tests).
    [[nodiscard]] bool ParallelBakeEnabled(const editor::EditorContext& context);

    // Collect the triangles (in the zone entity's local space) for a bake: every scene system's
    // static geometry touching the zone box (physics: static, non-trigger bodies on active
    // entities; terrain: its surface, sampled no finer than `cellSize`, since Recast re-voxelizes
    // to its own cells and denser grids are wasted). What moves (agents, dynamic and kinematic
    // bodies, characters) and bare render meshes are not level geometry. Returns the triangle
    // count (0 means nothing to bake).
    [[nodiscard]] usize CollectNavigationGeometry(scene::Scene& scene, scene::EntityHandle zoneEntity,
                                                  Float3 zoneExtents, f32 cellSize,
                                                  Array<Float3>& outVertices,
                                                  Array<u32>& outIndices);

    struct BakeResult
    {
        bool baked = false;      // a navmesh was produced and written
        usize triangleCount = 0; // input triangles collected
    };

    // Bake the zone owned by `zoneEntity` and write the result into `targetAsset` (the zone's
    // NavigationZoneAsset instance). The bake params come from the zone component. An empty
    // collection or a degenerate bake writes an empty asset (a valid "no navmesh yet" state).
    [[nodiscard]] BakeResult BakeNavigationZone(scene::Scene& scene, scene::EntityHandle zoneEntity,
                                                foundation::content::Instance& targetAsset,
                                                bool parallelBake = true);

    /// What a bake's result means, for whoever started it (the inspector's notice, the
    /// navigation_bake tool's answer): what was written, or what to change when nothing walkable
    /// came out of it.
    [[nodiscard]] String DescribeBake(const BakeResult& result);

    struct RegionRebakeResult
    {
        bool rebaked = false;    // the asset was updated
        bool fullBake = false;   // no tiled blob to patch, so it fell back to a full bake
        usize tilesRebuilt = 0;  // tiles regenerated (incl. ones that became empty)
    };

    // Partial rebake (the per-tile consumer): regenerate only the tiles whose bounds
    // intersect `worldMin`..`worldMax` (an edited region, such as a moved obstacle or a terrain
    // sculpt), patching them into the zone's existing tiled blob. Everything outside the
    // region keeps its exact bytes, and a patched blob equals a full rebake of the same
    // scene byte-for-byte (the grid is anchored by the original bake). Falls back to a full
    // BakeNavigationZone when the asset has no tiled blob yet.
    [[nodiscard]] RegionRebakeResult
    RebakeNavigationZoneRegion(scene::Scene& scene, scene::EntityHandle zoneEntity,
                               foundation::content::Instance& targetAsset, Float3 worldMin,
                               Float3 worldMax, bool parallelBake = true);

    RTTI_DEFINE_OBJECT(NavigationEditorSettings, "rtti::editor::navigation")
}

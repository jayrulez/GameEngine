// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_view_state partition.
//
// Where a scene was left, kept per scene in the project's editor settings (SceneViewPref): the
// overlays, the camera, the selection and the page's panes. The page reads it when it opens and
// writes the whole of it on a toggle and when it closes, so saving one part never brings back
// another's default. The panes are one splitter of three on the GUI (hierarchy, viewport,
// inspector) where the prefs keep the old page's two nested ratios (hierarchy | the rest, then
// viewport | inspector), so the weights convert both ways and the prefs' format stands. Out of the
// page's class (Documentation/Specs/editor-gui.md, 5.3, layer 2).

module;
#include "Core/Prelude.h"

export module editor.scene:scene_view_state;

import foundation.core;
import foundation.scene;
import foundation.settings;
import editor.core;
import editor.camera;
import :view_settings;
import :scene_grid;
import :scene_overlays;

using namespace foundation::core;

export namespace editor
{
    /// The page's three panes' shares of its width.
    struct ScenePaneWeights
    {
        f32 hierarchy = 0.2f;
        f32 viewport = 0.56f;
        f32 inspector = 0.24f;
    };

    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct SceneViewState
    {
        /// The weights from the prefs' nested ratios: `hierarchy` of the whole, then `inspector`
        /// (the viewport's share) of the rest.
        [[nodiscard]] static ScenePaneWeights WeightsFromSplits(f32 hierarchy, f32 inspector)
        {
            const f32 h = Clamp(hierarchy, 0.0f, 1.0f);
            const f32 i = Clamp(inspector, 0.0f, 1.0f);
            return ScenePaneWeights{h, (1.0f - h) * i, (1.0f - h) * (1.0f - i)};
        }
        /// The prefs' nested ratios from the weights (any scale).
        static void SplitsFromWeights(const ScenePaneWeights& weights, f32& hierarchy, f32& inspector)
        {
            const f32 total = weights.hierarchy + weights.viewport + weights.inspector;
            hierarchy = total > 0.0f ? weights.hierarchy / total : 0.2f;
            const f32 rest = weights.viewport + weights.inspector;
            inspector = rest > 0.0f ? weights.viewport / rest : 0.72f;
        }

        [[nodiscard]] static SceneOverlaySettings OverlaysFrom(const SceneViewPref& pref)
        {
            SceneOverlaySettings overlays;
            overlays.showGrid = pref.showGrid;
            overlays.showLodOverlay = pref.showLodOverlay;
            overlays.showColliders = pref.showColliders;
            overlays.showMarkers = pref.showMarkers;
            overlays.showFps = pref.showFps;
            overlays.gridPlane = pref.gridPlane <= static_cast<u8>(GridPlane::YZ) ? static_cast<GridPlane>(pref.gridPlane) : GridPlane::XZ;
            overlays.gridLines = pref.gridLines;
            return overlays;
        }

        /// This scene's overlays as saved; `current` where nothing is (an unsaved scene, a nil id).
        [[nodiscard]] static SceneOverlaySettings LoadOverlays(foundation::settings::Settings* store, const Guid& scene,
                                                               const SceneOverlaySettings& current)
        {
            SceneViewPref fallback{scene, current.showGrid, current.showLodOverlay, current.showColliders, current.showMarkers, current.showFps};
            fallback.gridPlane = static_cast<u8>(current.gridPlane);
            fallback.gridLines = current.gridLines;
            return OverlaysFrom(LoadSceneViewPref(store, scene, fallback));
        }

        /// The whole pref as the page has it now (`toolPanelPlace` where the tool panel was moved;
        /// null while it sits in its zone).
        [[nodiscard]] static SceneViewPref Capture(const Guid& scene, const SceneOverlaySettings& overlays, const EditorCamera& camera,
                                                   const ScenePaneWeights* panes, Span<const Guid> selection, const Float2* toolPanelPlace = nullptr)
        {
            SceneViewPref pref{scene, overlays.showGrid, overlays.showLodOverlay, overlays.showColliders, overlays.showMarkers, overlays.showFps};
            pref.gridPlane = static_cast<u8>(overlays.gridPlane);
            pref.gridLines = overlays.gridLines;
            pref.hasCamera = true;
            pref.cameraPosition = camera.position;
            pref.cameraYaw = camera.yaw;
            pref.cameraPitch = camera.pitch;
            pref.cameraFocusDistance = camera.focusDistance;
            pref.cameraOrthographic = camera.orthographic;
            if (panes != nullptr)
            {
                pref.hasSplits = true;
                SplitsFromWeights(*panes, pref.hierarchySplit, pref.inspectorSplit);
            }
            for (const Guid& id : selection)
                pref.selection.PushBack(id);
            if (toolPanelPlace != nullptr)
            {
                pref.hasToolPanelPlace = true;
                pref.toolPanelPlace = *toolPanelPlace;
            }
            return pref;
        }

        /// The camera where the pref left it; false when no page saved one (the caller's framing stands).
        static bool RestoreCamera(const SceneViewPref& pref, EditorCamera& camera)
        {
            if (!pref.hasCamera)
                return false;
            camera.position = pref.cameraPosition;
            camera.yaw = pref.cameraYaw;
            camera.pitch = pref.cameraPitch;
            camera.focusDistance = pref.cameraFocusDistance;
            camera.SetOrthographic(pref.cameraOrthographic);
            return true;
        }

        /// The saved selection's ids still in `scene` (deleted since, or spawned by a Simulate and
        /// selected at close: never a ghost).
        [[nodiscard]] static Array<Guid> LiveSelection(const SceneViewPref& pref, foundation::scene::Scene& scene)
        {
            Array<Guid> live;
            for (const Guid& id : pref.selection)
                if (scene.FindEntity(id).IsAssigned())
                    live.PushBack(id);
            return live;
        }
    };
}

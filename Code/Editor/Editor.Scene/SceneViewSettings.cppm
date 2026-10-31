// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :view_settings partition.
//
// Per-scene view state (not scene data). The scene page is the unit of editing context when several
// scenes are open, so everything a page toggles is per page, and it persists per scene. It is kept
// in the per-project editor settings store (EditorContext::ProjectEditorSettings()), keyed by the
// scene's guid, like the mesh and material preview prefs. It carries the grid and the other
// overlays, the camera, the selection, the splits and the tool panel's place; more page-scoped
// viewport toggles become more fields on SceneViewPref.
//
// It lives in its own light partition, not the heavy ScenePage.cppm interface, so the RTTI body
// stays out of that interface's compiled module and the section can be named from tests.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:view_settings;

import foundation.core;
import foundation.settings;

using namespace foundation::core;

export namespace editor
{
    namespace settings = foundation::settings;

    // One scene's saved view state, keyed by its guid. The defaults match the page's defaults
    // (grid on, LOD-overlay off) so an absent pref reads as the fresh-page look.
    struct SceneViewPref
    {
        // The toggles by position (the shape every caller uses); the version 4 camera and selection
        // fields are set by name, so a partial brace init never trips -Wmissing-field-initializers.
        SceneViewPref() = default;
        explicit SceneViewPref(Guid sceneId, bool grid = true, bool lod = false,
                               bool colliders = false, bool markers = false, bool fps = false)
            : scene(sceneId), showGrid(grid), showLodOverlay(lod), showColliders(colliders),
              showMarkers(markers), showFps(fps)
        {
        }

        Guid scene;
        bool showGrid = true;
        bool showLodOverlay = false;
        bool showColliders = false; // edit-time physics collider wireframes (v3)
        bool showMarkers = false;   // the origin cross on every entity (off by default to avoid
                                    // clutter; the selected entity keeps its own)
        bool showFps = false;       // the frame-rate readout in the viewport's top-right corner
        // The editor camera as the page last had it (v4): restored on reopen so a scene comes
        // back where it was left. hasCamera = false until a page has saved one.
        bool hasCamera = false;
        Float3 cameraPosition{};
        f32 cameraYaw = 0.0f;
        f32 cameraPitch = 0.0f;
        f32 cameraFocusDistance = 12.0f;
        bool cameraOrthographic = false; // an orthographic projection
        // The entity selection at close (persistent ids; ids no longer in the scene are dropped).
        Array<Guid> selection;
        // The page's split positions as it last had them: hierarchy against the rest, viewport
        // column against inspector, and viewport against the bottom dock. hasSplits is false until
        // a page has saved them, and the page's own defaults stand meanwhile.
        bool hasSplits = false;
        f32 hierarchySplit = 0.2f;
        f32 inspectorSplit = 0.72f;
        f32 bottomDockSplit = 0.72f;
        // The grid: the plane it lies on (GridPlane: 0 XZ, 1 XY, 2 YZ), and
        // whether it is drawn as debug lines instead of the shader grid (kept to compare them).
        u8 gridPlane = 0;
        bool gridLines = false;
        // Where the tool panel was moved to in the viewport: its top start corner, in pixels from
        // the viewport's; hasToolPanelPlace is false while it sits in its zone under the chips.
        bool hasToolPanelPlace = false;
        Float2 toolPanelPlace{};

        // The section's data version gates the keys added after version 3 (a keyed reader fails the
        // whole section on a missing key, so a version 3 file must not be asked for them).
        static constexpr u32 kVersionWithOverlaysAndCamera = 4;

        void Serialize(ISerializer& ar)
        {
            ar.Key("scene");
            ar.GuidValue(scene);
            foundation::core::Serialize(ar, "showGrid", showGrid);
            foundation::core::Serialize(ar, "showLodOverlay", showLodOverlay);
            foundation::core::Serialize(ar, "showColliders", showColliders);
            if (ar.Version() >= kVersionWithOverlaysAndCamera)
            {
                foundation::core::Serialize(ar, "showMarkers", showMarkers);
                foundation::core::Serialize(ar, "showFps", showFps);
                foundation::core::Serialize(ar, "hasCamera", hasCamera);
                foundation::core::Serialize(ar, "cameraPosition", cameraPosition);
                foundation::core::Serialize(ar, "cameraYaw", cameraYaw);
                foundation::core::Serialize(ar, "cameraPitch", cameraPitch);
                foundation::core::Serialize(ar, "cameraFocusDistance", cameraFocusDistance);
                foundation::core::Serialize(ar, "selection", selection);
                // Appended keys: a settings file saved before them still loads, with the defaults.
                foundation::core::SerializeAppended(ar, "hasSplits", hasSplits);
                foundation::core::SerializeAppended(ar, "hierarchySplit", hierarchySplit);
                foundation::core::SerializeAppended(ar, "inspectorSplit", inspectorSplit);
                foundation::core::SerializeAppended(ar, "bottomDockSplit", bottomDockSplit);
                foundation::core::SerializeAppended(ar, "gridPlane", gridPlane);
                foundation::core::SerializeAppended(ar, "gridLines", gridLines);
                foundation::core::SerializeAppended(ar, "cameraOrthographic", cameraOrthographic);
                foundation::core::SerializeAppended(ar, "hasToolPanelPlace", hasToolPanelPlace);
                foundation::core::SerializeAppended(ar, "toolPanelPlace", toolPanelPlace);
            }
        }
    };

    // The FPS overlay's readout for one sample window: "60 fps  16.7 ms" (the frame time is
    // the window's mean). An empty window (no frames yet) reads as "-- fps".
    [[nodiscard]] inline String FrameRateOverlayText(f64 windowSeconds, u32 frames)
    {
        if (frames == 0 || windowSeconds <= 0.0)
        {
            return String(u8"-- fps");
        }
        const f64 fps = static_cast<f64>(frames) / windowSeconds;
        const f64 ms = windowSeconds * 1000.0 / static_cast<f64>(frames);
        const i64 tenths = static_cast<i64>(ms * 10.0 + 0.5); // one decimal, rounded
        return Format(u8"{} fps  {}.{} ms", static_cast<i64>(fps + 0.5), tenths / 10, tenths % 10);
    }
    inline void Serialize(ISerializer& ar, SceneViewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }

    // The settings section: a flat list of per-scene prefs (a linear scan, since a project has a
    // handful of scenes, not thousands).
    class SceneViewSettings final : public ISerializable
    {
        RTTI_OBJECT(SceneViewSettings, ISerializable)
    public:
        Array<SceneViewPref> prefs;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "prefs", prefs);
        }
    };

    // Register the section type (call before the app loads the per-project store).
    inline void RegisterSceneViewSettingsType()
    {
        // Version 4 (markers, FPS, camera, selection) keeps a version 3 legacy reader, so prefs
        // saved before those keys load without them and are saved again as version 4.
        KeepLegacyReader(const_cast<TypeInfo&>(SceneViewSettings::StaticType()), 3);
        GlobalTypeRegistry().Register(SceneViewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<SceneViewSettings>();
    }

    // Reads a scene's saved view pref; returns `fallback` when the store or the scene has no saved
    // pref (a nil guid means an unsaved scene, edited live only). Never mutates the store. The
    // whole pref is loaded and saved together, so toggling one field never brings back another's
    // default.
    [[nodiscard]] inline SceneViewPref LoadSceneViewPref(settings::Settings* store, const Guid& scene,
                                                         const SceneViewPref& fallback)
    {
        if (store == nullptr || scene.IsNil())
        {
            return fallback;
        }
        if (const SceneViewSettings* section = store->Find<SceneViewSettings>())
        {
            for (const SceneViewPref& p : section->prefs)
            {
                if (p.scene == scene)
                {
                    return p;
                }
            }
        }
        return fallback;
    }

    // Inserts or updates a scene's view pref in the store (marking the section changed). Returns
    // false without touching the store when there is nothing to key by (no store, or an unsaved
    // scene with a nil guid); the caller then skips its RequestProjectEditorSettingsSave.
    inline bool SaveSceneViewPref(settings::Settings* store, const SceneViewPref& pref)
    {
        if (store == nullptr || pref.scene.IsNil())
        {
            return false;
        }
        SceneViewSettings& section = store->Section<SceneViewSettings>();
        for (SceneViewPref& p : section.prefs)
        {
            if (p.scene == pref.scene)
            {
                p = pref;
                store->MarkChanged<SceneViewSettings>();
                return true;
            }
        }
        section.prefs.PushBack(pref);
        store->MarkChanged<SceneViewSettings>();
        return true;
    }

    RTTI_DEFINE_OBJECT_VERSIONED(SceneViewSettings, "rtti::editor::editor.scene", 4)
}

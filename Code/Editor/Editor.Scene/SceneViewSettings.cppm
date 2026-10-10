// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :view_settings partition.
//
// PER-SCENE view state (not scene DATA): the scene page is the unit of editing context in the
// multi-scene model, so everything a page toggles is per-page - and its persistence is per-scene
// too. Kept in the per-project editor settings store (EditorContext::ProjectEditorSettings()) keyed
// by the scene's guid, mirroring the mesh/material preview prefs. Carries the grid on/off toggle;
// additional page-scoped viewport toggles (gizmo visibility, snap, ...) are more fields on SceneViewPref.
//
// Lives in its own light partition (not the heavy ScenePage.cppm interface) so the RTTI body stays
// out of that interface's gcm and the section is nameable from tests.

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
        // The toggles by position (every caller's shape); the v4 camera / selection fields are
        // set by name, so a partial brace init never trips -Wmissing-field-initializers.
        SceneViewPref() = default;
        explicit SceneViewPref(Guid sceneId, bool grid = true, bool lod = false,
                               bool colliders = false, bool markers = true, bool fps = false)
            : scene(sceneId), showGrid(grid), showLodOverlay(lod), showColliders(colliders),
              showMarkers(markers), showFps(fps)
        {
        }

        Guid scene;
        bool showGrid = true;
        bool showLodOverlay = false;
        bool showColliders = false; // edit-time physics collider wireframes (v3)
        bool showMarkers = true;    // the origin cross on every entity (off for a large scene)
        bool showFps = false;       // the frame-rate readout in the viewport's top-right corner
        // The editor camera as the page last had it (v4): restored on reopen so a scene comes
        // back where it was left. hasCamera = false until a page has saved one.
        bool hasCamera = false;
        Float3 cameraPosition{};
        f32 cameraYaw = 0.0f;
        f32 cameraPitch = 0.0f;
        f32 cameraFocusDistance = 12.0f;
        // The entity selection at close (persistent ids; ids no longer in the scene are dropped).
        Array<Guid> selection;
        // The page's split positions as it last had them (added 2026-10-10): hierarchy | rest,
        // viewport column | inspector, viewport / bottom dock. hasSplits = false until a page has
        // saved them, and the page's own defaults stand.
        bool hasSplits = false;
        f32 hierarchySplit = 0.2f;
        f32 inspectorSplit = 0.72f;
        f32 bottomDockSplit = 0.72f;
        // The grid (added 2026-10-10): the plane it lies on (GridPlane: 0 XZ, 1 XY, 2 YZ), and
        // whether it is drawn as debug lines instead of the shader grid (kept to compare them).
        u8 gridPlane = 0;
        bool gridLines = false;

        // The section's data version gates the keys added after v3 (a keyed reader FAILS the
        // whole section on a missing key, so a v3 file must not be asked for them).
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

    // The settings section: a flat list of per-scene prefs (linear scan - a project has a handful of
    // scenes, not thousands).
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
        // v4 (markers / FPS / camera / selection) keeps a v3 legacy reader: a project's saved
        // prefs from before 2026-09-24 load without the new keys and re-save as v4.
        const_cast<TypeInfo&>(SceneViewSettings::StaticType()).minReadDataVersion = 3;
        GlobalTypeRegistry().Register(SceneViewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<SceneViewSettings>();
    }

    // Read a scene's saved view pref; returns `fallback` when the store/scene has no saved pref (a
    // nil guid = an unsaved scene = edit-live-only). Never mutates the store. The WHOLE pref is
    // loaded/saved together so toggling one field never resurrects another's default.
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

    // Upsert a scene's view pref into the store (marks the section changed). Returns false without
    // touching the store when there is nothing to key by (no store / unsaved scene = nil guid) - the
    // caller then skips its RequestProjectEditorSettingsSave.
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

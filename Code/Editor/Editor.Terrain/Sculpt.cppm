// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :sculpt partition.
//
// TerrainSculptTool: the scene-viewport terrain brush (editor.viewporttools). It is a modal tool:
// the select tool stays the default, and sculpt is one of the palette modes. It ray-picks the
// terrain under the cursor (through the entity transform, into heightfield-local space), edits the
// shared runtime Heightfield in place with the cosine-falloff brushes (foundation.heightfield),
// and records one region-delta undo command per stroke. On save the tool's registered closure
// writes the runtime heightfield back to its source asset (HeightfieldSource and the "heights"
// sidecar) through the drain path.
//
// For GCC module hygiene, the heavy engine.terrain, foundation.terrain.resource and
// heightfield.resource imports live in SculptImpl.cpp; this interface imports only light modules.

module;
#include "Core/Prelude.h"

export module editor.terrain:sculpt;

import foundation.core;
import foundation.scene;       // EntityHandle
import foundation.render;      // debug::DebugDraw (the Draw override signature)
import foundation.heightfield; // Heightfield (RefPtr kept alive across a stroke), HeightfieldRegion
import editor.core;            // EditorCommandStack, IAssetEditSink
import editor.viewporttools;   // IViewportTool, IViewportToolProvider, ViewportToolInput

using namespace foundation::core;

export namespace editor
{
    /// The terrain sculpt brush. One per scene page (created by the provider); borrows the scene,
    /// command stack and asset-edit sink from the host context.
    class TerrainSculptTool final : public IViewportTool
    {
    public:
        enum class Mode : u8
        {
            Raise,   // push the surface up
            Lower,   // push it down
            Smooth,  // pull toward the local neighbourhood average
            Flatten, // pull toward a picked target height (Ctrl+click sets the target)
        };

        TerrainSculptTool(foundation::scene::Scene& scene, EditorCommandStack& commands,
                          IAssetEditSink* assetEdits)
            : m_scene(&scene), m_commands(&commands), m_assetEdits(assetEdits)
        {
        }

        [[nodiscard]] StringView Id() const override { return u8"terrain.sculpt"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Sculpt"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/Types/TerrainComponent.svg"; }
        [[nodiscard]] StringView Category() const override { return u8"Terrain"; }

        /// Available only when the scene has a terrain whose heightfield resolves.
        [[nodiscard]] bool IsAvailable() const override;
        [[nodiscard]] StringView UnavailableReason() const override
        {
            return u8"Sculpt needs a terrain with a heightfield in the scene.";
        }

        [[nodiscard]] bool Update(const ViewportToolInput& input) override;
        void Draw(foundation::render::debug::DebugDraw& drawList) override;
        void OnDeactivate() override;
        [[nodiscard]] StringView StatusText() const override { return m_status.AsView(); }

        // ---- brush parameters (the tool panel drives these; hotkeys and the wheel also set them) ----
        void SetMode(Mode mode) noexcept { m_mode = mode; }
        [[nodiscard]] Mode GetMode() const noexcept { return m_mode; }
        void SetRadius(f32 r)
        {
            m_radius = Clamp(r, kMinRadius, kMaxRadius);
        }
        [[nodiscard]] f32 Radius() const noexcept { return m_radius; }
        void SetStrength(f32 s) noexcept { m_strength = Clamp(s, 0.0f, kMaxStrength); }
        [[nodiscard]] f32 Strength() const noexcept { return m_strength; }

    private:
        static constexpr f32 kMinRadius = 0.5f;
        static constexpr f32 kMaxRadius = 128.0f;
        static constexpr f32 kMaxStrength = 50.0f;

        // The terrain the cursor is over this frame (resolved every Update), and the pick on it.
        struct Pick
        {
            foundation::heightfield::Heightfield* hf = nullptr;
            Guid heightfieldId;   // TerrainResource.heightfield.id (the source asset guid; may be nil)
            f32 localX = 0.0f;    // hit point in heightfield-local XZ
            f32 localZ = 0.0f;
            f32 localY = 0.0f;    // local surface Y at the hit (Ctrl+click flatten target)
            Float3 worldHit{};    // hit point in world space (cursor draw)
            Float3 worldNormal{0.0f, 1.0f, 0.0f};
            bool valid = false;
        };
        [[nodiscard]] Pick ResolvePick(const ViewportToolInput& input) const;

        void BeginStroke(const Pick& pick);
        void ApplyDab(const Pick& pick, f32 deltaSeconds);
        void EndStroke();
        void UpdateStatus();

        foundation::scene::Scene* m_scene;      // borrowed (the page owns it)
        EditorCommandStack* m_commands;         // borrowed
        IAssetEditSink* m_assetEdits = nullptr; // borrowed; null means nothing is saved (tests)

        Mode m_mode = Mode::Raise;
        f32 m_radius = 6.0f;      // world units
        f32 m_strength = 8.0f;    // world-Y units per second at the brush center
        f32 m_flattenTarget = 0.0f;
        bool m_hasFlattenTarget = false;

        // Hover state for the cursor overlay: refreshed every frame, cleared when off any terrain.
        bool m_hasHover = false;
        Float3 m_hoverWorld{};
        Float3 m_hoverNormal{0.0f, 1.0f, 0.0f};

        // Stroke state (one command from press to release). m_strokeHf keeps the grid alive for
        // the whole stroke. m_before is a full snapshot taken at press, because the union region
        // is unknown until release, when the command slices its before and after blocks out of it.
        bool m_stroking = false;
        RefPtr<foundation::heightfield::Heightfield> m_strokeHf;
        Guid m_strokeHeightfieldId;
        Array<foundation::heightfield::Height> m_before;
        foundation::heightfield::HeightfieldRegion m_region;

        String m_status;
    };

    /// The provider that adds the sculpt tool to every scene viewport. Editor.Terrain's registrar
    /// registers it explicitly; it is not discovered.
    class TerrainViewportToolProvider final : public IViewportToolProvider
    {
    public:
        void CreateTools(ViewportToolManager& manager,
                         const ViewportToolHostContext& context) override;
    };

    /// Register the terrain viewport tools (call once at editor start, from Tools.Editor).
    void RegisterTerrainViewportTools();
}

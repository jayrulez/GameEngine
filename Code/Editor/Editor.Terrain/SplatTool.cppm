// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :splat partition.
//
// TerrainSplatTool: the scene-viewport terrain layer-weight brush (editor.viewporttools), the second
// terrain tool after Sculpt. It ray-picks the terrain heightfield under the cursor, maps the hit to
// the splatmap's 0..1 footprint UV, and paints the SELECTED layer channel into the SHARED runtime
// Splatmap (lerp-to-one-hot - painting another layer erases this one), records ONE region-delta undo
// command per stroke, and on Save writes the painted RGBA8 back to its SOURCE asset's pixel sidecar.
//
// GCC module hygiene: the heavy engine.terrain / resource imports live in SplatToolImpl.cpp; this
// interface stays on the light modules only.

module;
#include "Core/Prelude.h"

export module editor.terrain:splat;

import foundation.core;
import foundation.scene;            // EntityHandle
import foundation.render;           // debug::DebugDraw (the Draw override signature)
import foundation.terrain.resource; // SplatWeights (RefPtr kept alive across a stroke), SplatRegion
import editor.core;                 // EditorCommandStack, IAssetEditSink
import editor.viewporttools;        // IViewportTool, ViewportToolInput

using namespace foundation::core;

export namespace editor
{
    /// The terrain splat (layer-weight) brush. One per scene page (created by the terrain provider);
    /// borrows the scene, command stack and asset-edit sink from the host context.
    class TerrainSplatTool final : public IViewportTool
    {
    public:
        TerrainSplatTool(foundation::scene::Scene& scene, EditorCommandStack& commands,
                         IAssetEditSink* assetEdits)
            : m_scene(&scene), m_commands(&commands), m_assetEdits(assetEdits)
        {
        }

        [[nodiscard]] StringView Id() const override { return u8"terrain.splat"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Paint Layers"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/texture.svg"; }
        [[nodiscard]] StringView Category() const override { return u8"Terrain"; }

        /// Relevant only when the scene has a terrain whose splatmap (and heightfield, for the pick)
        /// resolves.
        [[nodiscard]] bool IsAvailable() const override;
        [[nodiscard]] StringView UnavailableReason() const override
        {
            return u8"Paint Layers needs a terrain whose splatmap and heightfield resolve.";
        }

        [[nodiscard]] bool Update(const ViewportToolInput& input) override;
        void Draw(foundation::render::debug::DebugDraw& drawList) override;
        void OnDeactivate() override;
        [[nodiscard]] StringView StatusText() const override { return m_status.AsView(); }

        // ---- brush parameters (the tool panel + the page's palette list drive these) ----
        /// Select a PALETTE layer to paint (0..255; the palette is unbounded).
        void SetPaletteIndex(u32 index) noexcept
        {
            m_paletteIndex = Min(index, 255u);
            m_erase = false;
            m_smooth = false;
        }
        [[nodiscard]] u32 PaletteIndex() const noexcept { return m_paletteIndex; }
        /// Eraser mode: fades all painted weights toward 0, revealing the BASE layer.
        void SetEraser(bool erase) noexcept
        {
            m_erase = erase;
            if (erase)
            {
                m_smooth = false;
            }
        }
        [[nodiscard]] bool IsEraser() const noexcept { return m_erase; }
        /// Smooth mode: blurs each texel's weights toward its neighborhood average, feathering
        /// an already-painted seam without repainting either side.
        void SetSmooth(bool smooth) noexcept
        {
            m_smooth = smooth;
            if (smooth)
            {
                m_erase = false;
            }
        }
        [[nodiscard]] bool IsSmooth() const noexcept { return m_smooth; }
        void SetRadius(f32 r)
        {
            m_radius = Clamp(r, kMinRadius, kMaxRadius);
        }
        [[nodiscard]] f32 Radius() const noexcept { return m_radius; }
        void SetStrength(f32 s) noexcept { m_strength = Clamp(s, 0.0f, 1.0f); }
        [[nodiscard]] f32 Strength() const noexcept { return m_strength; }
        /// Stamp spacing as a fraction of the brush radius. Low spacing + low strength = dense,
        /// smooth blending along the stroke; high spacing = discrete dabs.
        void SetSpacing(f32 s) noexcept { m_spacing = Clamp(s, kMinSpacing, 1.0f); }
        [[nodiscard]] f32 Spacing() const noexcept { return m_spacing; }
        /// Airbrush mode: while the button is held, ALSO deposit stamps on a time cadence at the
        /// cursor. Airbrush deposits strength x period per stamp, so strength reads as coverage
        /// PER SECOND of hover (the soft-blend workflow); off = movement-only stamps.
        void SetAirbrush(bool on) noexcept { m_airbrush = on; }
        [[nodiscard]] bool IsAirbrush() const noexcept { return m_airbrush; }

    private:
        static constexpr f32 kMinRadius = 0.5f;
        static constexpr f32 kMaxRadius = 128.0f;
        static constexpr f32 kMinSpacing = 0.05f; // of the radius (spacing 0 would stamp forever)

        struct Pick
        {
            foundation::terrain::SplatWeights* weights = nullptr;
            Guid weightsId;       // TerrainResource.weights.id (the SOURCE asset guid; may be nil)
            f32 uvX = 0.0f;       // hit in the splatmap's 0..1 footprint UV
            f32 uvY = 0.0f;
            f32 worldSizeX = 1.0f; // footprint X (world radius -> per-axis uv radius)
            f32 worldSizeY = 1.0f; // footprint Z (keeps the brush a world circle on non-square terrain)
            Float3 worldHit{};
            Float3 worldNormal{0.0f, 1.0f, 0.0f};
            bool valid = false;
        };
        [[nodiscard]] Pick ResolvePick(const ViewportToolInput& input) const;

        void BeginStroke(const Pick& pick);
        // Distance-spaced stamps along the drag (+ time-cadence stamps when airbrush is on).
        void AdvanceStroke(const Pick& pick, f32 deltaSeconds);
        void ApplyStamp(f32 uvX, f32 uvY, const Pick& pick, f32 amount);
        void EndStroke();
        void UpdateStatus();

        foundation::scene::Scene* m_scene;      // borrowed (page owns it)
        EditorCommandStack* m_commands;         // borrowed
        IAssetEditSink* m_assetEdits = nullptr; // borrowed; null = no persistence (tests)

        u32 m_paletteIndex = 0; // which PALETTE layer the brush paints (0..255)
        bool m_erase = false;   // eraser mode (reveals the base)
        bool m_smooth = false;  // smooth mode (blurs weights toward the neighborhood average)
        f32 m_radius = 6.0f;   // world units
        // Per-STAMP coverage fraction at the brush centre (0..1; 1 = one-hot in one stamp, and a
        // hard eraser). Stamps are spaced along the stroke's world-space travel (m_spacing of the
        // radius) - Unity/Unreal-style: frame-rate AND drag-speed independent; holding still
        // deposits nothing beyond the press stamp unless AIRBRUSH is on (time-cadence stamps at
        // the cursor); sub-1 strengths otherwise build up by scrubbing.
        f32 m_strength = 1.0f;
        f32 m_spacing = 0.25f;  // stamp spacing, fraction of the radius
        bool m_airbrush = false;
        f32 m_airbrushClock = 0.0f; // accrued hold time toward the next time-cadence stamp

        // Hover (cursor overlay).
        bool m_hasHover = false;
        Float3 m_hoverWorld{};
        Float3 m_hoverNormal{0.0f, 1.0f, 0.0f};

        // Stroke state (one command per press..release). m_strokeWeights keeps the rasters alive
        // for the whole stroke; the full-raster snapshots cover BOTH rasters (the union region is
        // unknown until release, when the command slices before/after out of them).
        bool m_stroking = false;
        f32 m_lastStampU = 0.0f; // UV of the last deposited stamp (the spacing walker's anchor)
        f32 m_lastStampV = 0.0f;
        RefPtr<foundation::terrain::SplatWeights> m_strokeWeights;
        Guid m_strokeWeightsId;
        Array<u8> m_beforeWeights;
        Array<u8> m_beforeIndices;
        foundation::terrain::SplatRegion m_region;

        String m_status;
    };
}

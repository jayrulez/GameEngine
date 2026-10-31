// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :paint partition.
//
// VegetationPaintTool: the scene-viewport vegetation mask brush (editor.viewporttools). It
// ray-picks the terrain heightfield under the cursor, maps the hit to the mask's 0..1 footprint UV,
// and paints, erases or smooths the selected plane of the terrain's vegetation mask (the shared
// runtime VegetationMask that the TerrainVegetationComponent references). Each stamp tells the
// vegetation manager which footprint rect changed, so only the touched chunks regrow. Each stroke
// is one region-delta undo command. On Save the painted planes are written back to the source
// VegetationMaskAsset's "densities" sidecar, and an imported mask becomes embedded.
//
// For GCC module hygiene, the heavy engine and resource imports live in
// VegetationPaintToolImpl.cpp; this interface imports only light modules.

module;
#include "Core/Prelude.h"

export module editor.vegetation:paint;

import foundation.core;
import foundation.scene;               // EntityHandle
import foundation.render;              // debug::DebugDraw (the Draw override signature)
import foundation.vegetation.resource; // VegetationMask (RefPtr kept alive across a stroke), MaskRegion
import editor.core;                    // EditorCommandStack, IAssetEditSink
import editor.viewporttools;           // IViewportTool, IViewportToolProvider, ViewportToolInput
import :pick;                          // VegetationPick (the shared terrain pick)
import :scatter;                       // VegetationScatterTool (the provider creates both brushes)

using namespace foundation::core;

export namespace editor
{
    /// The vegetation mask brush. One per scene page (created by the vegetation provider);
    /// borrows the scene, command stack and asset-edit sink from the host context.
    class VegetationPaintTool final : public IViewportTool
    {
    public:
        VegetationPaintTool(foundation::scene::Scene& scene, EditorCommandStack& commands,
                            IAssetEditSink* assetEdits)
            : m_scene(&scene), m_commands(&commands), m_assetEdits(assetEdits)
        {
        }

        [[nodiscard]] StringView Id() const override { return u8"vegetation.paint"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Paint Vegetation"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/Types/TerrainVegetationComponent.svg"; }
        [[nodiscard]] StringView Category() const override { return u8"Vegetation"; }
        /// Available only when the scene has a vegetation component whose mask (and its terrain's
        /// heightfield, for the pick) resolves.
        [[nodiscard]] bool IsAvailable() const override;
        [[nodiscard]] StringView UnavailableReason() const override
        {
            return u8"Paint Vegetation needs a Terrain Vegetation component with a Mask asset, "
                   u8"on a terrain (Create > Terrain > Vegetation Mask, then pick it in Mask).";
        }
        [[nodiscard]] bool Update(const ViewportToolInput& input) override;
        void Draw(foundation::render::debug::DebugDraw& drawList) override;
        void OnDeactivate() override;
        [[nodiscard]] StringView StatusText() const override { return m_status.AsView(); }

        // ---- brush parameters (the tool panel drives these) ----
        /// Selects the mask plane the brush works on (a layer with Mask placement names its
        /// plane). The mode (paint, erase or smooth) is a separate choice and is kept: erasing
        /// works per plane, so choosing plane 2 while erasing erases plane 2.
        void SetPlane(u32 plane)
        {
            m_plane = plane;
            UpdateStatus(); // the status bar names the plane and mode without waiting for a frame
        }
        [[nodiscard]] u32 Plane() const noexcept { return m_plane; }
        /// Eraser mode: fades the plane toward 0 (nothing grows).
        void SetEraser(bool erase)
        {
            m_erase = erase;
            if (erase)
            {
                m_smooth = false;
            }
            UpdateStatus();
        }
        [[nodiscard]] bool IsEraser() const noexcept { return m_erase; }
        /// Smooth mode: blurs the plane toward its neighbourhood average (feathers an edge).
        void SetSmooth(bool smooth)
        {
            m_smooth = smooth;
            if (smooth)
            {
                m_erase = false;
            }
            UpdateStatus();
        }
        [[nodiscard]] bool IsSmooth() const noexcept { return m_smooth; }
        void SetRadius(f32 r)
        {
            m_radius = Clamp(r, kMinRadius, kMaxRadius);
        }
        [[nodiscard]] f32 Radius() const noexcept { return m_radius; }
        void SetStrength(f32 s) noexcept { m_strength = Clamp(s, 0.0f, 1.0f); }
        [[nodiscard]] f32 Strength() const noexcept { return m_strength; }
        /// Stamp spacing as a fraction of the brush radius.
        void SetSpacing(f32 s) noexcept { m_spacing = Clamp(s, kMinSpacing, 1.0f); }
        [[nodiscard]] f32 Spacing() const noexcept { return m_spacing; }
        /// Airbrush: while held, also deposit stamps on a time cadence at the cursor.
        void SetAirbrush(bool on) noexcept { m_airbrush = on; }
        [[nodiscard]] bool IsAirbrush() const noexcept { return m_airbrush; }

    private:
        static constexpr f32 kMinRadius = 0.5f;
        static constexpr f32 kMaxRadius = 128.0f;
        static constexpr f32 kMinSpacing = 0.05f;

        struct Pick
        {
            foundation::vegetation::VegetationMask* mask = nullptr;
            Guid maskId;  // TerrainVegetationComponent.mask.id (the source asset guid; may be nil)
            void* manager = nullptr; // the scene's TerrainVegetationComponentManager (regrow notices)
            i32 gridSize = 0;        // the terrain heightfield's samples per side (maps the footprint to the grid)
            f32 uvX = 0.0f;          // hit in the mask's 0..1 footprint UV
            f32 uvY = 0.0f;
            f32 worldSizeX = 1.0f;
            f32 worldSizeY = 1.0f;
            Float3 worldHit{};
            Float3 worldNormal{0.0f, 1.0f, 0.0f};
            bool valid = false;
        };
        [[nodiscard]] Pick ResolvePick(const ViewportToolInput& input) const;
        void BeginStroke(const Pick& pick);
        void AdvanceStroke(const Pick& pick, f32 deltaSeconds);
        void ApplyStamp(f32 uvX, f32 uvY, const Pick& pick, f32 amount);
        void EndStroke();
        void UpdateStatus();

        foundation::scene::Scene* m_scene;      // borrowed (the page owns it)
        EditorCommandStack* m_commands;         // borrowed
        IAssetEditSink* m_assetEdits = nullptr; // borrowed; null means nothing is saved (tests)

        u32 m_plane = 0;
        bool m_erase = false;
        bool m_smooth = false;
        f32 m_radius = 6.0f; // world units
        f32 m_strength = 1.0f;
        f32 m_spacing = 0.25f;
        bool m_airbrush = false;
        f32 m_airbrushClock = 0.0f;

        bool m_hasHover = false;
        Float3 m_hoverWorld{};
        Float3 m_hoverNormal{0.0f, 1.0f, 0.0f};

        // Stroke state (one command from press to release). m_strokeMask keeps the raster alive
        // for the stroke; the full-plane snapshot covers the union region that release slices out.
        bool m_stroking = false;
        f32 m_lastStampU = 0.0f;
        f32 m_lastStampV = 0.0f;
        RefPtr<foundation::vegetation::VegetationMask> m_strokeMask;
        Guid m_strokeMaskId;
        void* m_strokeManager = nullptr;
        i32 m_strokeGridSize = 0;
        Array<u8> m_beforePlane;
        foundation::vegetation::MaskRegion m_region;
        String m_status;
    };

    /// The provider that adds the vegetation brush to every scene viewport. This module's
    /// registrar registers it explicitly; it is not discovered.
    class VegetationViewportToolProvider final : public IViewportToolProvider
    {
    public:
        void CreateTools(ViewportToolManager& manager,
                         const ViewportToolHostContext& context) override;
    };

    /// Register the vegetation viewport tools (call once at editor start, from Tools.Editor).
    void RegisterVegetationViewportTools();
    /// Register the vegetation brush settings panel in the viewport-tool-panel registry.
    void RegisterVegetationToolPanels();
}

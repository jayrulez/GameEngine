// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Terrain - :hole partition.
//
// TerrainHoleTool: the scene-viewport terrain hole brush (Specs/terrain-holes.md). A MODAL tool
// beside sculpt and splat: it ray-picks the terrain under the cursor into heightfield-local
// space, CUTS or FILLS the samples inside a hard-edged disc (foundation.heightfield's
// CutHoles / FillHoles), and records ONE region-delta undo command per stroke over the hole
// plane. On save the registered closure writes the runtime heightfield back to its SOURCE asset
// - both the "heights" and the "holes" streams, so an imported heightfield keeps its heights
// when the file name is cleared.
//
// GCC module hygiene: the heavy engine.terrain / resource imports live in HoleToolImpl.cpp.
module;
#include "Core/Prelude.h"

export module editor.terrain:hole;

import foundation.core;
import foundation.scene;
import foundation.render;      // debug::DebugDraw
import foundation.heightfield; // Heightfield, HeightfieldRegion
import editor.core;            // EditorCommandStack, IAssetEditSink
import editor.viewporttools;   // IViewportTool, ViewportToolInput

using namespace foundation::core;

export namespace editor
{
    /// The terrain hole brush: Cut removes the surface under the disc, Fill restores it.
    class TerrainHoleTool final : public IViewportTool
    {
    public:
        enum class Mode : u8
        {
            Cut,
            Fill,
        };
        TerrainHoleTool(foundation::scene::Scene& scene, EditorCommandStack& commands,
                        IAssetEditSink* assetEdits)
            : m_scene(&scene), m_commands(&commands), m_assetEdits(assetEdits)
        {
        }
        [[nodiscard]] StringView Id() const override { return u8"terrain.hole"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Cut Holes"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/minus.svg"; }
        [[nodiscard]] StringView Category() const override { return u8"Terrain"; }
        [[nodiscard]] bool IsAvailable() const override;
        [[nodiscard]] StringView UnavailableReason() const override
        {
            return u8"Cut Holes needs a terrain with a heightfield in the scene.";
        }
        [[nodiscard]] bool Update(const ViewportToolInput& input) override;
        void Draw(foundation::render::debug::DebugDraw& drawList) override;
        void OnDeactivate() override;
        [[nodiscard]] StringView StatusText() const override { return m_status.AsView(); }

        void SetMode(Mode mode) noexcept
        {
            m_mode = mode;
            UpdateStatus();
        }
        [[nodiscard]] Mode GetMode() const noexcept { return m_mode; }
        void SetRadius(f32 r)
        {
            m_radius = Clamp(r, kMinRadius, kMaxRadius);
            UpdateStatus();
        }
        [[nodiscard]] f32 Radius() const noexcept { return m_radius; }

    private:
        static constexpr f32 kMinRadius = 0.5f;
        static constexpr f32 kMaxRadius = 128.0f;
        struct Pick
        {
            foundation::heightfield::Heightfield* hf = nullptr;
            Guid heightfieldId;
            f32 localX = 0.0f;
            f32 localZ = 0.0f;
            Float3 worldHit{};
            Float3 worldNormal{0.0f, 1.0f, 0.0f};
            bool valid = false;
        };
        [[nodiscard]] Pick ResolvePick(const ViewportToolInput& input) const;
        void BeginStroke(const Pick& pick);
        void ApplyDab(const Pick& pick);
        void EndStroke();
        void UpdateStatus();

        foundation::scene::Scene* m_scene;
        EditorCommandStack* m_commands;
        IAssetEditSink* m_assetEdits = nullptr;
        Mode m_mode = Mode::Cut;
        f32 m_radius = 6.0f;
        bool m_hasHover = false;
        Float3 m_hoverWorld{};
        Float3 m_hoverNormal{0.0f, 1.0f, 0.0f};
        bool m_stroking = false;
        RefPtr<foundation::heightfield::Heightfield> m_strokeHf;
        Guid m_strokeHeightfieldId;
        Array<u8> m_before; // the whole hole plane at press
        foundation::heightfield::HeightfieldRegion m_region;
        String m_status;
    };
}

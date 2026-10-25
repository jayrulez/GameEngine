// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :scatter partition.
//
// VegetationScatterTool ("Paint Props"): the scene-viewport prop brush. It ray-picks the terrain
// under the cursor and STAMPS authored instances into the selected Scattered layer of the terrain's
// vegetation component (foundation.vegetation ScatterStamp: density x disc area candidates, the
// layer's slope / height / scale / alignment rules, a spacing rule against existing props, and a
// collision query against the scene's physics bodies when a world exists), or ERASES the
// instances under the disc. One command per stroke (the layer's instances before / after); the
// instances persist on the component with the scene. Deterministic per stroke: the stamp seeds
// count up from the stroke's start, so a scripted stroke places the same instances every run.

module;
#include "Core/Prelude.h"

export module editor.vegetation:scatter;

import foundation.core;
import foundation.scene;     // EntityHandle
import foundation.render;    // debug::DebugDraw (the Draw override signature)
import editor.core;          // EditorCommandStack
import editor.viewporttools; // IViewportTool, ViewportToolInput

using namespace foundation::core;

export namespace editor
{
    class VegetationScatterTool final : public IViewportTool
    {
    public:
        /// `blocked(worldPosition, radius)` = where a prop may not go. Null = the scene's physics
        /// world (ShapeOverlap against every body but the terrain's), or nothing when no world.
        using BlockedQuery = Function<bool(Float3 worldPosition, f32 radius)>;

        VegetationScatterTool(foundation::scene::Scene& scene, EditorCommandStack& commands)
            : m_scene(&scene), m_commands(&commands)
        {
        }

        [[nodiscard]] StringView Id() const override { return u8"vegetation.scatter"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Paint Props"; }
        [[nodiscard]] StringView Icon() const override { return u8"Editor/Icons/Types/VegetationMaskAsset.svg"; }
        [[nodiscard]] StringView Category() const override { return u8"Vegetation"; }
        /// Relevant only when the scene has a vegetation component over a terrain with at least
        /// one prop layer.
        [[nodiscard]] bool IsAvailable() const override;
        [[nodiscard]] StringView UnavailableReason() const override
        {
            return u8"Paint Props needs a Terrain Vegetation component with a prop layer, on a "
                   u8"terrain.";
        }
        [[nodiscard]] bool Update(const ViewportToolInput& input) override;
        void Draw(foundation::render::debug::DebugDraw& drawList) override;
        void OnDeactivate() override;
        [[nodiscard]] StringView StatusText() const override { return m_status.AsView(); }

        // ---- brush parameters ----
        /// The prop layer index (into the component's propLayers) the brush works on. The MODE
        /// (paint / erase) is a separate choice and stays: erase is per layer, so "layer 2 while
        /// erasing" erases layer 2.
        void SetLayer(u32 index)
        {
            m_layer = index;
            UpdateStatus(); // the status bar names the layer + mode without waiting for a frame
        }
        [[nodiscard]] u32 Layer() const noexcept { return m_layer; }
        void SetEraser(bool erase)
        {
            m_erase = erase;
            UpdateStatus();
        }
        [[nodiscard]] bool IsEraser() const noexcept { return m_erase; }
        void SetRadius(f32 r)
        {
            m_radius = Clamp(r, kMinRadius, kMaxRadius);
        }
        [[nodiscard]] f32 Radius() const noexcept { return m_radius; }
        /// Props per square metre a full-strength stamp tries to place.
        void SetDensity(f32 d) noexcept { m_density = Clamp(d, 0.0f, 64.0f); }
        [[nodiscard]] f32 Density() const noexcept { return m_density; }
        /// Coverage per stamp (0..1) - a scrub builds a field up gently.
        void SetStrength(f32 s) noexcept { m_strength = Clamp(s, 0.0f, 1.0f); }
        [[nodiscard]] f32 Strength() const noexcept { return m_strength; }
        /// Minimum spacing between props as a multiple of the mesh's radius (0 = none).
        void SetSpacing(f32 s) noexcept { m_spacing = Clamp(s, 0.0f, 8.0f); }
        [[nodiscard]] f32 Spacing() const noexcept { return m_spacing; }
        /// Stamp spacing along the stroke as a fraction of the radius.
        void SetStampSpacing(f32 s) noexcept { m_stampSpacing = Clamp(s, kMinStampSpacing, 2.0f); }
        [[nodiscard]] f32 StampSpacing() const noexcept { return m_stampSpacing; }
        /// Replace the collision query (tests; a host with its own occupancy).
        void SetBlockedQuery(BlockedQuery query) { m_blockedOverride = Move(query); }

    private:
        static constexpr f32 kMinRadius = 0.5f;
        static constexpr f32 kMaxRadius = 128.0f;
        static constexpr f32 kMinStampSpacing = 0.1f;

        struct Pick;
        void BeginStroke(const ViewportToolInput& input);
        void AdvanceStroke(const ViewportToolInput& input);
        void ApplyStamp(Float3 localCentre, const ViewportToolInput& input);
        void EndStroke();
        void UpdateStatus();

        foundation::scene::Scene* m_scene; // borrowed (page owns it)
        EditorCommandStack* m_commands;    // borrowed

        u32 m_layer = 0;
        bool m_erase = false;
        f32 m_radius = 6.0f;
        f32 m_density = 0.25f;
        f32 m_strength = 1.0f;
        f32 m_spacing = 1.0f;
        f32 m_stampSpacing = 0.5f;
        BlockedQuery m_blockedOverride;

        bool m_hasHover = false;
        Float3 m_hoverWorld{};
        Float3 m_hoverNormal{0.0f, 1.0f, 0.0f};
        bool m_layerHasMesh = true; // the picked component's selected layer resolves its mesh

        // Stroke state (one command per press..release).
        bool m_stroking = false;
        foundation::scene::EntityHandle m_strokeOwner{};
        foundation::scene::EntityHandle m_strokeTerrain{};
        u32 m_strokeLayer = 0;
        u32 m_strokeStamps = 0; // the stamp seed within the stroke
        u32 m_strokeCount = 0;  // strokes so far (the stroke seed)
        Float3 m_lastStampLocal{};
        Array<Float4x4> m_before;
        String m_status;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_hud partition.
//
// The scene view's HUD (Documentation/Specs/editor-gui.md, 5.3, layer 5, part 2): GUI over the
// viewport in fixed zones, so nothing meets in a corner, in the editor's fonts rather than the
// debug draw's bitmap one.
//   - top start, the chips (ruling 22), each saying what is on and in the accent while it is off its
//     default: the projection (perspective or orthographic, the six axis views), the shading (Lit,
//     Unlit, the debug terms, the graph's textures through the picker, the effects as what is on,
//     MSAA; ruling 21), the overlays (the grid and its plane, markers, colliders, the LOD tint,
//     the stats);
//   - top end, the stats (frames a second and their time) and the view gizmo, which turns with the
//     camera: a knob looks along its axis, the centre switches the projection (which one is on,
//     the projection chip says);
//   - bottom start, the zoom readout a moment after a wheel zoom, and the active tool's status
//     (the select tool's in parts: its mode, its space, its snap, what is selected).
// The page hands in what the HUD shows and changes; the scene's own overlays (the grid, the
// gizmos) stay drawn into the view.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:scene_hud;

import foundation.core;
import foundation.render;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.camera;
import editor.viewporttools;
import :edit;
import :gizmo;
import :tools;
import :view_gizmo;
import :zoom_readout;
import :scene_overlays;

using namespace foundation::core;

export namespace editor
{
    /// The tripod: six knobs turning with the camera, the positive ones on arms, the centre the
    /// projection switch, on a disc (its sheet's background). Its letters are labels; the knobs
    /// and arms are drawn.
    class ViewGizmoView final : public foundation::gui::UIWidget
    {
        RTTI_OBJECT(ViewGizmoView, foundation::gui::UIWidget)
    public:
        ViewGizmoView();
        /// The camera's basis now; laid out again when it turned.
        void Show(Float3 right, Float3 up, Float3 forward, bool orthographic);
        /// A press at `local` (its own space) on a knob (its axis) or the centre.
        Function<void(Float3 axis)> OnAxis;
        Function<void()> OnCentre;
        [[nodiscard]] const ViewGizmoLayout& Layout() const noexcept { return m_layout; }
        /// Presses at a point in its own space, as the pointer does (for tests).
        bool PressAt(Float2 local);

    protected:
        [[nodiscard]] Float2 MeasureContent(Float2) override { return Float2{kSide, kSide}; }
        void OnLayout() override;
        void OnDraw(foundation::gui::DrawContext& ctx, const foundation::gui::Rect& localBounds) override;
        void OnMouseDown(const foundation::gui::MouseEvent& event) override;
        void OnMouseMove(const foundation::gui::MouseEvent& event) override;
        void OnMouseLeave(const foundation::gui::MouseEvent& event) override;

    private:
        static constexpr f32 kSide = 2.0f * (ViewGizmo::kArm + 0.5f * ViewGizmo::kKnob) + 4.0f;
        void Hover(Float2 local);

        ViewGizmoLayout m_layout;
        RefPtr<foundation::gui::Label> m_labels[6];
        Float3 m_basis[3]{};
        bool m_orthographic = false;
        bool m_shown = false;
        i32 m_hovered = -1; // a knob, 6 the centre
    };

    /// The zoom readout's bar: a round measure's length with its end ticks.
    class ScaleBarView final : public foundation::gui::UIWidget
    {
        RTTI_OBJECT(ScaleBarView, foundation::gui::UIWidget)
    public:
        ScaleBarView() { SetHitTestVisible(false); }
        void SetLength(f32 pixels)
        {
            if (pixels != m_pixels)
            {
                m_pixels = pixels;
                RequestLayout();
            }
        }

    protected:
        [[nodiscard]] Float2 MeasureContent(Float2) override { return Float2{m_pixels, 8.0f}; }
        void OnDraw(foundation::gui::DrawContext& ctx, const foundation::gui::Rect& localBounds) override;

    private:
        f32 m_pixels = 0.0f;
    };

    class SceneHud
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            SceneEditContext* edit = nullptr;
            EditorCamera* camera = nullptr;
            SceneOverlays* overlays = nullptr;
            foundation::render::ViewPostOverride* post = nullptr;
            foundation::render::ViewDebugView* debugView = nullptr;
            ViewportToolManager* tools = nullptr;
            const IViewportTool* selectTool = nullptr; // the default tool: its status in parts
            Function<GizmoController*()> gizmos;
            /// The graph's textures of the last frame (empty: none to offer).
            Function<void(Array<foundation::render::DebugResourceInfo>&)> debugResources;
            /// Opens the picker for `request`.
            Function<void(app::PickerRequest request)> pick;
            /// An overlay changed: the page keeps its view state.
            Function<void()> overlaysChanged;
        };

        /// The HUD's zones in `pane` (the markup's: `chip-projection`, `chip-shading`,
        /// `chip-overlays`, `hud-stats`, `hud-gizmo`, `hud-zoom` ..., found by `find`).
        SceneHud(IAllocator& allocator, Services services, const Function<foundation::gui::Node*(StringView)>& find);
        SceneHud(const SceneHud&) = delete;
        SceneHud& operator=(const SceneHud&) = delete;

        /// A wheel zoom happened: the readout shows for a moment.
        void Zoomed() { m_zoom.Zoomed(); }
        /// Once a frame: everything as it is now (`dt` the frame's time, for the stats and the
        /// readout's fade; the view's height in pixels for its scale bar).
        void Update(f32 dt, u32 viewHeight);

        // === What the menus do, by name (the menus call these; so do tests) ===
        enum class Projection : u8
        {
            Perspective,
            Orthographic,
        };
        void SetProjection(Projection projection);
        /// Looks along `axis` from its side, as the gizmo's knob does.
        void LookAlong(Float3 axis);
        /// Lit (no debug term), Unlit, or a debug term.
        void SetShading(foundation::render::ViewDebugSemantic semantic);
        void ShowTexture(StringView name);
        void OpenTextures();

        // === For tests ===
        [[nodiscard]] StringView ChipText(StringView id) const;
        [[nodiscard]] bool ChipOn(StringView id) const;
        [[nodiscard]] String StatusText() const;
        [[nodiscard]] ViewGizmoView* Gizmo() const noexcept { return m_gizmo.Get(); }
        [[nodiscard]] bool ZoomShown() const;
        [[nodiscard]] bool StatsShown() const;
        [[nodiscard]] foundation::gui::Menu* ChipMenu(StringView id) const;

    private:
        struct Chip
        {
            foundation::gui::ButtonBase* button = nullptr;
            String text;
            bool on = false;
        };
        // A menu item whose check follows a state, asked again each frame.
        struct Checked
        {
            foundation::gui::MenuItem* item = nullptr;
            Function<bool()> on;
        };
        void BuildProjection(foundation::gui::Menu& menu);
        void BuildShading(foundation::gui::Menu& menu);
        void BuildOverlays(foundation::gui::Menu& menu);
        foundation::gui::MenuItem* AddCheck(foundation::gui::Menu& menu, StringView label, Function<bool()> on, Function<void()> pick);
        void ShowChip(Chip& chip, StringView text, bool on);
        void UpdateStats(f32 dt);
        void UpdateStatus();
        void UpdateZoom(f32 dt, u32 viewHeight);
        [[nodiscard]] String AxisViewName() const;

        IAllocator& m_allocator;
        Services m_services;
        Chip m_projection;
        Chip m_shading;
        Chip m_overlays;
        Array<Checked> m_checks;
        foundation::gui::Label* m_stats = nullptr;
        RefPtr<ViewGizmoView> m_gizmo;
        foundation::gui::Node* m_zoomBox = nullptr;
        foundation::gui::Label* m_zoomFocus = nullptr;
        foundation::gui::Label* m_zoomCell = nullptr;
        foundation::gui::Label* m_zoomLength = nullptr;
        RefPtr<ScaleBarView> m_zoomBar;
        foundation::gui::Node* m_status = nullptr;
        foundation::gui::Image* m_statusIcon = nullptr;
        foundation::gui::Label* m_statusParts[4] = {};
        ZoomReadout m_zoom;
        f64 m_statsSeconds = 0.0;
        u32 m_statsFrames = 0;
        String m_statsText;
    };

    RTTI_DEFINE_OBJECT(ViewGizmoView, "rtti::editor::editor")
    RTTI_DEFINE_OBJECT(ScaleBarView, "rtti::editor::editor")
}

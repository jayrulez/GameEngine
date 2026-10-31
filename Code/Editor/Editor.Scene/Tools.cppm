// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :tools partition.
//
// SelectTransformTool: the scene viewport's default viewport tool (editor.viewporttools), with
// click-pick selection and the transform gizmo, so the viewport has one input-routing path. The
// page keeps the camera policy (it masks buttons and nulls the keyboard while the camera owns the
// mouse); this tool owns everything after that: the gizmo session, picking, the overlay and the
// status text.
//
// Behavior:
//   - the gizmo consumes the pointer while a handle is hot or a drag runs, and picking is
//     suppressed;
//   - with editingLocked (Simulate), the gizmo goes read-only (pose sync only, no drags, through
//     the controller's pointer-less update) but picking still works, since selection is not an
//     edit;
//   - a pick takes the closest entity within a roughly screen-constant radius; Ctrl toggles, and a
//     click on nothing clears the selection (unless Ctrl is held); the pointer must be over the
//     viewport.

module;
#include "Core/Prelude.h"

export module editor.scene:tools;

import foundation.core;
import foundation.render;
import editor.core;
import editor.viewporttools;
import :edit;
import :gizmo;

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor
{
    class SelectTransformTool final : public IViewportTool
    {
    public:
        explicit SelectTransformTool(SceneEditContext& edit) : m_edit(&edit), m_gizmos(edit) {}

        [[nodiscard]] StringView Id() const override { return u8"select"; }
        [[nodiscard]] StringView DisplayName() const override { return u8"Select"; }

        [[nodiscard]] bool Update(const ViewportToolInput& input) override;

        void Draw(render::debug::DebugDraw& drawList) override;

        [[nodiscard]] StringView StatusText() const override
        {
            return m_gizmos.IsActive() ? m_gizmos.StatusText() : StringView{};
        }

        void OnDeactivate() override;

        /// The toolbar drives mode and space through the controller directly, as W, E, R and X do.
        [[nodiscard]] GizmoController& Gizmos() noexcept { return m_gizmos; }

        /// GPU pick interface (null: CPU-only picking). A click with a picker and a pixel position
        /// asks the GPU for the surface under the pointer and applies the answer when it lands (a
        /// few frames later); the CPU origin pick is the fallback for entities no renderer draws
        /// (lights, cameras, empties) and for a GPU miss.
        void SetPicker(IViewportPicker* picker) noexcept { m_picker = picker; }
        [[nodiscard]] bool HasPendingPick() const noexcept { return m_pendingPick != 0; }

        /// Marquee: a press on empty space dragged past kMarqueeThreshold pixels becomes a
        /// rubber-band rect; release selects every entity drawn inside it (GPU rect pick), or
        /// whose origin projects inside it without a picker. Ctrl adds to the selection. The
        /// click that started it is undone (its pick cancelled, the selection at press restored).
        static constexpr i32 kMarqueeThreshold = 4;
        [[nodiscard]] bool IsMarqueeActive() const noexcept { return m_marquee.active; }
        [[nodiscard]] bool HasPendingMarquee() const noexcept { return m_pendingMarquee != 0; }

    private:
        void PickOnClick(const ViewportToolInput& input);
        // The CPU pick: the entity whose origin is nearest the ray within a screen-ish radius.
        [[nodiscard]] Guid CpuPick(const ViewportToolInput& input) const;
        void ApplyPick(const Guid& picked, bool ctrl);
        void PollPick();
        void UpdateMarquee(const ViewportToolInput& input);
        void FinishMarquee(const ViewportToolInput& input);
        void ApplyMarquee(Span<const Guid> picked, bool ctrl);
        // Entities whose origin projects inside `rect` of the view (the no-picker marquee).
        void CpuMarquee(const ViewportToolInput& input, i32 x0, i32 y0, i32 x1, i32 y1,
                        Array<Guid>& out) const;

        SceneEditContext* m_edit; // borrowed (the page owns it; the tool dies with the page)
        GizmoController m_gizmos;
        IViewportPicker* m_picker = nullptr; // borrowed from the host
        u32 m_pendingPick = 0;               // the GPU request in flight (0 = none)
        Guid m_pendingCpuPick;               // the CPU answer for that click (GPU-miss fallback)
        bool m_pendingCtrl = false;          // the click's modifier, applied with the answer

        struct Marquee
        {
            bool armed = false;  // a press on empty space, not yet dragged past the threshold
            bool active = false; // dragging the rect
            bool ctrl = false;
            i32 pressX = 0, pressY = 0;
            i32 currentX = 0, currentY = 0;
            Array<Guid> selectionAtPress; // restored when the drag becomes a marquee
        };
        Marquee m_marquee;
        u32 m_pendingMarquee = 0;   // the GPU rect request in flight (0 = none)
        bool m_marqueeCtrl = false; // its modifier
    };
}

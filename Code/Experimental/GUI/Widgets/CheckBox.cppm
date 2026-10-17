// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :check_box partition
//
// CheckBox: a toggleable box. A lean native control modeled on eepp's UICheckBox
// (role only). Clicking toggles the checked state and fires OnCheckedChanged; OnDraw renders
// a rounded box outline with a filled inner mark when checked. Hover/press reaction comes for
// free from UINode's control state (so a CSS background can highlight it). Pair it with a
// Label in a horizontal LinearLayout for the usual "[x] caption" look.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:check_box;

import foundation.core; // Color, Function, Move, Max
import foundation.vg;   // CornerRadii
import :reactive; // Observable (what bindings follow)
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class CheckBox : public UIWidget
    {
        RTTI_OBJECT(CheckBox, UIWidget)
    public:
        CheckBox()
        {
            SetTag(core::StringView(u8"checkbox"));
            SetTabFocusable(true);
        }

        /// Whether it is checked (a read a binding records).
        [[nodiscard]] bool IsChecked() const
        {
            m_checkedChanged.Track();
            return m_checked;
        }
        void SetChecked(bool checked)
        {
            if (checked == m_checked)
                return;
            m_checked = checked;
            m_indeterminate = false; // a choice settles the mixed state
            MarkStyleDirty(true); // :checked
            Invalidate();
            m_checkedChanged.Notify();
            if (m_onChanged)
                m_onChanged(m_checked);
            SendEvent(Event(EventType::Changed, this));
        }
        void Toggle() { SetChecked(!m_checked); }

        /// Neither checked nor unchecked (CSS's :indeterminate; a parent of mixed children): drawn
        /// as a dash until the box is set or toggled.
        void SetIndeterminate(bool indeterminate)
        {
            if (indeterminate == m_indeterminate)
                return;
            m_indeterminate = indeterminate;
            MarkStyleDirty(true);
            Invalidate();
        }
        [[nodiscard]] bool IsIndeterminate() const noexcept { return m_indeterminate; }
        [[nodiscard]] u32 GetWidgetStates() const override
        {
            return (m_checked ? WidgetStateChecked : WidgetStateNone) | (m_indeterminate ? WidgetStateIndeterminate : WidgetStateNone);
        }

        void SetOnCheckedChanged(core::Function<void(bool)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        void SetBoxColor(Color color)
        {
            m_boxColor = color;
            Invalidate();
        }
        void SetCheckColor(Color color)
        {
            m_checkColor = color;
            Invalidate();
        }

        // Theming parts: checkbox::box (outline) / ::mark (inner fill).
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"box"));
            out.PushBack(core::StringView(u8"mark"));
        }

        /// Its box's size when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{20.0f, 20.0f}; }

    protected:
        bool OnActivate() override { return ActivateAsClick(); } // Return, Space: a click
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            // Bound to a command, the command decides (its handler toggles its checked state, which
            // comes back here); else the box toggles itself.
            if (HasCommand())
                (void)RunBoundCommand();
            else
                Toggle();
        }
        void OnBoundCommandState(bool available, bool checked) override
        {
            SetEnabled(available);
            SetChecked(checked);
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            // checkbox::box its outline, ::mark the check or the dash, each its own look with the
            // sheet's over it.
            GetPartStyle(u8"box").Draw(ctx, box, OutlinedBox(m_boxColor, 2.0f, vg::CornerRadii(3.0f)));
            if (m_indeterminate)
            {
                const f32 h = core::Max(2.0f, box.height * 0.18f);
                const Rect dash{box.x + 4.0f, box.y + (box.height - h) * 0.5f, core::Max(0.0f, box.width - 8.0f), h};
                GetPartStyle(u8"mark").Draw(ctx, dash, FilledBox(m_checkColor, vg::CornerRadii(1.0f)));
            }
            else if (m_checked)
            {
                const f32 inset = 4.0f;
                const Rect inner{box.x + inset, box.y + inset,
                                 core::Max(0.0f, box.width - inset * 2.0f),
                                 core::Max(0.0f, box.height - inset * 2.0f)};
                GetPartStyle(u8"mark").Draw(ctx, inner, FilledBox(m_checkColor, vg::CornerRadii(2.0f)));
            }
        }

        bool m_checked = false;
        bool m_indeterminate = false;
        Observable m_checkedChanged; // what bindings reading the state follow
        Color m_boxColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_checkColor{0.31f, 0.63f, 0.85f, 1.0f};
        core::Function<void(bool)> m_onChanged;
    };
}

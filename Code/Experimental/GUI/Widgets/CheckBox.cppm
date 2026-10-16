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
            Invalidate();
            m_checkedChanged.Notify();
            if (m_onChanged)
                m_onChanged(m_checked);
            SendEvent(Event(EventType::Changed, this));
        }
        void Toggle() { SetChecked(!m_checked); }

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
        void SetThemePartColor(core::StringView part, Color color) override
        {
            if (part == core::StringView(u8"box"))
                SetBoxColor(color);
            else if (part == core::StringView(u8"mark"))
                SetCheckColor(color);
        }

    protected:
        bool OnActivate() override { return ActivateAsClick(); } // Return, Space: a click
        void OnMouseClick(const MouseEvent& event) override
        {
            (void)event;
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
            ctx.VG().StrokeRoundedRect(box.ToRectangle(), vg::CornerRadii(3.0f), m_boxColor, 2.0f);
            if (m_checked)
            {
                const f32 inset = 4.0f;
                const Rect inner{box.x + inset, box.y + inset,
                                 core::Max(0.0f, box.width - inset * 2.0f),
                                 core::Max(0.0f, box.height - inset * 2.0f)};
                ctx.VG().FillRoundedRect(inner.ToRectangle(), vg::CornerRadii(2.0f), m_checkColor);
            }
        }

        bool m_checked = false;
        Observable m_checkedChanged; // what bindings reading the state follow
        Color m_boxColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_checkColor{0.31f, 0.63f, 0.85f, 1.0f};
        core::Function<void(bool)> m_onChanged;
    };
}

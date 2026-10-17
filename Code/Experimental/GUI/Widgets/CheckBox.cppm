// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :check_box partition
//
// CheckBox: the button core (E6 of Specs/gui-lab-evolution.md) as a toggle with a box for its
// indicator, its text beside it. It is tri-state: checked, unchecked, or indeterminate (a parent
// of mixed children, CSS's :indeterminate), drawn as a dash until it is set or clicked; a click
// on an indeterminate box checks it. Its parts: checkbox::box (the outline) and ::mark (the
// check or the dash).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:check_box;

import foundation.core; // Color, Max
import foundation.vg;   // CornerRadii
import :rect;
import :draw_context;
import :button_base;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class CheckBox : public ButtonBase
    {
        RTTI_OBJECT(CheckBox, ButtonBase)
    public:
        CheckBox()
        {
            SetTag(core::StringView(u8"checkbox"));
            SetToggle(true);
            MakeIndicator();
        }

        void Toggle() { SetChecked(!IsChecked()); }
        /// Neither checked nor unchecked, until it is set or clicked.
        void SetIndeterminate(bool indeterminate) { SetIndeterminateState(indeterminate); }
        [[nodiscard]] bool IsIndeterminate() const noexcept { return IsIndeterminateState(); }

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

    protected:
        void DrawIndicator(DrawContext& ctx, const Rect& box) const override
        {
            GetPartStyle(u8"box").Draw(ctx, box, OutlinedBox(m_boxColor, 2.0f, vg::CornerRadii(3.0f)));
            if (IsIndeterminateState())
            {
                const f32 h = core::Max(2.0f, box.height * 0.18f);
                const Rect dash{box.x + 4.0f, box.y + (box.height - h) * 0.5f, core::Max(0.0f, box.width - 8.0f), h};
                GetPartStyle(u8"mark").Draw(ctx, dash, FilledBox(m_checkColor, vg::CornerRadii(1.0f)));
            }
            else if (IsChecked())
            {
                const f32 inset = 4.0f;
                const Rect inner{box.x + inset, box.y + inset, core::Max(0.0f, box.width - inset * 2.0f),
                                 core::Max(0.0f, box.height - inset * 2.0f)};
                GetPartStyle(u8"mark").Draw(ctx, inner, FilledBox(m_checkColor, vg::CornerRadii(2.0f)));
            }
        }

    private:
        Color m_boxColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_checkColor{0.31f, 0.63f, 0.85f, 1.0f};
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :button_base partition
//
// Abstract base for button types: click event, pressed state, ICommand binding, focus/keyboard
// handling, button-chrome drawing. Ported from Sedulous.UI/src/Controls/ButtonBase.bf.
// (Text measuring/drawing lives in subclasses like Button and is now live - the Fonts service is
// wired into UIContext; this base provides the button background chrome.)

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:button_base;

import foundation.core;
import foundation.vg;
import :view;
import :event;
import :icommand;
import :control_state;
import :style_property;
import :thickness;
import :draw_context;
import :drawable;
import :palette;
import :event_args;
import :input_enums;

using namespace foundation::core;

export namespace foundation::ui
{
    class ButtonBase : public View
    {
        RTTI_OBJECT(ButtonBase, View)
    public:
        ICommand* Command = nullptr; ///< Optional command binding.
        Event<void(ButtonBase*)> OnClick;

        [[nodiscard]] bool IsPressed() const noexcept { return m_isPressed; }

        [[nodiscard]] ControlState GetControlState() const override
        {
            ControlState state = ControlState::Normal;
            if (!IsEffectivelyEnabled() || (Command != nullptr && !Command->CanExecute()))
            {
                state |= ControlState::Disabled;
            }
            if (m_isPressed)
            {
                state |= ControlState::Pressed;
            }
            if (IsFocusVisible()) // keyboard-acquired only - a clicked button holds focus ringless
            {
                state |= ControlState::Focused;
            }
            if (IsHovered())
            {
                state |= ControlState::Hover;
            }
            return state;
        }

        /// Fire the click event and execute the bound command.
        void FireClick()
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (Command != nullptr && !Command->CanExecute())
            {
                return;
            }
            // Pin self: a click handler may destroy this button (e.g. rebuilding the panel that owns
            // it). Without the pin, reading this->Command after Invoke dereferences freed memory.
            RefPtr<ButtonBase> keepAlive(this);
            OnClick.Invoke(this);
            if (Command != nullptr)
            {
                Command->Execute();
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left)
            {
                m_isPressed = true;
                InvalidateVisual(); // press tint - geometry unchanged
                e.Handled = true;
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && m_isPressed)
            {
                m_isPressed = false;
                InvalidateVisual();
                if (IsHovered())
                {
                    FireClick();
                }
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (e.Key == KeyCode::Return || e.Key == KeyCode::Space)
            {
                FireClick();
                e.Handled = true;
            }
        }
        void OnActivate() override { FireClick(); }

    protected:
        ButtonBase()
        {
            IsFocusable = true;
            IsTabStop = true;
        }

        /// Draw button chrome: the resolved Background drawable, or a state-tinted default rounded rect.
        void DrawButtonBackground(UIDrawContext& ctx, const Rectangle& bounds, ControlState state)
        {
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                bg->Draw(ctx, bounds, state);
                return;
            }
            const f32 radius = ResolveStyleFloat(StyleProperty::CornerRadius, 4.0f);
            Color bg{55.0f / 255.0f, 58.0f / 255.0f, 70.0f / 255.0f, 1.0f};
            if (HasFlag(state, ControlState::Disabled))
            {
                bg = Palette::ComputeDisabled(bg);
            }
            else if (HasFlag(state, ControlState::Pressed))
            {
                bg = Palette::ComputePressed(bg);
            }
            else if (HasFlag(state, ControlState::Focused))
            {
                bg = Palette::ComputeFocused(bg);
            }
            else if (HasFlag(state, ControlState::Hover))
            {
                bg = Palette::ComputeHover(bg);
            }
            if (radius > 0)
            {
                ctx.VG().FillRoundedRect(bounds, radius, bg);
            }
            else
            {
                ctx.VG().FillRect(bounds, bg);
            }
        }

    private:
        bool m_isPressed = false;
    };

    RTTI_DEFINE_OBJECT(ButtonBase, "rtti::ui")
}

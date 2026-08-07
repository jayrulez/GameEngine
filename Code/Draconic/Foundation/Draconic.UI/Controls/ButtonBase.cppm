// Draconic UI - :button_base partition
//
// Abstract base for button types: click event, pressed state, ICommand binding, focus/keyboard
// handling, button-chrome drawing. Ported from Sedulous.UI/src/Controls/ButtonBase.bf.
// (Text measuring/drawing lives in subclasses like Button and is now live - the Fonts service is
// wired into UIContext; this base provides the button background chrome.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:button_base;

import draconic.foundation;
import draconic.vg;
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

using namespace draconic::foundation;

export namespace draconic::ui
{
    class ButtonBase : public View
    {
        DRACONIC_OBJECT(ButtonBase, View)
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
            if (IsFocused())
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
                Invalidate();
                e.Handled = true;
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && m_isPressed)
            {
                m_isPressed = false;
                Invalidate();
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

    DRACONIC_DEFINE_OBJECT(ButtonBase, "draconic::ui")
}

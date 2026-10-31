// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - :binding_names partition
//
// Human-readable names for input bindings and the enums around them: a key/pad/mouse code -> label
// ("A", "Pad South", "Mouse Left"), a whole Binding -> label (DescribeBinding), and the source,
// kind and interaction enum names. Pure formatting over shell codes and input::Binding, with no UI
// and no editor, so any runtime consumer (a HUD button prompt, a rebind screen) renders bindings
// the same way the editor does.

module;
#include "Core/Prelude.h"

export module foundation.input:binding_names;

import foundation.core;
import foundation.shell;
import :input_map;

using namespace foundation::core;

export namespace foundation::input
{
    /// A key code (shell::KeyCode) -> a short label: "A".."Z", "0".."9", "F1".."F24", the named keys,
    /// else "Key#<code>".
    [[nodiscard]] inline String KeyName(u32 code)
    {
        const foundation::shell::KeyCode key = static_cast<foundation::shell::KeyCode>(code);
        const u32 a = static_cast<u32>(foundation::shell::KeyCode::A);
        const u32 z = static_cast<u32>(foundation::shell::KeyCode::Z);
        const u32 n0 = static_cast<u32>(foundation::shell::KeyCode::Num0);
        const u32 n9 = static_cast<u32>(foundation::shell::KeyCode::Num9);
        const u32 f1 = static_cast<u32>(foundation::shell::KeyCode::F1);
        const u32 f24 = static_cast<u32>(foundation::shell::KeyCode::F24);
        String out;
        if (code >= a && code <= z)
        {
            out.PushBack(static_cast<utf8char>(u8'A' + (code - a)));
            return out;
        }
        if (code >= n0 && code <= n9)
        {
            out.PushBack(static_cast<utf8char>(u8'0' + (code - n0)));
            return out;
        }
        if (code >= f1 && code <= f24)
        {
            out.Append(u8"F");
            AppendValue(out, static_cast<u64>(code - f1 + 1));
            return out;
        }
        switch (key)
        {
        case foundation::shell::KeyCode::Return:
            return String(u8"Return");
        case foundation::shell::KeyCode::Escape:
            return String(u8"Escape");
        case foundation::shell::KeyCode::Backspace:
            return String(u8"Backspace");
        case foundation::shell::KeyCode::Tab:
            return String(u8"Tab");
        case foundation::shell::KeyCode::Space:
            return String(u8"Space");
        case foundation::shell::KeyCode::Left:
            return String(u8"Left");
        case foundation::shell::KeyCode::Right:
            return String(u8"Right");
        case foundation::shell::KeyCode::Up:
            return String(u8"Up");
        case foundation::shell::KeyCode::Down:
            return String(u8"Down");
        case foundation::shell::KeyCode::LeftShift:
            return String(u8"LShift");
        case foundation::shell::KeyCode::RightShift:
            return String(u8"RShift");
        case foundation::shell::KeyCode::LeftCtrl:
            return String(u8"LCtrl");
        case foundation::shell::KeyCode::RightCtrl:
            return String(u8"RCtrl");
        case foundation::shell::KeyCode::LeftAlt:
            return String(u8"LAlt");
        case foundation::shell::KeyCode::RightAlt:
            return String(u8"RAlt");
        default:
            break;
        }
        out.Append(u8"Key#");
        AppendValue(out, static_cast<u64>(code));
        return out;
    }

    /// A gamepad button code (shell::GamepadButton) -> a label ("Pad South", "Pad LB", "DPad Up"), one
    /// of its own for every button, so a prompt's sheet can tell any two apart.
    [[nodiscard]] inline StringView PadButtonName(u32 code)
    {
        switch (static_cast<foundation::shell::GamepadButton>(code))
        {
        case foundation::shell::GamepadButton::South:
            return u8"Pad South";
        case foundation::shell::GamepadButton::East:
            return u8"Pad East";
        case foundation::shell::GamepadButton::West:
            return u8"Pad West";
        case foundation::shell::GamepadButton::North:
            return u8"Pad North";
        case foundation::shell::GamepadButton::LeftShoulder:
            return u8"Pad LB";
        case foundation::shell::GamepadButton::RightShoulder:
            return u8"Pad RB";
        case foundation::shell::GamepadButton::DPadUp:
            return u8"DPad Up";
        case foundation::shell::GamepadButton::DPadDown:
            return u8"DPad Down";
        case foundation::shell::GamepadButton::DPadLeft:
            return u8"DPad Left";
        case foundation::shell::GamepadButton::DPadRight:
            return u8"DPad Right";
        case foundation::shell::GamepadButton::Start:
            return u8"Pad Start";
        case foundation::shell::GamepadButton::Back:
            return u8"Pad Back";
        case foundation::shell::GamepadButton::LeftStick:
            return u8"Pad LS"; // the left stick pressed
        case foundation::shell::GamepadButton::RightStick:
            return u8"Pad RS";
        case foundation::shell::GamepadButton::Guide:
            return u8"Pad Guide";
        case foundation::shell::GamepadButton::LeftPaddle1:
            return u8"Pad Left Paddle 1";
        case foundation::shell::GamepadButton::LeftPaddle2:
            return u8"Pad Left Paddle 2";
        case foundation::shell::GamepadButton::RightPaddle1:
            return u8"Pad Right Paddle 1";
        case foundation::shell::GamepadButton::RightPaddle2:
            return u8"Pad Right Paddle 2";
        case foundation::shell::GamepadButton::Touchpad:
            return u8"Pad Touchpad";
        case foundation::shell::GamepadButton::Misc1:
            return u8"Pad Misc";
        default:
            return u8"Pad Button";
        }
    }

    /// A whole Binding -> a label, dispatching on its source (the physical control it names).
    [[nodiscard]] inline String DescribeBinding(const Binding& b)
    {
        switch (b.source)
        {
        case BindingSource::Key:
            return KeyName(b.code);
        case BindingSource::MouseButton:
            switch (static_cast<foundation::shell::MouseButton>(b.code))
            {
            case foundation::shell::MouseButton::Left:
                return String(u8"Mouse Left");
            case foundation::shell::MouseButton::Right:
                return String(u8"Mouse Right");
            case foundation::shell::MouseButton::Middle:
                return String(u8"Mouse Middle");
            default:
                return String(u8"Mouse Button");
            }
        case BindingSource::MouseAxis:
            switch (static_cast<MouseAxisCode>(b.code))
            {
            case MouseAxisCode::DeltaX:
                return String(u8"Mouse dX");
            case MouseAxisCode::DeltaY:
                return String(u8"Mouse dY");
            case MouseAxisCode::Wheel:
                return String(u8"Mouse Wheel");
            }
            return String(u8"Mouse Axis");
        case BindingSource::MouseDelta:
            return String(u8"Mouse Delta (2D)");
        case BindingSource::GamepadButton:
            return String(PadButtonName(b.code));
        case BindingSource::GamepadAxis:
            switch (static_cast<foundation::shell::GamepadAxis>(b.code))
            {
            case foundation::shell::GamepadAxis::LeftX:
                return String(u8"Pad Left X");
            case foundation::shell::GamepadAxis::LeftY:
                return String(u8"Pad Left Y");
            case foundation::shell::GamepadAxis::RightX:
                return String(u8"Pad Right X");
            case foundation::shell::GamepadAxis::RightY:
                return String(u8"Pad Right Y");
            case foundation::shell::GamepadAxis::LeftTrigger:
                return String(u8"Pad LT");
            case foundation::shell::GamepadAxis::RightTrigger:
                return String(u8"Pad RT");
            default:
                return String(u8"Pad Axis");
            }
        case BindingSource::GamepadStick:
            return String(static_cast<StickCode>(b.code) == StickCode::Left ? u8"Left Stick"
                                                                            : u8"Right Stick");
        case BindingSource::TouchButton:
            return String(u8"Touch Region");
        case BindingSource::TouchStick:
            return String(u8"Touch Stick");
        case BindingSource::Composite2D:
        {
            // Up, left, down, right, as a player reads them ("W A S D"; the arrows "Up Left Down Right").
            String s(KeyName(b.posY));
            s += u8" ";
            s += KeyName(b.negX);
            s += u8" ";
            s += KeyName(b.negY);
            s += u8" ";
            s += KeyName(b.posX);
            return s;
        }
        }
        return String(u8"?");
    }

    /// The short name of a binding source ("Key", "PadBtn", "Keys4", ...).
    [[nodiscard]] inline StringView SourceName(BindingSource source)
    {
        switch (source)
        {
        case BindingSource::Key:
            return u8"Key";
        case BindingSource::MouseButton:
            return u8"MouseBtn";
        case BindingSource::MouseAxis:
            return u8"MouseAxis";
        case BindingSource::MouseDelta:
            return u8"MouseDelta";
        case BindingSource::GamepadButton:
            return u8"PadBtn";
        case BindingSource::GamepadAxis:
            return u8"PadAxis";
        case BindingSource::GamepadStick:
            return u8"PadStick";
        case BindingSource::Composite2D:
            return u8"Keys4";
        case BindingSource::TouchButton:
            return u8"TouchBtn";
        case BindingSource::TouchStick:
            return u8"TouchStick";
        }
        return u8"?";
    }

    /// The binding sources an action of `kind` accepts, in cycle order (mirrors ValidateInputMap).
    /// Writes up to 8 into `out`; returns the count.
    inline usize ValidSources(ActionKind kind, BindingSource out[8])
    {
        usize n = 0;
        switch (kind)
        {
        case ActionKind::Button:
            out[n++] = BindingSource::Key;
            out[n++] = BindingSource::MouseButton;
            out[n++] = BindingSource::GamepadButton;
            out[n++] = BindingSource::TouchButton;
            break;
        case ActionKind::Axis1D:
            out[n++] = BindingSource::Key;
            out[n++] = BindingSource::MouseButton;
            out[n++] = BindingSource::GamepadButton;
            out[n++] = BindingSource::MouseAxis;
            out[n++] = BindingSource::GamepadAxis;
            break;
        case ActionKind::Axis2D:
            out[n++] = BindingSource::GamepadStick;
            out[n++] = BindingSource::Composite2D;
            out[n++] = BindingSource::MouseDelta;
            out[n++] = BindingSource::TouchStick;
            break;
        }
        return n;
    }

    /// The name of an action kind ("Button", "Axis1D", "Axis2D").
    [[nodiscard]] inline StringView KindName(ActionKind kind)
    {
        switch (kind)
        {
        case ActionKind::Button:
            return u8"Button";
        case ActionKind::Axis1D:
            return u8"Axis1D";
        case ActionKind::Axis2D:
            return u8"Axis2D";
        }
        return u8"?";
    }

    /// The name of an interaction ("On Press", "Hold", "Tap", "Double Tap").
    [[nodiscard]] inline StringView InteractionName(InteractionKind kind)
    {
        switch (kind)
        {
        case InteractionKind::None:
            return u8"On Press";
        case InteractionKind::Hold:
            return u8"Hold";
        case InteractionKind::Tap:
            return u8"Tap";
        case InteractionKind::DoubleTap:
            return u8"Double Tap";
        }
        return u8"?";
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :input_enums partition
//
// Input enums ported from Sedulous.UI/src/Input (MouseButton, KeyModifiers, EventPhase,
// FocusDirection, KeyCode). Values match Sedulous.Shell.Input so a runtime bridge can cast directly.
// Grouped into one partition (Beef had a file per enum).

module;
#include "Core/Prelude.h"

export module foundation.ui:input_enums;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Mouse button identifiers.
    enum class MouseButton
    {
        Left,
        Middle,
        Right,
        X1,
        X2
    };

    /// Keyboard modifier flags.
    enum class KeyModifiers : u32
    {
        None = 0,
        LeftShift = 0x0001,
        RightShift = 0x0002,
        LeftCtrl = 0x0040,
        RightCtrl = 0x0080,
        LeftAlt = 0x0100,
        RightAlt = 0x0200,
        LeftGui = 0x0400,
        RightGui = 0x0800,
        NumLock = 0x1000,
        CapsLock = 0x2000,
        ScrollLock = 0x8000,

        Shift = LeftShift | RightShift,
        Ctrl = LeftCtrl | RightCtrl,
        Alt = LeftAlt | RightAlt,
        Gui = LeftGui | RightGui,
    };

    [[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept
    {
        return static_cast<KeyModifiers>(static_cast<u32>(a) | static_cast<u32>(b));
    }
    [[nodiscard]] constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) noexcept
    {
        return static_cast<KeyModifiers>(static_cast<u32>(a) & static_cast<u32>(b));
    }
    constexpr KeyModifiers& operator|=(KeyModifiers& a, KeyModifiers b) noexcept
    {
        a = a | b;
        return a;
    }
    /// True if `flag` is set in `value`.
    [[nodiscard]] constexpr bool HasFlag(KeyModifiers value, KeyModifiers flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
    }

    /// Phase of event propagation: Capture (root->target) -> Target -> Bubble (target->root).
    enum class EventPhase
    {
        Capture,
        Target,
        Bubble
    };

    /// Direction for spatial focus navigation.
    enum class FocusDirection
    {
        Up,
        Down,
        Left,
        Right
    };

    /// Keyboard key codes (values match Sedulous.Shell.Input.KeyCode).
    enum class KeyCode : u32
    {
        Unknown = 0,

        A = 4,
        B = 5,
        C = 6,
        D = 7,
        E = 8,
        F = 9,
        G = 10,
        H = 11,
        I = 12,
        J = 13,
        K = 14,
        L = 15,
        M = 16,
        N = 17,
        O = 18,
        P = 19,
        Q = 20,
        R = 21,
        S = 22,
        T = 23,
        U = 24,
        V = 25,
        W = 26,
        X = 27,
        Y = 28,
        Z = 29,

        Num1 = 30,
        Num2 = 31,
        Num3 = 32,
        Num4 = 33,
        Num5 = 34,
        Num6 = 35,
        Num7 = 36,
        Num8 = 37,
        Num9 = 38,
        Num0 = 39,

        Return = 40,
        Escape = 41,
        Backspace = 42,
        Tab = 43,
        Space = 44,

        Minus = 45,
        Equals = 46,
        LeftBracket = 47,
        RightBracket = 48,
        Backslash = 49,
        Semicolon = 51,
        Apostrophe = 52,
        Grave = 53,
        Comma = 54,
        Period = 55,
        Slash = 56,

        CapsLock = 57,

        F1 = 58,
        F2 = 59,
        F3 = 60,
        F4 = 61,
        F5 = 62,
        F6 = 63,
        F7 = 64,
        F8 = 65,
        F9 = 66,
        F10 = 67,
        F11 = 68,
        F12 = 69,

        PrintScreen = 70,
        ScrollLock = 71,
        Pause = 72,
        Insert = 73,
        Home = 74,
        PageUp = 75,
        Delete = 76,
        End = 77,
        PageDown = 78,

        Right = 79,
        Left = 80,
        Down = 81,
        Up = 82,

        NumLock = 83,

        KeypadDivide = 84,
        KeypadMultiply = 85,
        KeypadMinus = 86,
        KeypadPlus = 87,
        KeypadEnter = 88,
        Keypad1 = 89,
        Keypad2 = 90,
        Keypad3 = 91,
        Keypad4 = 92,
        Keypad5 = 93,
        Keypad6 = 94,
        Keypad7 = 95,
        Keypad8 = 96,
        Keypad9 = 97,
        Keypad0 = 98,
        KeypadPeriod = 99,

        Application = 101,
        KeypadEquals = 103,

        F13 = 104,
        F14 = 105,
        F15 = 106,
        F16 = 107,
        F17 = 108,
        F18 = 109,
        F19 = 110,
        F20 = 111,
        F21 = 112,
        F22 = 113,
        F23 = 114,
        F24 = 115,

        LeftCtrl = 224,
        LeftShift = 225,
        LeftAlt = 226,
        LeftGui = 227,
        RightCtrl = 228,
        RightShift = 229,
        RightAlt = 230,
        RightGui = 231,

        Count = 512,
    };

    /// The key's name as a shortcut shows it ("S", "F5", "Page Up", "Keypad +"); empty for
    /// Unknown. The table next to the enum, so a new key names itself here.
    [[nodiscard]] constexpr const char8_t* KeyCodeName(KeyCode key) noexcept
    {
        switch (key)
        {
        case KeyCode::A:
            return u8"A";
        case KeyCode::B:
            return u8"B";
        case KeyCode::C:
            return u8"C";
        case KeyCode::D:
            return u8"D";
        case KeyCode::E:
            return u8"E";
        case KeyCode::F:
            return u8"F";
        case KeyCode::G:
            return u8"G";
        case KeyCode::H:
            return u8"H";
        case KeyCode::I:
            return u8"I";
        case KeyCode::J:
            return u8"J";
        case KeyCode::K:
            return u8"K";
        case KeyCode::L:
            return u8"L";
        case KeyCode::M:
            return u8"M";
        case KeyCode::N:
            return u8"N";
        case KeyCode::O:
            return u8"O";
        case KeyCode::P:
            return u8"P";
        case KeyCode::Q:
            return u8"Q";
        case KeyCode::R:
            return u8"R";
        case KeyCode::S:
            return u8"S";
        case KeyCode::T:
            return u8"T";
        case KeyCode::U:
            return u8"U";
        case KeyCode::V:
            return u8"V";
        case KeyCode::W:
            return u8"W";
        case KeyCode::X:
            return u8"X";
        case KeyCode::Y:
            return u8"Y";
        case KeyCode::Z:
            return u8"Z";
        case KeyCode::Num1:
            return u8"1";
        case KeyCode::Num2:
            return u8"2";
        case KeyCode::Num3:
            return u8"3";
        case KeyCode::Num4:
            return u8"4";
        case KeyCode::Num5:
            return u8"5";
        case KeyCode::Num6:
            return u8"6";
        case KeyCode::Num7:
            return u8"7";
        case KeyCode::Num8:
            return u8"8";
        case KeyCode::Num9:
            return u8"9";
        case KeyCode::Num0:
            return u8"0";
        case KeyCode::Return:
            return u8"Return";
        case KeyCode::Escape:
            return u8"Escape";
        case KeyCode::Backspace:
            return u8"Backspace";
        case KeyCode::Tab:
            return u8"Tab";
        case KeyCode::Space:
            return u8"Space";
        case KeyCode::Minus:
            return u8"-";
        case KeyCode::Equals:
            return u8"=";
        case KeyCode::LeftBracket:
            return u8"[";
        case KeyCode::RightBracket:
            return u8"]";
        case KeyCode::Backslash:
            return u8"\\";
        case KeyCode::Semicolon:
            return u8";";
        case KeyCode::Apostrophe:
            return u8"'";
        case KeyCode::Grave:
            return u8"`";
        case KeyCode::Comma:
            return u8",";
        case KeyCode::Period:
            return u8".";
        case KeyCode::Slash:
            return u8"/";
        case KeyCode::CapsLock:
            return u8"Caps Lock";
        case KeyCode::F1:
            return u8"F1";
        case KeyCode::F2:
            return u8"F2";
        case KeyCode::F3:
            return u8"F3";
        case KeyCode::F4:
            return u8"F4";
        case KeyCode::F5:
            return u8"F5";
        case KeyCode::F6:
            return u8"F6";
        case KeyCode::F7:
            return u8"F7";
        case KeyCode::F8:
            return u8"F8";
        case KeyCode::F9:
            return u8"F9";
        case KeyCode::F10:
            return u8"F10";
        case KeyCode::F11:
            return u8"F11";
        case KeyCode::F12:
            return u8"F12";
        case KeyCode::PrintScreen:
            return u8"Print Screen";
        case KeyCode::ScrollLock:
            return u8"Scroll Lock";
        case KeyCode::Pause:
            return u8"Pause";
        case KeyCode::Insert:
            return u8"Insert";
        case KeyCode::Home:
            return u8"Home";
        case KeyCode::PageUp:
            return u8"Page Up";
        case KeyCode::Delete:
            return u8"Delete";
        case KeyCode::End:
            return u8"End";
        case KeyCode::PageDown:
            return u8"Page Down";
        case KeyCode::Right:
            return u8"Right";
        case KeyCode::Left:
            return u8"Left";
        case KeyCode::Down:
            return u8"Down";
        case KeyCode::Up:
            return u8"Up";
        case KeyCode::NumLock:
            return u8"Num Lock";
        case KeyCode::KeypadDivide:
            return u8"Keypad /";
        case KeyCode::KeypadMultiply:
            return u8"Keypad *";
        case KeyCode::KeypadMinus:
            return u8"Keypad -";
        case KeyCode::KeypadPlus:
            return u8"Keypad +";
        case KeyCode::KeypadEnter:
            return u8"Keypad Enter";
        case KeyCode::Keypad1:
            return u8"Keypad 1";
        case KeyCode::Keypad2:
            return u8"Keypad 2";
        case KeyCode::Keypad3:
            return u8"Keypad 3";
        case KeyCode::Keypad4:
            return u8"Keypad 4";
        case KeyCode::Keypad5:
            return u8"Keypad 5";
        case KeyCode::Keypad6:
            return u8"Keypad 6";
        case KeyCode::Keypad7:
            return u8"Keypad 7";
        case KeyCode::Keypad8:
            return u8"Keypad 8";
        case KeyCode::Keypad9:
            return u8"Keypad 9";
        case KeyCode::Keypad0:
            return u8"Keypad 0";
        case KeyCode::KeypadPeriod:
            return u8"Keypad .";
        case KeyCode::Application:
            return u8"Application";
        case KeyCode::KeypadEquals:
            return u8"Keypad =";
        case KeyCode::F13:
            return u8"F13";
        case KeyCode::F14:
            return u8"F14";
        case KeyCode::F15:
            return u8"F15";
        case KeyCode::F16:
            return u8"F16";
        case KeyCode::F17:
            return u8"F17";
        case KeyCode::F18:
            return u8"F18";
        case KeyCode::F19:
            return u8"F19";
        case KeyCode::F20:
            return u8"F20";
        case KeyCode::F21:
            return u8"F21";
        case KeyCode::F22:
            return u8"F22";
        case KeyCode::F23:
            return u8"F23";
        case KeyCode::F24:
            return u8"F24";
        case KeyCode::Unknown:
        default:
            return u8"";
        }
    }
}

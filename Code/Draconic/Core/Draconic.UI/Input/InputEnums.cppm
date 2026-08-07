// Draconic UI - :input_enums partition
//
// Input enums ported from Sedulous.UI/src/Input (MouseButton, KeyModifiers, EventPhase,
// FocusDirection, KeyCode). Values match Sedulous.Shell.Input so a runtime bridge can cast directly.
// Grouped into one partition (Beef had a file per enum).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:input_enums;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
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
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :event_args partition
//
// Pooled input event args (mouse/key/wheel/text). One instance reused per event type to avoid
// allocation in the hot input path. Ported from Sedulous.UI/src/Input/*EventArgs.bf. These carry only
// data - no View reference - so they can sit below :view in the module graph.

module;
#include "Core/Prelude.h"

export module foundation.ui:event_args;

import foundation.core; // Float2
import :input_enums;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Pooled mouse event args.
    struct MouseEventArgs
    {
        f32 X = 0.0f; ///< Position in UI logical coordinates.
        f32 Y = 0.0f;
        MouseButton Button = MouseButton::Left;
        i32 ClickCount = 0; ///< 1 = single, 2 = double, ...
        KeyModifiers Modifiers = KeyModifiers::None;
        f32 Timestamp = 0.0f;
        bool Handled = false; ///< Set by a handler to stop propagation.
        EventPhase Phase = EventPhase::Target;

        [[nodiscard]] Float2 Position() const noexcept { return Float2{X, Y}; }

        void Reset()
        {
            X = 0;
            Y = 0;
            Button = MouseButton::Left;
            ClickCount = 0;
            Modifiers = KeyModifiers::None;
            Timestamp = 0;
            Handled = false;
            Phase = EventPhase::Target;
        }

        void Set(f32 x, f32 y, MouseButton button = MouseButton::Left, i32 clickCount = 1,
                 f32 timestamp = 0.0f, KeyModifiers modifiers = KeyModifiers::None)
        {
            X = x;
            Y = y;
            Button = button;
            ClickCount = clickCount;
            Modifiers = modifiers;
            Timestamp = timestamp;
            Handled = false;
            Phase = EventPhase::Target;
        }
    };

    /// Pooled key event args.
    struct KeyEventArgs
    {
        KeyCode Key = KeyCode::Unknown;
        i32 ScanCode = 0;
        KeyModifiers Modifiers = KeyModifiers::None;
        bool IsRepeat = false;
        f32 Timestamp = 0.0f;
        bool Handled = false;
        EventPhase Phase = EventPhase::Target;

        void Reset()
        {
            Key = KeyCode::Unknown;
            ScanCode = 0;
            Modifiers = KeyModifiers::None;
            IsRepeat = false;
            Timestamp = 0;
            Handled = false;
            Phase = EventPhase::Target;
        }

        void Set(KeyCode key, KeyModifiers modifiers, bool isRepeat, f32 timestamp = 0.0f,
                 i32 scanCode = 0)
        {
            Key = key;
            ScanCode = scanCode;
            Modifiers = modifiers;
            IsRepeat = isRepeat;
            Timestamp = timestamp;
            Handled = false;
            Phase = EventPhase::Target;
        }
    };

    /// Pooled mouse wheel event args.
    struct MouseWheelEventArgs
    {
        f32 X = 0.0f;
        f32 Y = 0.0f;
        f32 DeltaX = 0.0f; ///< Positive = scroll up/right.
        f32 DeltaY = 0.0f;
        KeyModifiers Modifiers = KeyModifiers::None;
        bool Handled = false;
        EventPhase Phase = EventPhase::Target;

        void Reset()
        {
            X = 0;
            Y = 0;
            DeltaX = 0;
            DeltaY = 0;
            Modifiers = KeyModifiers::None;
            Handled = false;
            Phase = EventPhase::Target;
        }
    };

    /// Pooled text input event args (post-IME composition).
    struct TextInputEventArgs
    {
        char32_t Character = 0; ///< The Unicode codepoint entered.
        bool Handled = false;
        EventPhase Phase = EventPhase::Target;

        void Reset()
        {
            Character = 0;
            Handled = false;
            Phase = EventPhase::Target;
        }
    };
}

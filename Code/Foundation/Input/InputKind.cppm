// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Input - :input_kind partition.
//
// The kind of input the player is using now: the pointer, the keyboard or a pad. A game shows a button prompt for the device in hand, hides the
// cursor on a pad and the focus ring under a mouse; none of that needs a GUI (a prompt in the 3D
// world), so the tracker lives here, beside the actions, and a GUI's host passes its kind on.
//
// It follows deliberate input only, so a resting stick or a nudged desk does not flip it: a key
// pressed, a mouse button pressed, the mouse moved past a few pixels or its wheel turned, a finger
// put down, a pad button pressed, a stick or trigger pushed past the press point from inside it
// (a fresh push: one held there does not count again). When a frame has several, a press
// outranks motion, and a pad outranks the keyboard, the keyboard the mouse. A GUI builds its
// focus-ring rule on this: show the ring on navigation, hide it on a pointer press.

module;
#include "Core/Prelude.h"
#include <cmath>

export module foundation.input:input_kind;

import foundation.core;
import foundation.shell;
import :action_runtime; // IInputSourceProvider, ActionRuntime::kPressPoint

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::input
{
    enum class InputKind : u8
    {
        Pointer,  ///< the mouse, or a finger
        Keyboard,
        Pad,
    };

    /// The name a sheet or a script uses for `kind`: "pointer", "keyboard" or "pad".
    [[nodiscard]] inline core::StringView InputKindName(InputKind kind) noexcept
    {
        switch (kind)
        {
        case InputKind::Keyboard: return core::StringView(u8"keyboard");
        case InputKind::Pad: return core::StringView(u8"pad");
        case InputKind::Pointer: break;
        }
        return core::StringView(u8"pointer");
    }

    class InputKindTracker
    {
    public:
        using Listener = core::Function<void(InputKind)>;

        /// Reads this frame's input from `devices` (after the shell's pump, once a frame) and
        /// changes the kind when deliberate input of another kind came, telling the listeners.
        void Update(IInputSourceProvider& devices)
        {
            const Optional<InputKind> seen = Observe(devices);
            if (seen.HasValue())
                SetKind(seen.Value());
        }

        [[nodiscard]] InputKind Kind() const noexcept { return m_kind; }
        /// Sets the kind (a host starting on a pad, a test); listeners hear a change.
        void SetKind(InputKind kind)
        {
            if (kind == m_kind)
                return;
            m_kind = kind;
            m_notifying = true;
            for (usize i = 0; i < m_listeners.Size(); ++i)
                if (!m_listeners[i].Removed)
                    m_listeners[i].Callback(kind);
            m_notifying = false;
            for (usize i = 0; i < m_listeners.Size();)
            {
                if (m_listeners[i].Removed)
                    m_listeners.RemoveAt(i);
                else
                    ++i;
            }
        }

        /// Told each change of kind; a token for RemoveListener.
        u32 AddListener(Listener listener)
        {
            m_listeners.PushBack(Entry{core::Move(listener), ++m_nextToken, false});
            return m_nextToken;
        }
        /// False when the token is none of its. One removed from inside a listener goes once they return.
        bool RemoveListener(u32 token)
        {
            for (usize i = 0; i < m_listeners.Size(); ++i)
                if (m_listeners[i].Token == token && !m_listeners[i].Removed)
                {
                    if (m_notifying)
                        m_listeners[i].Removed = true;
                    else
                        m_listeners.RemoveAt(i);
                    return true;
                }
            return false;
        }

        /// How far a stick or a trigger must be pushed to count (0.5, the actions' press point).
        void SetStickThreshold(f32 threshold) noexcept { m_stickThreshold = core::Clamp(threshold, 0.0f, 1.0f); }
        [[nodiscard]] f32 GetStickThreshold() const noexcept { return m_stickThreshold; }
        /// How far the mouse must move in a frame to count, in its pixels (3).
        void SetPointerThreshold(f32 pixels) noexcept { m_pointerThreshold = core::Max(0.0f, pixels); }
        [[nodiscard]] f32 GetPointerThreshold() const noexcept { return m_pointerThreshold; }

    private:
        struct Entry
        {
            Listener Callback;
            u32 Token = 0;
            bool Removed = false;
        };
        // A pad's analog inputs past the threshold at the last frame, one bit each: the two
        // sticks, then the two triggers.
        static constexpr u32 kAnalogCount = 4;

        [[nodiscard]] Optional<InputKind> Observe(IInputSourceProvider& devices)
        {
            // A finger put down (read every frame, so the count stays current whatever wins).
            bool fingerDown = false;
            if (shell::ITouch* touch = devices.Touch())
            {
                const i32 fingers = touch->TouchCount();
                fingerDown = fingers > m_fingers;
                m_fingers = fingers;
            }
            bool pad = false;
            const i32 pads = devices.GamepadCount();
            if (m_padAnalog.Size() < static_cast<usize>(core::Max(pads, 0)))
                m_padAnalog.Resize(static_cast<usize>(pads), 0u);
            for (i32 p = 0; p < pads; ++p)
            {
                u32& was = m_padAnalog[static_cast<usize>(p)];
                shell::IGamepad* gamepad = devices.Gamepad(p);
                if (gamepad == nullptr || !gamepad->Connected())
                {
                    was = 0;
                    continue;
                }
                for (u32 code = 0; code < static_cast<u32>(shell::GamepadButton::Count); ++code)
                    pad = pad || gamepad->IsButtonPressed(static_cast<shell::GamepadButton>(code));
                const auto axis = [gamepad](shell::GamepadAxis a) { return gamepad->Axis(a); };
                const f32 magnitudes[kAnalogCount] = {
                    std::sqrt(axis(shell::GamepadAxis::LeftX) * axis(shell::GamepadAxis::LeftX) +
                              axis(shell::GamepadAxis::LeftY) * axis(shell::GamepadAxis::LeftY)),
                    std::sqrt(axis(shell::GamepadAxis::RightX) * axis(shell::GamepadAxis::RightX) +
                              axis(shell::GamepadAxis::RightY) * axis(shell::GamepadAxis::RightY)),
                    axis(shell::GamepadAxis::LeftTrigger),
                    axis(shell::GamepadAxis::RightTrigger),
                };
                u32 now = 0;
                for (u32 i = 0; i < kAnalogCount; ++i)
                    if (magnitudes[i] > m_stickThreshold)
                        now |= 1u << i;
                pad = pad || (now & ~was) != 0; // pushed past it this frame, not held there
                was = now;
            }
            if (pad)
                return InputKind::Pad;
            if (shell::IKeyboard* keyboard = devices.Keyboard())
                for (u32 code = 1; code < static_cast<u32>(shell::KeyCode::Count); ++code)
                    if (keyboard->IsKeyPressed(static_cast<shell::KeyCode>(code)))
                        return InputKind::Keyboard;
            bool pointer = false;
            if (shell::IMouse* mouse = devices.Mouse())
            {
                for (u32 code = 0; code < static_cast<u32>(shell::MouseButton::Count) && !pointer; ++code)
                    pointer = mouse->IsButtonPressed(static_cast<shell::MouseButton>(code));
                const f32 dx = mouse->DeltaX();
                const f32 dy = mouse->DeltaY();
                pointer = pointer || dx * dx + dy * dy > m_pointerThreshold * m_pointerThreshold || mouse->ScrollX() != 0.0f ||
                          mouse->ScrollY() != 0.0f;
            }
            pointer = pointer || fingerDown;
            if (pointer)
                return InputKind::Pointer;
            return {};
        }

        InputKind m_kind = InputKind::Pointer;
        f32 m_stickThreshold = ActionRuntime::kPressPoint;
        f32 m_pointerThreshold = 3.0f;
        Array<u32> m_padAnalog; // per pad: its analog inputs past the threshold at the last frame
        i32 m_fingers = 0;
        Array<Entry> m_listeners;
        u32 m_nextToken = 0;
        bool m_notifying = false;
    };
}

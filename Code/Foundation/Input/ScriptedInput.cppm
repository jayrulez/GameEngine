// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input - :scripted_input partition (agent-playtesting-and-asset-creation.md P4)
//
// ScriptedInputSource: an input source driven by a timeline instead of devices - a virtual
// keyboard, mouse and gamepads, and the event stream their changes make, for a playtest no one
// plays by hand. Device level on purpose: a playtest goes through the project's input map the way
// a player does. Advance once per frame with the time since the script started: every entry due by
// then applies in order, and the edges (pressed, released) and the events are that frame's, so a
// press and its release landing in one frame still read as a press. Nothing real is merged in:
// where the user's mouse is must not change a playtest.

module;
#include "Core/Prelude.h"

export module foundation.input:scripted_input;

import foundation.core;
import foundation.shell;
import :action_runtime;

using namespace foundation::core;

export namespace foundation::input
{
    /// What one timeline entry does to the virtual devices.
    enum class ScriptedInputKind : u8
    {
        Key,         ///< a key goes down or up
        MouseButton, ///< a mouse button goes down or up
        MouseMove,   ///< the pointer moves to x, y (window space)
        MouseWheel,  ///< the wheel turns by x, y
        PadButton,   ///< a gamepad button goes down or up
        PadAxis,     ///< a gamepad axis takes `value`
    };

    /// One entry of a scripted input timeline: at `at` seconds since the script started, a key,
    /// button or axis changes.
    struct ScriptedInput
    {
        f64 at = 0.0;
        ScriptedInputKind kind = ScriptedInputKind::Key;
        shell::KeyCode key = shell::KeyCode::Unknown;
        shell::MouseButton button = shell::MouseButton::Left;
        shell::GamepadButton padButton = shell::GamepadButton::South;
        shell::GamepadAxis padAxis = shell::GamepadAxis::LeftX;
        i32 gamepad = 0;
        bool down = false;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 value = 0.0f;
    };

    class ScriptedKeyboard final : public shell::IKeyboard
    {
    public:
        void BeginFrame();
        /// False when the key already was in that state: no edge, no event.
        bool Set(shell::KeyCode key, bool down);
        [[nodiscard]] bool IsKeyDown(shell::KeyCode key) const override;
        [[nodiscard]] bool IsKeyPressed(shell::KeyCode key) const override;
        [[nodiscard]] bool IsKeyReleased(shell::KeyCode key) const override;
        [[nodiscard]] shell::KeyModifiers Modifiers() const override;

    private:
        static constexpr usize kCount = static_cast<usize>(shell::KeyCode::Count);
        bool m_down[kCount] = {};
        bool m_pressed[kCount] = {};
        bool m_released[kCount] = {};
    };

    class ScriptedMouse final : public shell::IMouse
    {
    public:
        void BeginFrame();
        bool SetButton(shell::MouseButton button, bool down);
        void MoveTo(f32 x, f32 y);
        void Wheel(f32 x, f32 y);

        [[nodiscard]] f32 X() const override { return m_x; }
        [[nodiscard]] f32 Y() const override { return m_y; }
        [[nodiscard]] f32 GlobalX() const override { return m_x; }
        [[nodiscard]] f32 GlobalY() const override { return m_y; }
        [[nodiscard]] f32 DeltaX() const override { return m_deltaX; }
        [[nodiscard]] f32 DeltaY() const override { return m_deltaY; }
        [[nodiscard]] f32 ScrollX() const override { return m_scrollX; }
        [[nodiscard]] f32 ScrollY() const override { return m_scrollY; }
        [[nodiscard]] bool IsButtonDown(shell::MouseButton button) const override;
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton button) const override;
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton button) const override;
        [[nodiscard]] bool RelativeMode() const override { return m_relative; }
        void SetRelativeMode(bool enabled) override { m_relative = enabled; }
        [[nodiscard]] bool CursorVisible() const override { return m_cursorVisible; }
        void SetCursorVisible(bool visible) override { m_cursorVisible = visible; }
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}

    private:
        static constexpr usize kCount = static_cast<usize>(shell::MouseButton::Count);
        bool m_down[kCount] = {};
        bool m_pressed[kCount] = {};
        bool m_released[kCount] = {};
        f32 m_x = 0.0f;
        f32 m_y = 0.0f;
        f32 m_deltaX = 0.0f;
        f32 m_deltaY = 0.0f;
        f32 m_scrollX = 0.0f;
        f32 m_scrollY = 0.0f;
        bool m_relative = false;
        bool m_cursorVisible = true;
    };

    class ScriptedGamepad final : public shell::IGamepad
    {
    public:
        explicit ScriptedGamepad(i32 index) : m_index(index) {}
        void BeginFrame();
        bool SetButton(shell::GamepadButton button, bool down);
        void SetAxis(shell::GamepadAxis axis, f32 value);

        [[nodiscard]] i32 Index() const override { return m_index; }
        [[nodiscard]] StringView Name() const override { return u8"Scripted pad"; }
        [[nodiscard]] bool Connected() const override { return true; }
        [[nodiscard]] bool IsButtonDown(shell::GamepadButton button) const override;
        [[nodiscard]] bool IsButtonPressed(shell::GamepadButton button) const override;
        [[nodiscard]] bool IsButtonReleased(shell::GamepadButton button) const override;
        [[nodiscard]] f32 Axis(shell::GamepadAxis axis) const override;
        void SetRumble(f32, f32, u32) override {}

        static constexpr usize kButtons = static_cast<usize>(shell::GamepadButton::Count);
        static constexpr usize kAxes = static_cast<usize>(shell::GamepadAxis::Count);
        bool down[kButtons] = {};
        f32 axes[kAxes] = {};

    private:
        i32 m_index;
        bool m_pressed[kButtons] = {};
        bool m_released[kButtons] = {};
    };

    /// ScriptedInputSource::kMaxGamepads, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr i32 kScriptedInputSourceMaxGamepads = 4;

    class ScriptedInputSource final : public IInputSourceProvider
    {
    public:
        /// The gamepads a timeline can address, 0 to 3.
        static constexpr i32 kMaxGamepads = kScriptedInputSourceMaxGamepads;

        explicit ScriptedInputSource(IAllocator& allocator);

        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &m_keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &m_mouse; }
        /// The pads the timeline addresses: one past the highest index it names.
        [[nodiscard]] i32 GamepadCount() const override { return m_padCount; }
        [[nodiscard]] shell::IGamepad* Gamepad(i32 index) override;
        [[nodiscard]] Span<const shell::InputEvent> Events() override
        {
            return Span<const shell::InputEvent>{m_events.Data(), m_events.Size()};
        }

        /// The time the last Advance moved to, in seconds since the script started.
        [[nodiscard]] f64 Time() const noexcept { return m_time; }
        /// Every entry has applied.
        [[nodiscard]] bool Finished() const noexcept { return m_next >= m_timeline.Size(); }
        [[nodiscard]] usize Count() const noexcept { return m_timeline.Size(); }

        /// Adds an entry, kept in time order; entries at the same time apply in the order added.
        /// Refused (false) for a gamepad index outside 0 to kMaxGamepads - 1.
        bool Add(const ScriptedInput& input);

        /// One frame: this frame's edges and events are cleared, then every entry due by `time`
        /// applies.
        void Advance(f64 time);

        /// Lets go of everything held, as a frame of its own: a run that ends with a key down
        /// must not leave the game believing it is still held.
        void ReleaseAll();

        /// A key, mouse button, pad button or pad axis by its reflected case name, any case:
        /// "D", "Space", "LeftShift"; "Left"; "South", "DPadUp"; "LeftX", "RightTrigger".
        static bool ParseKey(StringView name, shell::KeyCode& out);
        static bool ParseMouseButton(StringView name, shell::MouseButton& out);
        static bool ParsePadButton(StringView name, shell::GamepadButton& out);
        static bool ParsePadAxis(StringView name, shell::GamepadAxis& out);

    private:
        void BeginFrame();
        void Apply(const ScriptedInput& input);

        Array<ScriptedInput> m_timeline;
        usize m_next = 0;
        f64 m_time = 0.0;
        ScriptedKeyboard m_keyboard;
        ScriptedMouse m_mouse;
        ScriptedGamepad m_pads[kMaxGamepads] = {ScriptedGamepad(0), ScriptedGamepad(1),
                                                ScriptedGamepad(2), ScriptedGamepad(3)};
        i32 m_padCount = 0;
        Array<shell::InputEvent> m_events;
    };
}

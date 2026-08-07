// Draconic::Shell - `:input` partition.
//
// Abstract input device interfaces: IKeyboard, IMouse, IGamepad, ITouch, and
// IInputManager (the aggregate the shell exposes). State is double-buffered
// per frame so callers can ask Down (held), Pressed (this frame), and Released
// (this frame). Backends implement these; IInputManager::Update() rolls the
// frame. Ported from Draconic (itself a Sedulous port).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell:input;

import draconic.foundation;
import :input_types;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class IKeyboard
    {
    public:
        virtual ~IKeyboard() = default;

        [[nodiscard]] virtual bool IsKeyDown(KeyCode key) const = 0;     // held
        [[nodiscard]] virtual bool IsKeyPressed(KeyCode key) const = 0;  // went down this frame
        [[nodiscard]] virtual bool IsKeyReleased(KeyCode key) const = 0; // went up this frame
        [[nodiscard]] virtual KeyModifiers Modifiers() const = 0;
    };

    class IMouse
    {
    public:
        virtual ~IMouse() = default;

        [[nodiscard]] virtual foundation::f32 X() const = 0; // window-space position
        [[nodiscard]] virtual foundation::f32 Y() const = 0;
        // Desktop-global cursor position in logical screen coordinates (surface/window-independent).
        // Multi-window drag math uses this because per-window-local coords are unreliable while a
        // window follows the cursor or resizes mid-event; global deltas stay valid regardless.
        [[nodiscard]] virtual foundation::f32 GlobalX() const = 0;
        [[nodiscard]] virtual foundation::f32 GlobalY() const = 0;
        [[nodiscard]] virtual foundation::f32 DeltaX() const = 0; // movement this frame
        [[nodiscard]] virtual foundation::f32 DeltaY() const = 0;
        [[nodiscard]] virtual foundation::f32 ScrollX() const = 0; // wheel this frame
        [[nodiscard]] virtual foundation::f32 ScrollY() const = 0;

        [[nodiscard]] virtual bool IsButtonDown(MouseButton button) const = 0;
        [[nodiscard]] virtual bool IsButtonPressed(MouseButton button) const = 0;
        [[nodiscard]] virtual bool IsButtonReleased(MouseButton button) const = 0;

        [[nodiscard]] virtual bool RelativeMode() const = 0;
        virtual void SetRelativeMode(bool enabled) = 0;
        [[nodiscard]] virtual bool CursorVisible() const = 0;
        virtual void SetCursorVisible(bool visible) = 0;
        virtual void SetCursor(CursorType cursor) = 0;

        // Capture the mouse app-globally so events (move + buttons) keep flowing even when the cursor
        // leaves a window. Required for multi-window drag / resize: without it the OS stops delivering
        // events once the pointer exits the window, so the operation stalls at the window edge and a
        // release outside the window is never seen. Enable at the start of a drag/resize, disable at end.
        virtual void SetGlobalCapture(bool enabled) = 0;
    };

    class IGamepad
    {
    public:
        virtual ~IGamepad() = default;

        [[nodiscard]] virtual foundation::i32 Index() const = 0;
        [[nodiscard]] virtual foundation::StringView Name() const = 0;
        [[nodiscard]] virtual bool Connected() const = 0;

        [[nodiscard]] virtual bool IsButtonDown(GamepadButton button) const = 0;
        [[nodiscard]] virtual bool IsButtonPressed(GamepadButton button) const = 0;
        [[nodiscard]] virtual bool IsButtonReleased(GamepadButton button) const = 0;
        [[nodiscard]] virtual foundation::f32 Axis(GamepadAxis axis) const = 0; // [-1,1], triggers [0,1]

        // Low/high-frequency motor strengths in [0,1] for durationMs milliseconds.
        virtual void SetRumble(foundation::f32 lowFreq, foundation::f32 highFreq, foundation::u32 durationMs) = 0;
    };

    class ITouch
    {
    public:
        virtual ~ITouch() = default;

        [[nodiscard]] virtual foundation::i32 TouchCount() const = 0;
        [[nodiscard]] virtual bool GetTouchPoint(foundation::i32 index, TouchPoint& out) const = 0;
        [[nodiscard]] virtual bool HasTouch() const = 0;
    };

    // Aggregate exposed by IShell::Input(). Devices are owned by the manager.
    class IInputManager
    {
    public:
        virtual ~IInputManager() = default;

        [[nodiscard]] virtual IKeyboard* Keyboard() = 0;
        [[nodiscard]] virtual IMouse* Mouse() = 0;
        [[nodiscard]] virtual ITouch* Touch() = 0;
        [[nodiscard]] virtual foundation::i32 GamepadCount() const = 0;
        [[nodiscard]] virtual IGamepad* GetGamepad(foundation::i32 index) = 0;

        // This frame's input events (the event-first source of truth; the device
        // snapshots above are a fold over these). Cleared each frame by Update().
        [[nodiscard]] virtual foundation::Span<const InputEvent> Events() const = 0;

        // Routing authority (per docs/design/viewport-input.md §4.1): the window under
        // the pointer (mouse routing) and the keyboard/gamepad-focused window. 0 = none.
        [[nodiscard]] virtual foundation::u32 HoverWindow() const = 0;
        [[nodiscard]] virtual foundation::u32 FocusedWindow() const = 0;

        // Rolls per-frame state (current -> previous, clears deltas + events). The
        // shell calls this once per frame before pumping OS events.
        virtual void Update() = 0;
    };
}

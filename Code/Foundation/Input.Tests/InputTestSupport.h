// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shared synthetic-device and representative-map helpers for the input tests.
//
// Header-only test scaffolding (internal linkage: each including TU gets its own copy).
// The includer must already have done, before including this:
//     import foundation.core; import foundation.shell; import foundation.input;
//     using namespace foundation::core; using namespace foundation::input;
//     namespace shell = foundation::shell;
// Used by both Foundation/Input.Tests (foundation action-model tests) and
// Engine/Engine.Input.Tests (the InputSubsystem and script-facade tests).
#pragma once

namespace
{
    // ---- synthetic devices --------------------------------------------------------------
    class FakeKeyboard final : public shell::IKeyboard
    {
    public:
        bool down[512] = {};
        bool pressed[512] = {};
        shell::KeyModifiers mods = shell::KeyModifiers::None;
        [[nodiscard]] bool IsKeyDown(shell::KeyCode key) const override
        {
            return down[static_cast<u32>(key) & 511];
        }
        [[nodiscard]] bool IsKeyPressed(shell::KeyCode key) const override
        {
            return pressed[static_cast<u32>(key) & 511];
        }
        [[nodiscard]] bool IsKeyReleased(shell::KeyCode) const override { return false; }
        [[nodiscard]] shell::KeyModifiers Modifiers() const override { return mods; }
        void Set(shell::KeyCode key, bool value) { down[static_cast<u32>(key) & 511] = value; }
    };

    class FakeMouse final : public shell::IMouse
    {
    public:
        f32 x = 0, y = 0, dx = 0, dy = 0, wheel = 0;
        bool buttons[8] = {};
        bool pressed[8] = {};  ///< this frame's presses (edges)
        bool released[8] = {}; ///< this frame's releases (edges)
        bool relative = false;
        bool cursorVisible = true;
        i32 relativeChanges = 0; ///< SetRelativeMode calls that changed it
        [[nodiscard]] f32 X() const override { return x; }
        [[nodiscard]] f32 Y() const override { return y; }
        [[nodiscard]] f32 GlobalX() const override { return 0; }
        [[nodiscard]] f32 GlobalY() const override { return 0; }
        [[nodiscard]] f32 DeltaX() const override { return dx; }
        [[nodiscard]] f32 DeltaY() const override { return dy; }
        [[nodiscard]] f32 ScrollX() const override { return 0; }
        [[nodiscard]] f32 ScrollY() const override { return wheel; }
        [[nodiscard]] bool IsButtonDown(shell::MouseButton b) const override
        {
            return buttons[static_cast<u32>(b) & 7];
        }
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton b) const override { return pressed[static_cast<u32>(b) & 7]; }
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton b) const override { return released[static_cast<u32>(b) & 7]; }
        [[nodiscard]] bool RelativeMode() const override { return relative; }
        void SetRelativeMode(bool enabled) override
        {
            relativeChanges += enabled != relative ? 1 : 0;
            relative = enabled;
        }
        [[nodiscard]] bool CursorVisible() const override { return cursorVisible; }
        void SetCursorVisible(bool visible) override { cursorVisible = visible; }
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    class FakeGamepad final : public shell::IGamepad
    {
    public:
        i32 index = 0;
        bool connected = true;
        bool buttons[32] = {};
        f32 axes[6] = {};
        [[nodiscard]] i32 Index() const override { return index; }
        [[nodiscard]] StringView Name() const override { return u8"fake"; }
        [[nodiscard]] bool Connected() const override { return connected; }
        bool buttonsPressed[32] = {};
        [[nodiscard]] bool IsButtonDown(shell::GamepadButton b) const override
        {
            return buttons[static_cast<u32>(b) & 31];
        }
        [[nodiscard]] bool IsButtonPressed(shell::GamepadButton b) const override
        {
            return buttonsPressed[static_cast<u32>(b) & 31];
        }
        [[nodiscard]] bool IsButtonReleased(shell::GamepadButton) const override { return false; }
        [[nodiscard]] f32 Axis(shell::GamepadAxis a) const override
        {
            return axes[static_cast<u32>(a) % 6];
        }
        // The last rumble asked of the pad, and how many times one was.
        f32 rumbleLow = 0.0f;
        f32 rumbleHigh = 0.0f;
        u32 rumbleMs = 0;
        i32 rumbleCalls = 0;
        void SetRumble(f32 low, f32 high, u32 durationMs) override
        {
            rumbleLow = low;
            rumbleHigh = high;
            rumbleMs = durationMs;
            ++rumbleCalls;
        }
    };

    class FakeTouch final : public shell::ITouch
    {
    public:
        Array<shell::TouchPoint> points;
        [[nodiscard]] i32 TouchCount() const override { return static_cast<i32>(points.Size()); }
        [[nodiscard]] bool GetTouchPoint(i32 index, shell::TouchPoint& out) const override
        {
            if (index < 0 || static_cast<usize>(index) >= points.Size())
            {
                return false;
            }
            out = points[static_cast<usize>(index)];
            return true;
        }
        [[nodiscard]] bool HasTouch() const override { return !points.IsEmpty(); }
        void Set(u64 id, f32 x, f32 y)
        {
            for (auto& p : points)
            {
                if (p.id == id)
                {
                    p.x = x;
                    p.y = y;
                    return;
                }
            }
            points.PushBack(shell::TouchPoint{id, x, y, 1.0f});
        }
        void Remove(u64 id)
        {
            for (usize i = 0; i < points.Size(); ++i)
            {
                if (points[i].id == id)
                {
                    points.RemoveAt(i);
                    return;
                }
            }
        }
    };

    class FakeDevices final : public IInputSourceProvider
    {
    public:
        FakeKeyboard keyboard;
        FakeMouse mouse;
        FakeTouch touch;
        Array<FakeGamepad*> pads;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] i32 GamepadCount() const override { return static_cast<i32>(pads.Size()); }
        [[nodiscard]] shell::IGamepad* Gamepad(i32 i) override
        {
            return (i >= 0 && i < static_cast<i32>(pads.Size())) ? pads[static_cast<usize>(i)]
                                                                 : nullptr;
        }
        [[nodiscard]] shell::ITouch* Touch() override { return &touch; }
    };

    // ---- a representative map ----------------------------------------------------------
    // [[maybe_unused]]: a TU may include this support header for FakeDevices but not the map (e.g.
    // Engine.Input.Tests once its script-facade case is #ifdef'd out in a backend-less build).
    [[nodiscard, maybe_unused]] InputMap MakeGameplayMap()
    {
        InputMap map;
        ActionSet gameplay;
        gameplay.name = String(u8"Gameplay");
        gameplay.priority = 0;
        {
            Action jump;
            jump.name = String(u8"Jump");
            jump.kind = ActionKind::Button;
            Binding key;
            key.source = BindingSource::Key;
            key.code = static_cast<u32>(shell::KeyCode::Space);
            jump.bindings.PushBack(key);
            Binding pad;
            pad.source = BindingSource::GamepadButton;
            pad.code = 0; // "south" button
            jump.bindings.PushBack(pad);
            gameplay.actions.PushBack(static_cast<Action&&>(jump));
        }
        {
            Action move;
            move.name = String(u8"Move");
            move.kind = ActionKind::Axis2D;
            Binding wasd;
            wasd.source = BindingSource::Composite2D;
            wasd.negX = static_cast<u32>(shell::KeyCode::A);
            wasd.posX = static_cast<u32>(shell::KeyCode::D);
            wasd.negY = static_cast<u32>(shell::KeyCode::S);
            wasd.posY = static_cast<u32>(shell::KeyCode::W);
            move.bindings.PushBack(wasd);
            Binding stick;
            stick.source = BindingSource::GamepadStick;
            stick.code = static_cast<u32>(StickCode::Left);
            stick.deadZone = 0.2f;
            move.bindings.PushBack(stick);
            gameplay.actions.PushBack(static_cast<Action&&>(move));
        }
        map.sets.PushBack(static_cast<ActionSet&&>(gameplay));

        ActionSet menu;
        menu.name = String(u8"Menu");
        menu.priority = 10;
        {
            Action confirm;
            confirm.name = String(u8"Confirm");
            confirm.kind = ActionKind::Button;
            Binding key;
            key.source = BindingSource::Key;
            key.code = static_cast<u32>(shell::KeyCode::Space); // shared with Jump on purpose
            confirm.bindings.PushBack(key);
            menu.actions.PushBack(static_cast<Action&&>(confirm));
        }
        map.sets.PushBack(static_cast<ActionSet&&>(menu));
        return map;
    }
}

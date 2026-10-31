// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell - reflection implementation unit: the input code enums.
//
// An input map stores key, button and axis codes as numbers, and a scripted playtest names them;
// reflecting the cases lets type_info and the tools name them from the one enum. Every case below
// Count is listed; ShellReflectionTests holds the list to Count.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.shell;

import foundation.core;

using namespace foundation::core;

namespace foundation::shell
{
    REFLECT_ENUM(KeyCode, "rtti::shell")
    {
        builder.Value("Unknown", KeyCode::Unknown);
        builder.Value("A", KeyCode::A);
        builder.Value("B", KeyCode::B);
        builder.Value("C", KeyCode::C);
        builder.Value("D", KeyCode::D);
        builder.Value("E", KeyCode::E);
        builder.Value("F", KeyCode::F);
        builder.Value("G", KeyCode::G);
        builder.Value("H", KeyCode::H);
        builder.Value("I", KeyCode::I);
        builder.Value("J", KeyCode::J);
        builder.Value("K", KeyCode::K);
        builder.Value("L", KeyCode::L);
        builder.Value("M", KeyCode::M);
        builder.Value("N", KeyCode::N);
        builder.Value("O", KeyCode::O);
        builder.Value("P", KeyCode::P);
        builder.Value("Q", KeyCode::Q);
        builder.Value("R", KeyCode::R);
        builder.Value("S", KeyCode::S);
        builder.Value("T", KeyCode::T);
        builder.Value("U", KeyCode::U);
        builder.Value("V", KeyCode::V);
        builder.Value("W", KeyCode::W);
        builder.Value("X", KeyCode::X);
        builder.Value("Y", KeyCode::Y);
        builder.Value("Z", KeyCode::Z);
        builder.Value("Num0", KeyCode::Num0);
        builder.Value("Num1", KeyCode::Num1);
        builder.Value("Num2", KeyCode::Num2);
        builder.Value("Num3", KeyCode::Num3);
        builder.Value("Num4", KeyCode::Num4);
        builder.Value("Num5", KeyCode::Num5);
        builder.Value("Num6", KeyCode::Num6);
        builder.Value("Num7", KeyCode::Num7);
        builder.Value("Num8", KeyCode::Num8);
        builder.Value("Num9", KeyCode::Num9);
        builder.Value("F1", KeyCode::F1);
        builder.Value("F2", KeyCode::F2);
        builder.Value("F3", KeyCode::F3);
        builder.Value("F4", KeyCode::F4);
        builder.Value("F5", KeyCode::F5);
        builder.Value("F6", KeyCode::F6);
        builder.Value("F7", KeyCode::F7);
        builder.Value("F8", KeyCode::F8);
        builder.Value("F9", KeyCode::F9);
        builder.Value("F10", KeyCode::F10);
        builder.Value("F11", KeyCode::F11);
        builder.Value("F12", KeyCode::F12);
        builder.Value("F13", KeyCode::F13);
        builder.Value("F14", KeyCode::F14);
        builder.Value("F15", KeyCode::F15);
        builder.Value("F16", KeyCode::F16);
        builder.Value("F17", KeyCode::F17);
        builder.Value("F18", KeyCode::F18);
        builder.Value("F19", KeyCode::F19);
        builder.Value("F20", KeyCode::F20);
        builder.Value("F21", KeyCode::F21);
        builder.Value("F22", KeyCode::F22);
        builder.Value("F23", KeyCode::F23);
        builder.Value("F24", KeyCode::F24);
        builder.Value("Return", KeyCode::Return);
        builder.Value("Escape", KeyCode::Escape);
        builder.Value("Backspace", KeyCode::Backspace);
        builder.Value("Tab", KeyCode::Tab);
        builder.Value("Space", KeyCode::Space);
        builder.Value("Minus", KeyCode::Minus);
        builder.Value("Equals", KeyCode::Equals);
        builder.Value("LeftBracket", KeyCode::LeftBracket);
        builder.Value("RightBracket", KeyCode::RightBracket);
        builder.Value("Backslash", KeyCode::Backslash);
        builder.Value("Semicolon", KeyCode::Semicolon);
        builder.Value("Apostrophe", KeyCode::Apostrophe);
        builder.Value("Grave", KeyCode::Grave);
        builder.Value("Comma", KeyCode::Comma);
        builder.Value("Period", KeyCode::Period);
        builder.Value("Slash", KeyCode::Slash);
        builder.Value("CapsLock", KeyCode::CapsLock);
        builder.Value("ScrollLock", KeyCode::ScrollLock);
        builder.Value("NumLock", KeyCode::NumLock);
        builder.Value("PrintScreen", KeyCode::PrintScreen);
        builder.Value("Pause", KeyCode::Pause);
        builder.Value("Insert", KeyCode::Insert);
        builder.Value("Home", KeyCode::Home);
        builder.Value("PageUp", KeyCode::PageUp);
        builder.Value("Delete", KeyCode::Delete);
        builder.Value("End", KeyCode::End);
        builder.Value("PageDown", KeyCode::PageDown);
        builder.Value("Right", KeyCode::Right);
        builder.Value("Left", KeyCode::Left);
        builder.Value("Down", KeyCode::Down);
        builder.Value("Up", KeyCode::Up);
        builder.Value("Keypad0", KeyCode::Keypad0);
        builder.Value("Keypad1", KeyCode::Keypad1);
        builder.Value("Keypad2", KeyCode::Keypad2);
        builder.Value("Keypad3", KeyCode::Keypad3);
        builder.Value("Keypad4", KeyCode::Keypad4);
        builder.Value("Keypad5", KeyCode::Keypad5);
        builder.Value("Keypad6", KeyCode::Keypad6);
        builder.Value("Keypad7", KeyCode::Keypad7);
        builder.Value("Keypad8", KeyCode::Keypad8);
        builder.Value("Keypad9", KeyCode::Keypad9);
        builder.Value("KeypadDivide", KeyCode::KeypadDivide);
        builder.Value("KeypadMultiply", KeyCode::KeypadMultiply);
        builder.Value("KeypadMinus", KeyCode::KeypadMinus);
        builder.Value("KeypadPlus", KeyCode::KeypadPlus);
        builder.Value("KeypadEnter", KeyCode::KeypadEnter);
        builder.Value("KeypadDecimal", KeyCode::KeypadDecimal);
        builder.Value("LeftCtrl", KeyCode::LeftCtrl);
        builder.Value("LeftShift", KeyCode::LeftShift);
        builder.Value("LeftAlt", KeyCode::LeftAlt);
        builder.Value("LeftGui", KeyCode::LeftGui);
        builder.Value("RightCtrl", KeyCode::RightCtrl);
        builder.Value("RightShift", KeyCode::RightShift);
        builder.Value("RightAlt", KeyCode::RightAlt);
        builder.Value("RightGui", KeyCode::RightGui);
        builder.Value("Menu", KeyCode::Menu);
    }

    REFLECT_ENUM(MouseButton, "rtti::shell")
    {
        builder.Value("Left", MouseButton::Left);
        builder.Value("Middle", MouseButton::Middle);
        builder.Value("Right", MouseButton::Right);
        builder.Value("X1", MouseButton::X1);
        builder.Value("X2", MouseButton::X2);
    }

    REFLECT_ENUM(GamepadButton, "rtti::shell")
    {
        builder.Value("South", GamepadButton::South);
        builder.Value("East", GamepadButton::East);
        builder.Value("West", GamepadButton::West);
        builder.Value("North", GamepadButton::North);
        builder.Value("LeftShoulder", GamepadButton::LeftShoulder);
        builder.Value("RightShoulder", GamepadButton::RightShoulder);
        builder.Value("LeftStick", GamepadButton::LeftStick);
        builder.Value("RightStick", GamepadButton::RightStick);
        builder.Value("DPadUp", GamepadButton::DPadUp);
        builder.Value("DPadDown", GamepadButton::DPadDown);
        builder.Value("DPadLeft", GamepadButton::DPadLeft);
        builder.Value("DPadRight", GamepadButton::DPadRight);
        builder.Value("Back", GamepadButton::Back);
        builder.Value("Guide", GamepadButton::Guide);
        builder.Value("Start", GamepadButton::Start);
        builder.Value("LeftPaddle1", GamepadButton::LeftPaddle1);
        builder.Value("LeftPaddle2", GamepadButton::LeftPaddle2);
        builder.Value("RightPaddle1", GamepadButton::RightPaddle1);
        builder.Value("RightPaddle2", GamepadButton::RightPaddle2);
        builder.Value("Touchpad", GamepadButton::Touchpad);
        builder.Value("Misc1", GamepadButton::Misc1);
    }

    REFLECT_ENUM(GamepadAxis, "rtti::shell")
    {
        builder.Value("LeftX", GamepadAxis::LeftX);
        builder.Value("LeftY", GamepadAxis::LeftY);
        builder.Value("RightX", GamepadAxis::RightX);
        builder.Value("RightY", GamepadAxis::RightY);
        builder.Value("LeftTrigger", GamepadAxis::LeftTrigger);
        builder.Value("RightTrigger", GamepadAxis::RightTrigger);
    }

    void RegisterShellInputReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_KeyCode();
            GlobalTypeRegistry().Register(TypeOf<KeyCode>()); // type_info finds it by name
            RttiRegisterEnum_MouseButton();
            GlobalTypeRegistry().Register(TypeOf<MouseButton>()); // type_info finds it by name
            RttiRegisterEnum_GamepadButton();
            GlobalTypeRegistry().Register(TypeOf<GamepadButton>()); // type_info finds it by name
            RttiRegisterEnum_GamepadAxis();
            GlobalTypeRegistry().Register(TypeOf<GamepadAxis>()); // type_info finds it by name
            return true;
        }();
        (void)once;
    }
}

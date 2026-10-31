// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.input tests: bindings and enums as human labels (the :binding_names helpers, which
// runtime UI and the editor share).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.input;

using namespace foundation::core;
using namespace foundation::input;
namespace shell = foundation::shell;

TEST_CASE("binding names: KeyName maps letters/digits/function/named keys, else Key#<code>")
{
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::A)) == StringView(u8"A"));
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::Z)) == StringView(u8"Z"));
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::Num0)) == StringView(u8"0"));
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::F1)) == StringView(u8"F1"));
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::Return)) == StringView(u8"Return"));
    CHECK(KeyName(static_cast<u32>(shell::KeyCode::Space)) == StringView(u8"Space"));
}

TEST_CASE("binding names: PadButtonName maps the face + dpad + shoulder buttons")
{
    CHECK(PadButtonName(static_cast<u32>(shell::GamepadButton::South)) == StringView(u8"Pad South"));
    CHECK(PadButtonName(static_cast<u32>(shell::GamepadButton::DPadUp)) == StringView(u8"DPad Up"));
    CHECK(PadButtonName(static_cast<u32>(shell::GamepadButton::LeftShoulder)) ==
          StringView(u8"Pad LB"));
}

// Every button its own name (a prompt's sheet draws a glyph per name), the stick presses
// included, so Sprint on the left stick is told apart from any other.
TEST_CASE("binding names: every pad button has a name of its own")
{
    CHECK(PadButtonName(static_cast<u32>(shell::GamepadButton::LeftStick)) == StringView(u8"Pad LS"));
    for (u32 a = 0; a < static_cast<u32>(shell::GamepadButton::Count); ++a)
    {
        CAPTURE(a);
        CHECK(PadButtonName(a) != StringView(u8"Pad Button"));
        for (u32 b = a + 1; b < static_cast<u32>(shell::GamepadButton::Count); ++b)
            CHECK(PadButtonName(a) != PadButtonName(b));
    }
}

TEST_CASE("binding names: DescribeBinding dispatches on the binding's source")
{
    Binding key;
    key.source = BindingSource::Key;
    key.code = static_cast<u32>(shell::KeyCode::E);
    CHECK(DescribeBinding(key) == StringView(u8"E"));

    Binding pad;
    pad.source = BindingSource::GamepadButton;
    pad.code = static_cast<u32>(shell::GamepadButton::South);
    CHECK(DescribeBinding(pad) == StringView(u8"Pad South"));

    Binding mouse;
    mouse.source = BindingSource::MouseButton;
    mouse.code = static_cast<u32>(shell::MouseButton::Left);
    CHECK(DescribeBinding(mouse) == StringView(u8"Mouse Left"));

    Binding wasd;
    wasd.source = BindingSource::Composite2D;
    wasd.negX = static_cast<u32>(shell::KeyCode::A);
    wasd.posX = static_cast<u32>(shell::KeyCode::D);
    wasd.negY = static_cast<u32>(shell::KeyCode::S);
    wasd.posY = static_cast<u32>(shell::KeyCode::W);
    CHECK(DescribeBinding(wasd) == StringView(u8"W A S D")); // up, left, down, right, as a player reads them
}

TEST_CASE("binding names: SourceName / KindName / InteractionName")
{
    CHECK(SourceName(BindingSource::Key) == StringView(u8"Key"));
    CHECK(SourceName(BindingSource::GamepadButton) == StringView(u8"PadBtn"));
    CHECK(KindName(ActionKind::Button) == StringView(u8"Button"));
    CHECK(KindName(ActionKind::Axis2D) == StringView(u8"Axis2D"));
    CHECK(InteractionName(InteractionKind::None) == StringView(u8"On Press"));
    CHECK(InteractionName(InteractionKind::Hold) == StringView(u8"Hold"));
}

TEST_CASE("binding names: ValidSources lists the accepted sources per action kind")
{
    BindingSource out[8];
    const usize buttons = ValidSources(ActionKind::Button, out);
    REQUIRE(buttons == 4u);
    CHECK(out[0] == BindingSource::Key);
    CHECK(out[2] == BindingSource::GamepadButton);

    const usize axis2d = ValidSources(ActionKind::Axis2D, out);
    REQUIRE(axis2d == 4u);
    CHECK(out[0] == BindingSource::GamepadStick);
    CHECK(out[1] == BindingSource::Composite2D);
}

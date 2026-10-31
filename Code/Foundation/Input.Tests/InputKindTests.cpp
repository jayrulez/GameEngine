// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.input tests: the input kind in use (the pointer, the keyboard or a pad), from
// deliberate input only, with a change told to listeners.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.input;

using namespace foundation::core;
using namespace foundation::input;
namespace shell = foundation::shell;

#include "InputTestSupport.h"

namespace
{
    // One frame's devices: each test sets this frame's edges, then the tracker reads them.
    struct KindBed
    {
        FakeDevices devices;
        FakeGamepad pad;
        InputKindTracker tracker;

        KindBed() { devices.pads.PushBack(&pad); }
        // The edges of the last frame end, as the shell's pump ends them.
        void NextFrame()
        {
            for (bool& p : devices.keyboard.pressed)
                p = false;
            for (bool& p : devices.mouse.pressed)
                p = false;
            for (bool& p : pad.buttonsPressed)
                p = false;
            devices.mouse.dx = 0.0f;
            devices.mouse.dy = 0.0f;
            devices.mouse.wheel = 0.0f;
        }
        InputKind Frame()
        {
            tracker.Update(devices);
            NextFrame();
            return tracker.Kind();
        }
        void Press(shell::KeyCode key) { devices.keyboard.pressed[static_cast<u32>(key)] = true; }
        void PressPad(shell::GamepadButton button) { pad.buttonsPressed[static_cast<u32>(button)] = true; }
        void Click() { devices.mouse.pressed[static_cast<u32>(shell::MouseButton::Left)] = true; }
    };
}

TEST_CASE("input kind: starts as the pointer and follows a key, a pad button and a click")
{
    KindBed bed;
    CHECK(bed.tracker.Kind() == InputKind::Pointer);
    bed.Press(shell::KeyCode::A);
    CHECK(bed.Frame() == InputKind::Keyboard);
    bed.PressPad(shell::GamepadButton::South);
    CHECK(bed.Frame() == InputKind::Pad);
    CHECK(bed.Frame() == InputKind::Pad); // nothing new: it stays
    bed.Click();
    CHECK(bed.Frame() == InputKind::Pointer);
}

TEST_CASE("input kind: the mouse counts past a few pixels or a wheel turn, not a nudge")
{
    KindBed bed;
    bed.tracker.SetKind(InputKind::Pad);
    bed.devices.mouse.dx = 1.0f;
    bed.devices.mouse.dy = 1.0f;
    CHECK(bed.Frame() == InputKind::Pad);
    bed.devices.mouse.dx = 6.0f;
    CHECK(bed.Frame() == InputKind::Pointer);
    bed.tracker.SetKind(InputKind::Keyboard);
    bed.devices.mouse.wheel = 1.0f;
    CHECK(bed.Frame() == InputKind::Pointer);
}

TEST_CASE("input kind: a stick counts when pushed past the press point, not while it rests or stays held there")
{
    KindBed bed;
    bed.pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.2f; // drift
    CHECK(bed.Frame() == InputKind::Pointer);
    bed.pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.9f; // pushed
    CHECK(bed.Frame() == InputKind::Pad);
    bed.Click();
    CHECK(bed.Frame() == InputKind::Pointer);
    CHECK(bed.Frame() == InputKind::Pointer); // still held past it: not a fresh push
    bed.pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.0f;
    CHECK(bed.Frame() == InputKind::Pointer);
    bed.pad.axes[static_cast<u32>(shell::GamepadAxis::RightTrigger)] = 1.0f; // a trigger too
    CHECK(bed.Frame() == InputKind::Pad);
}

TEST_CASE("input kind: in one frame a press outranks motion, a pad the keyboard, the keyboard the mouse")
{
    KindBed bed;
    bed.Press(shell::KeyCode::W);
    bed.devices.mouse.dx = 40.0f;
    CHECK(bed.Frame() == InputKind::Keyboard);
    bed.Press(shell::KeyCode::W);
    bed.PressPad(shell::GamepadButton::East);
    CHECK(bed.Frame() == InputKind::Pad);
    bed.Click();
    bed.Press(shell::KeyCode::Space);
    CHECK(bed.Frame() == InputKind::Keyboard);
}

TEST_CASE("input kind: a finger put down is the pointer; one held is not again")
{
    KindBed bed;
    bed.tracker.SetKind(InputKind::Pad);
    bed.devices.touch.Set(1, 0.5f, 0.5f);
    CHECK(bed.Frame() == InputKind::Pointer);
    bed.tracker.SetKind(InputKind::Pad);
    CHECK(bed.Frame() == InputKind::Pad); // still down: nothing new
}

TEST_CASE("input kind: listeners hear each change once, and one may leave from inside")
{
    KindBed bed;
    Array<InputKind> heard;
    u32 token = 0;
    token = bed.tracker.AddListener([&](InputKind kind)
                                    {
                                        heard.PushBack(kind);
                                        if (kind == InputKind::Pointer)
                                            CHECK(bed.tracker.RemoveListener(token));
                                    });
    bed.PressPad(shell::GamepadButton::South);
    (void)bed.Frame();
    bed.PressPad(shell::GamepadButton::South);
    (void)bed.Frame(); // the same kind: no change
    bed.Click();
    (void)bed.Frame();
    bed.Press(shell::KeyCode::A);
    (void)bed.Frame(); // it left
    REQUIRE(heard.Size() == 2);
    CHECK(heard[0] == InputKind::Pad);
    CHECK(heard[1] == InputKind::Pointer);
    CHECK_FALSE(bed.tracker.RemoveListener(token));
    CHECK(InputKindName(InputKind::Pad) == StringView(u8"pad"));
    CHECK(InputKindName(InputKind::Keyboard) == StringView(u8"keyboard"));
    CHECK(InputKindName(InputKind::Pointer) == StringView(u8"pointer"));
}

TEST_CASE("input kind: a listener after one that leaves from inside still hears that change")
{
    KindBed bed;
    u32 leaver = 0;
    leaver = bed.tracker.AddListener([&](InputKind) { CHECK(bed.tracker.RemoveListener(leaver)); });
    Array<InputKind> heard;
    (void)bed.tracker.AddListener([&](InputKind kind) { heard.PushBack(kind); });
    bed.PressPad(shell::GamepadButton::South);
    (void)bed.Frame();
    REQUIRE(heard.Size() == 1);
    CHECK(heard[0] == InputKind::Pad);
    bed.Click();
    (void)bed.Frame();
    REQUIRE(heard.Size() == 2); // the one that left is gone, the other stays
    CHECK(heard[1] == InputKind::Pointer);
}

TEST_CASE("input kind: a disconnected pad counts for nothing")
{
    KindBed bed;
    bed.pad.connected = false;
    bed.PressPad(shell::GamepadButton::South);
    CHECK(bed.Frame() == InputKind::Pointer);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Input tests - the engine-level input surface: the InputSubsystem (per-surface scene
// binding, source overrides) and the per-context Input facade.
//
// These exercise engine.input and the script backend, so they live in the Engine collection
// (Foundation must not link Engine). The pure foundation action-model, rebind, interaction and
// reflection tests stay in Foundation.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.input;
import foundation.settings;
import foundation.script;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif
import engine.input;

using namespace foundation::core;
using namespace engine::input;
using namespace foundation::input;
namespace shell = foundation::shell;

#include "InputTestSupport.h"

#if defined(OPTION_HAS_ANGELSCRIPT) || defined(OPTION_HAS_LUAU)
namespace
{
    // The per-context Input-facade drive, parameterized by the backend's manager and its read and
    // push scripts. Shared so AngelScript and Luau show the same per-context service resolution.
    void DrivePerContextInput(RefPtr<foundation::script::IScriptManager> manager,
                              StringView readScript, StringView pushScript)
    {
        REQUIRE(manager.Get() != nullptr);
        RegisterCoreTypes(); // Float2, which value2D returns
        engine::input::RegisterInputScriptFacade();

        // Two runtimes, two contexts: each script reads its own bound runtime (several players, or
        // the editor and the game). No process globals anywhere.
        ActionRuntime runtimeA;
        runtimeA.SetMap(MakeGameplayMap());
        runtimeA.DisableSet(u8"Menu");
        ActionRuntime runtimeB;
        runtimeB.SetMap(MakeGameplayMap());
        runtimeB.DisableSet(u8"Menu");

        FakeDevices devices;
        devices.keyboard.Set(shell::KeyCode::Space, true);
        devices.keyboard.Set(shell::KeyCode::W, true);
        runtimeA.Update(devices, 1.0f / 60.0f); // A sees the press...
        FakeDevices idle;
        runtimeB.Update(idle, 1.0f / 60.0f); // ...B sees nothing

        foundation::script::RegisterReflectedTypes(*manager);

        RefPtr<foundation::script::IScriptContext> ctxA = manager->CreateContext();
        RefPtr<foundation::script::IScriptContext> ctxB = manager->CreateContext();
        RefPtr<foundation::script::IScriptContext> ctxNone = manager->CreateContext();
        REQUIRE(ctxA.Get() != nullptr);
        ctxA->SetService(foundation::input::kInputScriptService, &runtimeA);
        ctxB->SetService(foundation::input::kInputScriptService, &runtimeB);

        REQUIRE(ctxA->Load(readScript, u8"main").IsOk());
        CHECK(ctxA->GetGlobal(u8"Down").Get<bool>() == true);
        CHECK(ctxA->GetGlobal(u8"MoveY").Get<f64>() == doctest::Approx(1.0));
        CHECK(ctxA->GetGlobal(u8"Move2DY").Get<f64>() == doctest::Approx(1.0));
        CHECK(ctxA->GetGlobal(u8"Move2DX").Get<f64>() == doctest::Approx(0.0));

        REQUIRE(ctxB->Load(readScript, u8"main").IsOk());
        CHECK(ctxB->GetGlobal(u8"Down").Get<bool>() == false); // B's runtime saw nothing
        CHECK(ctxB->GetGlobal(u8"MoveY").Get<f64>() == doctest::Approx(0.0));

        // No service bound: released, never a crash.
        REQUIRE(ctxNone->Load(readScript, u8"main").IsOk());
        CHECK(ctxNone->GetGlobal(u8"Down").Get<bool>() == false);

        // A script-driven exclusive push lands on the context's runtime only.
        runtimeA.EnableSet(u8"Menu");
        REQUIRE(ctxA->Load(pushScript, u8"main").IsOk());
        CHECK(runtimeA.ExclusiveDepth() == 1);
        CHECK(runtimeB.ExclusiveDepth() == 0);
    }
}
#endif

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("input: the Input facade resolves PER-CONTEXT services (AngelScript)")
{
    DrivePerContextInput(foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator()),
                         u8"bool Down; double MoveY; double Move2DX; double Move2DY;\n"
                         u8"void main() {\n"
                         u8"  Down = Input::isDown(\"Jump\");\n"
                         u8"  MoveY = Input::valueY(\"Move\");\n"
                         u8"  Float2 move = Input::value2D(\"Move\");\n"
                         u8"  Move2DX = move.x;\n"
                         u8"  Move2DY = move.y;\n"
                         u8"}\n",
                         u8"void main() { Input::pushSet(\"Menu\"); }\n");
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("input: a script rumbles its own context's pad, and stops it")
{
    RefPtr<foundation::script::IScriptManager> manager =
        foundation::script::angelscript::CreateScriptManager(DefaultAllocator());
    engine::input::RegisterInputScriptFacade();
    foundation::script::RegisterReflectedTypes(*manager);

    ActionRuntime runtimeA;
    ActionRuntime runtimeB;
    FakeGamepad padA;
    FakeGamepad padB;
    FakeDevices devicesA;
    FakeDevices devicesB;
    devicesA.pads.PushBack(&padA);
    devicesB.pads.PushBack(&padB);
    RefPtr<foundation::script::IScriptContext> ctxA = manager->CreateContext();
    RefPtr<foundation::script::IScriptContext> ctxNone = manager->CreateContext();
    ctxA->SetService(foundation::input::kInputScriptService, &runtimeA);

    REQUIRE(ctxA->Load(u8"void main() { Input::rumble(0.6f, 0.3f, 0.2f); }\n", u8"main").IsOk());
    runtimeA.Update(devicesA, 1.0f / 60.0f);
    runtimeB.Update(devicesB, 1.0f / 60.0f);
    CHECK(padA.rumbleLow == doctest::Approx(0.6f));
    CHECK(padA.rumbleHigh == doctest::Approx(0.3f));
    CHECK(padA.rumbleMs == 200u);
    CHECK(padB.rumbleCalls == 0); // another run's pad is not touched

    // By pad index, then stopped.
    REQUIRE(ctxA->Load(u8"void main() { Input::rumble(0, 1.0f, 0.0f, 0.5f); }\n", u8"main").IsOk());
    runtimeA.Update(devicesA, 1.0f / 60.0f);
    CHECK(padA.rumbleLow == doctest::Approx(1.0f));
    REQUIRE(ctxA->Load(u8"void main() { Input::stopRumble(); }\n", u8"main").IsOk());
    runtimeA.Update(devicesA, 1.0f / 60.0f);
    CHECK(padA.rumbleLow == doctest::Approx(0.0f));
    CHECK(padA.rumbleMs == 0u);

    // No input service: a no-op, never a fault.
    REQUIRE(ctxNone->Load(u8"void main() { Input::rumble(1.0f, 1.0f, 1.0f); }\n", u8"main").IsOk());
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("input: the Input facade resolves PER-CONTEXT services (Luau)")
{
    DrivePerContextInput(foundation::script::CreateLuauScriptManager(DefaultAllocator()),
                         u8"Down = Input.isDown(\"Jump\")\n"
                         u8"MoveY = Input.valueY(\"Move\")\n"
                         u8"local move = Input.value2D(\"Move\")\n"
                         u8"Move2DX = move.x\n"
                         u8"Move2DY = move.y\n",
                         u8"Input.pushSet(\"Menu\")\n");
}
#endif // OPTION_HAS_LUAU

TEST_CASE("input.subsystem: the per-surface scene binding rides the source override")
{
    // SetSourceProvider carries the scene the source represents (an opaque key, the
    // SceneOverlayView::sceneKey convention); the UI pump confines routing and consumption to it.
    // Unbound sources follow the policy setting.
    InputSubsystem input(nullptr);
    CHECK(input.BoundSceneKey() == nullptr);
    CHECK(input.UnboundScenePolicy() == UnboundInputScenePolicy::AllScenes); // player default

    FakeDevices devices;
    int sceneStandIn = 0; // any stable address works as a key
    input.SetSourceProvider(&devices, &sceneStandIn);
    CHECK(input.BoundSceneKey() == &sceneStandIn);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&devices));

    // Setting again without a scene unbinds (the Game tab's Stop: the provider stays, the run's
    // binding drops).
    input.SetSourceProvider(&devices, nullptr);
    CHECK(input.BoundSceneKey() == nullptr);

    // Clearing the provider always clears the binding; the shell source is unbound.
    input.SetSourceProvider(&devices, &sceneStandIn);
    input.SetSourceProvider(nullptr, &sceneStandIn);
    CHECK(input.BoundSceneKey() == nullptr);

    input.SetUnboundScenePolicy(UnboundInputScenePolicy::ScreenTierOnly);
    CHECK(input.UnboundScenePolicy() == UnboundInputScenePolicy::ScreenTierOnly);
}

TEST_CASE("input.subsystem: ClearSourceProviderIf drops only its own dangling override")
{
    // A closing editor Game tab clears its viewport source before it is destroyed, so
    // ActiveSource()/Update never dereference freed memory. The guard makes it a no-op when a
    // different still-open tab is the active source.
    InputSubsystem input(nullptr);
    FakeDevices tabA;
    FakeDevices tabB;
    int keyA = 0;

    input.SetSourceProvider(&tabA, &keyA);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&tabA));

    // Closing a non-active tab must not disturb the active override.
    input.ClearSourceProviderIf(&tabB);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&tabA));
    CHECK(input.BoundSceneKey() == &keyA);

    // Closing the active tab clears the override and its binding; ActiveSource falls back to the
    // (always valid) shell source, so the next pump cannot dangle.
    input.ClearSourceProviderIf(&tabA);
    CHECK(&input.ActiveSource() != static_cast<IInputSourceProvider*>(&tabA));
    CHECK(input.BoundSceneKey() == nullptr);

    // Idempotent once cleared.
    input.ClearSourceProviderIf(&tabA);
    CHECK(&input.ActiveSource() != static_cast<IInputSourceProvider*>(&tabA));
}

namespace
{
    // A mouse at a set window position with a set motion, and a provider over it.
    struct PointMouse final : foundation::shell::IMouse
    {
        f32 x = 0.0f, y = 0.0f, dx = 0.0f, dy = 0.0f;
        bool left = false;
        [[nodiscard]] f32 X() const override { return x; }
        [[nodiscard]] f32 Y() const override { return y; }
        [[nodiscard]] f32 GlobalX() const override { return x; }
        [[nodiscard]] f32 GlobalY() const override { return y; }
        [[nodiscard]] f32 DeltaX() const override { return dx; }
        [[nodiscard]] f32 DeltaY() const override { return dy; }
        [[nodiscard]] f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] f32 ScrollY() const override { return 0.0f; }
        [[nodiscard]] bool IsButtonDown(foundation::shell::MouseButton) const override { return left; }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(foundation::shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };
    // Fingers set by the test, as a shell reports them.
    struct PointTouch final : foundation::shell::ITouch
    {
        Array<foundation::shell::TouchPoint> points;
        [[nodiscard]] i32 TouchCount() const override { return static_cast<i32>(points.Size()); }
        [[nodiscard]] bool GetTouchPoint(i32 index, foundation::shell::TouchPoint& out) const override
        {
            if (index < 0 || static_cast<usize>(index) >= points.Size())
            {
                return false;
            }
            out = points[static_cast<usize>(index)];
            return true;
        }
        [[nodiscard]] bool HasTouch() const override { return !points.IsEmpty(); }
    };
    struct PointSource final : foundation::input::IInputSourceProvider
    {
        PointMouse mouse;
        PointTouch touch;
        bool hasMouse = true;
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override { return nullptr; }
        [[nodiscard]] foundation::shell::IMouse* Mouse() override { return hasMouse ? &mouse : nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] foundation::shell::ITouch* Touch() override { return &touch; }
    };
}

TEST_CASE("input: a fitted source reads the pointer in render pixels, through the bars too")
{
    // A 320x180 game letterboxed into a 1280x1080 window (scale 4, bars of 180 above and below):
    // the window's centre is the render's, a bar maps outside it rather than vanishing, motion
    // scales to render pixels, and the buttons pass through.
    PointSource inner;
    FittedInputSource fitted(inner);
    fitted.fit = ContentFit{Rectangle{0, 0, 1280, 1080}, Float2{320, 180}, FitMode::Letterbox};
    foundation::shell::IMouse* mouse = fitted.Mouse();
    REQUIRE(mouse != nullptr);

    inner.mouse.x = 640.0f;
    inner.mouse.y = 540.0f;
    CHECK(mouse->X() == doctest::Approx(160.0f));
    CHECK(mouse->Y() == doctest::Approx(90.0f));
    inner.mouse.y = 90.0f; // in the top bar
    CHECK(mouse->Y() < 0.0f);
    inner.mouse.dx = 8.0f;
    CHECK(mouse->DeltaX() == doctest::Approx(2.0f));
    inner.mouse.left = true;
    CHECK(mouse->IsButtonDown(foundation::shell::MouseButton::Left));

    inner.hasMouse = false; // no mouse below, none here
    CHECK(fitted.Mouse() == nullptr);
}

TEST_CASE("input: a fitted source maps the fingers as it maps the mouse")
{
    // The same letterboxed 320x180 game: a finger at the window's centre is the render's centre,
    // in render pixels for the UI and normalized over the image for the touch regions; one on a
    // bar falls outside the image.
    PointSource inner;
    FittedInputSource fitted(inner);
    fitted.fit = ContentFit{Rectangle{0, 0, 1280, 1080}, Float2{320, 180}, FitMode::Letterbox};
    foundation::shell::ITouch* touch = fitted.Touch();
    REQUIRE(touch != nullptr);
    inner.touch.points.PushBack(foundation::shell::TouchPoint{7, 0.5f, 0.5f, 1.0f, 640.0f, 540.0f});
    REQUIRE(touch->TouchCount() == 1);
    foundation::shell::TouchPoint point;
    REQUIRE(touch->GetTouchPoint(0, point));
    CHECK(point.id == 7u);
    CHECK(point.pointerX == doctest::Approx(160.0f));
    CHECK(point.pointerY == doctest::Approx(90.0f));
    CHECK(point.x == doctest::Approx(0.5f));
    CHECK(point.y == doctest::Approx(0.5f));
    inner.touch.points[0] = foundation::shell::TouchPoint{7, 0.5f, 90.0f / 1080.0f, 1.0f, 640.0f, 90.0f};
    REQUIRE(touch->GetTouchPoint(0, point));
    CHECK(point.y < 0.0f);
}

TEST_CASE("input: a fitted source turned with the present reads the game's frame")
{
    // A landscape game on a 1080x1920 portrait window, presented turned three quarters (its top on
    // the screen's left), at the window's pixels turned (1920x1080, as the host fits it without a
    // render resolution).
    PointSource inner;
    FittedInputSource fitted(inner);
    fitted.fit = ContentFit{Rectangle{0, 0, 1080, 1920}, Float2{1920, 1080}, FitMode::Stretch,
                            ContentRotation::ThreeQuarter};
    foundation::shell::IMouse* mouse = fitted.Mouse();
    REQUIRE(mouse != nullptr);

    // The window's corners are the game's: bottom-left its top-left, top-left its top-right,
    // top-right its bottom-right.
    const struct
    {
        f32 windowX, windowY, gameX, gameY;
    } corners[] = {{0, 1920, 0, 0}, {0, 0, 1920, 0}, {1080, 0, 1920, 1080}, {1080, 1920, 0, 1080}};
    for (const auto& corner : corners)
    {
        CAPTURE(corner.windowX);
        CAPTURE(corner.windowY);
        inner.mouse.x = corner.windowX;
        inner.mouse.y = corner.windowY;
        CHECK(mouse->X() == doctest::Approx(corner.gameX));
        CHECK(mouse->Y() == doctest::Approx(corner.gameY));
    }

    // A drag turns: up the window is right across the game, right across the window is down it.
    inner.mouse.dx = 0.0f;
    inner.mouse.dy = -10.0f;
    CHECK(mouse->DeltaX() == doctest::Approx(10.0f));
    CHECK(mouse->DeltaY() == doctest::Approx(0.0f));
    inner.mouse.dx = 10.0f;
    inner.mouse.dy = 0.0f;
    CHECK(mouse->DeltaX() == doctest::Approx(0.0f));
    CHECK(mouse->DeltaY() == doctest::Approx(10.0f));

    // A finger near the window's bottom-left is near the game's top-left, in its pixels and
    // normalized over it (where its touch regions are).
    foundation::shell::ITouch* touch = fitted.Touch();
    REQUIRE(touch != nullptr);
    inner.touch.points.PushBack(
        foundation::shell::TouchPoint{0, 100.0f / 1080.0f, 1820.0f / 1920.0f, 1.0f, 100.0f, 1820.0f});
    foundation::shell::TouchPoint point;
    REQUIRE(touch->GetTouchPoint(0, point));
    CHECK(point.pointerX == doctest::Approx(100.0f));
    CHECK(point.pointerY == doctest::Approx(100.0f));
    CHECK(point.x == doctest::Approx(100.0f / 1920.0f));
    CHECK(point.y == doctest::Approx(100.0f / 1080.0f));

    // A render resolution letterboxed in the turn: 300x180 at scale 6 leaves bars of 60 along the
    // window's top and bottom (the game's sides). A finger in the top bar is past the game's right
    // edge, outside it rather than gone.
    fitted.fit = ContentFit{Rectangle{0, 0, 1080, 1920}, Float2{300, 180}, FitMode::Letterbox,
                            ContentRotation::ThreeQuarter};
    inner.touch.points[0] = foundation::shell::TouchPoint{0, 0.5f, 30.0f / 1920.0f, 1.0f, 540.0f, 30.0f};
    REQUIRE(touch->GetTouchPoint(0, point));
    CHECK(point.x > 1.0f);
    CHECK(point.y == doctest::Approx(0.5f));
    inner.mouse.x = 540.0f;
    inner.mouse.y = 960.0f;
    CHECK(mouse->X() == doctest::Approx(150.0f)); // the window's centre is the game's
    CHECK(mouse->Y() == doctest::Approx(90.0f));
}

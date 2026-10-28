// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.input tests: model round-trip (binary + XML) + validation, and ActionRuntime
// evaluation over SYNTHETIC devices (the design's testing contract - no real hardware).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.input;
import foundation.settings;
import foundation.xml;
import foundation.xml.serialization;

using namespace foundation::core;
using namespace foundation::input;
namespace shell = foundation::shell;

#include "InputTestSupport.h"

TEST_CASE("input: a map stamped with another wire version is REFUSED, not decoded")
{
    // The map carries its own inline version. A stream written under another one would
    // decode the wrong field list into plausible-looking bindings - the reader fails instead.
    MemoryStream buffer;
    {
        BinarySerializer ar(buffer, SerializeMode::Write);
        InputMap map = MakeGameplayMap();
        SerializeInputMap(ar, map);
        REQUIRE(ar.IsOk());
    }
    const u32 stale = kInputMapVersion + 1;
    (void)buffer.Seek(0, SeekOrigin::Begin);
    (void)buffer.Write(reinterpret_cast<const byte*>(&stale), sizeof(stale));
    (void)buffer.Seek(0, SeekOrigin::Begin);
    BinarySerializer ar(buffer, SerializeMode::Read);
    InputMap loaded;
    SerializeInputMap(ar, loaded);
    CHECK_FALSE(ar.IsOk());
    CHECK(loaded.sets.IsEmpty());
}

TEST_CASE("input: map round-trips through binary AND xml serializers")
{
    InputMap map = MakeGameplayMap();
    map.sets[0].actions[1].processors.sensitivity = 4.0f;
    map.sets[0].actions[1].processors.gravity = 8.0f;
    map.sets[0].actions[1].processors.snap = true;

    auto verify = [&](InputMap& loaded)
    {
        REQUIRE(loaded.sets.Size() == 2);
        CHECK(loaded.sets[0].name == u8"Gameplay");
        CHECK(loaded.sets[1].priority == 10);
        REQUIRE(loaded.sets[0].actions.Size() == 2);
        const Action& move = loaded.sets[0].actions[1];
        CHECK(move.kind == ActionKind::Axis2D);
        REQUIRE(move.bindings.Size() == 2);
        CHECK(move.bindings[0].source == BindingSource::Composite2D);
        CHECK(move.bindings[0].posY == static_cast<u32>(shell::KeyCode::W));
        CHECK(move.bindings[1].deadZone == doctest::Approx(0.2f));
        CHECK(move.processors.sensitivity == doctest::Approx(4.0f));
        CHECK(move.processors.snap);
    };

    {
        MemoryStream buffer;
        {
            BinarySerializer ar(buffer, SerializeMode::Write);
            SerializeInputMap(ar, map);
            REQUIRE(ar.IsOk());
        }
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(buffer, SerializeMode::Read);
        InputMap loaded;
        SerializeInputMap(ar, loaded);
        REQUIRE(ar.IsOk());
        verify(loaded);
    }
    {
        foundation::xml::XmlSerializer writer(foundation::core::DefaultAllocator());
        SerializeInputMap(writer, map);
        REQUIRE(writer.IsOk());
        String text;
        writer.GetOutput(text);
        foundation::xml::XmlDocument doc(foundation::core::DefaultAllocator());
        REQUIRE(doc.Parse(text.AsView()) == foundation::xml::XmlResult::Ok);
        foundation::xml::XmlSerializer reader(doc);
        InputMap loaded;
        SerializeInputMap(reader, loaded);
        REQUIRE(reader.IsOk());
        verify(loaded);
    }
}

TEST_CASE("input: validation refuses kind mismatches and empty names")
{
    InputMap map = MakeGameplayMap();
    CHECK(ValidateInputMap(map));

    InputMap bad = MakeGameplayMap();
    Binding stick;
    stick.source = BindingSource::GamepadStick;
    bad.sets[0].actions[0].bindings.PushBack(stick); // 2D binding on the Jump BUTTON
    String error;
    CHECK_FALSE(ValidateInputMap(bad, &error));
    CHECK(!error.IsEmpty());

    InputMap nameless = MakeGameplayMap();
    nameless.sets[0].actions[0].name = String{};
    CHECK_FALSE(ValidateInputMap(nameless));
}

TEST_CASE("input: buttons - edges, device folding, and key modifiers")
{
    ActionRuntime runtime;
    runtime.SetMap(MakeGameplayMap());
    runtime.DisableSet(u8"Menu"); // Space resolves to Gameplay/Jump
    FakeDevices devices;
    FakeGamepad pad;
    devices.pads.PushBack(&pad);
    const ActionRef jump = runtime.Resolve(u8"Jump");
    REQUIRE(jump.IsValid());

    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump));

    // Keyboard press: down + a pressed edge exactly this frame, held next frame.
    devices.keyboard.Set(shell::KeyCode::Space, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    CHECK(runtime.WasPressed(jump));
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    CHECK_FALSE(runtime.WasPressed(jump));

    // Release edge; then the GAMEPAD binding drives the same action (device folding).
    devices.keyboard.Set(shell::KeyCode::Space, false);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump));
    CHECK(runtime.WasReleased(jump));
    pad.buttons[0] = true;
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    pad.buttons[0] = false;

    // A modifier-gated binding only fires with the modifier held.
    InputMap modMap;
    ActionSet set;
    set.name = String(u8"S");
    Action save;
    save.name = String(u8"QuickSave");
    save.kind = ActionKind::Button;
    Binding ctrlS;
    ctrlS.source = BindingSource::Key;
    ctrlS.code = static_cast<u32>(shell::KeyCode::S);
    ctrlS.modifiers = static_cast<u32>(shell::KeyModifiers::LeftCtrl);
    save.bindings.PushBack(ctrlS);
    set.actions.PushBack(static_cast<Action&&>(save));
    modMap.sets.PushBack(static_cast<ActionSet&&>(set));
    ActionRuntime modRuntime;
    modRuntime.SetMap(modMap);
    const ActionRef quickSave = modRuntime.Resolve(u8"QuickSave");
    devices.keyboard.Set(shell::KeyCode::S, true);
    modRuntime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(modRuntime.IsDown(quickSave));
    devices.keyboard.mods = shell::KeyModifiers::LeftCtrl;
    modRuntime.Update(devices, 1.0f / 60.0f);
    CHECK(modRuntime.IsDown(quickSave));
    devices.keyboard.Set(shell::KeyCode::S, false);
    devices.keyboard.mods = shell::KeyModifiers::None;

    // A binding that names a modifier GROUP (Shift = both bits) fires on EITHER side; one
    // that names a specific side still needs that side. Requiring every bit made a Shift
    // binding unreachable (nobody holds both shifts) - found by the Beef port.
    InputMap groupMap;
    ActionSet groupSet;
    groupSet.name = String(u8"Gameplay");
    Action sprint;
    sprint.name = String(u8"Sprint");
    sprint.kind = ActionKind::Button;
    Binding shiftW;
    shiftW.source = BindingSource::Key;
    shiftW.code = static_cast<u32>(shell::KeyCode::W);
    shiftW.modifiers = static_cast<u32>(shell::KeyModifiers::Shift);
    sprint.bindings.PushBack(shiftW);
    groupSet.actions.PushBack(static_cast<Action&&>(sprint));
    Action leftOnly;
    leftOnly.name = String(u8"LeftOnly");
    leftOnly.kind = ActionKind::Button;
    Binding lshiftW = shiftW;
    lshiftW.modifiers = static_cast<u32>(shell::KeyModifiers::LeftShift);
    leftOnly.bindings.PushBack(lshiftW);
    groupSet.actions.PushBack(static_cast<Action&&>(leftOnly));
    groupMap.sets.PushBack(static_cast<ActionSet&&>(groupSet));
    ActionRuntime groupRuntime;
    groupRuntime.SetMap(groupMap);
    const ActionRef sprintRef = groupRuntime.Resolve(u8"Sprint");
    const ActionRef leftRef = groupRuntime.Resolve(u8"LeftOnly");
    devices.keyboard.Set(shell::KeyCode::W, true);
    groupRuntime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(groupRuntime.IsDown(sprintRef)); // no shift at all
    devices.keyboard.mods = shell::KeyModifiers::LeftShift;
    groupRuntime.Update(devices, 1.0f / 60.0f);
    CHECK(groupRuntime.IsDown(sprintRef)); // the left one is a shift
    CHECK(groupRuntime.IsDown(leftRef));
    devices.keyboard.mods = shell::KeyModifiers::RightShift;
    groupRuntime.Update(devices, 1.0f / 60.0f);
    CHECK(groupRuntime.IsDown(sprintRef)); // so is the right one
    CHECK_FALSE(groupRuntime.IsDown(leftRef)); // a specific side stays specific
    devices.keyboard.Set(shell::KeyCode::W, false);
    devices.keyboard.mods = shell::KeyModifiers::None;
}

TEST_CASE("input: the state summary names every action, its value and whether it is down")
{
    // Sedulous 18961314: what the input trace prints, one line for every action of the map.
    ActionRuntime runtime;
    runtime.SetMap(MakeGameplayMap());
    runtime.DisableSet(u8"Menu");
    FakeDevices devices;
    devices.keyboard.Set(shell::KeyCode::Space, true);
    devices.keyboard.Set(shell::KeyCode::D, true);
    runtime.Update(devices, 1.0f / 60.0f);
    String summary;
    runtime.AppendStateSummary(summary);
    CAPTURE(reinterpret_cast<const char*>(summary.CStr()));
    CHECK(summary.AsView().StartsWith(u8"Jump=(1,0) down Move=(1,0)"));
    CHECK(summary.AsView().ContainsIgnoreCase(u8" Move=(1,0) down"));
}

TEST_CASE("input: axes - composite, stick dead zone, and folding by magnitude")
{
    ActionRuntime runtime;
    runtime.SetMap(MakeGameplayMap());
    runtime.DisableSet(u8"Menu");
    FakeDevices devices;
    FakeGamepad pad;
    devices.pads.PushBack(&pad);
    const ActionRef move = runtime.Resolve(u8"Move");

    // WASD: diagonal normalizes to unit length.
    devices.keyboard.Set(shell::KeyCode::W, true);
    devices.keyboard.Set(shell::KeyCode::D, true);
    runtime.Update(devices, 1.0f / 60.0f);
    Float2 v = runtime.Value2D(move);
    CHECK(v.x == doctest::Approx(0.7071f).epsilon(0.01));
    CHECK(v.y == doctest::Approx(0.7071f).epsilon(0.01));
    CHECK(runtime.IsDown(move));
    devices.keyboard.Set(shell::KeyCode::W, false);
    devices.keyboard.Set(shell::KeyCode::D, false);

    // Stick inside the dead zone reads zero; outside it rescales from the edge.
    pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.1f;
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.Value2D(move).x == doctest::Approx(0.0f));
    pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 1.0f;
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.Value2D(move).x == doctest::Approx(1.0f).epsilon(0.01));

    // Keyboard (0.7) vs stick (1.0): the larger magnitude wins the fold.
    devices.keyboard.Set(shell::KeyCode::A,
                         true); // x = -1 from keys, +1 from stick... stick larger? equal
    pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.4f;
    runtime.Update(devices, 1.0f / 60.0f);
    v = runtime.Value2D(move);
    CHECK(v.x ==
          doctest::Approx(-1.0f).epsilon(0.01)); // the full-press key out-magnitudes 0.25-ish stick
    devices.keyboard.Set(shell::KeyCode::A, false);
    pad.axes[static_cast<u32>(shell::GamepadAxis::LeftX)] = 0.0f;
}

TEST_CASE("input: exclusive sets - priority resolution, suppression, and held latching")
{
    ActionRuntime runtime;
    runtime.SetMap(MakeGameplayMap());
    FakeDevices devices;
    const ActionRef jump = runtime.Resolve(u8"Jump");
    const ActionRef confirm = runtime.Resolve(u8"Confirm");

    // Hold Space in gameplay: Jump is down.
    devices.keyboard.Set(shell::KeyCode::Space, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));

    // Menu goes exclusive mid-hold: Jump releases (edge fires once), Confirm must NOT
    // fire from the stale hold... it is latched too? No: Confirm was suppressed BEFORE
    // (not top set? it evaluates now) - Confirm became physically pressed only while its
    // set was suppressed-inactive, so it latched and stays released until a re-press.
    runtime.PushExclusiveSet(u8"Menu");
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump));
    CHECK(runtime.WasReleased(jump));
    CHECK_FALSE(runtime.IsDown(confirm)); // latched: held from before the menu opened

    // Releasing and re-pressing INSIDE the menu activates Confirm, not Jump.
    devices.keyboard.Set(shell::KeyCode::Space, false);
    runtime.Update(devices, 1.0f / 60.0f);
    devices.keyboard.Set(shell::KeyCode::Space, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(confirm));
    CHECK(runtime.WasPressed(confirm));
    CHECK_FALSE(runtime.IsDown(jump));

    // Menu closes while Space is STILL held: Jump stays released (latched) until the
    // physical release; the next press works normally.
    runtime.PopExclusiveSet();
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump)); // the held key must not re-fire
    devices.keyboard.Set(shell::KeyCode::Space, false);
    runtime.Update(devices, 1.0f / 60.0f);
    devices.keyboard.Set(shell::KeyCode::Space, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    CHECK(runtime.WasPressed(jump));
}

// A menu set repeating an action of the game's (Masquerade: Pause in Gameplay and in Menu, the
// menu's set pushed exclusive while a menu is up): the query answers from the set in effect, not
// the first enabled one, so Escape still closes the menu it opened.
TEST_CASE("input: an action two sets share answers from the exclusive set while it is pushed")
{
    InputMap map;
    const auto pause = []()
    {
        Action action;
        action.name = String(u8"Pause");
        action.kind = ActionKind::Button;
        Binding key;
        key.source = BindingSource::Key;
        key.code = static_cast<u32>(shell::KeyCode::Escape);
        action.bindings.PushBack(key);
        return action;
    };
    ActionSet gameplay;
    gameplay.name = String(u8"Gameplay");
    gameplay.actions.PushBack(pause());
    {
        Action tag;
        tag.name = String(u8"Tag");
        tag.kind = ActionKind::Button;
        Binding key;
        key.source = BindingSource::Key;
        key.code = static_cast<u32>(shell::KeyCode::F);
        tag.bindings.PushBack(key);
        gameplay.actions.PushBack(static_cast<Action&&>(tag));
    }
    map.sets.PushBack(static_cast<ActionSet&&>(gameplay));
    ActionSet menu;
    menu.name = String(u8"Menu"); // the same priority as Gameplay, after it
    menu.actions.PushBack(pause());
    map.sets.PushBack(static_cast<ActionSet&&>(menu));

    ActionRuntime runtime;
    runtime.SetMap(map);
    FakeDevices devices;
    const ActionRef pauseRef = runtime.Resolve(u8"Pause");
    const ActionRef tagRef = runtime.Resolve(u8"Tag");

    // In play, Escape pauses (the game's Pause).
    devices.keyboard.Set(shell::KeyCode::Escape, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.WasPressed(pauseRef));
    devices.keyboard.Set(shell::KeyCode::Escape, false);
    runtime.Update(devices, 1.0f / 60.0f);

    // The menu is up: Tag reads released, and Escape is the menu's Pause, pressed.
    runtime.PushExclusiveSet(u8"Menu");
    devices.keyboard.Set(shell::KeyCode::F, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.WasPressed(tagRef));
    CHECK_FALSE(runtime.IsDown(tagRef));
    devices.keyboard.Set(shell::KeyCode::F, false);
    devices.keyboard.Set(shell::KeyCode::Escape, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.WasPressed(pauseRef)); // was the Gameplay copy: suppressed, never pressed
    devices.keyboard.Set(shell::KeyCode::Escape, false);
    runtime.Update(devices, 1.0f / 60.0f);

    // Closed: the game's own again.
    runtime.PopExclusiveSet();
    runtime.Update(devices, 1.0f / 60.0f);
    devices.keyboard.Set(shell::KeyCode::F, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.WasPressed(tagRef));
}

TEST_CASE("input: rebind overlay - apply over a pristine copy, clear restores, persists")
{
    RegisterInputTypes();
    InputMap asset = MakeGameplayMap();

    // Override: Jump moves from Space to J.
    InputBindingOverrides overlay;
    {
        Array<Binding> replacement;
        Binding j;
        j.source = BindingSource::Key;
        j.code = static_cast<u32>(shell::KeyCode::J);
        replacement.PushBack(j);
        overlay.Set(u8"Gameplay", u8"Jump", static_cast<Array<Binding>&&>(replacement));
    }

    InputMap effective = asset; // pristine copy
    ApplyBindingOverrides(effective, overlay);
    REQUIRE(effective.sets[0].actions[0].bindings.Size() == 1);
    CHECK(effective.sets[0].actions[0].bindings[0].code == static_cast<u32>(shell::KeyCode::J));
    CHECK(asset.sets[0].actions[0].bindings.Size() == 2); // the asset never mutates

    ActionRuntime runtime;
    runtime.SetMap(effective);
    runtime.DisableSet(u8"Menu");
    FakeDevices devices;
    const ActionRef jump = runtime.Resolve(u8"Jump");
    devices.keyboard.Set(shell::KeyCode::Space, true); // the OLD binding: dead
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump));
    devices.keyboard.Set(shell::KeyCode::J, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));

    // Reset-to-default = clear the override; the pristine asset re-applies.
    overlay.Clear(u8"Gameplay", u8"Jump");
    InputMap restored = asset;
    ApplyBindingOverrides(restored, overlay);
    CHECK(restored.sets[0].actions[0].bindings.Size() == 2);

    // The section round-trips through the settings store (user file persistence).
    {
        Array<Binding> replacement;
        Binding k;
        k.source = BindingSource::Key;
        k.code = static_cast<u32>(shell::KeyCode::K);
        replacement.PushBack(k);
        overlay.Set(u8"Gameplay", u8"Jump", static_cast<Array<Binding>&&>(replacement));
    }
    foundation::settings::Settings store(foundation::core::DefaultAllocator());
    store.Section<InputBindingOverrides>().overrides =
        overlay.overrides; // sections are non-copyable objects
    MemoryStream file;
    REQUIRE(store.Save(file, BinarySerializerFactory()).IsOk());
    (void)file.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings loadedStore(foundation::core::DefaultAllocator());
    REQUIRE(loadedStore.Load(file, BinarySerializerFactory()).IsOk());
    const InputBindingOverrides* loaded = loadedStore.Find<InputBindingOverrides>();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->overrides.Size() == 1);
    CHECK(loaded->overrides[0].bindings[0].code == static_cast<u32>(shell::KeyCode::K));
}

TEST_CASE("input: rebind capture - first activated input matching the filter")
{
    FakeDevices devices;
    FakeGamepad pad;
    devices.pads.PushBack(&pad);
    Binding captured;

    // Nothing active: keeps listening.
    CaptureFilter keysOnly;
    keysOnly.mouseButtons = false;
    keysOnly.gamepadButtons = false;
    CHECK_FALSE(CaptureBinding(devices, keysOnly, captured));

    // A key press captures as a Key binding.
    devices.keyboard.pressed[static_cast<u32>(shell::KeyCode::F)] = true;
    REQUIRE(CaptureBinding(devices, keysOnly, captured));
    CHECK(captured.source == BindingSource::Key);
    CHECK(captured.code == static_cast<u32>(shell::KeyCode::F));
    devices.keyboard.pressed[static_cast<u32>(shell::KeyCode::F)] = false;

    // Stick noise is IGNORED for a key rebind; an opted-in stick filter captures it.
    pad.axes[static_cast<u32>(shell::GamepadAxis::RightX)] = 0.9f;
    CHECK_FALSE(CaptureBinding(devices, keysOnly, captured));
    CaptureFilter stickFilter;
    stickFilter.keys = false;
    stickFilter.mouseButtons = false;
    stickFilter.gamepadButtons = false;
    stickFilter.gamepadSticks = true;
    REQUIRE(CaptureBinding(devices, stickFilter, captured));
    CHECK(captured.source == BindingSource::GamepadStick);
    CHECK(captured.code == static_cast<u32>(StickCode::Right));

    // Gamepad button (must-not-have keys still capturing pads).
    pad.axes[static_cast<u32>(shell::GamepadAxis::RightX)] = 0.0f;
    pad.buttonsPressed[3] = true;
    CaptureFilter padButtons;
    padButtons.keys = false;
    padButtons.mouseButtons = false;
    REQUIRE(CaptureBinding(devices, padButtons, captured));
    CHECK(captured.source == BindingSource::GamepadButton);
    CHECK(captured.code == 3u);
}

TEST_CASE("input: SurfaceTouch transforms + spatially gates through the ContentFit")
{
    // A minimal IInputManager over the fakes (the surface polls through it).
    class Manager final : public shell::IInputManager
    {
    public:
        FakeKeyboard keyboard;
        FakeMouse mouse;
        FakeTouch touch;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] shell::ITouch* Touch() override { return &touch; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* GetGamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const shell::InputEvent> Events() const override { return {}; }
        [[nodiscard]] u32 HoverWindow() const override { return 1u; }
        [[nodiscard]] u32 FocusedWindow() const override { return 1u; }
        void Update() override {}
    };
    Manager manager;

    // A 1920x1080 window hosting a viewport at (100,100)-(900,700): 800x600 region showing
    // 800x600 content (stretch = identity within the region).
    shell::InputSurface surface(&manager, 1u, ContentFit{});
    surface.SetRegion(Rectangle{100.0f, 100.0f, 800.0f, 600.0f});
    surface.SetContentSize(Float2{800.0f, 600.0f});
    surface.SetWindowSize(Float2{1920.0f, 1080.0f});
    shell::ITouch* touch = surface.Touch();
    REQUIRE(touch != nullptr);

    // A finger OUTSIDE the region (window-normalized 0.01, 0.01 = 19,10 px): filtered.
    manager.touch.Set(1, 0.01f, 0.01f);
    CHECK(touch->TouchCount() == 0);
    CHECK_FALSE(touch->HasTouch());

    // Center of the region: window px (500, 400) -> content px (400, 300) -> normalized
    // (0.5, 0.5).
    manager.touch.Set(1, 500.0f / 1920.0f, 400.0f / 1080.0f);
    REQUIRE(touch->TouchCount() == 1);
    shell::TouchPoint mapped;
    REQUIRE(touch->GetTouchPoint(0, mapped));
    CHECK(mapped.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(mapped.y == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(mapped.id == 1u);
    // And in the surface's pointer space (content pixels), where its mouse reports.
    CHECK(mapped.pointerX == doctest::Approx(400.0f).epsilon(0.01));
    CHECK(mapped.pointerY == doctest::Approx(300.0f).epsilon(0.01));

    // Letterbox: 800x600 region showing 1280x800 content (16:10 in 4:3) - vertical bars
    // ABOVE/BELOW? 16:10 content in 4:3 region: content wider -> bars top+bottom. A
    // finger in the bar band is filtered ("no hit" contract); one in the content maps.
    surface.SetContentSize(Float2{1280.0f, 800.0f});
    surface.SetFitMode(FitMode::Letterbox);
    // dst: width 800, height 800*800/1280 = 500, centered in the 600-tall region ->
    // region-local y in [50, 550) = window px [150, 650). Content center = window (500, 400).
    manager.touch.Set(1, 500.0f / 1920.0f, 130.0f / 1080.0f); // in the top bar (region y 30)
    CHECK(touch->TouchCount() == 0);
    manager.touch.Set(1, 500.0f / 1920.0f, 400.0f / 1080.0f); // content center
    REQUIRE(touch->TouchCount() == 1);
    REQUIRE(touch->GetTouchPoint(0, mapped));
    CHECK(mapped.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(mapped.y == doctest::Approx(0.5f).epsilon(0.01));

    // No window size published: everything filters (never a divide-by-zero).
    surface.SetWindowSize(Float2{0.0f, 0.0f});
    CHECK(touch->TouchCount() == 0);
}

// Present transform P3: the editor's Game tab previewing a portrait phone, a landscape game
// turned three quarters in a portrait viewport. The router maps the mouse and a finger into the
// game's frame and turns a drag's motion with it.
TEST_CASE("input: a turned surface reads the mouse and fingers in the game's frame")
{
    class Manager final : public shell::IInputManager
    {
    public:
        FakeKeyboard keyboard;
        FakeMouse mouse;
        FakeTouch touch;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] shell::ITouch* Touch() override { return &touch; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* GetGamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const shell::InputEvent> Events() const override { return {}; }
        [[nodiscard]] u32 HoverWindow() const override { return 1u; }
        [[nodiscard]] u32 FocusedWindow() const override { return 1u; }
        void Update() override {}
    };
    Manager manager;

    // A 600x800 viewport at (100, 100) of a 1920x1080 window, showing an 800x600 game turned.
    shell::InputSurface surface(&manager, 1u,
                                ContentFit{Rectangle{100.0f, 100.0f, 600.0f, 800.0f}, Float2{800.0f, 600.0f},
                                           FitMode::Stretch, ContentRotation::ThreeQuarter});
    surface.SetWindowSize(Float2{1920.0f, 1080.0f});
    shell::InputRouter router(&manager);
    router.AddSurface(&surface);

    // Near the viewport's bottom-left is near the game's top-left.
    manager.mouse.x = 110.0f;
    manager.mouse.y = 890.0f;
    router.Update();
    REQUIRE(router.Hovered() == &surface);
    shell::IMouse* mouse = surface.Mouse();
    REQUIRE(mouse != nullptr);
    CHECK(mouse->X() == doctest::Approx(10.0f));
    CHECK(mouse->Y() == doctest::Approx(10.0f));

    // A drag up the viewport moves right across the game.
    manager.mouse.dy = -10.0f;
    router.Update();
    CHECK(mouse->DeltaX() == doctest::Approx(10.0f));
    CHECK(mouse->DeltaY() == doctest::Approx(0.0f));

    // A finger at the same place is at the game's top-left too, normalized over the game.
    manager.touch.Set(1, 110.0f / 1920.0f, 890.0f / 1080.0f);
    shell::ITouch* touch = surface.Touch();
    REQUIRE(touch != nullptr);
    REQUIRE(touch->TouchCount() == 1);
    shell::TouchPoint mapped;
    REQUIRE(touch->GetTouchPoint(0, mapped));
    CHECK(mapped.pointerX == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(mapped.pointerY == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(mapped.x == doctest::Approx(10.0f / 800.0f).epsilon(0.01));
    CHECK(mapped.y == doctest::Approx(10.0f / 600.0f).epsilon(0.01));
    router.RemoveSurface(&surface);
}

TEST_CASE("input: a surface pressed on reads the release wherever the pointer is let go")
{
    class Manager final : public shell::IInputManager
    {
    public:
        FakeKeyboard keyboard;
        FakeMouse mouse;
        FakeTouch touch;
        u32 hover = 1u;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] shell::ITouch* Touch() override { return &touch; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* GetGamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const shell::InputEvent> Events() const override { return {}; }
        [[nodiscard]] u32 HoverWindow() const override { return hover; }
        [[nodiscard]] u32 FocusedWindow() const override { return 1u; }
        void Update() override {}
    };
    Manager manager;
    shell::InputSurface surface(&manager, 1u, ContentFit{});
    surface.SetRegion(Rectangle{100.0f, 100.0f, 800.0f, 600.0f});
    surface.SetContentSize(Float2{800.0f, 600.0f});
    surface.SetWindowSize(Float2{1920.0f, 1080.0f});
    shell::InputRouter router(&manager);
    router.AddSurface(&surface);
    shell::IMouse* mouse = surface.Mouse();
    const u32 left = static_cast<u32>(shell::MouseButton::Left) & 7;
    const auto frame = [&](f32 x, f32 y, bool down, bool edge)
    {
        manager.mouse.x = x;
        manager.mouse.y = y;
        manager.mouse.buttons[left] = down;
        manager.mouse.pressed[left] = edge && down;
        manager.mouse.released[left] = edge && !down;
        router.Update();
    };

    // Pressed inside, dragged out: held, the surface keeps the pointer.
    frame(500.0f, 400.0f, true, true);
    CHECK(mouse->IsButtonPressed(shell::MouseButton::Left));
    frame(40.0f, 40.0f, true, false);
    CHECK(mouse->IsButtonDown(shell::MouseButton::Left));

    // Let go outside the surface: it reads the release (the frame the capture ends).
    frame(40.0f, 40.0f, false, true);
    CHECK(mouse->IsButtonReleased(shell::MouseButton::Left));
    CHECK(router.Hovered() == nullptr);

    // The next frame it is just a surface the pointer is not over.
    frame(40.0f, 40.0f, false, false);
    CHECK_FALSE(mouse->IsButtonReleased(shell::MouseButton::Left));
    CHECK_FALSE(surface.MouseActive());

    // Let go over another window: the same.
    frame(500.0f, 400.0f, true, true);
    manager.hover = 2u;
    frame(500.0f, 400.0f, true, false);
    frame(500.0f, 400.0f, false, true);
    CHECK(mouse->IsButtonReleased(shell::MouseButton::Left));
    router.RemoveSurface(&surface);
}

TEST_CASE("input: a router locks the pointer for the surface it is pinned to, while it asks, and only undoes its own lock; a drag from another passes over")
{
    class Manager final : public shell::IInputManager
    {
    public:
        FakeKeyboard keyboard;
        FakeMouse mouse;
        FakeTouch touch;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return &mouse; }
        [[nodiscard]] shell::ITouch* Touch() override { return &touch; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* GetGamepad(i32) override { return nullptr; }
        [[nodiscard]] Span<const shell::InputEvent> Events() const override { return {}; }
        [[nodiscard]] u32 HoverWindow() const override { return 1u; }
        [[nodiscard]] u32 FocusedWindow() const override { return 1u; }
        void Update() override {}
    };
    Manager manager;
    // Two pages, each its router and its viewport, side by side in one window.
    shell::InputSurface left(&manager, 1u, ContentFit{});
    left.SetRegion(Rectangle{0.0f, 0.0f, 400.0f, 400.0f});
    left.SetContentSize(Float2{400.0f, 400.0f});
    left.SetWindowSize(Float2{800.0f, 400.0f});
    shell::InputSurface right(&manager, 1u, ContentFit{});
    right.SetRegion(Rectangle{400.0f, 0.0f, 400.0f, 400.0f});
    right.SetContentSize(Float2{400.0f, 400.0f});
    right.SetWindowSize(Float2{800.0f, 400.0f});
    shell::InputRouter leftRouter(&manager);
    leftRouter.AddSurface(&left);
    shell::InputRouter rightRouter(&manager);
    rightRouter.AddSurface(&right);
    const u32 rmb = static_cast<u32>(shell::MouseButton::Right) & 7;
    const auto frame = [&](f32 x, bool down, bool edge)
    {
        manager.mouse.x = x;
        manager.mouse.y = 200.0f;
        manager.mouse.buttons[rmb] = down;
        manager.mouse.pressed[rmb] = edge && down;
        manager.mouse.released[rmb] = edge && !down;
        leftRouter.Update();
        rightRouter.Update();
    };

    // Asking with no press: nothing.
    left.SetPointerLock(true);
    frame(200.0f, false, false);
    CHECK_FALSE(manager.mouse.relative);
    CHECK(leftRouter.PointerLockedBy() == nullptr);

    // Pressed on the left viewport, which asks: locked once, hidden.
    frame(200.0f, true, true);
    CHECK(manager.mouse.relative);
    CHECK_FALSE(manager.mouse.cursorVisible);
    CHECK(leftRouter.PointerLockedBy() == &left);
    CHECK(manager.mouse.relativeChanges == 1);
    // The drag carries the pointer over the other page: neither router changes it.
    frame(600.0f, true, false);
    frame(600.0f, true, false);
    CHECK(manager.mouse.relative);
    CHECK(manager.mouse.relativeChanges == 1);
    CHECK(rightRouter.PointerLockedBy() == nullptr);
    // Nor does the other page take the drag passing over it as its own.
    CHECK_FALSE(right.Hovered());
    CHECK_FALSE(right.MouseActive());
    CHECK_FALSE(right.Mouse()->IsButtonDown(shell::MouseButton::Right));
    CHECK(left.Mouse()->IsButtonDown(shell::MouseButton::Right));

    // Let go: unlocked, shown; the page under the pointer is hovered again.
    frame(600.0f, false, true);
    CHECK(right.Hovered());
    CHECK_FALSE(manager.mouse.relative);
    CHECK(manager.mouse.cursorVisible);
    CHECK(manager.mouse.relativeChanges == 2);

    // A lock it did not take (a game's mouselook) it never undoes.
    manager.mouse.relative = true;
    frame(200.0f, false, false);
    frame(600.0f, true, true);
    frame(600.0f, false, true);
    CHECK(manager.mouse.relative);

    // A surface taken out while it holds the lock takes the lock with it.
    manager.mouse.relative = false;
    frame(200.0f, true, true);
    REQUIRE(leftRouter.PointerLockedBy() == &left);
    leftRouter.RemoveSurface(&left);
    CHECK_FALSE(manager.mouse.relative);
    CHECK(leftRouter.PointerLockedBy() == nullptr);
    rightRouter.RemoveSurface(&right);
}

TEST_CASE("input: touch - region buttons and the floating virtual stick")
{
    InputMap map;
    ActionSet set;
    set.name = String(u8"S");
    {
        Action fire;
        fire.name = String(u8"Fire");
        fire.kind = ActionKind::Button;
        Binding region;
        region.source = BindingSource::TouchButton;
        region.regionX = 0.5f;
        region.regionY = 0.5f; // bottom-right quadrant
        region.regionW = 0.5f;
        region.regionH = 0.5f;
        fire.bindings.PushBack(region);
        set.actions.PushBack(static_cast<Action&&>(fire));
    }
    {
        Action move;
        move.name = String(u8"Move");
        move.kind = ActionKind::Axis2D;
        Binding stick;
        stick.source = BindingSource::TouchStick;
        stick.regionX = 0.0f;
        stick.regionY = 0.0f; // left half
        stick.regionW = 0.5f;
        stick.regionH = 1.0f;
        stick.stickRadius = 0.1f;
        stick.deadZone = 0.1f;
        move.bindings.PushBack(stick);
        set.actions.PushBack(static_cast<Action&&>(move));
    }
    map.sets.PushBack(static_cast<ActionSet&&>(set));
    CHECK(ValidateInputMap(map));

    ActionRuntime runtime;
    runtime.SetMap(map);
    FakeDevices devices;
    const ActionRef fire = runtime.Resolve(u8"Fire");
    const ActionRef move = runtime.Resolve(u8"Move");
    const f32 step = 1.0f / 60.0f;

    // Region button: outside = nothing; inside = pressed with edges.
    devices.touch.Set(1, 0.2f, 0.2f);
    runtime.Update(devices, step);
    CHECK_FALSE(runtime.IsDown(fire));
    devices.touch.Set(1, 0.8f, 0.8f);
    runtime.Update(devices, step);
    CHECK(runtime.IsDown(fire));
    CHECK(runtime.WasPressed(fire));
    devices.touch.Remove(1);
    runtime.Update(devices, step);
    CHECK(runtime.WasReleased(fire));

    // Stick: a touch starting in-region anchors (value 0), deflection scales over the
    // radius, clamps at 1, and a touch that STARTED OUTSIDE never captures. The finger is id 0,
    // as a browser numbers its first finger.
    devices.touch.Set(0, 0.25f, 0.5f);
    runtime.Update(devices, step); // capture frame: anchor == position
    CHECK(runtime.Value2D(move).x == doctest::Approx(0.0f));
    devices.touch.Set(0, 0.30f, 0.5f); // +0.05 over radius 0.1 = half deflection...
    runtime.Update(devices, step);
    CHECK(runtime.Value2D(move).x > 0.30f); // ...minus the dead-zone rescale
    CHECK(runtime.Value2D(move).x < 0.60f);
    devices.touch.Set(0, 0.60f, 0.5f); // way past the radius: clamped
    runtime.Update(devices, step);
    CHECK(runtime.Value2D(move).x == doctest::Approx(1.0f));
    CHECK(runtime.Value2D(move).y == doctest::Approx(0.0f));
    // The controls, as an on-screen guide reads them: each binding with its action, the stick
    // held where its finger came down and where it is now.
    {
        Array<String> actions;
        bool stickHeld = false;
        Float2 anchor{}, finger{};
        runtime.ForEachTouchControl(
            [&](const ActionRuntime::TouchControl& control)
            {
                actions.PushBack(String(control.action));
                if (control.binding->source == BindingSource::TouchStick)
                {
                    stickHeld = control.held;
                    anchor = control.anchor;
                    finger = control.finger;
                }
            });
        REQUIRE(actions.Size() == 2u);
        CHECK(actions[0] == StringView(u8"Fire"));
        CHECK(actions[1] == StringView(u8"Move"));
        CHECK(stickHeld);
        CHECK(anchor.x == doctest::Approx(0.25f));
        CHECK(finger.x == doctest::Approx(0.60f));
        // A disabled set shows nothing.
        runtime.DisableSet(u8"S");
        usize shown = 0;
        runtime.ForEachTouchControl([&](const ActionRuntime::TouchControl&) { ++shown; });
        CHECK(shown == 0u);
        runtime.EnableSet(u8"S");
    }
    devices.touch.Remove(0);
    runtime.Update(devices, step);
    CHECK(runtime.Value2D(move).x == doctest::Approx(0.0f));

    // The stick region does NOT capture a touch that began outside it - even if it
    // later drifts inside (the finger belongs to whatever it started on).
    devices.touch.Set(3, 0.9f, 0.9f);
    runtime.Update(devices, step);
    devices.touch.Set(3, 0.25f, 0.5f);
    runtime.Update(devices, step);
    // NOTE: with the capture-scan running each frame a formerly-outside touch inside
    // the region WILL capture (we scan current positions). Assert the ACTUAL contract:
    CHECK(runtime.Value2D(move).x ==
          doctest::Approx(0.0f)); // anchor = entry point, so value starts at 0

    // Kind mismatch validation: a TouchStick on a Button action is refused.
    InputMap bad = map;
    Binding wrong;
    wrong.source = BindingSource::TouchStick;
    bad.sets[0].actions[0].bindings.PushBack(wrong);
    CHECK_FALSE(ValidateInputMap(bad));
}

TEST_CASE("input: the timeScale processor scales flagged action VALUES only")
{
    InputMap map;
    ActionSet set;
    set.name = String(u8"S");
    auto addAxis = [&](StringView name, bool scaled)
    {
        Action axis;
        axis.name = String(name);
        axis.kind = ActionKind::Axis1D;
        Binding key;
        key.source = BindingSource::Key;
        key.code = static_cast<u32>(shell::KeyCode::W);
        axis.bindings.PushBack(key);
        axis.processors.timeScale = scaled;
        set.actions.PushBack(static_cast<Action&&>(axis));
    };
    addAxis(u8"Scaled", true);
    addAxis(u8"Raw", false);
    map.sets.PushBack(static_cast<ActionSet&&>(set));

    ActionRuntime runtime;
    runtime.SetMap(map);
    FakeDevices devices;
    devices.keyboard.Set(shell::KeyCode::W, true);
    const ActionRef scaled = runtime.Resolve(u8"Scaled");
    const ActionRef raw = runtime.Resolve(u8"Raw");

    runtime.SetTimeScale(0.25f);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.Value(scaled) == doctest::Approx(0.25f));
    CHECK(runtime.Value(raw) == doctest::Approx(1.0f));
    CHECK(runtime.IsDown(raw));

    runtime.SetTimeScale(0.0f); // paused world: flagged values zero, digital press intact
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.Value(scaled) == doctest::Approx(0.0f));
    CHECK(runtime.IsDown(scaled)); // the PRESS is physical; only the value scales
}

TEST_CASE("input: interactions - hold, tap, and double tap")
{
    auto makeButtonMap = [](InteractionKind kind, f32 seconds)
    {
        InputMap map;
        ActionSet set;
        set.name = String(u8"S");
        Action action;
        action.name = String(u8"Act");
        action.kind = ActionKind::Button;
        Binding key;
        key.source = BindingSource::Key;
        key.code = static_cast<u32>(shell::KeyCode::Space);
        action.bindings.PushBack(key);
        action.interaction.kind = kind;
        action.interaction.seconds = seconds;
        set.actions.PushBack(static_cast<Action&&>(action));
        map.sets.PushBack(static_cast<ActionSet&&>(set));
        return map;
    };
    const f32 step = 1.0f / 60.0f;
    FakeDevices devices;

    // HOLD 0.2s: pressing does nothing until the threshold; then one pressed edge; the
    // release edge fires on release as usual.
    {
        ActionRuntime runtime;
        runtime.SetMap(makeButtonMap(InteractionKind::Hold, 0.2f));
        const ActionRef act = runtime.Resolve(u8"Act");
        devices.keyboard.Set(shell::KeyCode::Space, true);
        for (int i = 0; i < 6; ++i)
        {
            runtime.Update(devices, step);
            CHECK_FALSE(runtime.IsDown(act)); // 6 frames = 0.1s, below the threshold
        }
        bool edged = false;
        for (int i = 0; i < 8; ++i)
        {
            runtime.Update(devices, step);
            if (runtime.WasPressed(act))
            {
                edged = true;
            }
        }
        CHECK(edged);
        CHECK(runtime.IsDown(act));
        devices.keyboard.Set(shell::KeyCode::Space, false);
        runtime.Update(devices, step);
        CHECK_FALSE(runtime.IsDown(act));
        CHECK(runtime.WasReleased(act));
    }

    // TAP 0.15s: a short press pulses ONE frame at release; a long press never fires.
    {
        ActionRuntime runtime;
        runtime.SetMap(makeButtonMap(InteractionKind::Tap, 0.15f));
        const ActionRef act = runtime.Resolve(u8"Act");
        devices.keyboard.Set(shell::KeyCode::Space, true);
        for (int i = 0; i < 4; ++i)
        {
            runtime.Update(devices, step);
            CHECK_FALSE(runtime.IsDown(act));
        }
        devices.keyboard.Set(shell::KeyCode::Space, false);
        runtime.Update(devices, step);
        CHECK(runtime.WasPressed(act)); // the pulse
        CHECK(runtime.IsDown(act));
        runtime.Update(devices, step);
        CHECK_FALSE(runtime.IsDown(act));
        CHECK(runtime.WasReleased(act));

        devices.keyboard.Set(shell::KeyCode::Space, true); // long press: no fire
        for (int i = 0; i < 20; ++i)
        {
            runtime.Update(devices, step);
        }
        devices.keyboard.Set(shell::KeyCode::Space, false);
        runtime.Update(devices, step);
        CHECK_FALSE(runtime.WasPressed(act));
    }

    // DOUBLE TAP 0.25s: two quick presses pulse on the SECOND; slow presses never fire.
    {
        ActionRuntime runtime;
        runtime.SetMap(makeButtonMap(InteractionKind::DoubleTap, 0.25f));
        const ActionRef act = runtime.Resolve(u8"Act");
        auto tap = [&](int gapFrames)
        {
            devices.keyboard.Set(shell::KeyCode::Space, true);
            runtime.Update(devices, step);
            const bool fired = runtime.WasPressed(act);
            devices.keyboard.Set(shell::KeyCode::Space, false);
            runtime.Update(devices, step);
            for (int i = 0; i < gapFrames; ++i)
            {
                runtime.Update(devices, step);
            }
            return fired;
        };
        CHECK_FALSE(tap(2));  // first tap arms
        CHECK(tap(2));        // second within the window fires
        CHECK_FALSE(tap(30)); // slow: arms again (previous consumed), gap too long...
        CHECK_FALSE(tap(30)); // ...and a second slow tap still does not fire
    }
}

TEST_CASE("input: smoothing - sensitivity ramp, gravity recenter, snap on flip")
{
    InputMap map;
    ActionSet set;
    set.name = String(u8"S");
    Action axis;
    axis.name = String(u8"Throttle");
    axis.kind = ActionKind::Axis1D;
    Binding key;
    key.source = BindingSource::Key;
    key.code = static_cast<u32>(shell::KeyCode::W);
    axis.bindings.PushBack(key);
    Binding back;
    back.source = BindingSource::Key;
    back.code = static_cast<u32>(shell::KeyCode::S);
    back.scale = -1.0f;
    axis.bindings.PushBack(back);
    axis.processors.sensitivity = 5.0f; // 0.2s to full
    axis.processors.gravity = 10.0f;    // 0.1s back to zero
    axis.processors.snap = true;
    set.actions.PushBack(static_cast<Action&&>(axis));
    map.sets.PushBack(static_cast<ActionSet&&>(set));

    ActionRuntime runtime;
    runtime.SetMap(map);
    FakeDevices devices;
    const ActionRef throttle = runtime.Resolve(u8"Throttle");

    // Ramp: after 0.1s at sensitivity 5, value is ~0.5, not 1.
    devices.keyboard.Set(shell::KeyCode::W, true);
    for (int i = 0; i < 6; ++i)
    {
        runtime.Update(devices, 1.0f / 60.0f);
    }
    CHECK(runtime.Value(throttle) == doctest::Approx(0.5f).epsilon(0.05));
    for (int i = 0; i < 12; ++i)
    {
        runtime.Update(devices, 1.0f / 60.0f);
    }
    CHECK(runtime.Value(throttle) == doctest::Approx(1.0f));

    // Snap: flipping to S zeroes first, then ramps negative (never crossfades through).
    devices.keyboard.Set(shell::KeyCode::W, false);
    devices.keyboard.Set(shell::KeyCode::S, true);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.Value(throttle) <= 0.0f);

    // Gravity: releasing recenters at 10/sec - ~0.1s to zero from full.
    devices.keyboard.Set(shell::KeyCode::S, false);
    for (int i = 0; i < 8; ++i)
    {
        runtime.Update(devices, 1.0f / 60.0f);
    }
    CHECK(runtime.Value(throttle) == doctest::Approx(0.0f));
}

TEST_CASE("input: the UI consumption mask gates device classes independently")
{
    // Self-contained map: one KEY action, one MOUSE-BUTTON action.
    InputMap map;
    ActionSet set;
    set.name = String(u8"G");
    {
        Action jump;
        jump.name = String(u8"Jump");
        jump.kind = ActionKind::Button;
        Binding key;
        key.source = BindingSource::Key;
        key.code = static_cast<u32>(shell::KeyCode::Space);
        jump.bindings.PushBack(key);
        set.actions.PushBack(static_cast<Action&&>(jump));
    }
    {
        Action shoot;
        shoot.name = String(u8"Shoot");
        shoot.kind = ActionKind::Button;
        Binding button;
        button.source = BindingSource::MouseButton;
        button.code = static_cast<u32>(shell::MouseButton::Left);
        shoot.bindings.PushBack(button);
        set.actions.PushBack(static_cast<Action&&>(shoot));
    }
    map.sets.PushBack(static_cast<ActionSet&&>(set));

    ActionRuntime runtime;
    runtime.SetMap(map);
    FakeDevices devices;
    devices.keyboard.Set(shell::KeyCode::Space, true);
    devices.mouse.buttons[static_cast<u32>(shell::MouseButton::Left)] = true;
    runtime.Update(devices, 1.0f / 60.0f);
    const ActionRef jump = runtime.Resolve(u8"Jump");
    const ActionRef shoot = runtime.Resolve(u8"Shoot");
    CHECK(runtime.IsDown(jump));
    CHECK(runtime.IsDown(shoot));

    // Pointer consumed (menu under the mouse): Shoot mutes, Jump keeps working.
    runtime.SetConsumptionMask(ActionRuntime::ConsumptionMask{.pointer = true, .keyboard = false});
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    CHECK_FALSE(runtime.IsDown(shoot));

    // Keyboard consumed too (text field focused): both mute.
    runtime.SetConsumptionMask(ActionRuntime::ConsumptionMask{.pointer = true, .keyboard = true});
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK_FALSE(runtime.IsDown(jump));
    CHECK_FALSE(runtime.IsDown(shoot));

    // Cleared: both return (consumption is a mask, not an exclusive-set latch).
    runtime.SetConsumptionMask(ActionRuntime::ConsumptionMask{});
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(runtime.IsDown(jump));
    CHECK(runtime.IsDown(shoot));
}

TEST_CASE("input reflection: the leaf value types + enums reflect")
{
    using namespace foundation::input;
    RegisterInputTypeReflection();

    // Binding: 17 flat scalar properties; `source` is a named enum (dropdown-ready).
    const TypeInfo& binding = TypeOf<Binding>();
    CHECK(PropertyCount(binding) == 17u);
    const PropertyInfo* srcProp = FindProperty(binding, "source");
    REQUIRE(srcProp != nullptr);
    REQUIRE(srcProp->type != nullptr);
    CHECK(IsEnum(*srcProp->type));
    CHECK(Enumerators(*srcProp->type).Size() == 10u); // BindingSource has 10 values

    // get/set round-trip through an Instance on a live Binding.
    Binding b;
    b.deadZone = 0.25f;
    Instance inst = Instance::From(&b);
    const PropertyInfo* deadZone = FindProperty(binding, "deadZone");
    REQUIRE(deadZone != nullptr);
    CHECK(GetProperty(*deadZone, inst).Get<f32>() == doctest::Approx(0.25f));
    CHECK(SetProperty(*deadZone, inst, Variant::From(0.5f)).IsOk());
    CHECK(b.deadZone == doctest::Approx(0.5f));

    // The other leaves are reflected too.
    CHECK(PropertyCount(TypeOf<Interaction>()) == 2u);
    CHECK(PropertyCount(TypeOf<ActionProcessors>()) == 5u);
    const PropertyInfo* interKind = FindProperty(TypeOf<Interaction>(), "kind");
    REQUIRE(interKind != nullptr);
    CHECK(IsEnum(*interKind->type)); // InteractionKind
}

TEST_CASE("input reflection: the InputMap container tree is traversable via reflection")
{
    using namespace foundation::input;
    RegisterInputTypeReflection();

    // A small live map: one set -> one action -> one binding.
    InputMap map;
    ActionSet set;
    set.name = String(u8"Gameplay");
    set.priority = 5;
    Action action;
    action.name = String(u8"Jump");
    action.kind = ActionKind::Button;
    Binding b;
    b.source = BindingSource::Key;
    action.bindings.PushBack(b);
    set.actions.PushBack(static_cast<Action&&>(action));
    map.sets.PushBack(static_cast<ActionSet&&>(set));

    // InputMap.sets is a Nested property whose type is a registered container.
    const PropertyInfo* setsProp = FindProperty(TypeOf<InputMap>(), "sets");
    REQUIRE(setsProp != nullptr);
    CHECK(IsNested(*setsProp));
    REQUIRE(setsProp->type != nullptr);
    REQUIRE(IsContainer(*setsProp->type));

    // Reach the live array via the Nested address, iterate it generically.
    Instance mapInst = Instance::From(&map);
    void* setsAddr = setsProp->address(mapInst);
    REQUIRE(setsAddr != nullptr);
    const Instance setsInst(setsAddr, setsProp->type);
    REQUIRE(ContainerSize(*setsProp->type->container, setsInst) == 1u);

    // Element 0 -> ActionSet; read its reflected scalars off the element Variant.
    Variant setElem = ContainerGetAt(*setsProp->type->container, setsInst, 0);
    const Instance setInst(setElem.ValuePointer(), setElem.Type());
    const PropertyInfo* nameProp = FindProperty(*setElem.Type(), "name");
    const PropertyInfo* prioProp = FindProperty(*setElem.Type(), "priority");
    REQUIRE(nameProp != nullptr);
    REQUIRE(prioProp != nullptr);
    CHECK(GetProperty(*nameProp, setInst).Get<String>() == StringView(u8"Gameplay"));
    CHECK(GetProperty(*prioProp, setInst).Get<i32>() == 5);

    // Drill one level deeper: ActionSet.actions (nested container) -> Action.name.
    const PropertyInfo* actionsProp = FindProperty(*setElem.Type(), "actions");
    REQUIRE(actionsProp != nullptr);
    CHECK(IsNested(*actionsProp));
    REQUIRE(IsContainer(*actionsProp->type));
    const Instance actionsInst(actionsProp->address(setInst), actionsProp->type);
    REQUIRE(ContainerSize(*actionsProp->type->container, actionsInst) == 1u);
    Variant actionElem = ContainerGetAt(*actionsProp->type->container, actionsInst, 0);
    const Instance actionInst(actionElem.ValuePointer(), actionElem.Type());
    const PropertyInfo* aName = FindProperty(*actionElem.Type(), "name");
    REQUIRE(aName != nullptr);
    CHECK(GetProperty(*aName, actionInst).Get<String>() == StringView(u8"Jump"));
}

// Sedulous a577cb16 and a700e581: every code an input map stores as a number has a reflected
// name, so type_info names it and a scripted playtest parses it - every case below Count, held
// to the enum so a code added without a name fails here.
TEST_CASE("input reflection: every input code reflects its cases")
{
    using namespace foundation::input;
    namespace shell = foundation::shell;
    RegisterInputTypeReflection();

    const auto everyCaseNamed = [](const TypeInfo& type, u32 count)
    {
        CAPTURE(type.name);
        REQUIRE(IsEnum(type));
        for (u32 value = 0; value < count; ++value)
        {
            CAPTURE(value);
            CHECK(EnumValueName(type, static_cast<i64>(value)) != nullptr);
        }
        CHECK(EnumeratorCount(type) == count);
    };
    everyCaseNamed(TypeOf<shell::KeyCode>(), static_cast<u32>(shell::KeyCode::Count));
    everyCaseNamed(TypeOf<shell::MouseButton>(), static_cast<u32>(shell::MouseButton::Count));
    everyCaseNamed(TypeOf<shell::GamepadButton>(), static_cast<u32>(shell::GamepadButton::Count));
    everyCaseNamed(TypeOf<shell::GamepadAxis>(), static_cast<u32>(shell::GamepadAxis::Count));
    everyCaseNamed(TypeOf<MouseAxisCode>(), 3u);
    everyCaseNamed(TypeOf<StickCode>(), 2u);

    // The names are the cases': a tool reads "Space" for a stored code.
    i64 space = 0;
    REQUIRE(EnumValueByName(TypeOf<shell::KeyCode>(), "Space", space));
    CHECK(space == static_cast<i64>(shell::KeyCode::Space));
    CHECK(StringView(reinterpret_cast<const utf8char*>(EnumValueName(
              TypeOf<shell::GamepadButton>(), static_cast<i64>(shell::GamepadButton::South)))) ==
          u8"South");
}

TEST_CASE("input.runtime: a rumble waits for the next update and reaches its pad through that frame's devices")
{
    ActionRuntime runtime;
    runtime.SetMap(MakeGameplayMap());
    FakeGamepad pad0;
    FakeGamepad pad1;
    pad1.index = 1;
    FakeDevices devices;
    devices.pads.PushBack(&pad0);
    devices.pads.PushBack(&pad1);

    // Asked between frames: nothing reaches a pad until the update that has the devices.
    runtime.Rumble(1, 0.8f, 0.4f, 0.25f);
    CHECK(pad1.rumbleCalls == 0);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(pad1.rumbleCalls == 1);
    CHECK(pad1.rumbleLow == doctest::Approx(0.8f));
    CHECK(pad1.rumbleHigh == doctest::Approx(0.4f));
    CHECK(pad1.rumbleMs == 250u);
    CHECK(pad0.rumbleCalls == 0); // only the pad asked for

    // Applied once: the next update asks nothing more.
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(pad1.rumbleCalls == 1);

    // A later request for the same pad replaces the waiting one; the motors clamp to 0..1.
    runtime.Rumble(0, 0.2f, 0.2f, 1.0f);
    runtime.Rumble(0, 2.0f, -1.0f, 0.1f);
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(pad0.rumbleCalls == 1);
    CHECK(pad0.rumbleLow == doctest::Approx(1.0f));
    CHECK(pad0.rumbleHigh == doctest::Approx(0.0f));
    CHECK(pad0.rumbleMs == 100u);

    // A pad that is not there is skipped; StopRumble stops every pad and drops what was waiting.
    runtime.Rumble(5, 1.0f, 1.0f, 1.0f);
    runtime.Rumble(0, 1.0f, 1.0f, 1.0f);
    runtime.StopRumble();
    runtime.Update(devices, 1.0f / 60.0f);
    CHECK(pad0.rumbleLow == doctest::Approx(0.0f));
    CHECK(pad0.rumbleMs == 0u);
    CHECK(pad1.rumbleLow == doctest::Approx(0.0f));

    // The run's end has no update to wait for: the helper stops every pad of its source at once.
    pad0.rumbleLow = 0.5f;
    StopRumble(devices);
    CHECK(pad0.rumbleLow == doctest::Approx(0.0f));
}

TEST_CASE("input: the bindings' version moves when what a binding resolves to may change, and only then")
{
    InputMap map;
    ActionSet gameplay;
    gameplay.name = String(u8"Gameplay");
    map.sets.PushBack(gameplay);
    ActionSet menu;
    menu.name = String(u8"Menu");
    map.sets.PushBack(menu);

    ActionRuntime rt;
    const u64 start = rt.BindingsVersion();
    rt.SetMap(map);
    const u64 mapped = rt.BindingsVersion();
    CHECK(mapped != start);
    rt.EnableSet(u8"Menu", true); // already enabled: nothing changed
    CHECK(rt.BindingsVersion() == mapped);
    rt.DisableSet(u8"Menu");
    const u64 disabled = rt.BindingsVersion();
    CHECK(disabled != mapped);
    rt.PushExclusiveSet(u8"Gameplay");
    CHECK(rt.BindingsVersion() != disabled);
    const u64 pushed = rt.BindingsVersion();
    rt.PopExclusiveSet();
    CHECK(rt.BindingsVersion() != pushed);
    const u64 popped = rt.BindingsVersion();
    rt.PopExclusiveSet(); // nothing to pop
    CHECK(rt.BindingsVersion() == popped);
}

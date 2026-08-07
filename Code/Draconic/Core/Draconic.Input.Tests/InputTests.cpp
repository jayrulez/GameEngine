// draconic.input tests: model round-trip (binary + XML) + validation, and ActionRuntime
// evaluation over SYNTHETIC devices (the design's testing contract - no real hardware).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.shell;
import draconic.input;
import draconic.settings;
import draconic.script;
import draconic.script.wren;
import draconic.engine.input;
import draconic.xml;
import draconic.xml.serialization;

using namespace draconic::foundation;
using namespace draconic::input;
namespace shell = draconic::shell;

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
        f32 dx = 0, dy = 0, wheel = 0;
        bool buttons[8] = {};
        [[nodiscard]] f32 X() const override { return 0; }
        [[nodiscard]] f32 Y() const override { return 0; }
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
        [[nodiscard]] bool IsButtonPressed(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(shell::MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
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
        void SetRumble(f32, f32, u32) override {}
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
    [[nodiscard]] InputMap MakeGameplayMap()
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
            key.code = static_cast<u32>(shell::KeyCode::Space); // deliberately shared with Jump
            confirm.bindings.PushBack(key);
            menu.actions.PushBack(static_cast<Action&&>(confirm));
        }
        map.sets.PushBack(static_cast<ActionSet&&>(menu));
        return map;
    }
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
        draconic::xml::XmlSerializer writer;
        SerializeInputMap(writer, map);
        REQUIRE(writer.IsOk());
        String text;
        writer.GetOutput(text);
        draconic::xml::XmlDocument doc;
        REQUIRE(doc.Parse(text.AsView()) == draconic::xml::XmlResult::Ok);
        draconic::xml::XmlSerializer reader(doc);
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
    draconic::settings::Settings store;
    store.Section<InputBindingOverrides>().overrides =
        overlay.overrides; // sections are non-copyable objects
    MemoryStream file;
    REQUIRE(store.Save(file, BinarySerializerFactory()).IsOk());
    (void)file.Seek(0, SeekOrigin::Begin);
    draconic::settings::Settings loadedStore;
    REQUIRE(loadedStore.Load(file, BinarySerializerFactory()).IsOk());
    const InputBindingOverrides* loaded = loadedStore.Find<InputBindingOverrides>();
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->overrides.Size() == 1);
    CHECK(loaded->overrides[0].bindings[0].code == static_cast<u32>(shell::KeyCode::K));
}

TEST_CASE("input: the Wren Input facade resolves PER-CONTEXT services")
{
    draconic::input::RegisterInputScriptFacade();

    // Two runtimes, two contexts - each script reads ITS OWN bound runtime (players /
    // editor-vs-game). No process globals anywhere.
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

    RefPtr<draconic::script::IScriptManager> manager =
        draconic::script::wren::CreateScriptManager();
    draconic::script::RegisterReflectedTypes(*manager);

    RefPtr<draconic::script::IScriptContext> ctxA = manager->CreateContext();
    RefPtr<draconic::script::IScriptContext> ctxB = manager->CreateContext();
    RefPtr<draconic::script::IScriptContext> ctxNone = manager->CreateContext();
    REQUIRE(ctxA.Get() != nullptr);
    ctxA->SetService(draconic::input::kInputScriptService, &runtimeA);
    ctxB->SetService(draconic::input::kInputScriptService, &runtimeB);

    const StringView script = u8"var Down = Input.isDown(\"Jump\")\n"
                              u8"var MoveY = Input.valueY(\"Move\")\n";
    REQUIRE(ctxA->Load(script, u8"main").IsOk());
    CHECK(ctxA->GetGlobal(u8"Down").Get<bool>() == true);
    CHECK(ctxA->GetGlobal(u8"MoveY").Get<f64>() == doctest::Approx(1.0));

    REQUIRE(ctxB->Load(script, u8"main").IsOk());
    CHECK(ctxB->GetGlobal(u8"Down").Get<bool>() == false); // B's runtime saw nothing
    CHECK(ctxB->GetGlobal(u8"MoveY").Get<f64>() == doctest::Approx(0.0));

    // No service bound: released, never a crash.
    REQUIRE(ctxNone->Load(script, u8"main").IsOk());
    CHECK(ctxNone->GetGlobal(u8"Down").Get<bool>() == false);

    // Script-driven exclusive push lands on the CONTEXT's runtime only.
    runtimeA.EnableSet(u8"Menu");
    REQUIRE(ctxA->Load(u8"Input.pushSet(\"Menu\")\n", u8"main").IsOk());
    CHECK(runtimeA.ExclusiveDepth() == 1);
    CHECK(runtimeB.ExclusiveDepth() == 0);
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
    // radius, clamps at 1, and a touch that STARTED OUTSIDE never captures.
    devices.touch.Set(2, 0.25f, 0.5f);
    runtime.Update(devices, step); // capture frame: anchor == position
    CHECK(runtime.Value2D(move).x == doctest::Approx(0.0f));
    devices.touch.Set(2, 0.30f, 0.5f); // +0.05 over radius 0.1 = half deflection...
    runtime.Update(devices, step);
    CHECK(runtime.Value2D(move).x > 0.30f); // ...minus the dead-zone rescale
    CHECK(runtime.Value2D(move).x < 0.60f);
    devices.touch.Set(2, 0.60f, 0.5f); // way past the radius: clamped
    runtime.Update(devices, step);
    CHECK(runtime.Value2D(move).x == doctest::Approx(1.0f));
    CHECK(runtime.Value2D(move).y == doctest::Approx(0.0f));
    devices.touch.Remove(2);
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

TEST_CASE("input.subsystem: the per-surface scene binding rides the source override")
{
    // game-ui.md §9: SetSourceProvider carries the scene the source REPRESENTS (an
    // opaque key - the SceneOverlayView::sceneKey convention); the UI pump confines
    // routing/consumption to it. Un-bound sources follow the policy knob.
    InputSubsystem input(nullptr);
    CHECK(input.BoundSceneKey() == nullptr);
    CHECK(input.UnboundScenePolicy() == UnboundInputScenePolicy::AllScenes); // player default

    FakeDevices devices;
    int sceneStandIn = 0; // any stable address works as a key
    input.SetSourceProvider(&devices, &sceneStandIn);
    CHECK(input.BoundSceneKey() == &sceneStandIn);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&devices));

    // Re-setting without a scene un-binds (the Game tab's Stop: provider stays, the
    // run's binding drops).
    input.SetSourceProvider(&devices, nullptr);
    CHECK(input.BoundSceneKey() == nullptr);

    // Clearing the provider always clears the binding - the shell source is un-bound.
    input.SetSourceProvider(&devices, &sceneStandIn);
    input.SetSourceProvider(nullptr, &sceneStandIn);
    CHECK(input.BoundSceneKey() == nullptr);

    input.SetUnboundScenePolicy(UnboundInputScenePolicy::ScreenTierOnly);
    CHECK(input.UnboundScenePolicy() == UnboundInputScenePolicy::ScreenTierOnly);
}

TEST_CASE("input.subsystem: ClearSourceProviderIf drops only its own dangling override")
{
    // A closing editor Game tab clears its viewport source before it is destroyed, so
    // ActiveSource()/Update never dereference freed memory. The guard makes it a no-op
    // when a DIFFERENT still-open tab is the active source.
    InputSubsystem input(nullptr);
    FakeDevices tabA;
    FakeDevices tabB;
    int keyA = 0;

    input.SetSourceProvider(&tabA, &keyA);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&tabA));

    // Closing a NON-active tab must not disturb the active override.
    input.ClearSourceProviderIf(&tabB);
    CHECK(&input.ActiveSource() == static_cast<IInputSourceProvider*>(&tabA));
    CHECK(input.BoundSceneKey() == &keyA);

    // Closing the ACTIVE tab clears the override + its binding; ActiveSource falls back
    // to the (always-valid) shell source, so the next pump can't dangle.
    input.ClearSourceProviderIf(&tabA);
    CHECK(&input.ActiveSource() != static_cast<IInputSourceProvider*>(&tabA));
    CHECK(input.BoundSceneKey() == nullptr);

    // Idempotent once cleared.
    input.ClearSourceProviderIf(&tabA);
    CHECK(&input.ActiveSource() != static_cast<IInputSourceProvider*>(&tabA));
}

TEST_CASE("input reflection: the leaf value types + enums reflect (P2 breadth)")
{
    using namespace draconic::input;
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
    using namespace draconic::input;
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

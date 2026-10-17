// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - GuiInputBridge tests: translate synthetic platform InputEvents into
// EventDispatcher injections (hover/click/key/text), map enums, and apply a ContentFit.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.shell;
import foundation.shell.null;
import experimental.gui;
import experimental.gui.shell;
import foundation.input; // InputMap (a keymap filled from it)

using namespace experimental::gui;
namespace core = foundation::core;
namespace shell = foundation::shell;

namespace
{
    core::RefPtr<SceneRoot> MakeScene(core::Float2 size)
    {
        auto s = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        s->SetSize(size);
        return s;
    }
    core::RefPtr<Node> MakePanel(core::Float2 pos, core::Float2 size)
    {
        auto n = core::MakeRef<Node>(core::DefaultAllocator());
        n->SetSize(size);
        n->SetPosition(pos);
        return n;
    }

    shell::InputEvent MouseMove(float x, float y)
    {
        shell::InputEvent e;
        e.kind = shell::InputEventKind::MouseMove;
        e.x = x;
        e.y = y;
        return e;
    }
    shell::InputEvent MakeButtonEvent(shell::InputEventKind kind, float x, float y,
                                      shell::MouseButton button,
                                      shell::KeyModifiers mods = shell::KeyModifiers::None)
    {
        shell::InputEvent e;
        e.kind = kind;
        e.x = x;
        e.y = y;
        e.button = button;
        e.modifiers = mods;
        return e;
    }
    shell::InputEvent Key(shell::InputEventKind kind, shell::KeyCode key,
                          shell::KeyModifiers mods = shell::KeyModifiers::None)
    {
        shell::InputEvent e;
        e.kind = kind;
        e.key = key;
        e.modifiers = mods;
        return e;
    }
    shell::InputEvent MakeTextEvent(const char8_t* s)
    {
        shell::InputEvent e;
        e.kind = shell::InputEventKind::TextInput;
        core::usize i = 0;
        for (; s[i] != 0 && i < 31; ++i)
            e.text[i] = s[i];
        e.text[i] = 0;
        return e;
    }
}

TEST_CASE("bridge: mouse move drives hover")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{10.0f, 10.0f}, core::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    int enter = 0;
    child->AddEventListener(EventType::MouseEnter, [&](const Event&) { ++enter; });

    CHECK_FALSE(bridge.Dispatch(MouseMove(20.0f, 20.0f))); // a bare panel leaves the pointer to the game
    CHECK(root->GetEventDispatcher()->GetOverNode() == child.Get());
    CHECK(enter == 1);
}

TEST_CASE("bridge: left press+release becomes a click; button mapping is explicit")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{0.0f, 0.0f}, core::Float2{100.0f, 100.0f});
    child->SetTabFocusable(true); // a click focuses what takes focus
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    int clicks = 0;
    experimental::gui::MouseButton downButton = experimental::gui::MouseButton::Left;
    child->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });
    child->AddEventListener(EventType::MouseDown, [&](const Event& e)
                            { downButton = static_cast<const MouseEvent&>(e).Button; });

    // Only the left button activates (right/middle deliver Down/Up for context menus but
    // never click - see the EventDispatcher interaction rules).
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 20.0f, 20.0f,
                                    shell::MouseButton::Left));
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonUp, 20.0f, 20.0f,
                                    shell::MouseButton::Left));
    CHECK(clicks == 1);
    CHECK(root->GetEventDispatcher()->GetFocusNode() == child.Get());

    // shell Middle (index 1) maps to gui Middle (index 2) - explicitly, not by value.
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 20.0f, 20.0f,
                                    shell::MouseButton::Middle));
    CHECK(downButton == experimental::gui::MouseButton::Middle);
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonUp, 20.0f, 20.0f,
                                    shell::MouseButton::Middle));
    CHECK(clicks == 1); // still 1: the middle press did not add a click
}

TEST_CASE("bridge: key and text route to the focus node with mapped modifiers")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{0.0f, 0.0f}, core::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};
    root->GetEventDispatcher()->SetFocusNode(child.Get());

    unsigned seenMods = 0;
    // Copy the text: the event's Text views the dispatched InputEvent's stack buffer,
    // which is gone by the time the CHECK below runs.
    core::String seenText;
    child->AddEventListener(EventType::KeyDown, [&](const Event& e)
                            { seenMods = static_cast<const KeyEvent&>(e).Modifiers; });
    child->AddEventListener(EventType::TextInput, [&](const Event& e)
                            { seenText = core::String{static_cast<const TextInputEvent&>(e).Text}; });

    bridge.Dispatch(
        Key(shell::InputEventKind::KeyDown, shell::KeyCode::A, shell::KeyModifiers::LeftCtrl));
    bridge.Dispatch(MakeTextEvent(u8"hi"));
    CHECK((seenMods & KeyModCtrl) != 0u);
    CHECK(seenText.AsView() == core::StringView(u8"hi"));
}

TEST_CASE("bridge: navigation keys map platform KeyCode to the GUI KeyCode")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{0.0f, 0.0f}, core::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};
    root->GetEventDispatcher()->SetFocusNode(child.Get());

    core::u32 seenKey = 0xFFFFFFFFu;
    child->AddEventListener(EventType::KeyDown, [&](const Event& e)
                            { seenKey = static_cast<const KeyEvent&>(e).KeyCode; });

    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Backspace));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::Backspace));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Left));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::Left));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Home));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::Home));

    // The editing-shortcut letters map through so Ctrl+A/C/V/X reach widgets.
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::A));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::A));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::V));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::V));

    // Every letter, digit and F1 to F12 maps through, for bindings (G7: ctrl-s, f5); what a key
    // types still arrives as text, not from its code.
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::B));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::B));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Num7));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::Num7));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::F5));
    CHECK(seenKey == static_cast<core::u32>(KeyCode::F5));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::F13)); // past what bindings name
    CHECK(seenKey == static_cast<core::u32>(KeyCode::Unknown));
}

TEST_CASE("bridge: text-input target follows focus of an editable widget")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto field = core::MakeRef<TextField>(core::DefaultAllocator());
    field->SetSize(core::Float2{100.0f, 24.0f}); // covers (0,0)-(100,24)
    root->AddChild(field.Get());

    shell::NullWindow window(1u, shell::WindowSettings{});
    GuiInputBridge bridge{root->GetEventDispatcher()};
    bridge.SetTextInputTarget(&window);

    CHECK_FALSE(window.IsTextInputActive());

    // Click the field -> focus -> bridge enables the window's text input.
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 10.0f, 10.0f,
                                    shell::MouseButton::Left));
    CHECK(root->GetEventDispatcher()->GetFocusNode() == field.Get());
    CHECK(window.IsTextInputActive());

    // Click off the field (onto the non-editable root) -> focus leaves -> text input disabled.
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 150.0f, 150.0f,
                                    shell::MouseButton::Left));
    CHECK(root->GetEventDispatcher()->GetFocusNode() != field.Get());
    CHECK_FALSE(window.IsTextInputActive());
}

TEST_CASE("bridge: content fit maps window position into content space")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{10.0f, 10.0f}, core::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    core::ContentFit fit;
    fit.region = core::Rectangle{0.0f, 0.0f, 400.0f, 400.0f};
    fit.contentSize = core::Float2{200.0f, 200.0f};
    fit.mode = core::FitMode::Stretch;
    bridge.SetContentFit(fit);

    bridge.Dispatch(MouseMove(40.0f, 40.0f)); // window (40,40) -> content (20,20), inside child
    CHECK(root->GetEventDispatcher()->GetOverNode() == child.Get());
    CHECK(root->GetEventDispatcher()->GetMousePosition().x == doctest::Approx(20.0f));
}

TEST_CASE("bridge: unroutable events return false")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    GuiInputBridge bridge{root->GetEventDispatcher()};

    shell::InputEvent touch; // touch is not routed to the GUI yet (gamepads are, since G7)
    touch.kind = shell::InputEventKind::TouchDown;
    CHECK_FALSE(bridge.Dispatch(touch));
    shell::InputEvent paddle; // a gamepad button the GUI does not take
    paddle.kind = shell::InputEventKind::GamepadButtonDown;
    paddle.padButton = shell::GamepadButton::LeftPaddle1;
    CHECK_FALSE(bridge.Dispatch(paddle));
}

TEST_CASE("ShellClipboard: adapts the shell clipboard to gui::IClipboard and round-trips text")
{
    shell::NullShell shell; // in-memory clipboard backing
    ShellClipboard clipboard{&shell};

    CHECK_FALSE(clipboard.HasText());
    clipboard.SetText(core::StringView(u8"copied"));
    CHECK(clipboard.HasText());
    CHECK(clipboard.GetText() == core::StringView(u8"copied"));
    CHECK(shell.GetClipboardText() == core::StringView(u8"copied")); // reached the shell

    // A null shell degrades safely (no crash, empty text).
    ShellClipboard none{nullptr};
    CHECK_FALSE(none.HasText());
    CHECK(none.GetText().Size() == 0);
    none.SetText(core::StringView(u8"ignored"));
}

// --- Minimal settable input mocks for the poll-based PumpFromSurface path ---
namespace
{
    class MockKeyboard final : public shell::IKeyboard
    {
    public:
        shell::KeyModifiers mods = shell::KeyModifiers::None;
        bool IsKeyDown(shell::KeyCode) const override { return false; }
        bool IsKeyPressed(shell::KeyCode) const override { return false; }
        bool IsKeyReleased(shell::KeyCode) const override { return false; }
        shell::KeyModifiers Modifiers() const override { return mods; }
    };

    class MockMouse final : public shell::IMouse
    {
    public:
        bool leftPressed = false;
        core::f32 X() const override { return 0.0f; }
        core::f32 Y() const override { return 0.0f; }
        core::f32 GlobalX() const override { return 0.0f; }
        core::f32 GlobalY() const override { return 0.0f; }
        core::f32 DeltaX() const override { return 0.0f; }
        core::f32 DeltaY() const override { return 0.0f; }
        core::f32 ScrollX() const override { return 0.0f; }
        core::f32 ScrollY() const override { return 0.0f; }
        bool IsButtonDown(shell::MouseButton) const override { return false; }
        bool IsButtonPressed(shell::MouseButton b) const override
        {
            return b == shell::MouseButton::Left && leftPressed;
        }
        bool IsButtonReleased(shell::MouseButton) const override { return false; }
        bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(shell::CursorType c) override
        {
            cursor = c;
            ++cursorSets;
        }
        void SetGlobalCapture(bool) override {}
        shell::CursorType cursor = shell::CursorType::Default;
        int cursorSets = 0;
    };

    class MockInputManager final : public shell::IInputManager
    {
    public:
        MockKeyboard keyboard;
        MockMouse mouse;
        shell::IKeyboard* Keyboard() override { return &keyboard; }
        shell::IMouse* Mouse() override { return &mouse; }
        shell::ITouch* Touch() override { return nullptr; }
        core::i32 GamepadCount() const override { return 0; }
        shell::IGamepad* GetGamepad(core::i32) override { return nullptr; }
        core::Span<const shell::InputEvent> Events() const override { return {}; }
        core::u32 HoverWindow() const override { return 0; }
        core::u32 FocusedWindow() const override { return 0; }
        void Update() override {}
    };
}

TEST_CASE("bridge: PumpFromSurface carries keyboard modifiers into a click (Shift+click)")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto child = MakePanel(core::Float2{0.0f, 0.0f}, core::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    unsigned seenMods = 0;
    child->AddEventListener(EventType::MouseDown, [&](const Event& e)
                            { seenMods = static_cast<const MouseEvent&>(e).Modifiers; });

    MockInputManager manager;
    manager.keyboard.mods = shell::KeyModifiers::LeftShift;
    manager.mouse.leftPressed = true;

    shell::InputSurface surface{&manager, 1u, core::ContentFit{}};
    // Gate the surface active (hovered + focused) with the content cursor over the child.
    surface.ApplyGate(true, true, false, core::Float2{10.0f, 10.0f}, core::Float2{0.0f, 0.0f});

    bridge.PumpFromSurface(surface);
    CHECK((seenMods & KeyModShift) != 0u); // Shift reached the widget via the poll path
}

namespace
{
    shell::InputEvent Pad(shell::InputEventKind kind, shell::GamepadButton button)
    {
        shell::InputEvent e;
        e.kind = kind;
        e.padButton = button;
        return e;
    }
    shell::InputEvent Stick(shell::GamepadAxis axis, float value)
    {
        shell::InputEvent e;
        e.kind = shell::InputEventKind::GamepadAxis;
        e.padAxis = axis;
        e.value = value;
        return e;
    }
}

TEST_CASE("bridge: the d-pad and the left stick move focus; a button runs its binding")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto top = core::MakeRef<Button>(core::DefaultAllocator());
    auto bottom = core::MakeRef<Button>(core::DefaultAllocator());
    top->SetSize(core::Float2{60.0f, 20.0f});
    bottom->SetPosition(core::Float2{0.0f, 50.0f});
    bottom->SetSize(core::Float2{60.0f, 20.0f});
    root->AddChild(top.Get());
    root->AddChild(bottom.Get());
    EventDispatcher& d = *root->GetEventDispatcher();
    GuiInputBridge bridge{&d};
    d.SetFocusNode(top.Get());

    CHECK(bridge.Dispatch(Pad(shell::InputEventKind::GamepadButtonDown, shell::GamepadButton::DPadDown)));
    CHECK(d.GetFocusNode() == bottom.Get());
    CHECK(d.IsHoldingDirection());
    CHECK(bridge.Dispatch(Pad(shell::InputEventKind::GamepadButtonUp, shell::GamepadButton::DPadDown)));
    CHECK_FALSE(d.IsHoldingDirection());

    // The stick: past half way holds Up, a resting wobble near the threshold does not chatter.
    CHECK(bridge.Dispatch(Stick(shell::GamepadAxis::LeftY, -0.8f)));
    CHECK(d.GetFocusNode() == top.Get());
    (void)bridge.Dispatch(Stick(shell::GamepadAxis::LeftY, -0.45f)); // still held (above release)
    CHECK(d.IsHoldingDirection());
    (void)bridge.Dispatch(Stick(shell::GamepadAxis::LeftY, -0.1f));
    CHECK_FALSE(d.IsHoldingDirection());

    // East through a binding.
    bool back = false;
    (void)root->OnCommand(core::StringView(u8"back"), [&](CommandEvent&) { back = true; });
    REQUIRE(root->GetContext().GetKeymap().Bind(core::StringView(u8"pad-east"), core::StringView(u8"back")));
    CHECK(bridge.Dispatch(Pad(shell::InputEventKind::GamepadButtonDown, shell::GamepadButton::East)));
    CHECK(back);
}

TEST_CASE("bridge: a keymap filled from the game's input map follows its bindings")
{
    namespace input = foundation::input;
    input::InputMap map;
    input::ActionSet menu;
    menu.name = core::String(u8"Menu");
    input::Action backAction;
    backAction.name = core::String(u8"Back");
    input::Binding escape;
    escape.source = input::BindingSource::Key;
    escape.code = static_cast<core::u32>(shell::KeyCode::Escape);
    input::Binding east;
    east.source = input::BindingSource::GamepadButton;
    east.code = static_cast<core::u32>(shell::GamepadButton::East);
    input::Binding stick; // an axis: not a keystroke, skipped
    stick.source = input::BindingSource::GamepadAxis;
    backAction.bindings.PushBack(escape);
    backAction.bindings.PushBack(east);
    backAction.bindings.PushBack(stick);
    menu.actions.PushBack(backAction);
    map.sets.PushBack(menu);

    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    const InputMapCommand wanted[] = {{core::StringView(u8"Back"), core::StringView(u8"back")}};
    CHECK(BindFromInputMap(root->GetContext().GetKeymap(), map, core::Span<const InputMapCommand>(wanted, 1)) == 2u);
    int backs = 0;
    (void)root->OnCommand(core::StringView(u8"back"), [&](CommandEvent&) { ++backs; });
    EventDispatcher& d = *root->GetEventDispatcher();
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    d.InjectPadButton(PadButton::East);
    CHECK(backs == 2);
}

TEST_CASE("bridge: the cursor follows the node under the pointer; Dispatch says whether the GUI used it")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto field = core::MakeRef<TextField>(core::DefaultAllocator());
    field->SetSize(core::Float2{100.0f, 24.0f});
    root->AddChild(field.Get());
    field->SetCursor(Cursor::Text);
    GuiInputBridge bridge(root->GetEventDispatcher());
    MockMouse mouse;
    bridge.SetCursorTarget(&mouse);
    CHECK(bridge.Dispatch(MouseMove(10.0f, 10.0f))); // over the field: the GUI's
    CHECK(mouse.cursor == shell::CursorType::Text);
    CHECK_FALSE(bridge.Dispatch(MouseMove(150.0f, 150.0f))); // over nothing that takes it
    CHECK(mouse.cursor == shell::CursorType::Default);
    const int sets = mouse.cursorSets;
    (void)bridge.Dispatch(MouseMove(160.0f, 150.0f));
    CHECK(mouse.cursorSets == sets); // unchanged: not set again
    CHECK_FALSE(bridge.IsPointerOverGui());
}

namespace
{
    shell::InputEvent Finger(shell::InputEventKind kind, core::u64 id, float nx, float ny)
    {
        shell::InputEvent e;
        e.kind = kind;
        e.touchId = id;
        e.x = nx;
        e.y = ny;
        return e;
    }
    class MockTouch final : public shell::ITouch
    {
    public:
        core::Array<shell::TouchPoint> points;
        core::i32 TouchCount() const override { return static_cast<core::i32>(points.Size()); }
        bool GetTouchPoint(core::i32 index, shell::TouchPoint& out) const override
        {
            if (index < 0 || index >= TouchCount())
                return false;
            out = points[static_cast<core::usize>(index)];
            return true;
        }
        bool HasTouch() const override { return !points.IsEmpty(); }
    };
    class TouchInputManager final : public shell::IInputManager
    {
    public:
        MockKeyboard keyboard;
        MockMouse mouse;
        MockTouch touch;
        shell::IKeyboard* Keyboard() override { return &keyboard; }
        shell::IMouse* Mouse() override { return &mouse; }
        shell::ITouch* Touch() override { return &touch; }
        core::i32 GamepadCount() const override { return 0; }
        shell::IGamepad* GetGamepad(core::i32) override { return nullptr; }
        core::Span<const shell::InputEvent> Events() const override { return {}; }
        core::u32 HoverWindow() const override { return 0; }
        core::u32 FocusedWindow() const override { return 0; }
        void Update() override {}
    };
}

TEST_CASE("bridge: touch events tap a button, placed with the window's size; SDL's mouse copies are dropped")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto button = core::MakeRef<Button>(core::DefaultAllocator());
    button->SetPosition(core::Float2{20.0f, 20.0f});
    button->SetSize(core::Float2{80.0f, 40.0f});
    root->AddChild(button.Get());
    int clicks = 0;
    button->SetOnClick([&] { ++clicks; });
    GuiInputBridge bridge{root->GetEventDispatcher()};
    CHECK_FALSE(bridge.Dispatch(Finger(shell::InputEventKind::TouchDown, 3u, 0.2f, 0.2f))); // no window size yet
    bridge.SetWindowSize(core::Float2{200.0f, 200.0f});
    CHECK(bridge.Dispatch(Finger(shell::InputEventKind::TouchDown, 3u, 0.2f, 0.2f))); // (40, 40)
    // SDL's copy of the finger as the mouse: ignored while it is down...
    CHECK_FALSE(bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 40.0f, 40.0f, shell::MouseButton::Left)));
    CHECK(bridge.Dispatch(Finger(shell::InputEventKind::TouchUp, 3u, 0.2f, 0.2f)));
    CHECK(clicks == 1);
    // ...and after it, at the finger's last place.
    CHECK_FALSE(bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonUp, 40.0f, 40.0f, shell::MouseButton::Left)));
    CHECK_FALSE(bridge.Dispatch(MouseMove(40.0f, 40.0f)));
    CHECK_FALSE(button->IsHovered()); // no hover left behind by the copy
    CHECK(clicks == 1);
    CHECK(bridge.Dispatch(MouseMove(60.0f, 50.0f))); // the real mouse, elsewhere
    CHECK(button->IsHovered());
}

TEST_CASE("bridge: PumpFromSurface forwards the polled finger: down when it appears, up when it is gone")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto button = core::MakeRef<Button>(core::DefaultAllocator());
    button->SetSize(core::Float2{100.0f, 100.0f});
    root->AddChild(button.Get());
    int clicks = 0;
    button->SetOnClick([&] { ++clicks; });
    GuiInputBridge bridge{root->GetEventDispatcher()};
    TouchInputManager manager;
    shell::InputSurface surface{&manager, 1u,
                                core::ContentFit{core::Rectangle{0.0f, 0.0f, 200.0f, 200.0f}, core::Float2{200.0f, 200.0f}, core::FitMode::Stretch}};
    surface.SetWindowSize(core::Float2{200.0f, 200.0f});
    manager.touch.points.PushBack(shell::TouchPoint{9u, 0.25f, 0.25f, 1.0f, 50.0f, 50.0f});
    bridge.PumpFromSurface(surface);
    CHECK(root->GetEventDispatcher()->IsTouchActive());
    CHECK(button->IsPressed());
    manager.touch.points.Clear();
    bridge.PumpFromSurface(surface);
    CHECK_FALSE(root->GetEventDispatcher()->IsTouchActive());
    CHECK(clicks == 1);
}

TEST_CASE("bridge: an IME's composition reaches the focused field, and the window learns where it types")
{
    auto root = MakeScene(core::Float2{200.0f, 200.0f});
    auto field = core::MakeRef<TextField>(core::DefaultAllocator());
    field->SetPosition(core::Float2{20.0f, 40.0f});
    field->SetSize(core::Float2{100.0f, 24.0f});
    root->AddChild(field.Get());
    shell::NullWindow window(1u, shell::WindowSettings{});
    GuiInputBridge bridge{root->GetEventDispatcher()};
    bridge.SetTextInputTarget(&window);
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonDown, 30.0f, 50.0f, shell::MouseButton::Left));
    REQUIRE(field->IsFocused());
    // Where the field types: its line, in window coordinates.
    CHECK(window.LastTextInputArea().x == 20);
    CHECK(window.LastTextInputArea().y >= 40);
    CHECK(window.LastTextInputArea().y < 64);

    shell::InputEvent editing{};
    editing.kind = shell::InputEventKind::TextEditing;
    shell::CopyEventText(editing.text, "nihon");
    editing.editCursor = 5;
    CHECK(bridge.Dispatch(editing));
    CHECK(field->GetText().Size() == 0); // shown, not yet the value

    shell::InputEvent committed{};
    committed.kind = shell::InputEventKind::TextInput;
    shell::CopyEventText(committed.text, "\xe6\x97\xa5\xe6\x9c\xac"); // the committed text
    bridge.Dispatch(committed);
    CHECK(field->GetText() == core::StringView(u8"\u65e5\u672c"));
}

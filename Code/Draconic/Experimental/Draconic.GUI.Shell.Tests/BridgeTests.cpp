// Draconic GUI - GuiInputBridge tests: translate synthetic platform InputEvents into
// EventDispatcher injections (hover/click/key/text), map enums, and apply a ContentFit.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.shell;
import draconic.shell.null;
import draconic.gui;
import draconic.gui.shell;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace shell = draconic::shell;

namespace
{
    foundation::RefPtr<SceneNode> MakeScene(foundation::Float2 size)
    {
        auto s = foundation::MakeRef<SceneNode>(foundation::DefaultAllocator());
        s->SetSize(size);
        return s;
    }
    foundation::RefPtr<Node> MakePanel(foundation::Float2 pos, foundation::Float2 size)
    {
        auto n = foundation::MakeRef<Node>(foundation::DefaultAllocator());
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
        foundation::usize i = 0;
        for (; s[i] != 0 && i < 31; ++i)
            e.text[i] = s[i];
        e.text[i] = 0;
        return e;
    }
}

TEST_CASE("bridge: mouse move drives hover")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{10.0f, 10.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    int enter = 0;
    child->AddEventListener(EventType::MouseEnter, [&](const Event&) { ++enter; });

    CHECK(bridge.Dispatch(MouseMove(20.0f, 20.0f)));
    CHECK(root->GetEventDispatcher()->GetOverNode() == child.Get());
    CHECK(enter == 1);
}

TEST_CASE("bridge: left press+release becomes a click; button mapping is explicit")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    int clicks = 0;
    draconic::gui::MouseButton downButton = draconic::gui::MouseButton::Left;
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
    CHECK(downButton == draconic::gui::MouseButton::Middle);
    bridge.Dispatch(MakeButtonEvent(shell::InputEventKind::MouseButtonUp, 20.0f, 20.0f,
                                    shell::MouseButton::Middle));
    CHECK(clicks == 1); // still 1: the middle press did not add a click
}

TEST_CASE("bridge: key and text route to the focus node with mapped modifiers")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};
    root->GetEventDispatcher()->SetFocusNode(child.Get());

    unsigned seenMods = 0;
    foundation::StringView seenText;
    child->AddEventListener(EventType::KeyDown, [&](const Event& e)
                            { seenMods = static_cast<const KeyEvent&>(e).Modifiers; });
    child->AddEventListener(EventType::TextInput, [&](const Event& e)
                            { seenText = static_cast<const TextInputEvent&>(e).Text; });

    bridge.Dispatch(
        Key(shell::InputEventKind::KeyDown, shell::KeyCode::A, shell::KeyModifiers::LeftCtrl));
    bridge.Dispatch(MakeTextEvent(u8"hi"));
    CHECK((seenMods & KeyModCtrl) != 0u);
    CHECK(seenText == foundation::StringView(u8"hi"));
}

TEST_CASE("bridge: navigation keys map platform KeyCode to the GUI KeyCode")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};
    root->GetEventDispatcher()->SetFocusNode(child.Get());

    foundation::u32 seenKey = 0xFFFFFFFFu;
    child->AddEventListener(EventType::KeyDown, [&](const Event& e)
                            { seenKey = static_cast<const KeyEvent&>(e).KeyCode; });

    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Backspace));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::Backspace));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Left));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::Left));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::Home));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::Home));

    // The editing-shortcut letters (A/C/V/X) map through so Ctrl+A/C/V/X reach widgets.
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::A));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::A));
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::V));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::V));

    // A printable key with no shortcut meaning maps to Unknown (its glyph arrives as text).
    bridge.Dispatch(Key(shell::InputEventKind::KeyDown, shell::KeyCode::B));
    CHECK(seenKey == static_cast<foundation::u32>(KeyCode::Unknown));
}

TEST_CASE("bridge: text-input target follows focus of an editable widget")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto field = foundation::MakeRef<TextField>(foundation::DefaultAllocator());
    field->SetSize(foundation::Float2{100.0f, 24.0f}); // covers (0,0)-(100,24)
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
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{10.0f, 10.0f}, foundation::Float2{50.0f, 50.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    foundation::ContentFit fit;
    fit.region = foundation::Rectangle{0.0f, 0.0f, 400.0f, 400.0f};
    fit.contentSize = foundation::Float2{200.0f, 200.0f};
    fit.mode = foundation::FitMode::Stretch;
    bridge.SetContentFit(fit);

    bridge.Dispatch(MouseMove(40.0f, 40.0f)); // window (40,40) -> content (20,20), inside child
    CHECK(root->GetEventDispatcher()->GetOverNode() == child.Get());
    CHECK(root->GetEventDispatcher()->GetMousePosition().x == doctest::Approx(20.0f));
}

TEST_CASE("bridge: unroutable events return false")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    GuiInputBridge bridge{root->GetEventDispatcher()};

    shell::InputEvent gamepad;
    gamepad.kind = shell::InputEventKind::GamepadButtonDown;
    CHECK_FALSE(bridge.Dispatch(gamepad));
}

TEST_CASE("ShellClipboard: adapts the shell clipboard to gui::IClipboard and round-trips text")
{
    shell::NullShell shell; // in-memory clipboard backing
    ShellClipboard clipboard{&shell};

    CHECK_FALSE(clipboard.HasText());
    clipboard.SetText(foundation::StringView(u8"copied"));
    CHECK(clipboard.HasText());
    CHECK(clipboard.GetText() == foundation::StringView(u8"copied"));
    CHECK(shell.GetClipboardText() == foundation::StringView(u8"copied")); // reached the shell

    // A null shell degrades safely (no crash, empty text).
    ShellClipboard none{nullptr};
    CHECK_FALSE(none.HasText());
    CHECK(none.GetText().Size() == 0);
    none.SetText(foundation::StringView(u8"ignored"));
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
        foundation::f32 X() const override { return 0.0f; }
        foundation::f32 Y() const override { return 0.0f; }
        foundation::f32 GlobalX() const override { return 0.0f; }
        foundation::f32 GlobalY() const override { return 0.0f; }
        foundation::f32 DeltaX() const override { return 0.0f; }
        foundation::f32 DeltaY() const override { return 0.0f; }
        foundation::f32 ScrollX() const override { return 0.0f; }
        foundation::f32 ScrollY() const override { return 0.0f; }
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
        void SetCursor(shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    class MockInputManager final : public shell::IInputManager
    {
    public:
        MockKeyboard keyboard;
        MockMouse mouse;
        shell::IKeyboard* Keyboard() override { return &keyboard; }
        shell::IMouse* Mouse() override { return &mouse; }
        shell::ITouch* Touch() override { return nullptr; }
        foundation::i32 GamepadCount() const override { return 0; }
        shell::IGamepad* GetGamepad(foundation::i32) override { return nullptr; }
        foundation::Span<const shell::InputEvent> Events() const override { return {}; }
        foundation::u32 HoverWindow() const override { return 0; }
        foundation::u32 FocusedWindow() const override { return 0; }
        void Update() override {}
    };
}

TEST_CASE("bridge: PumpFromSurface carries keyboard modifiers into a click (Shift+click)")
{
    auto root = MakeScene(foundation::Float2{200.0f, 200.0f});
    auto child = MakePanel(foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(child.Get());
    GuiInputBridge bridge{root->GetEventDispatcher()};

    unsigned seenMods = 0;
    child->AddEventListener(EventType::MouseDown, [&](const Event& e)
                            { seenMods = static_cast<const MouseEvent&>(e).Modifiers; });

    MockInputManager manager;
    manager.keyboard.mods = shell::KeyModifiers::LeftShift;
    manager.mouse.leftPressed = true;

    shell::InputSurface surface{&manager, 1u, foundation::ContentFit{}};
    // Gate the surface active (hovered + focused) with the content cursor over the child.
    surface.ApplyGate(true, true, false, foundation::Float2{10.0f, 10.0f}, foundation::Float2{0.0f, 0.0f});

    bridge.PumpFromSurface(surface);
    CHECK((seenMods & KeyModShift) != 0u); // Shift reached the widget via the poll path
}

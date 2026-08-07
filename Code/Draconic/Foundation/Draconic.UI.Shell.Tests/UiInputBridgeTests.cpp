// Tests for UiInputBridge (draconic.ui.shell) - a Draconic reimplementation, covered per the additions
// rule. Synthetic shell::InputEvents drive a UIContext through the bridge; a mock IWindow verifies the
// focus-driven text-input (IME) sync.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.shell;
import draconic.shell;
import draconic.shell.null;

using namespace draconic::foundation;
using namespace draconic::ui;
namespace foundation = draconic::foundation;
namespace shell = draconic::shell;

namespace
{
    // A RootView-filling focusable/hittable EditText is created directly; no TestHelpers needed here.
    void SetupRoot(UIContext& ctx, RefPtr<RootView>& root)
    {
        root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
        root->ViewportSize = Float2{800, 600};
        ctx.AddRootView(root.Get());
    }

    void LayoutPass(UIContext& ctx, RootView* root)
    {
        ctx.BeginFrame(0.016f);
        ctx.UpdateRootView(root);
    }

    shell::InputEvent MouseDown(f32 x, f32 y)
    {
        shell::InputEvent e{};
        e.kind = shell::InputEventKind::MouseButtonDown;
        e.button = shell::MouseButton::Left;
        e.x = x;
        e.y = y;
        return e;
    }

    shell::InputEvent TextEvent(const char8_t* s)
    {
        shell::InputEvent e{};
        e.kind = shell::InputEventKind::TextInput;
        usize i = 0;
        for (; s[i] != 0 && i < 31; ++i)
        {
            e.text[i] = static_cast<utf8char>(s[i]);
        }
        e.text[i] = 0;
        return e;
    }

    // Minimal IWindow that records text-input state (all other members stubbed).
    class MockWindow final : public shell::IWindow
    {
    public:
        bool active = false;
        [[nodiscard]] foundation::u32 Id() const noexcept override { return 1; }
        [[nodiscard]] foundation::u32 Width() const noexcept override { return 800; }
        [[nodiscard]] foundation::u32 Height() const noexcept override { return 600; }
        [[nodiscard]] foundation::i32 X() const noexcept override { return 0; }
        [[nodiscard]] foundation::i32 Y() const noexcept override { return 0; }
        void SetPosition(foundation::i32, foundation::i32) override {}
        void SetSize(foundation::u32, foundation::u32) override {}
        [[nodiscard]] foundation::f32 ContentScale() const noexcept override { return 1.0f; }
        [[nodiscard]] shell::NativeWindow Native() const noexcept override { return {}; }
        [[nodiscard]] bool IsOpen() const noexcept override { return true; }
        [[nodiscard]] bool IsMinimized() const noexcept override { return false; }
        void Close() override {}
        void StartTextInput() override { active = true; }
        void StopTextInput() override { active = false; }
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return active; }
    };
}

TEST_CASE("ui-shell: click focuses and typed text reaches the field")
{
    UIContext ctx;
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto edit = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    UiInputBridge bridge(&ctx);

    bridge.Dispatch(MouseDown(10, 10)); // hits + focuses the full-window EditText
    CHECK(ctx.WantsTextInput());        // an editable field is focused

    bridge.Dispatch(TextEvent(u8"hi"));
    CHECK(edit->Text() == u8"hi");
}

TEST_CASE("ui-shell: text input target follows focus (IME sync)")
{
    UIContext ctx;
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto edit = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    edit->IsReadOnly.SetValue(true); // read-only -> does NOT want text input
    auto edit2 = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    root->AddView(edit.Get());
    root->AddView(edit2.Get());
    LayoutPass(ctx, root.Get());

    MockWindow window;
    UiInputBridge bridge(&ctx);
    bridge.SetTextInputTarget(&window);

    ctx.GetFocusManager()->SetFocus(edit2.Get());
    bridge.SyncTextInput();
    CHECK(window.active); // editable field focused -> IME on

    ctx.GetFocusManager()->SetFocus(edit.Get()); // read-only
    bridge.SyncTextInput();
    CHECK(!window.active); // read-only -> IME off

    ctx.GetFocusManager()->SetFocus(edit2.Get());
    bridge.SyncTextInput();
    CHECK(window.active);

    ctx.GetFocusManager()->ClearFocus();
    bridge.SyncTextInput();
    CHECK(!window.active); // nothing focused -> IME off
}

TEST_CASE("ui-shell: ShellClipboard bridges the platform clipboard")
{
    shell::NullShell nullShell; // implements IShell incl. clipboard
    ShellClipboard clip(&nullShell);

    CHECK(!clip.HasText());
    CHECK(clip.SetText(u8"hello").IsOk());
    CHECK(clip.HasText());

    String out;
    CHECK(clip.GetText(out).IsOk());
    CHECK(out == u8"hello");
}

TEST_CASE("ui-shell: ShellClipboard with null shell is graceful")
{
    ShellClipboard clip(nullptr);
    CHECK(!clip.HasText());
    CHECK(!clip.SetText(u8"x").IsOk());
    String out;
    CHECK(!clip.GetText(out).IsOk());
}

// Regression: function keys were unmapped (KeyCode::Unknown), so F2-rename etc. never reached
// the UI. The bridge now maps F1-F24 and the digit row.
TEST_CASE("ui-shell: function and digit keys map through the bridge")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    root->ViewportSize = foundation::Float2{800, 600};
    ctx.AddRootView(root.Get());

    // A focusable probe recording the keys it receives.
    class KeyProbe final : public View
    {
    public:
        KeyProbe() { IsFocusable = true; }
        KeyCode last = KeyCode::Unknown;
        void OnKeyDown(KeyEventArgs& e) override
        {
            last = e.Key;
            e.Handled = true;
        }
    };
    auto probe = foundation::MakeRef<KeyProbe>(foundation::DefaultAllocator());
    root->AddView(probe.Get());
    ctx.GetFocusManager()->SetFocus(probe.Get());

    UiInputBridge bridge(&ctx);
    auto key = [](shell::KeyCode k)
    {
        shell::InputEvent e{};
        e.kind = shell::InputEventKind::KeyDown;
        e.key = k;
        return e;
    };

    (void)bridge.Dispatch(key(shell::KeyCode::F2));
    CHECK(probe->last == KeyCode::F2);
    (void)bridge.Dispatch(key(shell::KeyCode::F12));
    CHECK(probe->last == KeyCode::F12);
    (void)bridge.Dispatch(key(shell::KeyCode::F24));
    CHECK(probe->last == KeyCode::F24);
    (void)bridge.Dispatch(key(shell::KeyCode::Num0));
    CHECK(probe->last == KeyCode::Num0);
    (void)bridge.Dispatch(key(shell::KeyCode::Num5));
    CHECK(probe->last == KeyCode::Num5);
    (void)bridge.Dispatch(key(shell::KeyCode::Delete)); // pre-existing mapping still intact
    CHECK(probe->last == KeyCode::Delete);
}

// Numpad Enter maps to Return: both mean "confirm" to the UI (commit-on-Enter etc.).
TEST_CASE("ui-shell: keypad enter maps to Return")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    root->ViewportSize = foundation::Float2{800, 600};
    ctx.AddRootView(root.Get());

    class KeyProbe final : public View
    {
    public:
        KeyProbe() { IsFocusable = true; }
        KeyCode last = KeyCode::Unknown;
        void OnKeyDown(KeyEventArgs& e) override
        {
            last = e.Key;
            e.Handled = true;
        }
    };
    auto probe = foundation::MakeRef<KeyProbe>(foundation::DefaultAllocator());
    root->AddView(probe.Get());
    ctx.GetFocusManager()->SetFocus(probe.Get());

    UiInputBridge bridge(&ctx);
    shell::InputEvent e{};
    e.kind = shell::InputEventKind::KeyDown;
    e.key = shell::KeyCode::KeypadEnter;
    (void)bridge.Dispatch(e);
    CHECK(probe->last == KeyCode::Return);
}

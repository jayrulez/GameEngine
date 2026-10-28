// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for UiInputBridge (foundation.ui.shell) - our reimplementation, covered per the additions
// rule. Synthetic shell::InputEvents drive a UIContext through the bridge; a mock IWindow verifies the
// focus-driven text-input (IME) sync.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.shell;
import foundation.shell;
import foundation.shell.null;

using namespace foundation::core;
using namespace foundation::ui;
namespace core = foundation::core;
namespace shell = foundation::shell;

namespace
{
    // A RootView-filling focusable/hittable EditText is created directly; no TestHelpers needed here.
    void SetupRoot(UIContext& ctx, RefPtr<RootView>& root)
    {
        root = core::MakeRef<RootView>(core::DefaultAllocator());
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
        [[nodiscard]] core::u32 Id() const noexcept override { return 1; }
        [[nodiscard]] core::u32 Width() const noexcept override { return 800; }
        [[nodiscard]] core::u32 Height() const noexcept override { return 600; }
        [[nodiscard]] core::u32 PixelWidth() const noexcept override { return 800; }
        [[nodiscard]] core::u32 PixelHeight() const noexcept override { return 600; }
        [[nodiscard]] core::i32 X() const noexcept override { return 0; }
        [[nodiscard]] core::i32 Y() const noexcept override { return 0; }
        void SetPosition(core::i32, core::i32) override {}
        void SetSize(core::u32, core::u32) override {}
        [[nodiscard]] core::f32 ContentScale() const noexcept override { return 1.0f; }
        [[nodiscard]] shell::NativeWindow Native() const noexcept override { return {}; }
        [[nodiscard]] bool IsOpen() const noexcept override { return true; }
        [[nodiscard]] bool IsMinimized() const noexcept override { return false; }
        void Close() override {}
        void Raise() override {}
        void SetTitle(foundation::core::StringView) override {}
        void StartTextInput(shell::TextInputKind k) override
        {
            active = true;
            kind = k;
        }
        void StopTextInput() override { active = false; }
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return active; }
        [[nodiscard]] shell::TextInputKind GetTextInputKind() const noexcept override { return kind; }
        shell::TextInputKind kind = shell::TextInputKind::Text;
    };
}

TEST_CASE("ui-shell: click focuses and typed text reaches the field")
{
    UIContext ctx{DefaultAllocator()};
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
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
    UIContext ctx{DefaultAllocator()};
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
    edit->IsReadOnly.SetValue(true); // read-only -> does NOT want text input
    auto edit2 = core::MakeRef<EditText>(core::DefaultAllocator());
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

TEST_CASE("ui-shell: a password box starts secret text input, and a move to a field switches it back")
{
    UIContext ctx{DefaultAllocator()};
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto field = core::MakeRef<EditText>(core::DefaultAllocator());
    auto password = core::MakeRef<PasswordBox>(core::DefaultAllocator());
    root->AddView(field.Get());
    root->AddView(password.Get());
    LayoutPass(ctx, root.Get());

    MockWindow window;
    UiInputBridge bridge(&ctx);
    bridge.SetTextInputTarget(&window);

    ctx.GetFocusManager()->SetFocus(password.Get());
    bridge.SyncTextInput();
    CHECK(window.active);
    CHECK(window.kind == shell::TextInputKind::Secret); // the IME keeps out
    ctx.GetFocusManager()->SetFocus(field.Get());
    bridge.SyncTextInput();
    CHECK(window.active);
    CHECK(window.kind == shell::TextInputKind::Text);
}

TEST_CASE("ui-shell: a window going away stops being the text input target")
{
    // The editor's crash: a dialog window was the target, it closed, and the next sync called into
    // it. Forgetting it leaves no target, so a sync touches no window until the host sets one.
    UIContext ctx{DefaultAllocator()};
    RefPtr<RootView> root;
    SetupRoot(ctx, root);
    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());
    ctx.GetFocusManager()->SetFocus(edit.Get());

    MockWindow main;
    MockWindow dialog;
    UiInputBridge bridge(&ctx);
    bridge.SetTextInputTarget(&dialog);
    bridge.ForgetWindow(&main); // another window: the target stays
    CHECK(bridge.TextInputTarget() == &dialog);
    bridge.ForgetWindow(&dialog);
    CHECK(bridge.TextInputTarget() == nullptr);
    bridge.SyncTextInput(); // no target: nothing called
    CHECK(!dialog.active);
    CHECK(!main.active);
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
TEST_CASE("ui-shell: function, digit and keypad keys map through the bridge")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = core::Float2{800, 600};
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
    auto probe = core::MakeRef<KeyProbe>(core::DefaultAllocator());
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
    // The modifier keys as keys (a chord capture sees Ctrl go down and keeps waiting).
    (void)bridge.Dispatch(key(shell::KeyCode::LeftCtrl));
    CHECK(probe->last == KeyCode::LeftCtrl);
    (void)bridge.Dispatch(key(shell::KeyCode::RightShift));
    CHECK(probe->last == KeyCode::RightShift);
    (void)bridge.Dispatch(key(shell::KeyCode::LeftAlt));
    CHECK(probe->last == KeyCode::LeftAlt);
    (void)bridge.Dispatch(key(shell::KeyCode::RightGui));
    CHECK(probe->last == KeyCode::RightGui);
    (void)bridge.Dispatch(key(shell::KeyCode::Delete)); // pre-existing mapping still intact
    CHECK(probe->last == KeyCode::Delete);
    // The keypad: digits (an editor's Numpad 5 reached the UI as Unknown) and its operators.
    (void)bridge.Dispatch(key(shell::KeyCode::Keypad5));
    CHECK(probe->last == KeyCode::Keypad5);
    (void)bridge.Dispatch(key(shell::KeyCode::Keypad1));
    CHECK(probe->last == KeyCode::Keypad1);
    (void)bridge.Dispatch(key(shell::KeyCode::Keypad9));
    CHECK(probe->last == KeyCode::Keypad9);
    (void)bridge.Dispatch(key(shell::KeyCode::Keypad0));
    CHECK(probe->last == KeyCode::Keypad0);
    (void)bridge.Dispatch(key(shell::KeyCode::KeypadPlus));
    CHECK(probe->last == KeyCode::KeypadPlus);
    (void)bridge.Dispatch(key(shell::KeyCode::KeypadDecimal));
    CHECK(probe->last == KeyCode::KeypadPeriod);
}

// Numpad Enter maps to Return: both mean "confirm" to the UI (commit-on-Enter etc.).
TEST_CASE("ui-shell: keypad enter maps to Return")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    root->ViewportSize = core::Float2{800, 600};
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
    auto probe = core::MakeRef<KeyProbe>(core::DefaultAllocator());
    root->AddView(probe.Get());
    ctx.GetFocusManager()->SetFocus(probe.Get());

    UiInputBridge bridge(&ctx);
    shell::InputEvent e{};
    e.kind = shell::InputEventKind::KeyDown;
    e.key = shell::KeyCode::KeypadEnter;
    (void)bridge.Dispatch(e);
    CHECK(probe->last == KeyCode::Return);
}

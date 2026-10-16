// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - commands (G7 of Specs/vg-and-gui-lab.md): one model controls bind to by id; running walks
// the focus path (capture down, bubble up, a bubble handler stopping it unless it propagates); a
// command is available when enabled with a handler on the focus path, and its state reaches a
// button, a checkbox and a menu item with no bookkeeping.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    void Frame(SceneRoot& root) { root.Update(core::Duration::FromSeconds(0.016)); }
}

TEST_CASE("command: running walks the focus path, capture down then bubble up")
{
    auto root = Make<SceneRoot>();
    auto panel = Make<UIWidget>();
    auto field = Make<Button>();
    field->SetTabFocusable(true);
    root->AddChild(panel.Get());
    panel->AddChild(field.Get());
    core::Array<core::String> order;
    const auto log = [&](const char8_t* who) { return [&order, who](CommandEvent&) { order.PushBack(core::String(who)); }; };
    (void)root->OnCommand(SV(u8"save"), log(u8"root-capture"), CommandPhase::Capture);
    (void)panel->OnCommand(SV(u8"save"), log(u8"panel-capture"), CommandPhase::Capture);
    (void)field->OnCommand(SV(u8"save"), log(u8"field"));
    (void)panel->OnCommand(SV(u8"save"), log(u8"panel"));
    (void)root->OnCommand(SV(u8"save"), log(u8"root"));
    root->GetEventDispatcher()->SetFocusNode(field.Get());

    CHECK(root->RunCommand(SV(u8"save")));
    REQUIRE(order.Size() == 3u); // the field's bubble handler stopped it
    CHECK(order[0] == core::String(u8"root-capture"));
    CHECK(order[1] == core::String(u8"panel-capture"));
    CHECK(order[2] == core::String(u8"field"));

    // A bubble handler that propagates lets it go on up.
    (void)field->OnCommand(SV(u8"open"), [](CommandEvent& e) { e.Propagate = true; });
    bool reachedRoot = false;
    (void)root->OnCommand(SV(u8"open"), [&](CommandEvent&) { reachedRoot = true; });
    CHECK(root->RunCommand(SV(u8"open")));
    CHECK(reachedRoot);

    // Disabled, it does not run; unhandled, it reports so.
    root->DefineCommand(SV(u8"save")).SetEnabled(false);
    order.Clear();
    CHECK_FALSE(root->RunCommand(SV(u8"save")));
    CHECK(order.IsEmpty());
    CHECK_FALSE(root->RunCommand(SV(u8"nobody")));
}

TEST_CASE("command: availability follows focus, and a bound button greys and lights with it")
{
    auto root = Make<SceneRoot>();
    auto editor = Make<UIWidget>(); // handles "cut" while focus is inside it
    auto inside = Make<Button>();
    auto outside = Make<Button>();
    auto cut = Make<Button>(); // a toolbar button bound to "cut"
    inside->SetTabFocusable(true);
    outside->SetTabFocusable(true);
    root->AddChild(editor.Get());
    editor->AddChild(inside.Get());
    root->AddChild(outside.Get());
    root->AddChild(cut.Get());
    int cuts = 0;
    (void)editor->OnCommand(SV(u8"cut"), [&](CommandEvent&) { ++cuts; });
    cut->SetCommand(SV(u8"cut"));
    EventDispatcher& d = *root->GetEventDispatcher();

    d.SetFocusNode(outside.Get());
    Frame(*root.Get());
    CHECK_FALSE(cut->IsEnabled()); // no handler on the focus path

    d.SetFocusNode(inside.Get());
    Frame(*root.Get());
    CHECK(cut->IsEnabled());
    cut->SetPosition(core::Float2{0.0f, 0.0f});
    cut->SetSize(core::Float2{40.0f, 20.0f});
    CHECK(cut->HandleActivate()); // a click (Return) on it runs the command along the focus path
    CHECK(cuts == 1);

    root->DefineCommand(SV(u8"cut")).SetEnabled(false); // one switch greys every control bound to it
    Frame(*root.Get());
    CHECK_FALSE(cut->IsEnabled());
}

TEST_CASE("command: a checkbox and a menu item show the command's checked state, and run it")
{
    auto root = Make<SceneRoot>();
    Command& music = root->DefineCommand(SV(u8"music"));
    (void)root->OnCommand(SV(u8"music"), [&music](CommandEvent&) { music.SetChecked(!music.IsChecked()); });
    auto box = Make<CheckBox>();
    box->SetSize(core::Float2{20.0f, 20.0f});
    auto item = Make<MenuItem>();
    item->SetCheckable(true);
    root->AddChild(box.Get());
    root->AddChild(item.Get());
    box->SetCommand(SV(u8"music"));
    item->SetCommand(SV(u8"music"));
    Frame(*root.Get());
    CHECK(box->IsEnabled());
    CHECK_FALSE(box->IsChecked());

    CHECK(box->HandleActivate()); // the box asks the command; the handler toggles it
    Frame(*root.Get());
    CHECK(music.IsChecked());
    CHECK(box->IsChecked());
    CHECK(item->IsChecked()); // the menu item follows the same command
}

TEST_CASE("command: markup binds a control to a command by id, before it is attached")
{
    RegisterGuiElements();
    auto root = Make<SceneRoot>();
    (void)root->OnCommand(SV(u8"play"), [](CommandEvent&) {});
    root->DefineCommand(SV(u8"play")).SetEnabled(false);
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, core::DefaultAllocator());
    auto tree = loader.LoadFromString(SV(u8R"(<Button command="play" text="Play"/>)"));
    REQUIRE(tree.Get() != nullptr);
    auto* play = core::Cast<Button>(tree.Get());
    REQUIRE(play != nullptr);
    CHECK(play->GetCommand() == SV(u8"play"));
    root->AddChild(tree.Get()); // joining the GUI reaches its binding
    Frame(*root.Get());
    CHECK_FALSE(play->IsEnabled());
    root->DefineCommand(SV(u8"play")).SetEnabled(true);
    Frame(*root.Get());
    CHECK(play->IsEnabled());
}

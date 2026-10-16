// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - bindings (G7 of Specs/vg-and-gui-lab.md): keystrokes and gamepad buttons bound to commands,
// scoped by context predicates over the focus path (the deepest match winning, then the later
// binding), unbinding, chords, and the order keys take (the widget, then bindings, then navigation).
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

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<UIWidget> screen = Make<UIWidget>(); // key context "menu"
        core::RefPtr<Button> item = Make<Button>();
        core::Array<core::String> ran;
        Bed()
        {
            root->AddChild(screen.Get());
            screen->AddChild(item.Get());
            screen->SetKeyContext(SV(u8"menu mode=main"));
            item->SetTabFocusable(true);
            item->SetSize(core::Float2{40.0f, 20.0f});
            root->GetEventDispatcher()->SetFocusNode(item.Get());
        }
        void Handle(const char8_t* id)
        {
            (void)root->OnCommand(SV(id), [this, id](CommandEvent&) { ran.PushBack(core::String(id)); });
        }
        void Key(KeyCode key, core::u32 mods = 0) { root->GetEventDispatcher()->InjectKeyDown(static_cast<core::u32>(key), mods); }
    };
}

TEST_CASE("keymap: keystrokes, chords and predicates parse")
{
    const auto s = ParseKeystroke(SV(u8"ctrl-shift-a"));
    REQUIRE(s.HasValue());
    CHECK(s.Value().Code == static_cast<core::u32>(KeyCode::A));
    CHECK(s.Value().Modifiers == (KeyModCtrl | KeyModShift));
    CHECK(ParseKeystroke(SV(u8"pad-east")).Value().Pad);
    CHECK_FALSE(ParseKeystroke(SV(u8"hyper-a")).HasValue());
    CHECK_FALSE(ParseKeystroke(SV(u8"pad-turbo")).HasValue());

    ContextPredicate p;
    REQUIRE(p.Parse(SV(u8"menu && (mode == main || !modal)")));
    KeyContextLayer layer;
    layer.Declare(SV(u8"menu mode=main"));
    CHECK(p.Eval(layer));
    layer.Declare(SV(u8"mode=settings modal"));
    CHECK_FALSE(p.Eval(layer));
    CHECK_FALSE(p.Parse(SV(u8"menu &&")));
    CHECK_FALSE(p.Eval(layer)); // malformed matches nothing
    Keymap keymap;
    CHECK_FALSE(keymap.Bind(SV(u8"ctrl-nonesuch"), SV(u8"x")));
    CHECK_FALSE(keymap.Bind(SV(u8"escape"), SV(u8"x"), SV(u8"(menu")));
}

TEST_CASE("keymap: the deepest matching context wins, then the later binding")
{
    Bed bed;
    bed.Handle(u8"quit");
    bed.Handle(u8"back");
    bed.Handle(u8"close");
    Keymap& keys = bed.root->GetContext().GetKeymap();
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"quit")));                   // everywhere
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"back"), SV(u8"menu")));     // in the menu: deeper
    bed.Key(KeyCode::Escape);
    REQUIRE(bed.ran.Size() == 1u);
    CHECK(bed.ran[0] == core::String(u8"back"));

    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"close"), SV(u8"mode == main"))); // as deep, later: wins
    bed.Key(KeyCode::Escape);
    CHECK(bed.ran[1] == core::String(u8"close"));

    // Focus outside the menu: only the context-free binding holds.
    auto outside = Make<Button>();
    outside->SetTabFocusable(true);
    bed.root->AddChild(outside.Get());
    bed.root->GetEventDispatcher()->SetFocusNode(outside.Get());
    bed.Key(KeyCode::Escape);
    CHECK(bed.ran[2] == core::String(u8"quit"));
}

TEST_CASE("keymap: an outer node's context does not outrank the focused one's")
{
    // The game screen declares "game"; the menu inside it declares "menu". Escape pauses in the
    // game and closes in the menu, whichever was bound later.
    Bed bed;
    bed.Handle(u8"pause");
    bed.Handle(u8"close");
    auto game = Make<UIWidget>();
    bed.root->RemoveChild(bed.screen.Get());
    bed.root->AddChild(game.Get());
    game->AddChild(bed.screen.Get());
    game->SetKeyContext(SV(u8"game"));
    bed.root->GetEventDispatcher()->SetFocusNode(bed.item.Get());
    Keymap& keys = bed.root->GetContext().GetKeymap();
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"close"), SV(u8"menu")));
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"pause"), SV(u8"game"))); // later, but outer
    bed.Key(KeyCode::Escape);
    REQUIRE(bed.ran.Size() == 1u);
    CHECK(bed.ran[0] == core::String(u8"close"));

    // "not in a menu" holds only outside it.
    REQUIRE(keys.Bind(SV(u8"f1"), SV(u8"pause"), SV(u8"game && !menu")));
    bed.Key(KeyCode::F1);
    CHECK(bed.ran.Size() == 1u);
    auto hud = Make<Button>();
    hud->SetTabFocusable(true);
    game->AddChild(hud.Get());
    bed.root->GetEventDispatcher()->SetFocusNode(hud.Get());
    bed.Key(KeyCode::F1);
    REQUIRE(bed.ran.Size() == 2u);
    CHECK(bed.ran[1] == core::String(u8"pause"));
    bed.Key(KeyCode::Escape); // outside the menu: the game's binding
    REQUIRE(bed.ran.Size() == 3u);
    CHECK(bed.ran[2] == core::String(u8"pause"));
}

TEST_CASE("keymap: a binding to nothing unbinds a key there")
{
    Bed bed;
    bed.Handle(u8"quit");
    Keymap& keys = bed.root->GetContext().GetKeymap();
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8"quit")));
    REQUIRE(keys.Bind(SV(u8"escape"), SV(u8""), SV(u8"menu"))); // not in the menu
    bed.Key(KeyCode::Escape);
    CHECK(bed.ran.IsEmpty());
}

TEST_CASE("keymap: a chord waits for its next keystroke, and lapses")
{
    Bed bed;
    bed.Handle(u8"comment");
    bed.Handle(u8"select-all");
    Keymap& keys = bed.root->GetContext().GetKeymap();
    REQUIRE(keys.Bind(SV(u8"ctrl-x ctrl-c"), SV(u8"comment")));
    REQUIRE(keys.Bind(SV(u8"ctrl-a"), SV(u8"select-all")));
    bed.Key(KeyCode::X, KeyModCtrl); // waits
    CHECK(bed.ran.IsEmpty());
    bed.Key(KeyCode::C, KeyModCtrl);
    REQUIRE(bed.ran.Size() == 1u);
    CHECK(bed.ran[0] == core::String(u8"comment"));

    bed.Key(KeyCode::X, KeyModCtrl);
    bed.root->GetContext().Update(core::Duration::FromSeconds(1.5)); // too slow: dropped
    bed.Key(KeyCode::C, KeyModCtrl);
    CHECK(bed.ran.Size() == 1u);

    bed.Key(KeyCode::X, KeyModCtrl);
    bed.Key(KeyCode::A, KeyModCtrl); // broken off: the keystroke alone
    REQUIRE(bed.ran.Size() == 2u);
    CHECK(bed.ran[1] == core::String(u8"select-all"));
}

TEST_CASE("keymap: a gamepad's buttons bind too; South activates when unbound")
{
    Bed bed;
    bed.Handle(u8"back");
    REQUIRE(bed.root->GetContext().GetKeymap().Bind(SV(u8"pad-east"), SV(u8"back")));
    int clicks = 0;
    (void)bed.item->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });
    EventDispatcher& d = *bed.root->GetEventDispatcher();
    d.InjectPadButton(PadButton::East);
    REQUIRE(bed.ran.Size() == 1u);
    d.InjectPadButton(PadButton::South);
    CHECK(clicks == 1);
}

TEST_CASE("keymap: a text field keeps its keys, and leaves a shortcut to the bindings")
{
    Bed bed;
    bed.Handle(u8"save");
    REQUIRE(bed.root->GetContext().GetKeymap().Bind(SV(u8"ctrl-s"), SV(u8"save")));
    REQUIRE(bed.root->GetContext().GetKeymap().Bind(SV(u8"ctrl-a"), SV(u8"save"))); // the field's own: it keeps it
    auto field = Make<TextField>();
    bed.screen->AddChild(field.Get());
    bed.root->GetEventDispatcher()->SetFocusNode(field.Get());
    field->SetText(SV(u8"hello"));
    bed.Key(KeyCode::A, KeyModCtrl);
    CHECK(bed.ran.IsEmpty()); // select-all in the field
    bed.root->GetEventDispatcher()->InjectKeyDown(static_cast<core::u32>(KeyCode::Unknown), KeyModCtrl);
    CHECK(bed.ran.IsEmpty());
}

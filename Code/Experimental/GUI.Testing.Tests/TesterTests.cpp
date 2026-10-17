// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Testing - the tester (E8f of Specs/gui-lab-evolution.md): finders by selector and text,
// within a scope, hidden ones only when asked; a finder expecting one that finds none or several
// fails with what it found and the tree; the user's actions reach what they aim at, or fail when
// something covers it; the clock steps a frame after each.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;
import experimental.gui.testing;

using namespace experimental::gui;
using experimental::gui::testing::Finder;
using experimental::gui::testing::Tester;
namespace core = foundation::core;
using core::f32;
using core::usize;
using core::u32;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    template <typename T>
    core::RefPtr<T> Add(Node& parent, const char8_t* id, f32 x, f32 y, f32 w, f32 h)
    {
        auto node = core::MakeRef<T>(core::DefaultAllocator());
        node->SetId(SV(id));
        node->SetPosition(core::Float2{x, y});
        node->SetSize(core::Float2{w, h});
        parent.AddChild(node.Get());
        return node;
    }

    // A form: a "Show" button, a field, a dialog with its own "OK" beside the form's "OK", and a
    // hidden "Secret" label.
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<Button> show;
        core::RefPtr<TextField> field;
        core::RefPtr<Panel> dialog;
        core::RefPtr<Button> ok;
        core::RefPtr<Button> dialogOk;
        core::RefPtr<Label> secret;
        int clicks = 0;

        Bed()
        {
            root->SetSize(core::Float2{400.0f, 300.0f});
            show = Add<Button>(*root.Get(), u8"show", 10.0f, 10.0f, 80.0f, 30.0f);
            show->SetText(SV(u8"Show"));
            show->SetOnClick([this]() { ++clicks; });
            field = Add<TextField>(*root.Get(), u8"field", 10.0f, 50.0f, 150.0f, 30.0f);
            ok = Add<Button>(*root.Get(), u8"ok", 10.0f, 90.0f, 80.0f, 30.0f);
            ok->SetText(SV(u8"OK"));
            dialog = Add<Panel>(*root.Get(), u8"dialog", 200.0f, 10.0f, 180.0f, 150.0f);
            dialogOk = Add<Button>(*dialog.Get(), u8"dialog-ok", 10.0f, 10.0f, 80.0f, 30.0f);
            dialogOk->SetText(SV(u8"OK"));
            secret = Add<Label>(*root.Get(), u8"secret", 10.0f, 200.0f, 80.0f, 20.0f);
            secret->SetText(SV(u8"Secret"));
            secret->SetVisible(false);
        }
    };
}

TEST_CASE("testing: finders by selector and by text, within a scope, hidden ones only when asked")
{
    Bed bed;
    Tester t(*bed.root.Get());
    CHECK(t.One(Finder::Selector(SV(u8"#show"))) == bed.show.Get());
    CHECK(t.One(Finder::Text(SV(u8"Show"))) == bed.show.Get()); // the button, not its label part too
    CHECK(t.One(Finder::TextContaining(SV(u8"Sho"))) == bed.show.Get());
    CHECK(t.One(Finder::Text(SV(u8"OK")).Within(Finder::Selector(SV(u8"#dialog")))) == bed.dialogOk.Get());
    CHECK_FALSE(t.Exists(Finder::Text(SV(u8"Secret"))));
    CHECK(t.One(Finder::Text(SV(u8"Secret")).IncludingHidden()) == bed.secret.Get());
    CHECK(t.Failures().IsEmpty());
    CHECK(Finder::Parse(SV(u8"text=OK")).Within(Finder::Parse(SV(u8"#dialog"))).Describe() == SV(u8"text=OK within selector #dialog"));
}

TEST_CASE("testing: expecting one and finding several or none fails, naming what it found, with the tree")
{
    Bed bed;
    Tester t(*bed.root.Get());
    core::Array<core::String> seen;
    t.SetFailureHandler([&seen](core::StringView message) { seen.PushBack(core::String(message)); });
    CHECK(t.One(Finder::Text(SV(u8"OK"))) == nullptr);
    REQUIRE(seen.Size() == 1u);
    const core::StringView message = seen[0].AsView();
    CHECK(message.StartsWith(SV(u8"text=OK found 2 nodes, expected one")));
    CHECK(message.ContainsIgnoreCase(SV(u8"found: Button #ok")));
    CHECK(message.ContainsIgnoreCase(SV(u8"Button #dialog-ok \"OK\""))); // the tree, with texts
    CHECK(t.One(Finder::Selector(SV(u8"#nothing"))) == nullptr);
    CHECK(t.Failures().Size() == 2u);
}

TEST_CASE("testing: clicks reach what they aim at, and fail when something lies over it")
{
    Bed bed;
    Tester t(*bed.root.Get());
    CHECK(t.Click(Finder::Text(SV(u8"Show"))));
    CHECK(bed.clicks == 1);
    u32 most = 0;
    (void)bed.show->AddEventListener(EventType::MouseClick, [&most](const Event& e)
    {
        most = core::Max(most, static_cast<const MouseEvent&>(e).ClickCount);
    });
    t.Advance(core::Duration::FromSeconds(1.0)); // the click before is no part of it
    CHECK(t.DoubleClick(Finder::Selector(SV(u8"#show"))));
    CHECK(most == 2u);
    // A panel laid over the button: the click would land on it.
    auto cover = Add<Panel>(*bed.root.Get(), u8"cover", 0.0f, 0.0f, 120.0f, 60.0f);
    t.Frame();
    const int before = bed.clicks;
    CHECK_FALSE(t.Click(Finder::Selector(SV(u8"#show"))));
    CHECK(bed.clicks == before);
    REQUIRE_FALSE(t.Failures().IsEmpty());
    CHECK(t.Failures()[t.Failures().Size() - 1].AsView().ContainsIgnoreCase(SV(u8"covered at its middle by Panel #cover")));
}

TEST_CASE("testing: typing goes to the focus, keys move it, and a held arrow repeats")
{
    Bed bed;
    Tester t(*bed.root.Get());
    CHECK(t.Click(Finder::Selector(SV(u8"#field"))));
    t.Type(SV(u8"hello"));
    CHECK(bed.field->GetText() == SV(u8"hello"));
    CHECK(t.Press(SV(u8"tab")));
    CHECK(bed.root->GetEventDispatcher()->GetFocusNode() != bed.field.Get());
    CHECK_FALSE(t.Press(SV(u8"no-such-key")));

    // A column of buttons: Down held for a second walks focus down several of them.
    auto column = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    column->SetSize(core::Float2{200.0f, 600.0f});
    core::Array<core::RefPtr<Button>> buttons;
    for (int i = 0; i < 12; ++i)
    {
        char8_t id[4] = {u8'b', static_cast<char8_t>(u8'a' + i), 0, 0};
        buttons.PushBack(Add<Button>(*column.Get(), id, 10.0f, 10.0f + 40.0f * static_cast<f32>(i), 80.0f, 30.0f));
    }
    Tester c(*column.Get());
    column->GetEventDispatcher()->SetFocusNode(buttons[0].Get());
    c.HoldDirection(NavDirection::Down, core::Duration::FromSeconds(1.0));
    usize at = 0;
    for (usize i = 0; i < buttons.Size(); ++i)
        if (column->GetEventDispatcher()->GetFocusNode() == buttons[i].Get())
            at = i;
    CHECK(at >= 3u);
}

TEST_CASE("testing: a drag presses on one node, moves in steps and releases over another")
{
    Bed bed;
    Tester t(*bed.root.Get());
    int moves = 0;
    core::Float2 released{};
    (void)bed.show->AddEventListener(EventType::MouseMove, [&moves](const Event&) { ++moves; });
    (void)bed.show->AddEventListener(EventType::MouseUp, [&released](const Event& e) { released = static_cast<const MouseEvent&>(e).Position; });
    CHECK(t.DragTo(Finder::Selector(SV(u8"#show")), Finder::Selector(SV(u8"#ok"))));
    CHECK(moves >= 8);                                  // the press captures the pointer: every step reaches it
    CHECK(released.y == doctest::Approx(105.0f));       // over the OK button's middle
}

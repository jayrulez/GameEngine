// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - EditableLabel (E6): a double-click or F2 edits it in place; Return and focus leaving
// commit, Escape cancels; a validator keeps it editing or puts the old text back.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<EditableLabel> label = core::MakeRef<EditableLabel>(core::DefaultAllocator());
        core::RefPtr<Panel> other = core::MakeRef<Panel>(core::DefaultAllocator());
        int commits = 0, changeEvents = 0;
        Bed()
        {
            root->SetSize(core::Float2{300.0f, 200.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            label->SetText(core::StringView(u8"layer 1"));
            label->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(120.0f);
                s.Height = LayoutLength::Px(24.0f);
            });
            root->AddChild(label.Get());
            other->SetTabFocusable(true);
            other->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(20.0f); s.Height = LayoutLength::Px(20.0f); });
            root->AddChild(other.Get());
            (void)root->UpdateLayout();
            label->SetOnTextCommitted([this](core::StringView) { ++commits; });
            (void)label->AddEventListener(EventType::Changed, [this](const Event&) { ++changeEvents; });
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        void Key(KeyCode k) { D().InjectKeyDown(static_cast<core::u32>(k)); }
        void Retype(const char8_t* text)
        {
            label->GetEditor()->GetModel().SelectAll();
            D().InjectText(core::StringView(text));
        }
    };
}

TEST_CASE("editable label: a double-click edits it in place, all selected; Return commits")
{
    Bed b;
    b.D().InjectMouseDown(core::Float2{20.0f, 12.0f}, MouseButton::Left);
    b.D().InjectMouseUp(core::Float2{20.0f, 12.0f}, MouseButton::Left);
    CHECK_FALSE(b.label->IsEditing()); // one click: no
    b.D().InjectMouseDown(core::Float2{20.0f, 12.0f}, MouseButton::Left);
    b.D().InjectMouseUp(core::Float2{20.0f, 12.0f}, MouseButton::Left);
    REQUIRE(b.label->IsEditing());
    TextField* editor = b.label->GetEditor();
    CHECK(editor->IsFocused());
    CHECK(editor->GetText() == core::StringView(u8"layer 1"));
    (void)b.root->UpdateLayout();
    CHECK(editor->GetSize().x == doctest::Approx(120.0f)); // over its whole box

    b.D().InjectText(core::StringView(u8"sky")); // all selected: replaced
    b.Key(KeyCode::Return);
    CHECK_FALSE(b.label->IsEditing());
    CHECK(b.label->GetText() == core::StringView(u8"sky"));
    CHECK(b.label->IsFocused()); // the label keeps the focus
    CHECK(b.commits == 1);
    CHECK(b.changeEvents == 1);
}

TEST_CASE("editable label: F2 edits it; Escape cancels; focus leaving commits")
{
    Bed b;
    b.label->RequestFocus();
    b.Key(KeyCode::F2);
    REQUIRE(b.label->IsEditing());
    b.Retype(u8"gone");
    b.Key(KeyCode::Escape);
    CHECK_FALSE(b.label->IsEditing());
    CHECK(b.label->GetText() == core::StringView(u8"layer 1"));
    CHECK(b.commits == 0);

    b.Key(KeyCode::F2);
    b.Retype(u8"kept");
    b.other->RequestFocus();
    CHECK_FALSE(b.label->IsEditing());
    CHECK(b.label->GetText() == core::StringView(u8"kept"));
    CHECK(b.other->IsFocused()); // focus stays where it went
    CHECK(b.commits == 1);
}

TEST_CASE("editable label: a validator keeps Return editing, and focus leaving puts the old text back")
{
    Bed b;
    b.label->SetValidator([](core::StringView text) { return text.Size() != 0; });
    b.label->BeginEdit();
    b.Retype(u8"");
    b.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Delete)); // empty it
    b.Key(KeyCode::Return);
    CHECK(b.label->IsEditing());
    CHECK(b.label->GetEditor()->HasClass(core::StringView(u8"invalid")));
    b.other->RequestFocus();
    CHECK_FALSE(b.label->IsEditing());
    CHECK(b.label->GetText() == core::StringView(u8"layer 1"));
    CHECK(b.commits == 0);
}

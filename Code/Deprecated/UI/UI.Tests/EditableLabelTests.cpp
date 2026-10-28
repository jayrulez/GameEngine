// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/EditableLabelTests.bf (faithful). Beef [Friend]mBehavior ->
// Behavior(); el.Text -> Text(); the ValidateRename `text.Contains("bad")` -> a local substring helper
// (StringView has no Contains). Mode/transition/event logic needs no font service.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

static core::RefPtr<RootView> MakeRoot()
{
    return core::MakeRef<RootView>(core::DefaultAllocator());
}
static core::RefPtr<EditableLabel> MakeLabel()
{
    return core::MakeRef<EditableLabel>(core::DefaultAllocator());
}

static bool Contains(StringView hay, StringView needle)
{
    if (needle.Size() == 0)
    {
        return true;
    }
    if (needle.Size() > hay.Size())
    {
        return false;
    }
    for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
    {
        bool match = true;
        for (usize j = 0; j < needle.Size(); ++j)
        {
            if (hay[i + j] != needle[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("editable-label: StartsInLabelMode")
{
    auto el = MakeLabel();
    CHECK(!el->IsEditing());
    CHECK(el->IsReadOnly.Value() == true);
    CHECK(el->IsFocusable == false);
    CHECK(el->Cursor == CursorType::Arrow);
}

TEST_CASE("editable-label: BeginEditTransitions")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Hello");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    el->BeginEdit();

    CHECK(el->IsEditing());
    CHECK(el->IsReadOnly.Value() == false);
    CHECK(el->IsFocusable == true);
    CHECK(el->Cursor == CursorType::IBeam);
}

TEST_CASE("editable-label: CommitEditFiresEvent")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Hello");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    bool committed = false;
    String committedText;
    el->OnRenameCommitted.Add(Event<void(EditableLabel*, StringView)>::Handler{
        [&committed, &committedText](EditableLabel*, StringView text)
        {
            committed = true;
            committedText = String(text);
        }});

    el->BeginEdit();
    el->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl);
    el->Behavior().HandleTextInput(U'W');
    el->Behavior().HandleTextInput(U'o');
    el->Behavior().HandleTextInput(U'r');
    el->Behavior().HandleTextInput(U'l');
    el->Behavior().HandleTextInput(U'd');
    el->CommitEdit();

    CHECK(committed);
    CHECK(committedText == u8"World");
    CHECK(!el->IsEditing());
}

TEST_CASE("editable-label: CancelEditRestoresText")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Original");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    bool cancelled = false;
    el->OnRenameCancelled.Add(
        Event<void(EditableLabel*)>::Handler{[&cancelled](EditableLabel*) { cancelled = true; }});

    el->BeginEdit();
    el->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl);
    el->Behavior().HandleTextInput(U'X');
    el->CancelEdit();

    CHECK(cancelled);
    CHECK(el->Text() == u8"Original");
    CHECK(!el->IsEditing());
}

TEST_CASE("editable-label: EmptyTextRejected")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Test");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    bool committed = false, cancelled = false;
    el->OnRenameCommitted.Add(Event<void(EditableLabel*, StringView)>::Handler{
        [&committed](EditableLabel*, StringView) { committed = true; }});
    el->OnRenameCancelled.Add(
        Event<void(EditableLabel*)>::Handler{[&cancelled](EditableLabel*) { cancelled = true; }});

    el->BeginEdit();
    el->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl);
    el->Behavior().HandleKeyDown(KeyCode::Delete, KeyModifiers::None);
    el->CommitEdit();

    CHECK(!committed);
    CHECK(cancelled);
    CHECK(el->Text() == u8"Test"); // restored
}

TEST_CASE("editable-label: ValidateRenameCalled")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Hello");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    el->ValidateRename = [](StringView text) { return !Contains(text, u8"bad"); };

    bool committed = false, cancelled = false;
    el->OnRenameCommitted.Add(Event<void(EditableLabel*, StringView)>::Handler{
        [&committed](EditableLabel*, StringView) { committed = true; }});
    el->OnRenameCancelled.Add(
        Event<void(EditableLabel*)>::Handler{[&cancelled](EditableLabel*) { cancelled = true; }});

    el->BeginEdit();
    el->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl);
    el->Behavior().HandleTextInput(U'b');
    el->Behavior().HandleTextInput(U'a');
    el->Behavior().HandleTextInput(U'd');
    el->CommitEdit();

    CHECK(!committed);
    CHECK(cancelled); // validator rejected
}

TEST_CASE("editable-label: DoubleClickToEditDefault")
{
    auto el = MakeLabel();
    CHECK(el->DoubleClickToEdit.Value() == true);
    CHECK(el->SlowClickToEdit.Value() == true);
}

TEST_CASE("editable-label: UnchangedTextRejected")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto el = MakeLabel();
    el->SetText(u8"Same");
    root->AddView(el.Get());
    LayoutPass(ctx, root.Get());

    bool committed = false, cancelled = false;
    el->OnRenameCommitted.Add(Event<void(EditableLabel*, StringView)>::Handler{
        [&committed](EditableLabel*, StringView) { committed = true; }});
    el->OnRenameCancelled.Add(
        Event<void(EditableLabel*)>::Handler{[&cancelled](EditableLabel*) { cancelled = true; }});

    el->BeginEdit();
    el->CommitEdit(); // unchanged

    CHECK(!committed);
    CHECK(cancelled);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/EditTextTests.bf - the font-independent subset (cursor/selection/
// insert/delete/undo/redo logic, all of which run through TextEditingBehavior on CHARACTER indices and
// need no glyph shaping). Beef `[Friend]mBehavior` -> the public Behavior() accessor; `edit.Filter =`
// -> SetFilter(); Beef property setters -> Set*/.SetValue(). The pixel/caret-position cases (which need
// the not-yet-wired Fonts service) are not ported. Undo/Redo coverage is included here (Sedulous had
// none - it exercised undo only interactively).
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
static core::RefPtr<EditText> MakeEdit()
{
    return core::MakeRef<EditText>(core::DefaultAllocator());
}
static core::RefPtr<PasswordBox> MakePassword()
{
    return core::MakeRef<PasswordBox>(core::DefaultAllocator());
}

static core::i32 CharCount(StringView v) { return static_cast<core::i32>(core::Utf8Length(v)); }

// === EditText ===

TEST_CASE("edit-text: TextGetSet")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    root->AddView(edit.Get());

    edit->SetText(u8"Hello");
    CHECK(edit->Text() == u8"Hello");

    edit->SetText(u8"World");
    CHECK(edit->Text() == u8"World");
}

TEST_CASE("edit-text: OnTextChangedFires")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    bool fired = false;
    edit->OnTextChanged.Add(Event<void(EditText*)>::Handler{[&fired](EditText*) { fired = true; }});

    // Simulate typing a character via the host interface.
    edit->ReplaceText(0, 0, u8"A");
    edit->OnTextModified();

    CHECK(fired);
}

TEST_CASE("edit-text: MaxLengthEnforced")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->MaxLength.SetValue(5);
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    for (int i = 0; i < 10; i++)
    {
        edit->Behavior().HandleTextInput(U'a');
    }

    CHECK(CharCount(edit->Text()) == 5);
}

TEST_CASE("edit-text: InputFilterBlocksInvalid")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetFilter(InputFilter::Digits());
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleTextInput(U'5');
    edit->Behavior().HandleTextInput(U'a');
    edit->Behavior().HandleTextInput(U'3');

    CHECK(edit->Text() == u8"53");
}

TEST_CASE("edit-text: IsReadOnlyPreventsModification")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetText(u8"Original");
    edit->IsReadOnly.SetValue(true);
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleTextInput(U'X');

    CHECK(edit->Text() == u8"Original");
}

TEST_CASE("edit-text: PlaceholderProperty")
{
    auto edit = MakeEdit();
    edit->SetPlaceholder(u8"Enter text...");
    CHECK(edit->Placeholder.Value() == u8"Enter text...");
}

TEST_CASE("edit-text: IsFocusableAndCursor")
{
    auto edit = MakeEdit();
    CHECK(edit->IsFocusable == true);
    CHECK(edit->IsTabStop == true);
    CHECK(edit->Cursor == CursorType::IBeam);
}

TEST_CASE("edit-text: MultilineProperty")
{
    auto edit = MakeEdit();
    CHECK(edit->Multiline.Value() == false);
    edit->Multiline.SetValue(true);
    CHECK(edit->Multiline.Value() == true);
}

TEST_CASE("edit-text: CursorMovement")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetText(u8"Hello");
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    // Cursor starts at 0 after SetText (reset).
    CHECK(edit->CursorPosition() == 0);

    edit->Behavior().HandleKeyDown(KeyCode::Right, KeyModifiers::None);
    CHECK(edit->CursorPosition() == 1);

    edit->Behavior().HandleKeyDown(KeyCode::End, KeyModifiers::None);
    CHECK(edit->CursorPosition() == 5);

    edit->Behavior().HandleKeyDown(KeyCode::Home, KeyModifiers::None);
    CHECK(edit->CursorPosition() == 0);
}

TEST_CASE("edit-text: SelectAll")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetText(u8"Hello");
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl);

    CHECK(edit->SelectionStart() == 0);
    CHECK(edit->SelectionEnd() == 5);
}

TEST_CASE("edit-text: DeleteBackspace")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetText(u8"Hello");
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleKeyDown(KeyCode::End, KeyModifiers::None);
    edit->Behavior().HandleKeyDown(KeyCode::Backspace, KeyModifiers::None);

    CHECK(edit->Text() == u8"Hell");
}

TEST_CASE("edit-text: DeleteForward")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    edit->SetText(u8"Hello");
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleKeyDown(KeyCode::Delete, KeyModifiers::None);

    CHECK(edit->Text() == u8"ello");
}

// Undo/Redo (not in the upstream test file; consecutive inserts coalesce into one undo entry).
TEST_CASE("edit-text: UndoRedo")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    root->AddView(edit.Get());
    LayoutPass(ctx, root.Get());

    edit->Behavior().HandleTextInput(U'a');
    edit->Behavior().HandleTextInput(U'b');
    edit->Behavior().HandleTextInput(U'c');
    CHECK(edit->Text() == u8"abc");

    // Ctrl+Z restores the pre-typing snapshot (inserts coalesced into one entry).
    edit->Behavior().HandleKeyDown(KeyCode::Z, KeyModifiers::Ctrl);
    CHECK(edit->Text() == u8"");

    // Ctrl+Y redoes back to "abc".
    edit->Behavior().HandleKeyDown(KeyCode::Y, KeyModifiers::Ctrl);
    CHECK(edit->Text() == u8"abc");
}

// OnEditingFinished fires on focus loss (the blur-commit hook); OnSubmit stays Enter/activate-only.
TEST_CASE("edit-text: OnEditingFinished_FiresOnceOnBlur_NoSubmit")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    auto other = MakeEdit();
    root->AddView(edit.Get());
    root->AddView(other.Get());
    LayoutPass(ctx, root.Get());

    int finished = 0;
    int submitted = 0;
    edit->OnEditingFinished.Add(
        Event<void(EditText*)>::Handler{[&finished](EditText*) { ++finished; }});
    edit->OnSubmit.Add(Event<void(EditText*)>::Handler{[&submitted](EditText*) { ++submitted; }});

    ctx.GetFocusManager()->SetFocus(edit.Get());
    edit->Behavior().HandleTextInput(U'x'); // an edit-in-progress to finish
    CHECK(finished == 0);                   // gaining focus / typing is not an edit end

    ctx.GetFocusManager()->SetFocus(other.Get()); // blur

    CHECK(finished == 1);  // exactly once
    CHECK(submitted == 0); // blur does NOT submit
}

// OnCommit: Enter fires it; blur fires it only when the text changed since focus gain.
TEST_CASE("edit-text: OnCommit_FiresOnEnterAndChangedBlur_NotUntouchedBlur")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    auto other = MakeEdit();
    root->AddView(edit.Get());
    root->AddView(other.Get());
    LayoutPass(ctx, root.Get());

    int committed = 0;
    edit->OnCommit.Add(Event<void(EditText*)>::Handler{[&committed](EditText*) { ++committed; }});

    // Untouched focus round-trip: no commit.
    ctx.GetFocusManager()->SetFocus(edit.Get());
    ctx.GetFocusManager()->SetFocus(other.Get());
    CHECK(committed == 0);

    // Typed then clicked away: exactly one commit.
    ctx.GetFocusManager()->SetFocus(edit.Get());
    edit->Behavior().HandleTextInput(U'x');
    ctx.GetFocusManager()->SetFocus(other.Get());
    CHECK(committed == 1);

    // Enter commits; the following blur (no further change) must not re-commit.
    ctx.GetFocusManager()->SetFocus(edit.Get());
    edit->Behavior().HandleTextInput(U'y');
    edit->OnActivate();
    CHECK(committed == 2);
    ctx.GetFocusManager()->SetFocus(other.Get());
    CHECK(committed == 2);
}

// === PasswordBox ===

TEST_CASE("edit-text: PasswordBox_DisplayTextIsMasked")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto pw = MakePassword();
    pw->SetText(u8"secret");
    root->AddView(pw.Get());
    LayoutPass(ctx, root.Get());

    String display;
    pw->GetDisplayText(display);

    CHECK(display == u8"******");
    CHECK(pw->Text() == u8"secret");
}

TEST_CASE("edit-text: PasswordBox_CustomPasswordChar")
{
    auto pw = MakePassword();
    pw->SetText(u8"abc");
    pw->PasswordChar.SetValue(U'#');

    String display;
    pw->GetDisplayText(display);

    CHECK(display == u8"###");
}

TEST_CASE("edit-text: PasswordBox_CopyDisabled")
{
    auto pw = MakePassword();
    CHECK(pw->Behavior().AllowClipboardCopy == false);
}

TEST_CASE("edit-text: Escape is not the box's key - it bubbles, while an editing key is handled")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto edit = MakeEdit();
    root->AddView(edit.Get());
    edit->SetText(u8"abc");

    KeyEventArgs escape;
    escape.Key = KeyCode::Escape;
    edit->OnKeyDown(escape);
    CHECK_FALSE(escape.Handled); // the dialog around the box closes on it
    KeyEventArgs left;
    left.Key = KeyCode::Left;
    edit->OnKeyDown(left);
    CHECK(left.Handled);
    CHECK(edit->Text() == u8"abc");
}

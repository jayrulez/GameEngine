// Ported from Sedulous.UI.Tests/src/EditTextTests.bf - the font-independent subset (cursor/selection/
// insert/delete/undo/redo logic, all of which run through TextEditingBehavior on CHARACTER indices and
// need no glyph shaping). Beef `[Friend]mBehavior` -> the public Behavior() accessor; `edit.Filter =`
// -> SetFilter(); Beef property setters -> Set*/.SetValue(). The pixel/caret-position cases (which need
// the deferred Fonts service) are not ported. Undo/Redo coverage is added here (Sedulous had none - it
// exercised undo only interactively).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<EditText> MakeEdit()
{
    return foundation::MakeRef<EditText>(foundation::DefaultAllocator());
}
static foundation::RefPtr<PasswordBox> MakePassword()
{
    return foundation::MakeRef<PasswordBox>(foundation::DefaultAllocator());
}

static foundation::i32 CharCount(StringView v) { return static_cast<foundation::i32>(foundation::Utf8Length(v)); }

// === EditText ===

TEST_CASE("edit-text: TextGetSet")
{
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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
    UIContext ctx;
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

// === PasswordBox ===

TEST_CASE("edit-text: PasswordBox_DisplayTextIsMasked")
{
    UIContext ctx;
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

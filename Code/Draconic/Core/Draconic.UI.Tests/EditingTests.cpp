// Editing subsystem tests. InputFilter cases are a faithful port of Sedulous.UI.Tests/src/
// InputFilterTests.bf (Beef `scope`/`new` -> value, delegate -> lambda; char literals are char32_t
// U'...'). UndoStack has no upstream test file (Sedulous exercises it only through EditText), so its
// cases below are direct unit coverage for the ported primitive.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
namespace foundation = draconic::foundation;

TEST_CASE("input-filter: None_AcceptsAll")
{
    InputFilter filter;
    CHECK(filter.Accept(U'a'));
    CHECK(filter.Accept(U'Z'));
    CHECK(filter.Accept(U'5'));
    CHECK(filter.Accept(U' '));
    CHECK(filter.Accept(U'!'));
}

TEST_CASE("input-filter: Digits_AcceptsOnlyDigits")
{
    InputFilter filter = InputFilter::Digits();
    CHECK(filter.Accept(U'0'));
    CHECK(filter.Accept(U'5'));
    CHECK(filter.Accept(U'9'));
    CHECK(!filter.Accept(U'a'));
    CHECK(!filter.Accept(U' '));
    CHECK(!filter.Accept(U'.'));
}

TEST_CASE("input-filter: HexDigits_AcceptsHexChars")
{
    InputFilter filter = InputFilter::HexDigits();
    CHECK(filter.Accept(U'0'));
    CHECK(filter.Accept(U'9'));
    CHECK(filter.Accept(U'a'));
    CHECK(filter.Accept(U'f'));
    CHECK(filter.Accept(U'A'));
    CHECK(filter.Accept(U'F'));
    CHECK(!filter.Accept(U'g'));
    CHECK(!filter.Accept(U'G'));
    CHECK(!filter.Accept(U' '));
}

TEST_CASE("input-filter: Custom_UsesDelegate")
{
    InputFilter filter;
    filter.SetCustomFilter([](char32_t c) { return c == U'x' || c == U'y'; });
    CHECK(filter.Accept(U'x'));
    CHECK(filter.Accept(U'y'));
    CHECK(!filter.Accept(U'z'));
    CHECK(!filter.Accept(U'a'));
}

// === UndoStack (consolidated coverage of upstream UndoStackTests.bf's 8 cases; each case here bundles
//     several upstream assertions - e.g. the first covers Empty_CannotUndo + PushState_CanUndo +
//     Undo_RestoresState + Undo_PushesToRedo) ===

TEST_CASE("undo-stack: PushState then Undo restores the pushed snapshot")
{
    UndoStack stack;
    CHECK(!stack.CanUndo());
    CHECK(!stack.CanRedo());

    stack.PushState(u8"a", 1, 1);
    CHECK(stack.CanUndo());
    CHECK(stack.UndoCount() == 1);

    foundation::String out;
    foundation::i32 cursor = 0, anchor = 0;
    const bool ok = stack.Undo(u8"ab", 2, 2, out, cursor, anchor);
    CHECK(ok);
    CHECK(out == u8"a");
    CHECK(cursor == 1);
    CHECK(anchor == 1);
    CHECK(!stack.CanUndo());
    CHECK(stack.CanRedo()); // the "ab" state went onto redo
}

TEST_CASE("undo-stack: Redo restores the state undone away")
{
    UndoStack stack;
    stack.PushState(u8"a", 1, 1);

    foundation::String out;
    foundation::i32 cursor = 0, anchor = 0;
    stack.Undo(u8"ab", 2, 2, out, cursor, anchor);

    // Redo should give back "ab" (the state that was current at Undo time).
    foundation::String redoOut;
    foundation::i32 rc = 0, ra = 0;
    const bool ok = stack.Redo(out.AsView(), cursor, anchor, redoOut, rc, ra);
    CHECK(ok);
    CHECK(redoOut == u8"ab");
    CHECK(rc == 2);
    CHECK(ra == 2);
    CHECK(stack.CanUndo());
    CHECK(!stack.CanRedo());
}

TEST_CASE("undo-stack: PushState clears the redo stack")
{
    UndoStack stack;
    stack.PushState(u8"a", 1, 1);

    foundation::String out;
    foundation::i32 cursor = 0, anchor = 0;
    stack.Undo(u8"ab", 2, 2, out, cursor, anchor);
    CHECK(stack.CanRedo());

    stack.PushState(u8"new", 3, 3);
    CHECK(!stack.CanRedo()); // redo cleared by the new push
}

TEST_CASE("undo-stack: capacity drops the oldest entry")
{
    UndoStack stack;
    stack.SetMaxEntries(2);
    CHECK(stack.MaxEntries() == 2);

    stack.PushState(u8"one", 0, 0);
    stack.PushState(u8"two", 0, 0);
    stack.PushState(u8"three", 0, 0); // drops "one"
    CHECK(stack.UndoCount() == 2);

    foundation::String out;
    foundation::i32 c = 0, a = 0;
    stack.Undo(u8"cur", 0, 0, out, c, a);
    CHECK(out == u8"three");
    stack.Undo(out.AsView(), c, a, out, c, a);
    CHECK(out == u8"two"); // "one" is gone
    CHECK(!stack.CanUndo());
}

TEST_CASE("undo-stack: Undo/Redo on empty stacks return false")
{
    UndoStack stack;
    foundation::String out;
    foundation::i32 c = 0, a = 0;
    CHECK(!stack.Undo(u8"x", 0, 0, out, c, a));
    CHECK(!stack.Redo(u8"x", 0, 0, out, c, a));

    stack.PushState(u8"a", 0, 0);
    stack.Clear();
    CHECK(!stack.CanUndo());
    CHECK(!stack.CanRedo());
}

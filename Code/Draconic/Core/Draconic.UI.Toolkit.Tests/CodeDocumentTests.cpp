// CodeDocument headless tests: line buffer + span extraction, the single Edit mutation with
// delta undo/redo and typing coalescing, marker line-tracking under edits, diagnostics and the
// execution line, word boundaries, and the word harvest behind document-word completion.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui.toolkit;

using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    CodeDocument MakeDoc(const char8_t* text)
    {
        CodeDocument doc;
        doc.SetText(StringView(text));
        return doc;
    }

    // Types one codepoint at the cursor, the way the widget will drive Edit for a keystroke.
    CodePosition Type(CodeDocument& doc, CodePosition at, const char8_t* ch, f64 time)
    {
        return doc.Edit(CodeSpan{at, at}, StringView(ch), CodeEditKind::Typing,
                        CodeCursorState{at, at}, time);
    }
}

TEST_CASE("toolkit-codedocument: LinesAndText")
{
    CodeDocument doc;
    CHECK(doc.LineCount() == 1);
    CHECK(doc.Line(0).IsEmpty());
    CHECK(doc.Text().IsEmpty());

    doc.SetText(u8"alpha\nbeta\r\ngamma");
    CHECK(doc.LineCount() == 3);
    CHECK(doc.Line(0) == StringView(u8"alpha"));
    CHECK(doc.Line(1) == StringView(u8"beta")); // \r stripped
    CHECK(doc.Line(2) == StringView(u8"gamma"));
    CHECK(doc.Text().AsView() == StringView(u8"alpha\nbeta\ngamma"));

    // Trailing newline produces a final empty line.
    doc.SetText(u8"one\n");
    CHECK(doc.LineCount() == 2);
    CHECK(doc.Line(1).IsEmpty());
}

TEST_CASE("toolkit-codedocument: CodepointColumns")
{
    // "héllo" - the 'é' is two bytes, one column.
    CodeDocument doc = MakeDoc(u8"héllo");
    CHECK(doc.LineLength(0) == 5);
    CHECK(doc.ColumnToByte(0, 1) == 1);
    CHECK(doc.ColumnToByte(0, 2) == 3); // past the 2-byte e-acute
    CHECK(doc.ByteToColumn(0, 3) == 2);
    CHECK(doc.ByteToColumn(0, 999) == 5); // clamps

    const CodePosition clamped = doc.ClampPosition(CodePosition{0, 99});
    CHECK(clamped.column == 5);
    CHECK(doc.ClampPosition(CodePosition{99, 0}) == doc.EndPosition());
}

TEST_CASE("toolkit-codedocument: TextInSpan")
{
    CodeDocument doc = MakeDoc(u8"alpha\nbeta\ngamma");
    CHECK(doc.TextInSpan(CodeSpan{{0, 1}, {0, 4}}).AsView() == StringView(u8"lph"));
    CHECK(doc.TextInSpan(CodeSpan{{0, 3}, {2, 2}}).AsView() == StringView(u8"ha\nbeta\nga"));
    // Reversed spans normalize.
    CHECK(doc.TextInSpan(CodeSpan{{2, 2}, {0, 3}}).AsView() == StringView(u8"ha\nbeta\nga"));
}

TEST_CASE("toolkit-codedocument: EditInsertAndDelete")
{
    CodeDocument doc = MakeDoc(u8"alpha\ngamma");

    // Multi-line paste in the middle of line 0.
    const CodePosition end =
        doc.Edit(CodeSpan{{0, 5}, {0, 5}}, StringView(u8"\nbeta"), CodeEditKind::Paste,
                 CodeCursorState{{0, 5}, {0, 5}}, 0.0);
    CHECK(doc.LineCount() == 3);
    CHECK(doc.Text().AsView() == StringView(u8"alpha\nbeta\ngamma"));
    CHECK(end == CodePosition{1, 4});

    // Delete a range spanning lines (joins them).
    (void)doc.Edit(CodeSpan{{0, 3}, {2, 2}}, StringView(u8""), CodeEditKind::Delete,
                   CodeCursorState{{0, 3}, {2, 2}}, 1.0);
    CHECK(doc.LineCount() == 1);
    CHECK(doc.Text().AsView() == StringView(u8"alpmma"));
}

TEST_CASE("toolkit-codedocument: UndoCoalescesTyping")
{
    CodeDocument doc;
    CodePosition cursor{0, 0};
    cursor = Type(doc, cursor, u8"a", 0.0);
    cursor = Type(doc, cursor, u8"b", 0.2);
    cursor = Type(doc, cursor, u8"c", 0.4);
    CHECK(doc.Text().AsView() == StringView(u8"abc"));

    // All three keystrokes were contiguous + same kind + inside the window: ONE undo entry.
    CodeCursorState state;
    CHECK(doc.Undo(state));
    CHECK(doc.Text().IsEmpty());
    CHECK(state.cursor == CodePosition{0, 0});
    CHECK(!doc.CanUndo());

    // Redo restores the whole run and lands the cursor after it.
    CHECK(doc.Redo(state));
    CHECK(doc.Text().AsView() == StringView(u8"abc"));
    CHECK(state.cursor == CodePosition{0, 3});
}

TEST_CASE("toolkit-codedocument: UndoChainBreaks")
{
    CodeDocument doc;
    CodePosition cursor{0, 0};

    SUBCASE("time gap splits entries")
    {
        cursor = Type(doc, cursor, u8"a", 0.0);
        cursor = Type(doc, cursor, u8"b", 5.0); // past kUndoCoalesceSeconds
        CodeCursorState state;
        CHECK(doc.Undo(state));
        CHECK(doc.Text().AsView() == StringView(u8"a"));
    }

    SUBCASE("kind change splits entries")
    {
        cursor = Type(doc, cursor, u8"a", 0.0);
        (void)doc.Edit(CodeSpan{cursor, cursor}, StringView(u8"\n"), CodeEditKind::Newline,
                       CodeCursorState{cursor, cursor}, 0.1);
        CodeCursorState state;
        CHECK(doc.Undo(state));
        CHECK(doc.Text().AsView() == StringView(u8"a"));
    }

    SUBCASE("explicit BreakUndoChain splits entries")
    {
        cursor = Type(doc, cursor, u8"a", 0.0);
        doc.BreakUndoChain();
        cursor = Type(doc, cursor, u8"b", 0.1);
        CodeCursorState state;
        CHECK(doc.Undo(state));
        CHECK(doc.Text().AsView() == StringView(u8"a"));
    }

    SUBCASE("non-contiguous typing splits entries")
    {
        cursor = Type(doc, cursor, u8"a", 0.0);
        (void)Type(doc, CodePosition{0, 0}, u8"x", 0.1); // typed at line start, not at the chain end
        CodeCursorState state;
        CHECK(doc.Undo(state));
        CHECK(doc.Text().AsView() == StringView(u8"a"));
    }
}

TEST_CASE("toolkit-codedocument: BackspaceCoalescing")
{
    CodeDocument doc = MakeDoc(u8"abc");
    // Backspace runs right-to-left; contiguity is end == previous begin.
    (void)doc.Edit(CodeSpan{{0, 2}, {0, 3}}, StringView(u8""), CodeEditKind::Backspace,
                   CodeCursorState{{0, 3}, {0, 3}}, 0.0);
    (void)doc.Edit(CodeSpan{{0, 1}, {0, 2}}, StringView(u8""), CodeEditKind::Backspace,
                   CodeCursorState{{0, 2}, {0, 2}}, 0.2);
    CHECK(doc.Text().AsView() == StringView(u8"a"));

    CodeCursorState state;
    CHECK(doc.Undo(state));
    CHECK(doc.Text().AsView() == StringView(u8"abc"));
    CHECK(state.cursor == CodePosition{0, 3});
    CHECK(!doc.CanUndo());
}

TEST_CASE("toolkit-codedocument: RedoClearedByNewEdit")
{
    CodeDocument doc;
    (void)Type(doc, CodePosition{0, 0}, u8"a", 0.0);
    CodeCursorState state;
    CHECK(doc.Undo(state));
    CHECK(doc.CanRedo());
    (void)Type(doc, CodePosition{0, 0}, u8"z", 9.0);
    CHECK(!doc.CanRedo());
}

TEST_CASE("toolkit-codedocument: MarkersTrackEdits")
{
    CodeDocument doc = MakeDoc(u8"a\nb\nc\nd");
    doc.SetMarker(2, CodeMarkers::Breakpoint);

    SUBCASE("insert above shifts down")
    {
        (void)doc.Edit(CodeSpan{{0, 1}, {0, 1}}, StringView(u8"\nnew"), CodeEditKind::Paste,
                       CodeCursorState{}, 0.0);
        CHECK(HasMarker(doc.MarkersOn(3), CodeMarkers::Breakpoint));
        CHECK(doc.MarkersOn(2) == CodeMarkers::None);
    }

    SUBCASE("delete above shifts up")
    {
        (void)doc.Edit(CodeSpan{{0, 0}, {1, 0}}, StringView(u8""), CodeEditKind::Delete,
                       CodeCursorState{}, 0.0);
        CHECK(HasMarker(doc.MarkersOn(1), CodeMarkers::Breakpoint));
    }

    SUBCASE("deleting the marker's line drops the marker")
    {
        (void)doc.Edit(CodeSpan{{1, 0}, {2, 1}}, StringView(u8""), CodeEditKind::Delete,
                       CodeCursorState{}, 0.0);
        Array<i32> lines;
        doc.CollectMarkerLines(CodeMarkers::Breakpoint, lines);
        CHECK(lines.Size() == 0);
    }

    SUBCASE("edit below leaves it alone")
    {
        (void)doc.Edit(CodeSpan{{3, 0}, {3, 1}}, StringView(u8"x"), CodeEditKind::Typing,
                       CodeCursorState{}, 0.0);
        CHECK(HasMarker(doc.MarkersOn(2), CodeMarkers::Breakpoint));
    }
}

TEST_CASE("toolkit-codedocument: ToggleAndCollect")
{
    CodeDocument doc = MakeDoc(u8"a\nb\nc");
    CHECK(doc.ToggleMarker(2, CodeMarkers::Breakpoint));
    CHECK(doc.ToggleMarker(0, CodeMarkers::Breakpoint));
    CHECK(!doc.ToggleMarker(2, CodeMarkers::Breakpoint)); // toggles off

    Array<i32> lines;
    doc.CollectMarkerLines(CodeMarkers::Breakpoint, lines);
    REQUIRE(lines.Size() == 1);
    CHECK(lines[0] == 0);

    // Out-of-range lines are ignored.
    doc.SetMarker(99, CodeMarkers::Breakpoint);
    doc.CollectMarkerLines(CodeMarkers::Breakpoint, lines);
    CHECK(lines.Size() == 1);
}

TEST_CASE("toolkit-codedocument: DiagnosticsAndExecutionLine")
{
    CodeDocument doc = MakeDoc(u8"a\nb\nc\nd");
    Array<CodeDiagnostic> diags;
    diags.PushBack(CodeDiagnostic{true, 1, String(u8"boom")});
    diags.PushBack(CodeDiagnostic{false, 3, String(u8"meh")});
    doc.SetDiagnostics(Move(diags));
    doc.SetExecutionLine(2);

    CHECK(HasMarker(doc.MarkersOn(1), CodeMarkers::Error));
    CHECK(HasMarker(doc.MarkersOn(3), CodeMarkers::Warning));
    CHECK(HasMarker(doc.MarkersOn(2), CodeMarkers::ExecutionLine));
    REQUIRE(doc.DiagnosticOn(1) != nullptr);
    CHECK(doc.DiagnosticOn(1)->message.AsView() == StringView(u8"boom"));
    CHECK(doc.DiagnosticOn(0) == nullptr);

    // Inserting a line above shifts diagnostics and the execution line.
    (void)doc.Edit(CodeSpan{{0, 0}, {0, 0}}, StringView(u8"top\n"), CodeEditKind::Paste,
                   CodeCursorState{}, 0.0);
    CHECK(HasMarker(doc.MarkersOn(2), CodeMarkers::Error));
    CHECK(doc.ExecutionLine() == 3);

    // Deleting the execution line clears it.
    (void)doc.Edit(CodeSpan{{2, 0}, {3, 1}}, StringView(u8""), CodeEditKind::Delete,
                   CodeCursorState{}, 1.0);
    CHECK(doc.ExecutionLine() == -1);
}

TEST_CASE("toolkit-codedocument: WordBoundaries")
{
    CodeDocument doc = MakeDoc(u8"foo bar_baz(qux)");

    CHECK(doc.NextWordBoundary(CodePosition{0, 0}) == CodePosition{0, 4});   // past "foo "
    CHECK(doc.NextWordBoundary(CodePosition{0, 4}) == CodePosition{0, 11});  // past "bar_baz"
    CHECK(doc.PrevWordBoundary(CodePosition{0, 11}) == CodePosition{0, 4});  // back to its start
    CHECK(doc.PrevWordBoundary(CodePosition{0, 4}) == CodePosition{0, 0});

    const CodeSpan word = doc.WordAt(CodePosition{0, 6}); // inside bar_baz
    CHECK(word.begin == CodePosition{0, 4});
    CHECK(word.end == CodePosition{0, 11});

    // Cursor at end of a word still finds it; next to a non-word char it finds nothing.
    CHECK(doc.WordAt(CodePosition{0, 3}).begin == CodePosition{0, 0});
    CHECK(doc.WordAt(CodePosition{0, 16}).IsEmpty()); // line end; ')' on the left is not a word
}

TEST_CASE("toolkit-codedocument: WordBoundariesAcrossLines")
{
    CodeDocument doc = MakeDoc(u8"foo\nbar");
    CHECK(doc.NextWordBoundary(CodePosition{0, 3}) == CodePosition{1, 0});
    CHECK(doc.PrevWordBoundary(CodePosition{1, 0}) == CodePosition{0, 3});
}

TEST_CASE("toolkit-codedocument: WordHarvest")
{
    CodeDocument doc = MakeDoc(u8"var count = 10\nfn update(count, delta_time)\n");
    const Span<const String> words = doc.Words();

    auto contains = [&](const char8_t* w) {
        for (usize i = 0; i < words.Size(); ++i)
        {
            if (words[i].AsView() == StringView(w))
            {
                return true;
            }
        }
        return false;
    };
    CHECK(contains(u8"var"));
    CHECK(contains(u8"count"));
    CHECK(contains(u8"update"));
    CHECK(contains(u8"delta_time"));
    CHECK(!contains(u8"10")); // numbers are not identifiers

    // Duplicates collapse: "count" appears twice, harvested once.
    usize countHits = 0;
    for (usize i = 0; i < words.Size(); ++i)
    {
        if (words[i].AsView() == StringView(u8"count"))
        {
            ++countHits;
        }
    }
    CHECK(countHits == 1);

    // Cache follows the version: an edit introduces a new identifier.
    (void)doc.Edit(CodeSpan{{2, 0}, {2, 0}}, StringView(u8"newWord"), CodeEditKind::Typing,
                   CodeCursorState{}, 0.0);
    const Span<const String> after = doc.Words();
    bool found = false;
    for (usize i = 0; i < after.Size(); ++i)
    {
        found = found || after[i].AsView() == StringView(u8"newWord");
    }
    CHECK(found);
}

TEST_CASE("toolkit-codedocument: FindAll")
{
    CodeDocument doc = MakeDoc(u8"Count count\ncounter\nno match here");

    Array<CodeSpan> matches;
    doc.FindAll(StringView(u8"count"), false, false, matches);
    REQUIRE(matches.Size() == 3); // Count, count, counter's prefix
    CHECK(matches[0].begin == CodePosition{0, 0});
    CHECK(matches[1].begin == CodePosition{0, 6});
    CHECK(matches[2].begin == CodePosition{1, 0});

    doc.FindAll(StringView(u8"count"), true, false, matches);
    REQUIRE(matches.Size() == 2); // case-sensitive drops "Count"

    doc.FindAll(StringView(u8"count"), false, true, matches);
    REQUIRE(matches.Size() == 2); // whole-word drops "counter"

    // Non-overlapping: "aaa" in "aaaa" matches once.
    doc.SetText(u8"aaaa");
    doc.FindAll(StringView(u8"aaa"), true, false, matches);
    CHECK(matches.Size() == 1);

    doc.FindAll(StringView(u8""), true, false, matches);
    CHECK(matches.Size() == 0);
}

TEST_CASE("toolkit-codedocument: CompoundEditIsOneUndoEntry")
{
    CodeDocument doc = MakeDoc(u8"aa bb aa");
    doc.BeginCompoundEdit();
    (void)doc.Edit(CodeSpan{{0, 6}, {0, 8}}, StringView(u8"XX"), CodeEditKind::Other,
                   CodeCursorState{}, 0.0);
    (void)doc.Edit(CodeSpan{{0, 0}, {0, 2}}, StringView(u8"XX"), CodeEditKind::Other,
                   CodeCursorState{}, 0.0);
    doc.EndCompoundEdit();
    CHECK(doc.Text().AsView() == StringView(u8"XX bb XX"));

    CodeCursorState state;
    CHECK(doc.Undo(state));
    CHECK(doc.Text().AsView() == StringView(u8"aa bb aa")); // BOTH edits reverted at once
    CHECK(!doc.CanUndo());
    CHECK(doc.Redo(state));
    CHECK(doc.Text().AsView() == StringView(u8"XX bb XX"));
}

TEST_CASE("toolkit-codedocument: BracketMatching")
{
    CodeDocument doc = MakeDoc(u8"fn(a, [b {\nnested}\n])");

    CodePosition match{};
    // The '(' at (0,2) pairs with the ')' at (2,1).
    REQUIRE(doc.FindMatchingBracket(CodePosition{0, 2}, match));
    CHECK(match == CodePosition{2, 1});
    // ...and backwards from the ')'.
    REQUIRE(doc.FindMatchingBracket(CodePosition{2, 1}, match));
    CHECK(match == CodePosition{0, 2});
    // The '{' at (0,9) closes at (1,6) across the line break, nesting-aware.
    REQUIRE(doc.FindMatchingBracket(CodePosition{0, 9}, match));
    CHECK(match == CodePosition{1, 6});

    // Not a bracket / unmatched.
    CHECK(!doc.FindMatchingBracket(CodePosition{0, 0}, match));
    doc.SetText(u8"(((");
    CHECK(!doc.FindMatchingBracket(CodePosition{0, 0}, match));
}

TEST_CASE("toolkit-codedocument: OnLinesChanged")
{
    CodeDocument doc = MakeDoc(u8"a\nb\nc");
    i32 first = -99, removed = -99, added = -99;
    doc.OnLinesChanged = [&](i32 f, i32 r, i32 a) {
        first = f;
        removed = r;
        added = a;
    };

    (void)doc.Edit(CodeSpan{{1, 0}, {1, 0}}, StringView(u8"x\ny"), CodeEditKind::Paste,
                   CodeCursorState{}, 0.0);
    CHECK(first == 1);
    CHECK(removed == 1);
    CHECK(added == 2);

    doc.SetText(u8"z");
    CHECK(first == 0);
    CHECK(removed == -1); // full-reload signal
    CHECK(added == 1);
}

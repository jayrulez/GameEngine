// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CodeEditView headless tests: real InputManager key/mouse driving (typing, navigation,
// undo chords, Tab-through-WantsTabKey, gutter breakpoint clicks, clipboard round trip) plus
// CompletionModel unit coverage (filter ranking + popup key routing without a UIContext).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    class TestClipboard final : public IClipboard
    {
    public:
        String stored;
        Status GetText(String& outText) override
        {
            outText = String(stored.AsView());
            return Status{};
        }
        Status SetText(StringView text) override
        {
            stored = String(text);
            return Status{};
        }
        [[nodiscard]] bool HasText() override { return !stored.IsEmpty(); }
    };

    struct Harness
    {
        UIContext ctx{DefaultAllocator()};
        RefPtr<RootView> root;
        RefPtr<CodeEditView> view;
        TestClipboard clipboard;

        Harness()
        {
            root = MakeRef<RootView>(DefaultAllocator());
            root->ViewportSize = Float2{800, 600};
            ctx.AddRootView(root.Get());
            ctx.SetClipboard(&clipboard);
            view = MakeRef<CodeEditView>(DefaultAllocator());
            root->AddView(view.Get());
            LayoutPass();
            ctx.GetFocusManager()->SetFocus(view.Get());
        }

        void LayoutPass()
        {
            ctx.BeginFrame(0.016f);
            root->Measure(BoxConstraints::Tight(800, 600));
            root->Layout(0, 0, 800, 600);
        }

        void Key(KeyCode key, KeyModifiers mods = KeyModifiers::None)
        {
            (void)ctx.GetInputManager()->ProcessKeyDown(key, mods, false);
        }

        void Type(const char8_t* text)
        {
            StringView s(text);
            usize i = 0;
            while (i < s.Size())
            {
                const u32 cp = DecodeUtf8(s, i);
                (void)ctx.GetInputManager()->ProcessTextInput(static_cast<char32_t>(cp));
            }
        }

        // Local coordinates of a buffer position (the view is at the root origin).
        [[nodiscard]] Float2 PointAt(i32 line, i32 column) const
        {
            const f32 x = view->GutterWidth() + 6.0f +
                          static_cast<f32>(column) * view->ColumnAdvance() + 1.0f;
            const f32 y = 4.0f + (static_cast<f32>(line) + 0.5f) * view->LineHeight();
            return Float2{x, y};
        }

        void Click(i32 line, i32 column, KeyModifiers mods = KeyModifiers::None)
        {
            const Float2 p = PointAt(line, column);
            (void)mods;
            (void)ctx.GetInputManager()->ProcessMouseMove(p.x, p.y);
            (void)ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, p.x, p.y, 0.0f);
            (void)ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, p.x, p.y);
        }
    };
}

TEST_CASE("toolkit-codeeditview: TypingAndNewline")
{
    Harness h;
    h.Type(u8"let x = 1");
    CHECK(h.view->Text().AsView() == StringView(u8"let x = 1"));
    CHECK(h.view->CursorPosition() == CodePosition{0, 9});

    h.Key(KeyCode::Return);
    h.Type(u8"done");
    CHECK(h.view->Text().AsView() == StringView(u8"let x = 1\ndone"));
    CHECK(h.view->CursorPosition() == CodePosition{1, 4});
}

TEST_CASE("toolkit-codeeditview: NewlineCopiesIndent")
{
    Harness h;
    h.Type(u8"    indented");
    h.Key(KeyCode::Return);
    CHECK(h.view->Document().Line(1) == StringView(u8"    "));
    CHECK(h.view->CursorPosition() == CodePosition{1, 4});
}

TEST_CASE("toolkit-codeeditview: NavigationAndSelection")
{
    Harness h;
    h.view->SetText(u8"alpha beta\ngamma");

    // Word motion.
    h.Key(KeyCode::Right, KeyModifiers::Ctrl);
    CHECK(h.view->CursorPosition() == CodePosition{0, 6});

    // Shift extends; the selection reads back.
    h.Key(KeyCode::End, KeyModifiers::Shift);
    CHECK(h.view->SelectedText().AsView() == StringView(u8"beta"));

    // Plain arrow collapses to the selection edge.
    h.Key(KeyCode::Left);
    CHECK(!h.view->HasSelection());
    CHECK(h.view->CursorPosition() == CodePosition{0, 6});

    // Down keeps the goal column even across a shorter line.
    h.Key(KeyCode::End);
    h.Key(KeyCode::Down);
    CHECK(h.view->CursorPosition() == CodePosition{1, 5}); // clamped to "gamma"
}

TEST_CASE("toolkit-codeeditview: SmartHome")
{
    Harness h;
    h.view->SetText(u8"    body");
    h.view->SetCursorPosition(CodePosition{0, 8});
    h.Key(KeyCode::Home);
    CHECK(h.view->CursorPosition() == CodePosition{0, 4}); // first non-space
    h.Key(KeyCode::Home);
    CHECK(h.view->CursorPosition() == CodePosition{0, 0}); // then hard start
}

TEST_CASE("toolkit-codeeditview: TabThroughFocusManager")
{
    Harness h;
    // Tab must REACH the editor (WantsTabKey) instead of moving focus.
    h.Type(u8"a");
    h.Key(KeyCode::Tab);
    CHECK(h.ctx.GetFocusManager()->FocusedView() == h.view.Get());
    CHECK(h.view->Document().Line(0) == StringView(u8"a   ")); // to the 4-column stop

    // Multi-line selection indents; Shift+Tab dedents.
    h.view->SetText(u8"one\ntwo");
    h.view->SelectAll();
    h.Key(KeyCode::Tab);
    CHECK(h.view->Document().Line(0) == StringView(u8"    one"));
    CHECK(h.view->Document().Line(1) == StringView(u8"    two"));
    h.Key(KeyCode::Tab, KeyModifiers::Shift);
    CHECK(h.view->Document().Line(0) == StringView(u8"one"));
    CHECK(h.view->Document().Line(1) == StringView(u8"two"));
}

TEST_CASE("toolkit-codeeditview: UndoRedoChords")
{
    Harness h;
    h.Type(u8"abc");
    h.Key(KeyCode::Z, KeyModifiers::Ctrl);
    CHECK(h.view->Text().IsEmpty());
    h.Key(KeyCode::Y, KeyModifiers::Ctrl);
    CHECK(h.view->Text().AsView() == StringView(u8"abc"));
    h.Key(KeyCode::Z, KeyModifiers::Ctrl | KeyModifiers::Shift); // Ctrl+Shift+Z = redo (no-op here)
    CHECK(h.view->Text().AsView() == StringView(u8"abc"));
}

TEST_CASE("toolkit-codeeditview: ClipboardRoundTrip")
{
    Harness h;
    h.view->SetText(u8"copy me\nsecond");
    h.view->SetCursorPosition(CodePosition{0, 0});
    h.Key(KeyCode::End, KeyModifiers::Shift);
    h.Key(KeyCode::C, KeyModifiers::Ctrl);
    CHECK(h.clipboard.stored.AsView() == StringView(u8"copy me"));

    h.view->SetCursorPosition(CodePosition{1, 6});
    h.Key(KeyCode::Return);
    h.Key(KeyCode::V, KeyModifiers::Ctrl);
    CHECK(h.view->Document().Line(2) == StringView(u8"copy me"));

    // Cut removes.
    h.view->SetCursorPosition(CodePosition{2, 0});
    h.Key(KeyCode::End, KeyModifiers::Shift);
    h.Key(KeyCode::X, KeyModifiers::Ctrl);
    CHECK(h.view->Document().Line(2).IsEmpty());
}

TEST_CASE("toolkit-codeeditview: UnknownChordsStayUnhandled")
{
    Harness h;
    h.Type(u8"x");
    // Ctrl+S is an application shortcut - the editor must NOT consume it.
    const bool handled =
        h.ctx.GetInputManager()->ProcessKeyDown(KeyCode::S, KeyModifiers::Ctrl, false);
    (void)handled; // dispatch itself may return true via shortcut table; the text is untouched
    CHECK(h.view->Text().AsView() == StringView(u8"x"));
}

TEST_CASE("toolkit-codeeditview: MouseCursorAndSelection")
{
    Harness h;
    h.view->SetText(u8"alpha beta\ngamma");
    h.Click(1, 3);
    CHECK(h.view->CursorPosition() == CodePosition{1, 3});

    // Drag selects.
    const Float2 from = h.PointAt(0, 2);
    const Float2 to = h.PointAt(1, 2);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(from.x, from.y);
    (void)h.ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, from.x, from.y, 0.0f);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(to.x, to.y);
    (void)h.ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, to.x, to.y);
    CHECK(h.view->SelectedText().AsView() == StringView(u8"pha beta\nga"));
}

TEST_CASE("toolkit-codeeditview: GutterBreakpointClick")
{
    Harness h;
    h.view->SetText(u8"one\ntwo\nthree");
    i32 toggledLine = -1;
    bool toggledSet = false;
    h.view->OnBreakpointToggled.Add(Event<void(i32, bool)>::Handler{[&](i32 line, bool set)
                                                                    {
                                                                        toggledLine = line;
                                                                        toggledSet = set;
                                                                    }});

    const f32 y = 4.0f + 1.5f * h.view->LineHeight(); // line 1, marker margin x
    (void)h.ctx.GetInputManager()->ProcessMouseMove(6.0f, y);
    (void)h.ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, 6.0f, y, 0.0f);
    (void)h.ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, 6.0f, y);

    CHECK(toggledLine == 1);
    CHECK(toggledSet);
    CHECK(HasMarker(h.view->Document().MarkersOn(1), CodeMarkers::Breakpoint));
}

TEST_CASE("toolkit-codeeditview: CompletionEndToEnd")
{
    Harness h;
    h.view->SetText(u8"counter = 0\ncontinue_run = 1\n");
    h.view->SetCursorPosition(h.view->Document().EndPosition());

    // Two identifier chars auto-open the popup with both harvested words.
    h.Type(u8"co");
    REQUIRE(h.view->Completion().IsOpen());
    CHECK(h.view->Completion().ItemCount() == 2);

    // Down + Enter accepts the second candidate (sorted: continue_run, counter).
    h.Key(KeyCode::Down);
    h.Key(KeyCode::Return);
    CHECK(!h.view->Completion().IsOpen());
    CHECK(h.view->Document().Line(2) == StringView(u8"counter"));

    // Escape dismisses without inserting.
    h.Key(KeyCode::Return);
    h.Type(u8"co");
    REQUIRE(h.view->Completion().IsOpen());
    h.Key(KeyCode::Escape);
    CHECK(!h.view->Completion().IsOpen());
    CHECK(h.view->Document().Line(3) == StringView(u8"co"));
}

TEST_CASE("toolkit-codeeditview: AutoScrollKeepsCaretLineFullyVisible")
{
    // Regression (first smoke run): each Enter at the bottom edge advanced the scroll via
    // EnsureCursorVisible, then the vertical ScrollBar - fed the new value BEFORE its max was
    // updated - clamped it against the stale max and wrote the clamped value back, leaving
    // the caret line half-hidden. Reproduce the exact sequence: Enter, then a layout pass.
    Harness h;
    const f32 lineH = h.view->LineHeight();
    const i32 overflow = static_cast<i32>(600.0f / lineH) + 10;
    for (i32 i = 0; i < overflow; ++i)
    {
        h.Key(KeyCode::Return);
        h.LayoutPass();
    }
    // The caret line's bottom must sit fully inside the widget after every pass.
    const f32 caretBottom = 4.0f +
                            (static_cast<f32>(h.view->CursorPosition().line) + 1.0f) * lineH -
                            h.view->ScrollY();
    CHECK(caretBottom <= h.view->Height() + 0.01f);
    CHECK(caretBottom >= lineH); // and on screen at all, not scrolled past
}

TEST_CASE("toolkit-codeeditview: CursorPerRegion")
{
    // The gutter + scrollbars must not show the IBeam. The text area is IBeam;
    // the gutter and the scrollbar children resolve to the arrow.
    Harness h;
    h.view->SetText(u8"one\ntwo");

    const Float2 text = h.PointAt(0, 2);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(text.x, text.y);
    CHECK(h.ctx.GetInputManager()->CurrentCursor() == CursorType::IBeam);

    (void)h.ctx.GetInputManager()->ProcessMouseMove(6.0f, text.y); // marker margin
    CHECK(h.ctx.GetInputManager()->CurrentCursor() == CursorType::Arrow);

    // Overflow vertically so the scrollbar child exists, then hover it.
    String longText;
    for (i32 i = 0; i < 200; ++i)
    {
        longText.Append(u8"line\n");
    }
    h.view->SetText(longText.AsView());
    h.LayoutPass();
    (void)h.ctx.GetInputManager()->ProcessMouseMove(800.0f - 4.0f, 300.0f);
    CHECK(h.ctx.GetInputManager()->CurrentCursor() == CursorType::Arrow);
}

TEST_CASE("toolkit-codeeditview: ReadOnlyBlocksEdits")
{
    Harness h;
    h.view->SetText(u8"locked");
    h.view->ReadOnly = true;
    h.Type(u8"x");
    h.Key(KeyCode::Backspace);
    h.Key(KeyCode::Return);
    CHECK(h.view->Text().AsView() == StringView(u8"locked"));
}

// ---- find/replace, comment toggle, brace indent, tooltip ----

namespace
{
    CLikeLexerSpec CommentableSpec()
    {
        CLikeLexerSpec spec; // no tables needed; the toggle only reads LineCommentPrefix
        return spec;
    }

    UniquePtr<ICodeLexer> MakeCLike()
    {
        return UniquePtr<ICodeLexer>(
            core::DefaultAllocator().New<CLikeLexer>(CommentableSpec()),
            core::DefaultAllocator());
    }
}

TEST_CASE("toolkit-codeeditview: FindBarSearchAndNavigate")
{
    Harness h;
    h.view->SetText(u8"alpha beta\nalpha gamma\nend alpha");

    h.Key(KeyCode::F, KeyModifiers::Ctrl);
    CHECK(h.view->FindBar() == CodeEditView::FindBarMode::Find);
    // Focus moved into the bar's field; typing lands there, and the search runs live.
    CHECK(h.ctx.GetFocusManager()->FocusedView() != h.view.Get());
    h.Type(u8"alpha");
    REQUIRE(h.view->SearchMatches().Size() == 3);
    CHECK(h.view->CurrentMatchIndex() == 0);
    CHECK(h.view->SelectedText().AsView() == StringView(u8"alpha")); // typing selects

    // F3 from the FIELD advances (capture-phase interplay), wrapping at the end.
    h.Key(KeyCode::F3);
    CHECK(h.view->CurrentMatchIndex() == 1);
    h.Key(KeyCode::F3);
    h.Key(KeyCode::F3);
    CHECK(h.view->CurrentMatchIndex() == 0); // wrapped
    h.Key(KeyCode::F3, KeyModifiers::Shift);
    CHECK(h.view->CurrentMatchIndex() == 2);

    // Escape closes, clears highlights, and returns focus to the editor.
    h.Key(KeyCode::Escape);
    CHECK(h.view->FindBar() == CodeEditView::FindBarMode::Closed);
    CHECK(h.view->SearchMatches().Size() == 0);
    CHECK(h.ctx.GetFocusManager()->FocusedView() == h.view.Get());
}

TEST_CASE("toolkit-codeeditview: ReplaceAllIsOneUndo")
{
    Harness h;
    h.view->SetText(u8"foo x foo\nfoo");
    h.view->OpenFindBar(true);
    h.view->SetSearchQuery(u8"foo");
    h.view->SetReplaceText(u8"barbar");
    REQUIRE(h.view->SearchMatches().Size() == 3);

    h.view->ReplaceAll();
    CHECK(h.view->Text().AsView() == StringView(u8"barbar x barbar\nbarbar"));

    h.view->CloseFindBar();
    h.Key(KeyCode::Z, KeyModifiers::Ctrl);
    CHECK(h.view->Text().AsView() == StringView(u8"foo x foo\nfoo")); // one undo step
}

TEST_CASE("toolkit-codeeditview: GoToLine")
{
    Harness h;
    String text;
    for (i32 i = 0; i < 50; ++i)
    {
        text.Append(u8"line\n");
    }
    h.view->SetText(text.AsView());
    h.Key(KeyCode::G, KeyModifiers::Ctrl);
    CHECK(h.view->FindBar() == CodeEditView::FindBarMode::GoToLine);
    h.Type(u8"42");
    h.Key(KeyCode::Return);
    CHECK(h.view->FindBar() == CodeEditView::FindBarMode::Closed);
    CHECK(h.view->CursorPosition().line == 41); // 1-based entry
}

TEST_CASE("toolkit-codeeditview: ToggleLineComment")
{
    Harness h;
    h.view->SetLexer(MakeCLike()); // LineCommentPrefix "//"
    h.view->SetText(u8"one\n\ntwo");
    h.view->SelectAll();
    h.Key(KeyCode::Slash, KeyModifiers::Ctrl);
    CHECK(h.view->Document().Line(0) == StringView(u8"// one"));
    CHECK(h.view->Document().Line(1) == StringView(u8"")); // blank untouched
    CHECK(h.view->Document().Line(2) == StringView(u8"// two"));

    // Toggle back off (selection was re-established over the lines).
    h.Key(KeyCode::Slash, KeyModifiers::Ctrl);
    CHECK(h.view->Document().Line(0) == StringView(u8"one"));
    CHECK(h.view->Document().Line(2) == StringView(u8"two"));

    // One undo step per toggle.
    h.Key(KeyCode::Z, KeyModifiers::Ctrl);
    CHECK(h.view->Document().Line(0) == StringView(u8"// one"));
}

TEST_CASE("toolkit-codeeditview: ToggleLineCommentXmlIsNoOp")
{
    Harness h;
    h.view->SetLexer(UniquePtr<ICodeLexer>(core::DefaultAllocator().New<XmlLexer>(),
                                           core::DefaultAllocator()));
    h.view->SetText(u8"<a/>");
    h.Key(KeyCode::Slash, KeyModifiers::Ctrl);
    CHECK(h.view->Text().AsView() == StringView(u8"<a/>")); // no line comments in XML
}

TEST_CASE("toolkit-codeeditview: BraceAwareIndent")
{
    Harness h;
    h.Type(u8"    if (x) {");
    h.Key(KeyCode::Return);
    CHECK(h.view->Document().Line(1) == StringView(u8"        ")); // base 4 + one step
}

TEST_CASE("toolkit-codeeditview: HoverValueTooltip")
{
    Harness h;
    h.view->SetText(u8"var speed = 4\nplain");
    h.view->HoverValueProvider = [](StringView identifier) -> String
    {
        if (identifier == StringView(u8"speed"))
        {
            return String(u8"4 : Num");
        }
        return String();
    };

    // Hovering the known identifier yields value content...
    const Float2 onWord = h.PointAt(0, 5); // inside "speed"
    (void)h.ctx.GetInputManager()->ProcessMouseMove(onWord.x, onWord.y);
    CHECK(h.view->CreateTooltipContent().Get() != nullptr);

    // ...an unknown word yields none (and no diagnostic on the line either).
    const Float2 onPlain = h.PointAt(1, 2);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(onPlain.x, onPlain.y);
    CHECK(h.view->CreateTooltipContent().Get() == nullptr);

    // Diagnostics remain the fallback when the provider has nothing.
    Array<CodeDiagnostic> diagnostics;
    diagnostics.PushBack(CodeDiagnostic{true, 1, String(u8"broken")});
    h.view->Document().SetDiagnostics(Move(diagnostics));
    (void)h.ctx.GetInputManager()->ProcessMouseMove(onPlain.x, onPlain.y);
    CHECK(h.view->CreateTooltipContent().Get() != nullptr);
}

TEST_CASE("toolkit-codeeditview: DiagnosticTooltip")
{
    Harness h;
    h.view->SetText(u8"ok line\nbad line");
    Array<CodeDiagnostic> diagnostics;
    diagnostics.PushBack(CodeDiagnostic{true, 1, String(u8"something broke")});
    h.view->Document().SetDiagnostics(Move(diagnostics));

    // Hover the diagnostic line: the provider yields content; a clean line yields none.
    const Float2 bad = h.PointAt(1, 2);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(bad.x, bad.y);
    CHECK(h.view->CreateTooltipContent().Get() != nullptr);

    const Float2 good = h.PointAt(0, 2);
    (void)h.ctx.GetInputManager()->ProcessMouseMove(good.x, good.y);
    CHECK(h.view->CreateTooltipContent().Get() == nullptr);
}

// ---- trigger characters + markup completion ----

namespace
{
    // Records the prefix it was asked for and returns one fixed candidate.
    class ProbeProvider final : public ICompletionProvider
    {
    public:
        String lastPrefix;
        i32 calls = 0;
        void Collect(const CodeDocument&, CodePosition, StringView prefix,
                     Array<CompletionCandidate>& out) override
        {
            ++calls;
            lastPrefix = String(prefix);
            out.PushBack(CompletionCandidate{String(u8"Member"), String(u8"Member")});
        }
    };
}

TEST_CASE("toolkit-codeeditview: TriggerCharacterOpensCompletion")
{
    Harness h;
    ProbeProvider probe;
    h.view->AddCompletionProvider(&probe);
    h.view->DocumentWordCompletion = false;

    // '.' (the default trigger) opens the popup with an EMPTY prefix.
    h.Type(u8"x.");
    REQUIRE(h.view->Completion().IsOpen());
    CHECK(probe.lastPrefix.IsEmpty());
    REQUIRE(h.view->Completion().ItemCount() == 1);
    CHECK(h.view->Completion().Item(0)->label.AsView() == StringView(u8"Member"));

    // Accepting inserts after the dot.
    h.Key(KeyCode::Return);
    CHECK(h.view->Text().AsView() == StringView(u8"x.Member"));

    // Ranking: provider results sort ABOVE document words even when the words are
    // capitalized (ASCII-uppercase would otherwise win the alphabetical sort and bury
    // context results below the popup fold - the first-smoke-run finding).
    h.view->DocumentWordCompletion = true;
    h.view->SetText(u8"Aardvark Banana\ny.");
    h.view->SetCursorPosition(CodePosition{1, 2});
    h.view->RequestCompletion();
    REQUIRE(h.view->Completion().IsOpen());
    CHECK(h.view->Completion().Item(0)->label.AsView() == StringView(u8"Member"));
}

TEST_CASE("toolkit-markupcompletion: ElementsAndAttributes")
{
    MarkupLoader::Initialize(); // the registry tables the provider reads

    MarkupCompletionProvider provider;
    CodeDocument doc;
    Array<CompletionCandidate> out;

    const auto contains = [&](const char8_t* name)
    {
        for (usize i = 0; i < out.Size(); ++i)
        {
            if (out[i].label.AsView() == StringView(name))
            {
                return true;
            }
        }
        return false;
    };

    // Element position: right after '<' (prefix "La", cursor after it).
    doc.SetText(u8"<La");
    out.Clear();
    provider.Collect(doc, CodePosition{0, 3}, StringView(u8"La"), out);
    CHECK(contains(u8"Label"));
    CHECK(contains(u8"Flex"));

    // Attribute position: past the element name.
    doc.SetText(u8"<Label tex");
    out.Clear();
    provider.Collect(doc, CodePosition{0, 10}, StringView(u8"tex"), out);
    CHECK(contains(u8"text"));
    CHECK(!contains(u8"Label")); // element names are not attribute candidates

    // Plain text between tags: nothing.
    doc.SetText(u8"<Label>hello");
    out.Clear();
    provider.Collect(doc, CodePosition{0, 12}, StringView(u8"hello"), out);
    CHECK(out.Size() == 0);
}

// ---- CompletionModel unit coverage (no context) ----

TEST_CASE("toolkit-completionmodel: FilterRanking")
{
    Array<CompletionCandidate> items;
    items.PushBack(CompletionCandidate{String(u8"Count"), String(u8"Count")});
    items.PushBack(CompletionCandidate{String(u8"counter"), String(u8"counter")});
    items.PushBack(CompletionCandidate{String(u8"other"), String(u8"other")});

    CompletionModel model;
    model.Open(CodePosition{0, 0}, Move(items), StringView(u8"co"));
    REQUIRE(model.IsOpen());
    REQUIRE(model.ItemCount() == 2);
    // Exact-case prefix match ranks first, case-insensitive after.
    CHECK(model.Item(0)->label.AsView() == StringView(u8"counter"));
    CHECK(model.Item(1)->label.AsView() == StringView(u8"Count"));

    // Filtering down to nothing closes the popup.
    model.Filter(StringView(u8"cox"));
    CHECK(!model.IsOpen());
}

TEST_CASE("toolkit-completionmodel: ClosesWhenOnlyMatchIsPrefix")
{
    Array<CompletionCandidate> items;
    items.PushBack(CompletionCandidate{String(u8"done"), String(u8"done")});
    CompletionModel model;
    model.Open(CodePosition{0, 0}, Move(items), StringView(u8"done"));
    CHECK(!model.IsOpen()); // nothing left to complete
}

TEST_CASE("toolkit-completionmodel: KeyRouting")
{
    Array<CompletionCandidate> items;
    items.PushBack(CompletionCandidate{String(u8"aaa"), String(u8"aaa")});
    items.PushBack(CompletionCandidate{String(u8"bbb"), String(u8"bbb")});
    CompletionModel model;
    model.Open(CodePosition{0, 0}, Move(items), StringView(u8"a"));

    CHECK(model.HandleKey(KeyCode::Down) == CompletionKeyResult::Consumed);
    CHECK(model.HandleKey(KeyCode::Up) == CompletionKeyResult::Consumed);
    CHECK(model.HandleKey(KeyCode::Left) == CompletionKeyResult::Ignored);
    CHECK(model.HandleKey(KeyCode::Tab) == CompletionKeyResult::Accepted);
    CHECK(model.HandleKey(KeyCode::Escape) == CompletionKeyResult::Dismissed);
    CHECK(!model.IsOpen());
    CHECK(model.HandleKey(KeyCode::Down) == CompletionKeyResult::Ignored); // closed = inert
}

TEST_CASE("toolkit-codeeditview: InsertAtCursor is one discrete undo unit")
{
    Harness h;
    h.Type(u8"call ");
    h.view->InsertAtCursor(u8"Float3");
    CHECK(h.view->Text().AsView() == StringView(u8"call Float3"));
    CHECK(h.view->CursorPosition() == CodePosition{0, 11});

    // Replaces a selection, and the empty string is a no-op.
    h.view->SetCursorPosition(CodePosition{0, 5});
    h.Key(KeyCode::End, KeyModifiers::Shift);
    h.view->InsertAtCursor(u8"Quaternion");
    CHECK(h.view->Text().AsView() == StringView(u8"call Quaternion"));
    h.view->InsertAtCursor(u8"");
    CHECK(h.view->Text().AsView() == StringView(u8"call Quaternion"));

    // Paste-kind: one undo removes the whole insert, not a keystroke's worth.
    h.Key(KeyCode::Z, KeyModifiers::Ctrl);
    CHECK(h.view->Text().AsView() == StringView(u8"call Float3"));
}

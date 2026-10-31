// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - :code_editor partition
//
// The code editor's view over a CodeDocument, either its own or one its host shares. Each line in
// view is shaped with the GUI's text and kept until its text changes. Tabs advance to tab stops in
// the shown text, and the caret and the mouse follow the shaped glyph positions, so any font works,
// not only a fixed-pitch one. Code runs left to right in every language, though Arabic inside a
// string is still shaped. Only the rows in view are drawn. The editor is a scroll container over its
// lines, scrolled by the core's scroll bars and the wheel.
//
// There may be several selections, kept sorted and apart. A click places one caret, Shift+click
// extends it, and Alt+click adds another. A double click selects a word, a triple click a line, and
// a drag extends the selection by the unit the press selected. The keys are the keymap's text
// commands (the arrows, Home and End, the word and page moves, deleting, undo, the clipboard).
// BindCodeEditorKeys binds them in the editor's own "codeeditor" context, and each command runs at
// every caret. Typing goes in at each caret as one undo step, and undoing it puts every caret back.
// Return keeps the line's indent, Tab indents, and Escape leaves one caret. The gutter numbers the
// lines.
//
// With a language set, each line's tokens are kept together with the tokenizer state the line began
// in and the line's hash. A line is tokenized again only when it changed or when the line before it
// now ends in a different state, and tokenizing runs only to the last line in view. Each token draws
// in its part's colour (`codeeditor::keyword { color: ... }`), so a theme can restyle code.
//
// With the host's language service set, typing a name (or a trigger character, or Ctrl+Space)
// opens the completion list below the word. The list is a popup that leaves the focus in the
// editor. While it shows, the editor declares the "completing" key context beside "codeeditor", so
// the list's keys win. The service's text for the word under the pointer is added to the tooltip
// after the diagnostics.
//
// Fold ranges come from the language's brackets, its indentation or its regions, or from the host.
// The lines inside a folded range are hidden. Soft wrap breaks long lines at the view's width. One
// map from lines to rows serves the carets, the pointer, the scroll and the drawing, so a fold
// takes one row and a wrapped line takes several. The whitespace drawing options follow VS Code's
// (none, boundary, selection, trailing, all).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.code:code_editor;

import foundation.core;
import foundation.fonts;
import foundation.vg; // PathBuilder, for a diagnostic's wave and a marker's shapes
import foundation.gui;
import :code_document;
import :code_language;
import :code_find_bar;
import :code_completion;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    // The code editor's own commands, bound in its "codeeditor" key context.
    inline constexpr const char8_t* kCodeToggleComment = u8"code.toggle-comment";
    inline constexpr const char8_t* kCodeIndentLines = u8"code.indent-lines";
    inline constexpr const char8_t* kCodeOutdentLines = u8"code.outdent-lines";
    inline constexpr const char8_t* kCodeMoveLinesUp = u8"code.move-lines-up";
    inline constexpr const char8_t* kCodeMoveLinesDown = u8"code.move-lines-down";
    inline constexpr const char8_t* kCodeDuplicateLinesUp = u8"code.duplicate-lines-up";
    inline constexpr const char8_t* kCodeDuplicateLinesDown = u8"code.duplicate-lines-down";
    inline constexpr const char8_t* kCodeDeleteLines = u8"code.delete-lines";
    inline constexpr const char8_t* kCodeAddNextOccurrence = u8"code.add-next-occurrence";
    inline constexpr const char8_t* kCodeSelectAllOccurrences = u8"code.select-all-occurrences";
    inline constexpr const char8_t* kCodeAddCursorAbove = u8"code.add-cursor-above";
    inline constexpr const char8_t* kCodeAddCursorBelow = u8"code.add-cursor-below";
    inline constexpr const char8_t* kCodeFind = u8"code.find";
    inline constexpr const char8_t* kCodeReplace = u8"code.replace";
    inline constexpr const char8_t* kCodeGoToLine = u8"code.go-to-line";
    inline constexpr const char8_t* kCodeFindNext = u8"code.find-next";
    inline constexpr const char8_t* kCodeFindPrevious = u8"code.find-previous";
    inline constexpr const char8_t* kCodeComplete = u8"code.complete";
    inline constexpr const char8_t* kCodeToggleSoftWrap = u8"code.toggle-soft-wrap";
    inline constexpr const char8_t* kCodeToggleWhitespace = u8"code.toggle-whitespace";
    inline constexpr const char8_t* kCodeFold = u8"code.fold";
    inline constexpr const char8_t* kCodeUnfold = u8"code.unfold";
    inline constexpr const char8_t* kCodeFoldAll = u8"code.fold-all";
    inline constexpr const char8_t* kCodeUnfoldAll = u8"code.unfold-all";
    inline constexpr const char8_t* kCodeCompletionNext = u8"code.completion-next";
    inline constexpr const char8_t* kCodeCompletionPrevious = u8"code.completion-previous";
    inline constexpr const char8_t* kCodeCompletionPageDown = u8"code.completion-page-down";
    inline constexpr const char8_t* kCodeCompletionPageUp = u8"code.completion-page-up";
    inline constexpr const char8_t* kCodeCompletionAccept = u8"code.completion-accept";
    inline constexpr const char8_t* kCodeCompletionClose = u8"code.completion-close";

    /// Binds the code editor's keys (VS Code's bindings), which can be rebound like any others. A
    /// host calls it once for a GUI's keymap, after the GUI's own bindings, so the GUI's bindings
    /// win a tie. The text editing keys and the editor's own commands apply where the editor itself
    /// has the focus (not a field inside it); the find commands also apply from the find bar's
    /// fields.
    inline void BindCodeEditorKeys(Keymap& keymap)
    {
        constexpr core::StringView kItself = core::StringView(u8"codeeditor && !textinput");
        BindTextEditingKeys(keymap, kItself);
        struct Binding
        {
            const char8_t* Keys;
            const char8_t* Command;
        };
        static constexpr Binding kBindings[] = {
            {u8"ctrl-slash", kCodeToggleComment},
            {u8"ctrl-rightbracket", kCodeIndentLines},
            {u8"ctrl-leftbracket", kCodeOutdentLines},
            {u8"alt-up", kCodeMoveLinesUp},
            {u8"alt-down", kCodeMoveLinesDown},
            {u8"shift-alt-up", kCodeDuplicateLinesUp},
            {u8"shift-alt-down", kCodeDuplicateLinesDown},
            {u8"ctrl-shift-k", kCodeDeleteLines},
            {u8"ctrl-d", kCodeAddNextOccurrence},
            {u8"ctrl-shift-l", kCodeSelectAllOccurrences},
            {u8"ctrl-alt-up", kCodeAddCursorAbove},
            {u8"ctrl-alt-down", kCodeAddCursorBelow},
            {u8"ctrl-f", kCodeFind},
            {u8"ctrl-h", kCodeReplace},
            {u8"ctrl-g", kCodeGoToLine},
            {u8"f3", kCodeFindNext},
            {u8"shift-f3", kCodeFindPrevious},
            {u8"ctrl-space", kCodeComplete},
            {u8"alt-z", kCodeToggleSoftWrap},
            {u8"ctrl-shift-leftbracket", kCodeFold},
            {u8"ctrl-shift-rightbracket", kCodeUnfold},
            {u8"ctrl-k ctrl-0", kCodeFoldAll},
            {u8"ctrl-k ctrl-j", kCodeUnfoldAll},
        };
        for (const Binding& b : kBindings)
        {
            const core::StringView command(b.Command);
            const bool finding = command == core::StringView(kCodeFind) || command == core::StringView(kCodeReplace) ||
                                 command == core::StringView(kCodeGoToLine) || command == core::StringView(kCodeFindNext) ||
                                 command == core::StringView(kCodeFindPrevious);
            (void)keymap.Bind(core::StringView(b.Keys), command, finding ? core::StringView(u8"codeeditor") : kItself);
        }
        // The completion list's keys, in the context the editor declares while the list shows. They
        // are bound after the editing keys, so at the same depth they win. Tab accepts as well, even
        // though Tab is otherwise the editor's own key.
        static constexpr Binding kListBindings[] = {
            {u8"down", kCodeCompletionNext},
            {u8"up", kCodeCompletionPrevious},
            {u8"pagedown", kCodeCompletionPageDown},
            {u8"pageup", kCodeCompletionPageUp},
            {u8"enter", kCodeCompletionAccept},
            {u8"escape", kCodeCompletionClose},
        };
        for (const Binding& b : kListBindings)
            (void)keymap.Bind(core::StringView(b.Keys), core::StringView(b.Command), core::StringView(u8"codeeditor && completing && !textinput"));
    }

    /// Which whitespace is drawn. A space is drawn as a dot and a tab as an arrow.
    enum class CodeWhitespace : u8
    {
        None,
        Boundary,  ///< all except a single space between words
        Selection, ///< the whitespace inside the selections
        Trailing,  ///< the whitespace at the end of a line
        All,
    };

    /// A fold. Its first line stays in view; when it is folded, the lines after it, through its
    /// last line, are hidden.
    struct CodeFoldRange
    {
        u32 First = 0;
        u32 Last = 0;
        [[nodiscard]] constexpr bool operator==(const CodeFoldRange&) const noexcept = default;
    };

    /// How severe a diagnostic is. Each severity draws in its part's colour (`codeeditor::error`).
    enum class CodeSeverity : u8
    {
        Hint,
        Info,
        Warning,
        Error,
    };
    [[nodiscard]] inline core::StringView CodeSeverityName(CodeSeverity severity) noexcept
    {
        switch (severity)
        {
        case CodeSeverity::Error: return core::StringView(u8"error");
        case CodeSeverity::Warning: return core::StringView(u8"warning");
        case CodeSeverity::Info: return core::StringView(u8"info");
        case CodeSeverity::Hint:
        default: return core::StringView(u8"hint");
        }
    }

    /// A host's message about a span of the code, such as a compiler's error or warning.
    struct CodeDiagnostic
    {
        CodeRange Range;
        CodeSeverity Severity = CodeSeverity::Error;
        core::String Message;
    };

    class CodeEditor : public UIWidget
    {
        RTTI_OBJECT(CodeEditor, UIWidget)
    public:
        static constexpr f32 kBlinkSeconds = 0.5f;
        static constexpr u32 kLongestLaidOut = 4096; ///< a longer line is cut off when laid out

        CodeEditor() : m_own(MemoryAllocator()), m_selections(MemoryAllocator()), m_goals(MemoryAllocator()), m_views(MemoryAllocator())
        {
            SetTag(core::StringView(u8"codeeditor"));
            SetTabFocusable(true);
            SetTabCaptures(true); // Tab indents
            // The editor has its own key context rather than "textinput", so a field inside it (the
            // find bar's) is the only text input there and keeps its own Return. BindCodeEditorKeys
            // binds the editing keys in this context.
            SetKeyContext(core::StringView(u8"codeeditor"));
            SetClipChildren(true);
            SetScrollsByKeys(false); // the arrows and page keys move the carets, through the keymap
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    s.OverflowX = Overflow::Auto;
                    s.OverflowY = Overflow::Auto;
                });
                SetCursor(Cursor::Text);
            }
            for (usize i = 0; i < static_cast<usize>(TextEditCommand::Count); ++i)
            {
                const TextEditCommand command = static_cast<TextEditCommand>(i);
                (void)OnCommand(TextEditCommandName(command), [this, command](CommandEvent&) { (void)Run(command); });
            }
            const auto command = [this](const char8_t* name, core::Function<void()> run)
            {
                (void)OnCommand(core::StringView(name), [action = core::Move(run)](CommandEvent&) mutable { action(); });
            };
            command(kCodeToggleComment, [this]() { ToggleComment(); });
            command(kCodeIndentLines, [this]() { IndentLines(false); });
            command(kCodeOutdentLines, [this]() { IndentLines(true); });
            command(kCodeMoveLinesUp, [this]() { MoveLines(true); });
            command(kCodeMoveLinesDown, [this]() { MoveLines(false); });
            command(kCodeDuplicateLinesUp, [this]() { DuplicateLines(true); });
            command(kCodeDuplicateLinesDown, [this]() { DuplicateLines(false); });
            command(kCodeDeleteLines, [this]() { DeleteLines(); });
            command(kCodeAddNextOccurrence, [this]() { (void)AddNextOccurrence(); });
            command(kCodeSelectAllOccurrences, [this]() { SelectAllOccurrences(); });
            command(kCodeAddCursorAbove, [this]() { AddCursorVertically(true); });
            command(kCodeAddCursorBelow, [this]() { AddCursorVertically(false); });
            command(kCodeFind, [this]() { OpenFind(false); });
            command(kCodeReplace, [this]() { OpenFind(true); });
            command(kCodeGoToLine, [this]() { OpenGoToLine(); });
            command(kCodeFindNext, [this]() { (void)FindNext(false); });
            command(kCodeFindPrevious, [this]() { (void)FindNext(true); });
            command(kCodeComplete, [this]() { (void)TriggerCompletion(true); });
            command(kCodeFold, [this]()
            {
                const core::Array<CodeSelection> at = m_selections;
                for (const CodeSelection& sel : at)
                    (void)Fold(sel.Head.Line);
            });
            command(kCodeUnfold, [this]()
            {
                const core::Array<CodeSelection> at = m_selections;
                for (const CodeSelection& sel : at)
                    (void)Unfold(sel.Head.Line);
            });
            command(kCodeToggleSoftWrap, [this]() { SetSoftWrap(!m_wrap); });
            command(kCodeToggleWhitespace, [this]()
            {
                SetShowWhitespace(m_whitespace == CodeWhitespace::None ? CodeWhitespace::All : CodeWhitespace::None);
            });
            command(kCodeFoldAll, [this]() { FoldAll(); });
            command(kCodeUnfoldAll, [this]() { UnfoldAll(); });
            command(kCodeCompletionNext, [this]() { m_completion->MoveSelection(1, true); });
            command(kCodeCompletionPrevious, [this]() { m_completion->MoveSelection(-1, true); });
            command(kCodeCompletionPageDown, [this]() { m_completion->MoveSelection(CodeCompletionList::PageRows(), false); });
            command(kCodeCompletionPageUp, [this]() { m_completion->MoveSelection(-CodeCompletionList::PageRows(), false); });
            command(kCodeCompletionAccept, [this]() { (void)AcceptCompletion(); });
            command(kCodeCompletionClose, [this]() { CloseCompletion(); });
            m_findBar = core::MakeRef<CodeFindBar>(MemoryAllocator());
            CodeEditor* self = this;
            m_findBar->SetOnChanged([self]() { self->FindChanged(); });
            m_findBar->SetOnNext([self](bool backwards) { (void)self->FindNext(backwards); });
            m_findBar->SetOnReplace([self](bool all) { all ? self->ReplaceAll() : self->ReplaceCurrent(); });
            m_findBar->SetOnGoTo([self](core::StringView text) { self->GoToLine(text); });
            m_findBar->SetOnClose([self]()
            {
                self->RequestFocus();
                self->Invalidate();
            });
            AddChild(m_findBar.Get());
            m_completion = core::MakeRef<CodeCompletionList>(MemoryAllocator());
            m_completion->SetOnPicked([self](usize) { (void)self->AcceptCompletion(); });
            Attach(m_own);
            m_selections.PushBack(CodeSelection{});
            m_goals.PushBack(-1.0f);
        }
        ~CodeEditor() override { Detach(); }

        // === The document ===
        /// Edits `document`, shared with the host, which must outlive the editor's use of it. Null
        /// switches back to the editor's own document.
        void SetDocument(CodeDocument* document)
        {
            Detach();
            Attach(document != nullptr ? *document : m_own);
            SetSelections(core::Span<const CodeSelection>{});
        }
        [[nodiscard]] CodeDocument& GetDocument() const noexcept { return *m_doc; }
        /// Replaces the text, clears the undo history, and leaves one caret at the start.
        void SetText(core::StringView text)
        {
            m_doc->SetText(text);
            SetSelections(core::Span<const CodeSelection>{});
        }
        [[nodiscard]] core::String GetText() const { return m_doc->GetText(); }

        /// Sets the language of the code. The caller keeps the language alive and has prepared it
        /// (Prepared). Null means plain text.
        void SetLanguage(const CodeLanguage* language)
        {
            m_foldRangesVersion = ~u64(0);
            ++m_foldStamp;
            m_language = language;
            m_lex.Clear();
            m_cleanTo = 0;
            m_views.Clear();
            Invalidate();
        }
        [[nodiscard]] const CodeLanguage* GetLanguage() const noexcept { return m_language; }
        /// Returns line `line`'s tokens, tokenizing up to that line first.
        [[nodiscard]] core::Span<const CodeSpan> TokensOf(u32 line) const
        {
            if (m_language == nullptr || line >= m_doc->LineCount())
                return {};
            EnsureLexed(line);
            return m_lex[line].Spans.AsSpan();
        }
        /// Counts every line tokenization since the editor was made (repeats included), for tests.
        [[nodiscard]] u64 TokenizedCount() const noexcept { return m_tokenized; }

        /// Sets whether the editor is read-only, so it only shows, selects and copies.
        void SetReadOnly(bool readOnly) noexcept { m_readOnly = readOnly; }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }
        /// The columns between tab stops (4 by default), and whether Tab inserts spaces (true) or a
        /// tab character.
        void SetTabSize(u32 columns)
        {
            m_tabSize = core::Max(1u, columns);
            m_views.Clear();
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] u32 GetTabSize() const noexcept { return m_tabSize; }
        void SetInsertSpaces(bool spaces) noexcept { m_insertSpaces = spaces; }
        /// Turns soft wrap on or off. Long lines wrap at the view's width, and a word too long for a
        /// row is broken where the row runs out. A wrapped line's rows are rows of the line map, so
        /// Up and Down move by them.
        void SetSoftWrap(bool wrap)
        {
            if (wrap == m_wrap)
                return;
            m_wrap = wrap;
            m_views.Clear();
            if (wrap)
                SetScrollOffset(core::Float2{0.0f, GetScrollOffset().y});
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] bool GetSoftWrap() const noexcept { return m_wrap; }
        /// Sets which whitespace is drawn (none by default), in the `whitespace` part's colour.
        void SetShowWhitespace(CodeWhitespace show)
        {
            m_whitespace = show;
            Invalidate();
        }
        [[nodiscard]] CodeWhitespace GetShowWhitespace() const noexcept { return m_whitespace; }
        /// Returns whether the space or tab at `at` is drawn under the current setting.
        [[nodiscard]] bool ShowsWhitespaceAt(CodePosition at) const
        {
            if (m_whitespace == CodeWhitespace::None || at.Line >= m_doc->LineCount())
                return false;
            const core::StringView text = m_doc->LineText(at.Line);
            if (at.Column >= text.Size() || (text[at.Column] != u8' ' && text[at.Column] != u8'\t'))
                return false;
            const auto blank = [](char8_t c) { return c == u8' ' || c == u8'\t'; };
            switch (m_whitespace)
            {
            case CodeWhitespace::All:
                return true;
            case CodeWhitespace::Trailing:
            {
                for (usize i = at.Column; i < text.Size(); ++i)
                    if (!blank(text[i]))
                        return false;
                return true;
            }
            case CodeWhitespace::Boundary:
            {
                // A lone space with text on either side is not drawn. A tab, a run of spaces, an
                // indent or trailing whitespace is.
                if (text[at.Column] == u8'\t')
                    return true;
                const bool before = at.Column > 0 && !blank(text[at.Column - 1]);
                const bool after = at.Column + 1 < text.Size() && !blank(text[at.Column + 1]);
                return !(before && after);
            }
            case CodeWhitespace::Selection:
                for (const CodeSelection& sel : m_selections)
                {
                    const CodeRange r = sel.Range();
                    if (!sel.IsEmpty() && r.Start <= at && at < r.End)
                        return true;
                }
                return false;
            default:
                return false;
            }
        }

        // === Selections ===
        /// Returns the selections in document order; none overlap.
        [[nodiscard]] core::Span<const CodeSelection> GetSelections() const noexcept { return m_selections.AsSpan(); }
        /// Returns the primary selection, which the keys and the view follow (the last one added).
        [[nodiscard]] const CodeSelection& GetPrimary() const noexcept { return m_selections[m_primary]; }
        /// Sets the selections, clamping and sorting them and merging those that touch. An empty span
        /// leaves one caret at the start.
        void SetSelections(core::Span<const CodeSelection> selections, usize primary = ~usize(0))
        {
            m_selections.Clear();
            for (const CodeSelection& s : selections)
                m_selections.PushBack(CodeSelection{m_doc->Clamp(s.Anchor), m_doc->Clamp(s.Head)});
            if (m_selections.IsEmpty())
                m_selections.PushBack(CodeSelection{});
            m_primary = primary < m_selections.Size() ? primary : m_selections.Size() - 1;
            Normalize();
            ResetGoals();
            SelectionsChanged();
        }
        void SetCaret(CodePosition at)
        {
            const CodeSelection one{at, at};
            SetSelections(core::Span<const CodeSelection>(&one, 1));
        }
        /// Adds a caret, as Alt+click does; it becomes the primary.
        void AddCaret(CodePosition at)
        {
            at = m_doc->Clamp(at);
            m_selections.PushBack(CodeSelection{at, at});
            m_primary = m_selections.Size() - 1;
            Normalize();
            ResetGoals();
            SelectionsChanged();
        }

        // === Commands ===
        /// Runs a text command at every caret, as its keys do. Returns false when it did nothing.
        bool Run(TextEditCommand command);
        /// Types `text` at every caret, replacing what each selects. Continued typing forms one undo
        /// step. When a language is set and `text` is one character, the language's rules apply:
        /// brackets and quotes close automatically (typing the closing one over the inserted one
        /// steps over it), a selection is surrounded, and a closing word or mark typed first on its
        /// line outdents the line.
        void Type(core::StringView text);

        // === Editing commands (each is also a named command, with its keys in BindCodeEditorKeys) ===
        /// Comments out the lines the selections cover, or uncomments them when all are already
        /// commented. Where the language has no line comment, a block comment goes around each
        /// selection instead.
        void ToggleComment();
        /// Indents (or outdents) the lines the selections cover by a tab stop.
        void IndentLines(bool outdent);
        /// Moves the lines the selections cover up or down past their neighbour.
        void MoveLines(bool up);
        /// Copies the lines the selections cover below (or above) them. With `up` false the selections
        /// move to the copy.
        void DuplicateLines(bool up);
        void DeleteLines();
        /// Selects the word at the primary caret, or adds a selection at the next match of the
        /// primary selection's text (Ctrl+D). Returns false when there is no further match.
        bool AddNextOccurrence();
        /// Selects every match of the primary selection's text (Ctrl+Shift+L).
        void SelectAllOccurrences();
        /// Adds a caret a line above (or below) the primary, at the primary's horizontal goal.
        void AddCursorVertically(bool up);
        /// Finds the bracket at or before `at` (not in a string or comment) and the one matching it.
        bool FindMatchingBracket(CodePosition at, CodeRange& bracket, CodeRange& match) const;

        // === Find, replace, go to line ===
        /// Shows the find (or replace) bar. When the primary selection is on one line, the bar starts
        /// with its text.
        void OpenFind(bool replace)
        {
            const CodeRange r = GetPrimary().Range();
            const core::String text = !r.IsEmpty() && r.Start.Line == r.End.Line ? m_doc->TextIn(r) : core::String{};
            m_findBar->Show(replace ? CodeFindBar::Mode::Replace : CodeFindBar::Mode::Find, text.AsView());
            RequestLayout();
        }
        void OpenGoToLine()
        {
            m_findBar->Show(CodeFindBar::Mode::GoToLine, core::Format(u8"{}", GetPrimary().Head.Line + 1).AsView());
            RequestLayout();
        }
        [[nodiscard]] CodeFindBar& GetFindBar() const noexcept { return *m_findBar; }
        /// Selects the next match of the bar's text after the primary selection (or, with
        /// `backwards`, the previous one before it), wrapping around the document. Returns false
        /// when there is no match.
        bool FindNext(bool backwards);
        /// Replaces the match the primary selection is on and finds the next. When the selection is
        /// on no match, it finds one first.
        void ReplaceCurrent();
        /// Replaces every match as one undo step.
        void ReplaceAll();
        /// Puts the caret at `line` or `line:column` (from 1), and closes the bar.
        void GoToLine(core::StringView text);
        /// Returns the current matches of the bar's text, in order. CurrentMatch returns which one the
        /// primary selection is on, counting from 1, or 0 for none.
        [[nodiscard]] core::Span<const CodeRange> GetMatches() const
        {
            RefreshMatches();
            return m_matches.AsSpan();
        }
        [[nodiscard]] usize CurrentMatch() const
        {
            RefreshMatches();
            for (usize i = 0; i < m_matches.Size(); ++i)
                if (m_matches[i] == GetPrimary().Range())
                    return i + 1;
            return 0;
        }

        // === The gutter's markers, diagnostics ===
        /// Marks `line` as `kind` ("breakpoint", "execution", "bookmark", or the host's own kind,
        /// drawn by the `marker` part). The mark stays with its line through edits. Returns an id
        /// for RemoveMarker.
        u32 AddMarker(u32 line, core::StringView kind)
        {
            const CodeAnchorId anchor = m_doc->AddAnchor(CodePosition{line, 0}, CodeBias::Right);
            m_markers.PushBack(Marker{++m_lastMarker, anchor, core::String(kind)});
            Invalidate();
            return m_lastMarker;
        }
        void RemoveMarker(u32 id)
        {
            for (usize i = 0; i < m_markers.Size(); ++i)
                if (m_markers[i].Id == id)
                {
                    m_doc->RemoveAnchor(m_markers[i].Anchor);
                    m_markers.RemoveAt(i);
                    Invalidate();
                    return;
                }
        }
        /// Removes every mark of `kind`, or every mark when `kind` is empty.
        void ClearMarkers(core::StringView kind = {})
        {
            for (usize i = m_markers.Size(); i-- > 0;)
                if (kind.Size() == 0 || m_markers[i].Kind.AsView() == kind)
                {
                    m_doc->RemoveAnchor(m_markers[i].Anchor);
                    m_markers.RemoveAt(i);
                }
            Invalidate();
        }
        /// Returns the line a mark is on now.
        [[nodiscard]] u32 MarkerLine(u32 id) const
        {
            for (const Marker& marker : m_markers)
                if (marker.Id == id)
                    return m_doc->GetAnchor(marker.Anchor).Line;
            return ~u32(0);
        }
        [[nodiscard]] bool HasMarker(u32 line, core::StringView kind) const
        {
            for (const Marker& marker : m_markers)
                if (marker.Kind.AsView() == kind && m_doc->GetAnchor(marker.Anchor).Line == line)
                    return true;
            return false;
        }
        /// Sets the callback for a click in the gutter's marker column, given the line (a host toggles
        /// a breakpoint there).
        void SetOnGutterClick(core::Function<void(u32 line)> callback) { m_onGutterClick = core::Move(callback); }

        /// Sets the diagnostics, replacing all of them; a host passes its whole set each time it has
        /// one. They keep their places through edits until the next set.
        void SetDiagnostics(core::Span<const CodeDiagnostic> diagnostics)
        {
            ClearDiagnosticAnchors();
            for (const CodeDiagnostic& d : diagnostics)
            {
                const CodeRange r = CodeRange::Ordered(m_doc->Clamp(d.Range.Start), m_doc->Clamp(d.Range.End));
                m_diagnostics.PushBack(Diagnostic{m_doc->AddAnchor(r.Start, CodeBias::Left), m_doc->AddAnchor(r.End, CodeBias::Right),
                                                  d.Severity, d.Message});
            }
            Invalidate();
        }
        /// Returns the diagnostics at their current places.
        [[nodiscard]] core::Array<CodeDiagnostic> GetDiagnostics() const
        {
            core::Array<CodeDiagnostic> out(const_cast<CodeEditor*>(this)->MemoryAllocator());
            for (const Diagnostic& d : m_diagnostics)
                out.PushBack(CodeDiagnostic{CodeRange{m_doc->GetAnchor(d.Start), m_doc->GetAnchor(d.End)}, d.Severity, d.Message});
            return out;
        }
        /// Gives the worst severity on `line`. Returns false when the line has no diagnostic.
        [[nodiscard]] bool WorstOn(u32 line, CodeSeverity& out) const
        {
            bool any = false;
            for (const Diagnostic& d : m_diagnostics)
            {
                const CodePosition start = m_doc->GetAnchor(d.Start), end = m_doc->GetAnchor(d.End);
                if (start.Line <= line && line <= end.Line && (!any || d.Severity > out))
                {
                    out = d.Severity;
                    any = true;
                }
            }
            return any;
        }
        /// Returns the messages of the diagnostics over `at`, one per line. An empty diagnostic covers
        /// the word at its position.
        [[nodiscard]] core::String DiagnosticsAt(CodePosition at) const
        {
            core::String out;
            for (const Diagnostic& d : m_diagnostics)
            {
                const CodeRange r = MarkedRange(d);
                if (r.Start <= at && at <= r.End)
                {
                    if (!out.IsEmpty())
                        out.Append(core::StringView(u8"\n"));
                    out.Append(d.Message.AsView());
                }
            }
            return out;
        }
        /// The hover tooltip: the diagnostics under the pointer, or else the widget's own tooltip.
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            if (!m_hoverTip.IsEmpty())
                return m_hoverTip.AsView();
            return UIWidget::GetTooltipText();
        }

        // === Folding ===
        /// Returns the ranges that can fold, ordered by first line, with only the widest kept for
        /// each line. They are the host's when it set them. Otherwise they come from the language:
        /// its brackets (outside strings and comments), or indentation (for a language that folds
        /// by indent, or for no language), plus its regions.
        [[nodiscard]] core::Span<const CodeFoldRange> GetFoldRanges() const
        {
            EnsureFoldRanges();
            return m_foldRanges.AsSpan();
        }
        /// Uses the host's ranges in place of the language's. They are carried through edits until set
        /// again.
        void SetFoldRanges(core::Span<const CodeFoldRange> ranges)
        {
            m_hostFolds = true;
            m_hostRanges.Clear();
            for (const CodeFoldRange& r : ranges)
                m_hostRanges.PushBack(r);
            m_foldRangesVersion = ~u64(0);
            FoldsChanged();
        }
        /// Goes back to the language's ranges.
        void UseLanguageFolds()
        {
            m_hostFolds = false;
            m_hostRanges.Clear();
            m_foldRangesVersion = ~u64(0);
            FoldsChanged();
        }
        /// Folds the innermost open range at `line` (one starting on it or containing it), moving any
        /// carets inside out to its first line. Returns false when no range is open there.
        bool Fold(u32 line);
        /// Unfolds every folded range holding `line` (or starting on it).
        bool Unfold(u32 line);
        /// Folds or unfolds the range starting on `line`, as a click in the gutter does.
        bool ToggleFold(u32 line);
        void FoldAll();
        void UnfoldAll();
        [[nodiscard]] bool IsFolded(u32 firstLine) const;
        /// Returns whether `line` is inside a folded range, and so out of view.
        [[nodiscard]] bool IsLineHidden(u32 line) const
        {
            EnsureRows();
            return line < m_doc->LineCount() && m_rowStart[line + 1] == m_rowStart[line];
        }
        /// Returns the number of rows shown (one per line when nothing is folded or wrapped).
        [[nodiscard]] u32 RowCount() const
        {
            EnsureRows();
            return m_rowStart.Back();
        }

        // === The language service ===
        /// Sets the service to ask for completions and for hover text about the word under the
        /// pointer. The host owns it and must keep it alive while it is set; null means none.
        void SetLanguageService(ICodeLanguageService* service)
        {
            CloseCompletion();
            m_service = service;
            m_hoverVersion = ~u64(0);
        }
        [[nodiscard]] ICodeLanguageService* GetLanguageService() const noexcept { return m_service; }
        /// Opens the completion list at the primary caret with the service's completions, filtered
        /// by the word before the caret. When `invoked` (Ctrl+Space) it opens anywhere. Otherwise it
        /// opens while typing, outside comments and strings, and only once a word has begun or
        /// after a `trigger` character. Returns false when nothing matched, when there is no
        /// service, or when the primary selection is not empty.
        bool TriggerCompletion(bool invoked, char8_t trigger = 0);
        [[nodiscard]] bool IsCompleting() const noexcept { return m_completing; }
        [[nodiscard]] CodeCompletionList& GetCompletionList() const noexcept { return *m_completion; }
        /// Replaces the typed word with the selected completion, at every caret where the same word
        /// is typed (or else at the primary caret), as one undo step, and closes the list.
        bool AcceptCompletion();
        void CloseCompletion()
        {
            if (EventDispatcher* dispatcher = GetEventDispatcher(); dispatcher != nullptr && dispatcher->IsPopupOpen(*m_completion))
                (void)dispatcher->ClosePopup(*m_completion);
            CompletionClosed();
        }
        /// Returns the service's text for the word at `at` (empty when `at` is not on a word). The
        /// service is asked once per word.
        [[nodiscard]] core::StringView HoverAt(CodePosition at);

        // === Geometry (in the editor's own space) ===
        [[nodiscard]] f32 LineHeight() const
        {
            const f32 height = m_measure.GetLineHeight();
            return height > 0.0f ? height : 16.0f;
        }
        /// Returns the x where the text starts, past the gutter and its gap.
        [[nodiscard]] f32 TextLeft() const { return GetContentBounds().x + GutterWidth(); }
        [[nodiscard]] f32 GutterWidth() const
        {
            u32 digits = 1;
            for (u32 n = m_doc->LineCount(); n >= 10; n /= 10)
                ++digits;
            return kMarkerColumn + static_cast<f32>(core::Max(digits, 3u)) * DigitWidth() + 2.0f * kGutterPad + kFoldColumn + kTextGap;
        }
        /// Returns the position nearest a point.
        [[nodiscard]] CodePosition PositionAt(core::Float2 local) const;
        /// Returns the caret rectangle for a position: its left, its top and the line's height.
        [[nodiscard]] Rect CaretRect(CodePosition at) const;
        /// Scrolls by the least amount that shows the primary caret.
        void ScrollToCaret();
        /// Gives the lines in view now, first to last (inclusive).
        void VisibleLines(u32& first, u32& last) const;

        // Asks for platform text input while focused, placed at the primary caret.
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled() && !m_readOnly; }
        [[nodiscard]] bool GetTextInputArea(Rect& area, f32& caretX) const override
        {
            const Rect caret = CaretRect(GetPrimary().Head);
            area = Rect{caret.x, caret.y, core::Max(1.0f, ComposedWidth()), caret.height};
            caretX = caret.x;
            return true;
        }
        /// Returns the current line, for a screen reader.
        [[nodiscard]] core::String GetAccessValue() const { return core::String(m_doc->LineText(GetPrimary().Head.Line)); }

        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"gutter"));
            out.PushBack(core::StringView(u8"line-number"));
            out.PushBack(core::StringView(u8"current-line-number"));
            out.PushBack(core::StringView(u8"current-line"));
            out.PushBack(core::StringView(u8"selection"));
            out.PushBack(core::StringView(u8"caret"));
            out.PushBack(core::StringView(u8"composition"));
            out.PushBack(core::StringView(u8"bracket-match"));
            out.PushBack(core::StringView(u8"find-match"));
            out.PushBack(core::StringView(u8"find-current"));
            out.PushBack(core::StringView(u8"fold"));
            out.PushBack(core::StringView(u8"fold-placeholder"));
            out.PushBack(core::StringView(u8"whitespace"));
            static constexpr const char8_t* kMarks[] = {u8"marker", u8"breakpoint", u8"execution", u8"execution-line", u8"bookmark",
                                                        u8"error", u8"warning", u8"info", u8"hint"};
            for (const char8_t* part : kMarks)
                out.PushBack(core::StringView(part));
            for (usize i = 1; i < static_cast<usize>(CodeToken::Count); ++i)
                out.PushBack(CodeTokenName(static_cast<CodeToken>(i)));
        }

    protected:
        void OnTextStyleChange() override
        {
            const auto style = [this](Text& text)
            {
                text.SetFont(GetFont());
                text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
                text.SetColor(GetTextColor());
                text.SetWhiteSpace(TextWhiteSpace::Pre);
                text.SetDirection(ParagraphDirection{fonts::TextDirection::LeftToRight, false});
            };
            style(m_measure);
            m_measure.SetString(core::StringView(u8"0000000000"));
            style(m_foldText);
            m_foldText.SetString(core::StringView(u8"..."));
            m_styleText = style;
            m_views.Clear();
            RequestLayout();
            Invalidate();
        }
        // A token's colour changed, so the lines are coloured again.
        void OnPartStyleChange(core::StringView part) override
        {
            UIWidget::OnPartStyleChange(part);
            m_views.Clear();
            Invalidate();
        }
        void OnLayout() override
        {
            // Overrides the base layout. The editor places its one child (the find bar) itself, and
            // its children's extent is not what it scrolls over; laying them out would clamp the
            // scroll to them. It scrolls down over every line, and across over the widest line laid
            // out so far, or else each line's length at the width of a digit. That estimate is
            // corrected when a wider line is laid out.
            const f32 digit = DigitWidth();
            if (m_widestVersion != m_doc->GetVersion())
            {
                m_widestVersion = m_doc->GetVersion();
                m_estimated = 0.0f;
                for (u32 line = 0; line < m_doc->LineCount(); ++line)
                    m_estimated = core::Max(m_estimated, static_cast<f32>(core::Min<usize>(m_doc->LineText(line).Size(), kLongestLaidOut)) * digit);
            }
            const Rect content = GetContentBounds();
            // While wrapping, lines wrap at the text's width (a digit short of the edge), and there is
            // nothing to scroll across.
            if (m_wrap)
            {
                const f32 wrap = core::Max(digit * 8.0f, content.width - GutterWidth() - digit);
                if (wrap != m_wrapWidth)
                {
                    m_wrapWidth = wrap;
                    Invalidate();
                }
            }
            const f32 width = m_wrap ? content.width : GutterWidth() + core::Max(m_estimated, m_widest) + digit * 2.0f;
            const f32 height = static_cast<f32>(RowCount()) * LineHeight() + core::Max(0.0f, content.height - LineHeight());
            SetScrollExtent(core::Float2{width, height});
            // The find bar sits over the top end corner and stays put as the code scrolls under it.
            if (m_findBar->IsDisplayed())
            {
                LayoutConstraints constraints;
                constraints.Max = core::Float2{content.width, content.height};
                const core::Float2 size = m_findBar->Measure(constraints);
                const core::Float2 shift = ScrollShift();
                const f32 x = IsRightToLeft() ? content.x + 8.0f : content.Right() - size.x - 16.0f;
                m_findBar->Arrange(Rect{x + shift.x, content.y + 2.0f + shift.y, size.x, size.y});
            }
        }
        // Focus moving into the find bar scrolls nothing, because the bar is always in view.
        void OnDescendantFocused(Node& focused) override
        {
            for (Node* n = &focused; n != nullptr && n != this; n = n->GetParent())
                if (n == m_findBar.Get())
                    return;
            UIWidget::OnDescendantFocused(focused);
        }
        void OnScrollChange() override
        {
            UIWidget::OnScrollChange();
            if (m_findBar->IsDisplayed())
                RequestLayout(); // the bar keeps its place in view
            if (m_completing)
                PlaceCompletion(); // keeps the list at its word
        }
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;
        void OnFocusGained() override
        {
            UIWidget::OnFocusGained();
            ResetBlink();
            KeepTicking();
        }
        void OnFocusLost() override
        {
            UIWidget::OnFocusLost();
            m_composition.Clear();
            CloseCompletion();
            Invalidate();
        }
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseMove(const MouseEvent& event) override;
        void OnMouseLeave(const MouseEvent& event) override
        {
            UIWidget::OnMouseLeave(event);
            if (m_gutterHover)
            {
                m_gutterHover = false;
                Invalidate();
            }
        }
        void OnMouseUp(const MouseEvent& event) override;
        void OnTextInput(const TextInputEvent& event) override
        {
            if (!IsTreeEnabled() || m_readOnly || event.Text.Size() == 0)
                return;
            event.Handled = true;
            m_composition.Clear();
            Type(event.Text);
        }
        void OnComposition(const CompositionEvent& event) override
        {
            if (!IsTreeEnabled() || m_readOnly)
                return;
            event.Handled = true;
            m_composition = core::String(event.Text);
            m_compositionText.SetString(m_composition.AsView());
            if (m_styleText)
                m_styleText(m_compositionText);
            m_compositionText.SetString(m_composition.AsView());
            ResetBlink();
        }
        void OnKeyDown(const KeyEvent& event) override;

    private:
        static constexpr f32 kGutterPad = 6.0f;
        static constexpr f32 kMarkerColumn = 16.0f; // at the gutter's start: markers and diagnostic icons
        static constexpr f32 kTextGap = 8.0f;
        static constexpr f32 kFoldColumn = 14.0f; // at the gutter's end: fold chevrons

        struct LineLex
        {
            u64 Hash = 0;
            CodeLexState Entry;
            CodeLexState Exit;
            core::Array<CodeSpan> Spans;
            u64 Stamp = 0; // increased each time the line is tokenized
            bool Valid = false;
        };
        struct LineView
        {
            u64 Hash = 0;
            u32 TabSize = 0;
            u64 LexStamp = ~u64(0);
            bool Laid = false;
            core::String Shown;      // the line with its tabs expanded to their stops (cut when very long)
            core::Array<u32> ShownAt; // maps a byte column to its place in Shown (empty when they are the same)
            Text Glyphs;
            Text Number;
            u32 NumberOf = ~0u;
        };

        void Attach(CodeDocument& document)
        {
            m_doc = &document;
            m_views.Clear();
            m_foldRangesVersion = ~u64(0);
            m_rowsVersion = ~u64(0);
            m_widestVersion = ~u64(0);
            m_widest = 0.0f;
            CodeEditor* self = this;
            m_listener = m_doc->AddChangeListener([self](const CodeChange& change) { self->DocumentChanged(change); });
        }
        void Detach()
        {
            if (m_doc != nullptr)
            {
                m_doc->RemoveChangeListener(m_listener);
                ClearDiagnosticAnchors(); // the marks are anchors in the old document, so they go with it
                for (const Marker& marker : m_markers)
                    m_doc->RemoveAnchor(marker.Anchor);
                m_markers.Clear();
                for (const CodeAnchorId anchor : m_folded)
                    m_doc->RemoveAnchor(anchor);
                m_folded.Clear();
            }
        }
        void DocumentChanged(const CodeChange& change)
        {
            // The tokens of the lines below the change move with those lines, and the changed lines'
            // tokens are dropped. Lines after a change are checked again from there, because their
            // entry state may differ now.
            const usize lexFirst = core::Min<usize>(change.FirstLine, m_lex.Size());
            const usize lexRemoved = core::Min<usize>(change.RemovedLines, m_lex.Size() - lexFirst);
            for (usize i = 0; i < lexRemoved; ++i)
                m_lex.RemoveAt(lexFirst);
            for (u32 i = 0; i < change.InsertedLines && lexFirst <= m_lex.Size(); ++i)
                m_lex.Insert(lexFirst, LineLex{});
            m_cleanTo = core::Min<u32>(m_cleanTo, change.FirstLine);
            // The host's fold ranges move with their lines.
            for (usize i = m_hostRanges.Size(); i-- > 0;)
            {
                CodeFoldRange& r = m_hostRanges[i];
                r.First = CodeDocument::Moved(CodePosition{r.First, 0}, CodeBias::Right, change.Replaced, change.End).Line;
                r.Last = CodeDocument::Moved(CodePosition{r.Last, 0}, CodeBias::Right, change.Replaced, change.End).Line;
                if (r.Last <= r.First)
                    m_hostRanges.RemoveAt(i);
            }
            // Move the selections through the edit. This matters for an edit another view made; the
            // editor's own edits set the selections afterwards.
            for (CodeSelection& sel : m_selections)
                sel = CodeSelection{CodeDocument::Moved(sel.Anchor, CodeBias::Right, change.Replaced, change.End),
                                    CodeDocument::Moved(sel.Head, CodeBias::Right, change.Replaced, change.End)};
            // The views of lines below the change move with their lines, and the changed lines' views
            // are dropped.
            const usize first = core::Min<usize>(change.FirstLine, m_views.Size());
            const usize removed = core::Min<usize>(change.RemovedLines, m_views.Size() - first);
            for (usize i = 0; i < removed; ++i)
                m_views.RemoveAt(first);
            for (u32 i = 0; i < change.InsertedLines && first <= m_views.Size(); ++i)
                m_views.Insert(first, core::UniquePtr<LineView>{});
            RequestLayout();
            Invalidate();
        }
        // Returns a line's laid-out view, making it, or remaking it when its text has changed.
        LineView& View(u32 line) const;
        [[nodiscard]] f32 DigitWidth() const
        {
            const f32 width = m_measure.LayoutFor(Rect{}).Width / 10.0f;
            return width > 0.0f ? width : 8.0f;
        }
        [[nodiscard]] static u32 ShownColumn(const LineView& view, u32 column)
        {
            if (view.ShownAt.IsEmpty())
                return core::Min(column, static_cast<u32>(view.Shown.Size()));
            return view.ShownAt[core::Min<usize>(column, view.ShownAt.Size() - 1)];
        }
        [[nodiscard]] static u32 ByteColumn(const LineView& view, u32 shown)
        {
            if (view.ShownAt.IsEmpty())
                return shown;
            // Find the column whose place is nearest; inside a tab's spaces, take the nearer side.
            u32 column = 0;
            while (column + 1 < view.ShownAt.Size() && view.ShownAt[column + 1] <= shown)
                ++column;
            if (column + 1 < view.ShownAt.Size() && shown - view.ShownAt[column] > view.ShownAt[column + 1] - shown)
                ++column;
            return column;
        }
        // Returns a line's layout: at the wrap width while wrapping, otherwise unbounded.
        [[nodiscard]] const TextLayout& LayoutOf(const LineView& view) const
        {
            return view.Glyphs.LayoutFor(Rect{0.0f, 0.0f, m_wrap ? m_wrapWidth : 0.0f, 0.0f});
        }
        // Returns how many rows a shown line takes: one, or one per wrapped line.
        [[nodiscard]] u32 RowsOf(u32 line) const
        {
            if (!m_wrap)
                return 1;
            return core::Max<u32>(1, static_cast<u32>(LayoutOf(View(line)).Lines.Size()));
        }
        // Where a position is: its x (in the text's own space) and its row.
        struct Place
        {
            f32 X = 0.0f;
            u32 Row = 0;
        };
        [[nodiscard]] Place PlaceOf(CodePosition at) const
        {
            const LineView& view = View(at.Line);
            const TextLayout& layout = LayoutOf(view);
            const usize shown = ShownColumn(view, at.Column);
            if (layout.Lines.IsEmpty())
                return Place{0.0f, RowOfLine(at.Line)};
            const usize sub = layout.LineOf(shown);
            return Place{layout.CaretXOnLine(shown, sub), RowOfLine(at.Line) + (IsLineHidden(at.Line) ? 0u : static_cast<u32>(sub))};
        }
        [[nodiscard]] f32 XOf(CodePosition at) const { return PlaceOf(at).X; }
        // Returns the column nearest `x` on row `sub` of `line`.
        [[nodiscard]] u32 ColumnAt(u32 line, u32 sub, f32 x) const
        {
            const LineView& view = View(line);
            const TextLayout& layout = LayoutOf(view);
            if (layout.Lines.IsEmpty())
                return 0;
            const usize shown = layout.OffsetOnLine(core::Min<usize>(sub, layout.Lines.Size() - 1), x);
            return m_doc->Clamp(CodePosition{line, ByteColumn(view, static_cast<u32>(shown))}).Column;
        }
        [[nodiscard]] u32 ColumnAtX(u32 line, f32 x) const { return ColumnAt(line, 0, x); }
        // Calls `fn` for each row that a run of `line` covers, from byte column `from` to `to`, with
        // the row and the span from x0 to x1 on it. A run that continues past a row's end onto the
        // next row fills to the end of the row. With `toEnd` (a selection that continues onto the
        // next line), the last row's span extends a little past the text.
        template <typename Fn>
        void ForEachRun(u32 line, u32 from, u32 to, bool toEnd, Fn&& fn) const
        {
            const LineView& view = View(line);
            const TextLayout& layout = LayoutOf(view);
            const usize a = ShownColumn(view, from), b = ShownColumn(view, to);
            const u32 row = RowOfLine(line);
            if (layout.Lines.IsEmpty())
            {
                fn(0.0f, toEnd ? DigitWidth() * 0.5f : 0.0f, row);
                return;
            }
            for (usize i = 0; i < layout.Lines.Size(); ++i)
            {
                const LaidOutLine& l = layout.Lines[i];
                const bool last = i + 1 == layout.Lines.Size();
                if (b < l.Start || a > l.End)
                    continue; // wholly on another row
                if (!last && a == l.End && b > a && layout.Lines[i + 1].Start == l.End)
                    continue; // starts where a word broke, so it belongs to the next row
                if (i > 0 && b == l.Start && a < l.Start && !(last && toEnd))
                    continue; // ends where this row starts
                const usize s = core::Max(a, l.Start), e = core::Min(b, l.End);
                f32 x0 = layout.CaretXOnLine(s, i);
                f32 x1 = layout.CaretXOnLine(core::Max(s, e), i);
                if (b > l.End && !last)
                    x1 = core::Max(x1, l.Left + l.Width); // continues onto the next row
                if (last && toEnd)
                    x1 += DigitWidth() * 0.5f;
                fn(x0, x1, row + static_cast<u32>(i));
            }
        }
        [[nodiscard]] f32 ComposedWidth() const
        {
            return m_composition.IsEmpty() ? 0.0f : m_compositionText.LayoutFor(Rect{}).Width;
        }

        // Sorts the selections by start and merges those that overlap or start at the same place (two
        // carets), keeping track of the primary.
        void Normalize()
        {
            const CodeSelection primary = m_selections[m_primary];
            for (usize i = 1; i < m_selections.Size(); ++i) // insertion sort: there are few, and they are nearly sorted
                for (usize j = i; j > 0 && m_selections[j].Range().Start < m_selections[j - 1].Range().Start; --j)
                    core::Swap(m_selections[j], m_selections[j - 1]);
            core::Array<CodeSelection> merged(MemoryAllocator());
            for (const CodeSelection& s : m_selections)
            {
                if (!merged.IsEmpty())
                {
                    CodeSelection& last = merged.Back();
                    const CodeRange a = last.Range(), b = s.Range();
                    if (b.Start < a.End || b.Start == a.Start) // overlapping, or starting at the same place
                    {
                        const CodePosition end = a.End < b.End ? b.End : a.End;
                        const bool forward = !(s.Head < s.Anchor);
                        last = forward ? CodeSelection{a.Start, end} : CodeSelection{end, a.Start};
                        continue;
                    }
                }
                merged.PushBack(s);
            }
            m_selections = core::Move(merged);
            m_primary = 0;
            for (usize i = 0; i < m_selections.Size(); ++i)
            {
                const CodeRange r = m_selections[i].Range();
                if (r.Start <= primary.Head && primary.Head <= r.End)
                    m_primary = i;
            }
        }
        void ResetGoals()
        {
            m_goals.Clear();
            for (usize i = 0; i < m_selections.Size(); ++i)
                m_goals.PushBack(-1.0f);
        }
        void SelectionsChanged()
        {
            RevealCarets();
            ResetBlink();
            ScrollToCaret();
            Invalidate();
            if (m_completing)
                UpdateCompletion();
        }
        // === Folding ===
        // Computes the fold ranges again when the text has changed.
        void EnsureFoldRanges() const
        {
            if (m_foldRangesVersion == m_doc->GetVersion())
                return;
            m_foldRangesVersion = m_doc->GetVersion();
            m_foldRanges.Clear();
            const u32 count = m_doc->LineCount();
            if (m_hostFolds)
            {
                for (const CodeFoldRange& r : m_hostRanges)
                    if (r.First < r.Last && r.Last < count)
                        m_foldRanges.PushBack(r);
            }
            else if (m_language != nullptr && !m_language->FoldByIndent && !m_language->Brackets.IsEmpty())
                BracketFolds();
            else
                IndentFolds();
            if (m_language != nullptr && !m_language->RegionStart.IsEmpty() && !m_language->RegionEnd.IsEmpty())
            {
                // A region runs from the line of its start marker through the line of its end marker.
                core::Array<u32> open(const_cast<CodeEditor*>(this)->MemoryAllocator());
                for (u32 line = 0; line < count; ++line)
                {
                    const core::StringView text = m_doc->LineText(line);
                    const core::StringView trimmed = text.SubStr(IndentOf(text), text.Size() - IndentOf(text));
                    if (trimmed.StartsWith(m_language->RegionStart.AsView()))
                        open.PushBack(line);
                    else if (trimmed.StartsWith(m_language->RegionEnd.AsView()) && !open.IsEmpty())
                    {
                        if (line > open.Back())
                            m_foldRanges.PushBack(CodeFoldRange{open.Back(), line});
                        open.PopBack();
                    }
                }
            }
            // Sort by first line, keeping the widest of the ranges that start on the same line.
            m_foldRanges.Sort([](const CodeFoldRange& a, const CodeFoldRange& b)
            {
                return a.First != b.First ? a.First < b.First : a.Last > b.Last;
            });
            for (usize i = m_foldRanges.Size(); i-- > 1;)
                if (m_foldRanges[i].First == m_foldRanges[i - 1].First)
                    m_foldRanges.RemoveAt(i);
        }
        // Each bracket pair folds from the open bracket's line to the line before the close's line,
        // or to the close's own line when something comes before the close on it. Brackets inside
        // strings and comments are skipped.
        void BracketFolds() const
        {
            const u32 count = m_doc->LineCount();
            EnsureLexed(count - 1);
            struct Open
            {
                char8_t Close;
                u32 Line;
            };
            core::Array<Open> stack(const_cast<CodeEditor*>(this)->MemoryAllocator());
            for (u32 line = 0; line < count; ++line)
            {
                const core::StringView text = m_doc->LineText(line);
                const u32 indent = IndentOf(text);
                for (const CodeSpan& span : m_lex[line].Spans)
                {
                    if (span.Token == CodeToken::String || span.Token == CodeToken::Comment || span.Token == CodeToken::Escape)
                        continue;
                    for (u32 i = span.Start; i < span.End && i < text.Size(); ++i)
                        for (const CodePair& pair : m_language->Brackets)
                        {
                            if (pair.Open.Size() != 1 || pair.Close.Size() != 1)
                                continue;
                            if (text[i] == pair.Open[0])
                                stack.PushBack(Open{pair.Close[0], line});
                            else if (text[i] == pair.Close[0])
                            {
                                while (!stack.IsEmpty() && stack.Back().Close != text[i])
                                    stack.PopBack(); // drop an unclosed bracket inside
                                if (stack.IsEmpty())
                                    break;
                                const u32 first = stack.Back().Line;
                                stack.PopBack();
                                const u32 last = i == indent ? line - (line > 0 ? 1u : 0u) : line;
                                if (last > first)
                                    m_foldRanges.PushBack(CodeFoldRange{first, last});
                            }
                        }
                }
            }
        }
        // Each line folds through the last following line that is indented further. Blank lines in
        // between are included; blank lines after that last line are left out.
        void IndentFolds() const
        {
            struct Open
            {
                u32 Indent;
                u32 Line;
            };
            core::Array<Open> stack(const_cast<CodeEditor*>(this)->MemoryAllocator());
            const u32 count = m_doc->LineCount();
            u32 lastText = 0;
            const auto close = [&](u32 indent)
            {
                while (!stack.IsEmpty() && stack.Back().Indent >= indent)
                {
                    if (lastText > stack.Back().Line)
                        m_foldRanges.PushBack(CodeFoldRange{stack.Back().Line, lastText});
                    stack.PopBack();
                }
            };
            for (u32 line = 0; line < count; ++line)
            {
                const core::StringView text = m_doc->LineText(line);
                const u32 bytes = IndentOf(text);
                if (bytes == text.Size())
                    continue; // blank
                const u32 indent = IndentColumns(text.SubStr(0, bytes));
                close(indent);
                stack.PushBack(Open{indent, line});
                lastText = line;
            }
            close(0);
        }
        // Computes each line's first row. A line inside a folded range takes no row of its own (it
        // gets the next line's), and the last entry is the total.
        void EnsureRows() const
        {
            const f32 wrap = m_wrap ? m_wrapWidth : 0.0f;
            if (m_rowsVersion == m_doc->GetVersion() && m_rowsStamp == m_foldStamp && m_rowsWrap == wrap && m_rowsTabSize == m_tabSize)
                return;
            m_rowsVersion = m_doc->GetVersion();
            m_rowsStamp = m_foldStamp;
            m_rowsWrap = wrap;
            m_rowsTabSize = m_tabSize;
            const u32 count = m_doc->LineCount();
            m_rowStart.Clear();
            core::Array<CodeFoldRange> folded(const_cast<CodeEditor*>(this)->MemoryAllocator());
            if (!m_folded.IsEmpty())
            {
                EnsureFoldRanges();
                // A fold whose range has gone (its bracket was deleted) is forgotten.
                for (usize i = m_folded.Size(); i-- > 0;)
                {
                    const CodeFoldRange* r = RangeStarting(m_doc->GetAnchor(m_folded[i]).Line);
                    if (r == nullptr)
                    {
                        m_doc->RemoveAnchor(m_folded[i]);
                        m_folded.RemoveAt(i);
                    }
                    else
                        folded.PushBack(*r);
                }
                folded.Sort([](const CodeFoldRange& a, const CodeFoldRange& b) { return a.First < b.First; });
            }
            u32 row = 0;
            i64 hiddenTo = -1;
            usize next = 0;
            for (u32 line = 0; line < count; ++line)
            {
                m_rowStart.PushBack(row);
                if (static_cast<i64>(line) <= hiddenTo)
                    continue;
                row += RowsOf(line);
                for (; next < folded.Size() && folded[next].First <= line; ++next)
                    if (folded[next].First == line)
                        hiddenTo = core::Max<i64>(hiddenTo, folded[next].Last);
            }
            m_rowStart.PushBack(row);
        }
        [[nodiscard]] const CodeFoldRange* RangeStarting(u32 line) const
        {
            EnsureFoldRanges();
            for (const CodeFoldRange& r : m_foldRanges)
                if (r.First == line)
                    return &r;
            return nullptr;
        }
        // Returns a line's first row; for a hidden line, the row of its fold.
        [[nodiscard]] u32 RowOfLine(u32 line) const
        {
            EnsureRows();
            line = core::Min(line, m_doc->LineCount() - 1);
            const u32 row = m_rowStart[line];
            if (m_rowStart[line + 1] != row || row == 0)
                return row;
            u32 shown = line; // a hidden line uses its fold's first row (the line it folds into may wrap)
            while (shown > 0 && m_rowStart[shown + 1] == m_rowStart[shown])
                --shown;
            return m_rowStart[shown];
        }
        // Returns the line shown on `row`: the last line starting at or before it. A hidden line
        // starts on the same row as the line after its fold, so the search lands on that line.
        [[nodiscard]] u32 LineOfRow(u32 row, u32* sub = nullptr) const
        {
            EnsureRows();
            usize low = 0, high = m_doc->LineCount(); // the first line starting after the row
            while (low < high)
            {
                const usize mid = (low + high) / 2;
                if (m_rowStart[mid] <= row)
                    low = mid + 1;
                else
                    high = mid;
            }
            u32 line = static_cast<u32>(low == 0 ? 0 : low - 1);
            while (line > 0 && m_rowStart[line + 1] == m_rowStart[line])
                --line; // past the end, inside a fold: use the fold's first line
            if (sub != nullptr)
                *sub = row >= m_rowStart[line] ? core::Min(row - m_rowStart[line], m_rowStart[line + 1] - m_rowStart[line] - 1) : 0;
            return line;
        }
        void FoldsChanged()
        {
            ++m_foldStamp;
            RequestLayout();
            Invalidate();
        }
        // Draws a line's whitespace as the setting says: a dot in the middle of a space, an arrow
        // across a tab.
        void DrawWhitespace(DrawContext& ctx, u32 line, const LineView& view, const TextLayout& layout, f32 textX, f32 top, Color color) const
        {
            if (layout.Lines.IsEmpty())
                return;
            const core::StringView text = m_doc->LineText(line);
            const f32 lh = LineHeight();
            const usize upTo = core::Min<usize>(text.Size(), kLongestLaidOut);
            for (u32 column = 0; column < upTo; ++column)
            {
                if ((text[column] != u8' ' && text[column] != u8'\t') || !ShowsWhitespaceAt(CodePosition{line, column}))
                    continue;
                const usize from = ShownColumn(view, column), to = ShownColumn(view, column + 1);
                const usize sub = layout.LineOf(from);
                const f32 x0 = textX + layout.CaretXOnLine(from, sub), x1 = textX + layout.CaretXOnLine(to, sub);
                const f32 y = top + static_cast<f32>(sub) * lh + lh * 0.5f;
                if (text[column] == u8' ')
                    ctx.VG().FillCircle(core::Float2{(x0 + x1) * 0.5f, y}, core::Max(1.0f, lh * 0.07f), color);
                else
                {
                    const f32 head = core::Min(lh * 0.2f, (x1 - x0) * 0.4f);
                    vg::PathBuilder arrow;
                    arrow.MoveTo(x0 + 2.0f, y);
                    arrow.LineTo(x1 - 2.0f, y);
                    arrow.MoveTo(x1 - 2.0f - head, y - head);
                    arrow.LineTo(x1 - 2.0f, y);
                    arrow.LineTo(x1 - 2.0f - head, y + head);
                    ctx.VG().StrokePath(arrow.ToPath(), color, vg::StrokeStyle(1.0f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
                }
            }
        }
        // Returns a folded line's placeholder rectangle, after its text (in local space).
        [[nodiscard]] Rect PlaceholderRect(u32 line) const
        {
            const Rect end = CaretRect(CodePosition{line, static_cast<u32>(m_doc->LineText(line).Size())});
            const f32 width = m_foldText.LayoutFor(Rect{}).Width + 8.0f;
            return Rect{end.x + DigitWidth() * 0.5f, end.y + 1.0f, width, end.height - 2.0f};
        }
        // Folds `range`, which is not folded yet, and moves its carets out to the line that stays
        // visible.
        void FoldRange(const CodeFoldRange& range)
        {
            m_folded.PushBack(m_doc->AddAnchor(CodePosition{range.First, 0}, CodeBias::Right));
            FoldsChanged();
            CaretsOutOfFolds();
        }
        // Moves each caret inside a folded range to the end of the fold's visible line.
        void CaretsOutOfFolds()
        {
            bool moved = false;
            for (CodeSelection& sel : m_selections)
                if (IsLineHidden(sel.Head.Line))
                {
                    const u32 line = LineOfRow(RowOfLine(sel.Head.Line));
                    const CodePosition end{line, static_cast<u32>(m_doc->LineText(line).Size())};
                    sel = CodeSelection{end, end};
                    moved = true;
                }
            if (!moved)
                return;
            Normalize();
            ResetGoals();
            SelectionsChanged();
        }
        // Unfolds any folded range that holds a caret (after a match is found, a line is gone to, or
        // the caret steps into it).
        void RevealCarets()
        {
            if (m_folded.IsEmpty())
                return;
            for (const CodeSelection& sel : m_selections)
                while (IsLineHidden(sel.Head.Line) && Unfold(sel.Head.Line))
                {
                }
        }

        // === Completion ===
        // Whether an event comes from the open list, on its way up through its owner.
        [[nodiscard]] bool FromCompletion(const Event& event) const
        {
            for (const Node* n = event.Target; n != nullptr && n != this; n = n->GetParent())
                if (n == m_completion.Get())
                    return true;
            return false;
        }
        [[nodiscard]] bool IsWordByte(char8_t c) const noexcept
        {
            return IsNameByte(c) || (c == u8'-' && m_language != nullptr && m_language->IdentifierDashes);
        }
        // Returns where the word ending at `at` begins, or `at` itself when no word ends there.
        [[nodiscard]] CodePosition WordStartBefore(CodePosition at) const
        {
            const core::StringView text = m_doc->LineText(at.Line);
            u32 column = core::Min<u32>(at.Column, static_cast<u32>(text.Size()));
            while (column > 0 && IsWordByte(text[column - 1]))
                --column;
            return CodePosition{at.Line, column};
        }
        // Gives the word typed since the list opened. Returns false when the caret has left it.
        [[nodiscard]] bool TypedWord(core::StringView& word) const
        {
            const CodeSelection& primary = GetPrimary();
            if (!primary.IsEmpty() || primary.Head.Line != m_wordStart.Line || primary.Head.Column < m_wordStart.Column)
                return false;
            const core::StringView text = m_doc->LineText(primary.Head.Line);
            if (primary.Head.Column > text.Size())
                return false;
            word = text.SubStr(m_wordStart.Column, primary.Head.Column - m_wordStart.Column);
            if (word.Size() == 0 && !m_completedEmpty)
                return false; // the caret went back past the start of the word the list opened on
            for (usize i = 0; i < word.Size(); ++i)
                if (!IsWordByte(word[i]))
                    return false;
            return true;
        }
        // Called when the caret moved or the text changed under the open list. Filters the list
        // again by the word, or closes it when the caret has left the word or nothing matches.
        void UpdateCompletion()
        {
            core::StringView word;
            if (!TypedWord(word) || m_completion->Filter(word) == 0)
            {
                CloseCompletion();
                return;
            }
            PlaceCompletion();
        }
        // Returns the anchor rectangle at the word's start, in the root's space, placed so the list's
        // labels line up with the word.
        [[nodiscard]] Rect CompletionAnchor() const
        {
            const Rect caret = CaretRect(m_wordStart);
            const core::Float2 p0 = ConvertToRootSpace(core::Float2{caret.x - m_completion->LabelInset(), caret.y});
            const core::Float2 p1 = ConvertToRootSpace(core::Float2{caret.x - m_completion->LabelInset(), caret.y + caret.height});
            return Rect{p0.x, core::Min(p0.y, p1.y), 0.0f, core::Abs(p1.y - p0.y)};
        }
        void PlaceCompletion()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (dispatcher == nullptr)
                return;
            if (dispatcher->IsPopupOpen(*m_completion))
            {
                (void)dispatcher->SetPopupAnchorRect(*m_completion, CompletionAnchor());
                return;
            }
            // Manual close: a press elsewhere reaches the editor, which closes the list, rather than being
            // swallowed.
            PopupOptions options;
            options.Kind = PopupKind::Manual;
            options.HasAnchorRect = true;
            options.AnchorRect = CompletionAnchor();
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 2.0f, true};
            options.FitContent = true;
            CodeEditor* self = this;
            options.OnClosed = [self](PopupCloseReason) { self->CompletionClosed(); };
            dispatcher->OpenPopup(*this, *m_completion, core::Move(options));
        }
        void CompletionClosed()
        {
            if (!m_completing)
                return;
            m_completing = false;
            SetKeyContext(core::StringView(u8"codeeditor"));
        }
        // Runs after a character is typed. A trigger character opens the list afresh; a letter of a
        // word opens it.
        void CompleteAfterTyping(core::StringView text)
        {
            if (m_service == nullptr || text.Size() != 1)
                return;
            const char8_t c = text[0];
            const core::StringView triggers = m_service->CompletionTriggers();
            for (usize i = 0; i < triggers.Size(); ++i)
                if (triggers[i] == c)
                {
                    (void)TriggerCompletion(false, c);
                    return;
                }
            if (!m_completing && IsWordByte(c))
                (void)TriggerCompletion(false);
        }
        void ResetBlink()
        {
            m_caretShown = true;
            m_blink = 0.0f;
            Invalidate();
        }
        void KeepTicking()
        {
            if (m_ticking || GetActionManager() == nullptr)
                return;
            m_ticking = true;
            CodeEditor* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
                                                {
                                                    const f32 seconds = static_cast<f32>(elapsed.AsSeconds());
                                                    self->m_doc->AdvanceClock(seconds);
                                                    if (!self->IsFocused())
                                                    {
                                                        self->m_ticking = false;
                                                        return false;
                                                    }
                                                    self->m_blink += seconds;
                                                    if (self->m_blink >= kBlinkSeconds)
                                                    {
                                                        self->m_blink -= kBlinkSeconds;
                                                        self->m_caretShown = !self->m_caretShown;
                                                        self->Invalidate();
                                                    }
                                                    return true;
                                                }));
        }

        // Applies each selection's edit, from the last to the first (an edit moves only what follows
        // it), as one undo step. Each caret ends where `edit` says, then is moved by the edits after
        // it.
        template <typename Fn>
        void EditEach(CodeEditKind kind, Fn&& edit)
        {
            if (m_readOnly)
                return;
            const core::Array<CodeSelection> before = m_selections;
            core::Array<CodeSelection> after(MemoryAllocator());
            for (usize i = 0; i < m_selections.Size(); ++i)
                after.PushBack(m_selections[i]);
            m_doc->Begin(before.AsSpan(), kind);
            for (usize i = m_selections.Size(); i-- > 0;)
            {
                CodeRange replaced;
                core::String text;
                u32 back = 0; // the caret goes this many bytes before the end of the inserted text
                if constexpr (requires { edit(m_selections[i], replaced, text, back); })
                {
                    if (!edit(m_selections[i], replaced, text, back))
                        continue;
                }
                else if (!edit(m_selections[i], replaced, text))
                    continue;
                const CodeRange put = m_doc->Replace(replaced, text.AsView(), kind);
                // Place the caret `back` bytes before the end of the inserted text, counting across line
                // breaks.
                CodePosition caret = put.Start;
                const usize upTo = text.Size() >= back ? text.Size() - back : 0;
                for (usize k = 0; k < upTo; ++k)
                    caret = text.AsView()[k] == u8'\n' ? CodePosition{caret.Line + 1, 0} : CodePosition{caret.Line, caret.Column + 1};
                if (back == 0)
                    caret = put.End;
                after[i] = CodeSelection{caret, caret};
                for (usize j = i + 1; j < after.Size(); ++j)
                    after[j] = CodeSelection{CodeDocument::Moved(after[j].Anchor, CodeBias::Right, replaced, put.End),
                                             CodeDocument::Moved(after[j].Head, CodeBias::Right, replaced, put.End)};
            }
            m_doc->End(after.AsSpan());
            const usize primary = m_primary;
            m_selections = core::Move(after);
            m_primary = core::Min(primary, m_selections.Size() - 1);
            Normalize();
            ResetGoals();
            SelectionsChanged();
        }
        // Moves each selection's head. With `extend` the anchor stays; otherwise the selection
        // collapses onto the head.
        template <typename Fn>
        void MoveEach(bool extend, Fn&& move)
        {
            for (usize i = 0; i < m_selections.Size(); ++i)
            {
                CodeSelection& s = m_selections[i];
                const CodePosition head = move(s, m_goals[i]);
                s = extend ? CodeSelection{s.Anchor, head} : CodeSelection{head, head};
            }
            m_doc->BreakMerge();
            const core::Array<f32> goals = m_goals;
            Normalize();
            if (m_goals.Size() != m_selections.Size())
                ResetGoals();
            else
                m_goals = goals;
            SelectionsChanged();
        }
        // Edits planned against the current text: each is a range and its new text, and none overlap.
        // ApplyPlanned applies them from the last as one undo step and carries every selection
        // through each edit. A selection's start is biased left; its end and a caret are biased
        // right.
        struct Planned
        {
            CodeRange Range;
            core::String Text;
        };
        void ApplyPlanned(core::Array<Planned>& edits, CodeEditKind kind = CodeEditKind::Other)
        {
            if (m_readOnly || edits.IsEmpty())
                return;
            for (usize i = 1; i < edits.Size(); ++i)
                for (usize j = i; j > 0 && edits[j - 1].Range.Start < edits[j].Range.Start; --j)
                    core::Swap(edits[j], edits[j - 1]);
            const core::Array<CodeSelection> before = m_selections;
            core::Array<CodeSelection> after = m_selections;
            m_doc->Begin(before.AsSpan(), kind);
            for (const Planned& edit : edits)
            {
                const CodeRange put = m_doc->Replace(edit.Range, edit.Text.AsView(), kind);
                for (CodeSelection& sel : after)
                {
                    const bool forward = sel.Anchor <= sel.Head;
                    const CodeBias anchorBias = sel.IsEmpty() ? CodeBias::Right : (forward ? CodeBias::Left : CodeBias::Right);
                    const CodeBias headBias = sel.IsEmpty() ? CodeBias::Right : (forward ? CodeBias::Right : CodeBias::Left);
                    sel = CodeSelection{CodeDocument::Moved(sel.Anchor, anchorBias, edit.Range, put.End),
                                        CodeDocument::Moved(sel.Head, headBias, edit.Range, put.End)};
                }
            }
            m_doc->End(after.AsSpan());
            m_selections = core::Move(after);
            Normalize();
            ResetGoals();
            SelectionsChanged();
        }
        // Gives the lines the selections cover, as runs [first, last] in order. A selection that ends
        // at a line's start does not include that line.
        void LineBlocks(core::Array<u32>& firsts, core::Array<u32>& lasts) const
        {
            for (const CodeSelection& sel : m_selections)
            {
                const CodeRange r = sel.Range();
                const u32 first = r.Start.Line;
                const u32 last = r.End.Line > r.Start.Line && r.End.Column == 0 ? r.End.Line - 1 : r.End.Line;
                if (!lasts.IsEmpty() && first <= lasts.Back() + 1)
                {
                    lasts.Back() = core::Max(lasts.Back(), last);
                    continue;
                }
                firsts.PushBack(first);
                lasts.PushBack(last);
            }
        }
        [[nodiscard]] core::String IndentText(u32 columns) const
        {
            core::String out;
            if (!m_insertSpaces)
                for (; columns >= m_tabSize; columns -= m_tabSize)
                    out.Append(core::StringView(u8"\t"));
            for (; columns > 0; --columns)
                out.Append(core::StringView(u8" "));
            return out;
        }
        // Returns a line's indent in columns, with tabs expanded to their stops.
        [[nodiscard]] u32 IndentColumns(core::StringView text) const
        {
            u32 columns = 0;
            for (usize i = 0; i < text.Size() && (text[i] == u8' ' || text[i] == u8'\t'); ++i)
                columns = text[i] == u8'\t' ? (columns / m_tabSize + 1) * m_tabSize : columns + 1;
            return columns;
        }
        // Whether `text` (a line's text before the caret) ends with one of the space-separated words
        // or marks in `list`. A word counts only when no name character comes right before it.
        [[nodiscard]] static bool EndsWithAny(core::StringView text, core::StringView list)
        {
            usize end = text.Size();
            while (end > 0 && (text[end - 1] == u8' ' || text[end - 1] == u8'\t'))
                --end;
            text = text.SubStr(0, end);
            usize at = 0;
            while (at < list.Size())
            {
                while (at < list.Size() && list[at] == u8' ')
                    ++at;
                usize stop = at;
                while (stop < list.Size() && list[stop] != u8' ')
                    ++stop;
                const core::StringView item = list.SubStr(at, stop - at);
                at = stop;
                if (item.Size() == 0 || item.Size() > text.Size() || text.SubStr(text.Size() - item.Size(), item.Size()) != item)
                    continue;
                const bool word = IsNameByte(item[0]);
                if (word && text.Size() > item.Size() && IsNameByte(text[text.Size() - item.Size() - 1]))
                    continue;
                return true;
            }
            return false;
        }
        [[nodiscard]] static bool IsListed(core::StringView item, core::StringView list)
        {
            usize at = 0;
            while (at < list.Size())
            {
                while (at < list.Size() && list[at] == u8' ')
                    ++at;
                usize stop = at;
                while (stop < list.Size() && list[stop] != u8' ')
                    ++stop;
                if (stop > at && list.SubStr(at, stop - at) == item)
                    return true;
                at = stop;
            }
            return false;
        }
        [[nodiscard]] static bool IsNameByte(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_' ||
                   static_cast<u8>(c) >= 0x80u;
        }
        // Returns the token of the character before `at` on its line, which is what a caret is "in".
        [[nodiscard]] CodeToken TokenBefore(CodePosition at) const
        {
            if (m_language == nullptr || at.Column == 0)
                return CodeToken::Text;
            for (const CodeSpan& span : TokensOf(at.Line))
                if (span.Start < at.Column && at.Column <= span.End)
                    return span.Token;
            return CodeToken::Text;
        }
        [[nodiscard]] CodeToken TokenAtColumn(u32 line, u32 column) const
        {
            if (m_language == nullptr)
                return CodeToken::Text;
            for (const CodeSpan& span : TokensOf(line))
                if (span.Start <= column && column < span.End)
                    return span.Token;
            return CodeToken::Text;
        }
        void TypeCharacter(core::StringView text);
        // Special Backspace cases: between an auto-closed pair, both characters go; in an indent of
        // spaces, it deletes back to the previous tab stop.
        bool DeleteBackwardSpecial(CodePosition at, CodeRange& replaced) const
        {
            const core::StringView line = m_doc->LineText(at.Line);
            if (m_language != nullptr)
                for (const CodePair& pair : m_language->AutoClose)
                {
                    const core::StringView open = pair.Open.AsView(), close = pair.Close.AsView();
                    if (at.Column >= open.Size() && at.Column + close.Size() <= line.Size() &&
                        line.SubStr(at.Column - open.Size(), open.Size()) == open && line.SubStr(at.Column, close.Size()) == close)
                    {
                        replaced = CodeRange{CodePosition{at.Line, at.Column - static_cast<u32>(open.Size())},
                                             CodePosition{at.Line, at.Column + static_cast<u32>(close.Size())}};
                        return true;
                    }
                }
            if (m_insertSpaces && at.Column > 0 && at.Column <= IndentOf(line))
            {
                bool spaces = true;
                for (u32 i = 0; i < at.Column; ++i)
                    spaces = spaces && line[i] == u8' ';
                if (spaces)
                {
                    const u32 stop = (at.Column - 1) / m_tabSize * m_tabSize;
                    replaced = CodeRange{CodePosition{at.Line, stop}, at};
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] CodePosition WordLeft(CodePosition at) const
        {
            if (at.Column == 0)
                return m_doc->PreviousCharacter(at);
            return CodePosition{at.Line, static_cast<u32>(PrevWordBoundary(m_doc->LineText(at.Line), at.Column))};
        }
        [[nodiscard]] CodePosition WordRight(CodePosition at) const
        {
            const core::StringView text = m_doc->LineText(at.Line);
            if (at.Column >= text.Size())
                return m_doc->NextCharacter(at);
            return CodePosition{at.Line, static_cast<u32>(NextWordBoundary(text, at.Column))};
        }
        // Smart Home: the line's first character past its indent, or the line's start when the caret
        // is already there.
        [[nodiscard]] CodePosition LineHome(CodePosition at) const
        {
            const u32 indent = IndentOf(m_doc->LineText(at.Line));
            return CodePosition{at.Line, at.Column == indent ? 0u : indent};
        }
        [[nodiscard]] static u32 IndentOf(core::StringView text)
        {
            u32 n = 0;
            while (n < text.Size() && (text[n] == u8' ' || text[n] == u8'\t'))
                ++n;
            return n;
        }
        // Moves `lines` rows up or down, at the horizontal goal (taken from the caret when unset).
        [[nodiscard]] CodePosition Vertical(CodePosition at, i32 lines, f32& goal) const
        {
            const Place place = PlaceOf(at);
            if (goal < 0.0f)
                goal = place.X;
            const i64 target = static_cast<i64>(place.Row) + lines; // by rows: a fold is one row, a wrapped line several
            if (target < 0)
                return CodePosition{0, 0};
            if (target >= static_cast<i64>(RowCount()))
                return m_doc->End();
            u32 sub = 0;
            const u32 line = LineOfRow(static_cast<u32>(target), &sub);
            return CodePosition{line, ColumnAt(line, sub, goal)};
        }
        [[nodiscard]] i32 PageLines() const { return core::Max(1, static_cast<i32>(GetContentBounds().height / LineHeight()) - 1); }
        [[nodiscard]] IClipboard* Clipboard() const
        {
            const EventDispatcher* dispatcher = GetEventDispatcher();
            return dispatcher != nullptr ? dispatcher->GetClipboard() : nullptr;
        }
        void Copy(bool cut);
        void Paste();
        void Newline();
        void Indent();

        // Tokenizes up to `line`, starting from the first line not known to agree with the line
        // before it. A line is tokenized again when its text or its entry state changed; the rest
        // keep their tokens.
        void EnsureLexed(u32 line) const
        {
            if (m_language == nullptr)
                return;
            while (m_lex.Size() < m_doc->LineCount())
                m_lex.PushBack(LineLex{});
            for (u32 i = m_cleanTo; i <= line && i < m_doc->LineCount(); ++i)
            {
                const CodeLexState entry = i == 0 ? CodeLexState{} : m_lex[i - 1].Exit;
                LineLex& lex = m_lex[i];
                const u64 hash = m_doc->LineHash(i);
                if (lex.Valid && lex.Hash == hash && lex.Entry == entry)
                    continue;
                lex.Valid = true;
                lex.Hash = hash;
                lex.Entry = entry;
                CodeLexState state = entry;
                m_language->Tokenize(m_doc->LineText(i), state, lex.Spans);
                lex.Exit = state;
                ++lex.Stamp;
                ++m_tokenized;
            }
            m_cleanTo = core::Max(m_cleanTo, core::Min(line + 1, m_doc->LineCount()));
        }
        // Returns a token's colour: its part's colour, or else the text colour.
        [[nodiscard]] Color TokenColor(CodeToken token) const
        {
            if (token == CodeToken::Text)
                return GetTextColor();
            const PartStyle& part = GetPartStyle(CodeTokenName(token));
            return part.HasColor ? part.TextColor : GetTextColor();
        }

        void FindChanged()
        {
            m_matchesVersion = ~u64(0);
            UpdateCount();
            Invalidate();
        }
        void UpdateCount()
        {
            RefreshMatches();
            m_findBar->SetCount(CurrentMatch(), m_matches.Size());
        }
        // Finds the matches again when the text, the bar's text or the bar's toggles have changed.
        void RefreshMatches() const
        {
            const core::StringView query = m_findBar->GetQuery();
            const u64 key = m_doc->GetVersion() * 4u + (m_findBar->MatchCase() ? 1u : 0u) + (m_findBar->WholeWord() ? 2u : 0u);
            if (m_matchesVersion == key && m_matchesQuery.AsView() == query)
                return;
            m_matchesVersion = key;
            m_matchesQuery = core::String(query);
            m_matches.Clear();
            if (query.Size() == 0)
                return;
            CodePosition from{0, 0};
            CodeRange found;
            while (m_matches.Size() < 100000 &&
                   m_doc->Find(query, from, m_findBar->MatchCase(), m_findBar->WholeWord(), false, found))
            {
                m_matches.PushBack(found);
                from = found.End;
                if (found.End == m_doc->End())
                    break;
            }
        }

        struct Marker
        {
            u32 Id = 0;
            CodeAnchorId Anchor = 0;
            core::String Kind;
        };
        struct Diagnostic
        {
            CodeAnchorId Start = 0;
            CodeAnchorId End = 0;
            CodeSeverity Severity = CodeSeverity::Error;
            core::String Message;
        };
        void ClearDiagnosticAnchors()
        {
            for (const Diagnostic& d : m_diagnostics)
            {
                m_doc->RemoveAnchor(d.Start);
                m_doc->RemoveAnchor(d.End);
            }
            m_diagnostics.Clear();
        }
        // Returns the range a diagnostic marks. An empty diagnostic marks the word at its position,
        // or one character.
        [[nodiscard]] CodeRange MarkedRange(const Diagnostic& d) const
        {
            CodeRange r{m_doc->GetAnchor(d.Start), m_doc->GetAnchor(d.End)};
            if (r.IsEmpty())
            {
                const core::StringView text = m_doc->LineText(r.Start.Line);
                const TextRange word = WordRangeAt(text, r.Start.Column);
                r = word.End > word.Start ? CodeRange{CodePosition{r.Start.Line, static_cast<u32>(word.Start)}, CodePosition{r.Start.Line, static_cast<u32>(word.End)}}
                                          : CodeRange{r.Start, m_doc->NextCharacter(r.Start)};
            }
            return r;
        }

        CodeDocument m_own;
        CodeDocument* m_doc = nullptr;
        core::Array<Marker> m_markers;
        u32 m_lastMarker = 0;
        core::Array<Diagnostic> m_diagnostics;
        core::Function<void(u32)> m_onGutterClick;
        core::String m_hoverTip; // the diagnostics under the pointer, then the service's hover text
        ICodeLanguageService* m_service = nullptr;
        core::RefPtr<CodeCompletionList> m_completion; // a child popup while the list is open
        bool m_completing = false;
        CodePosition m_wordStart;  // where the word the list completes begins
        // Folding state: the ranges (the host's or computed ones), the folded ranges (anchored on
        // their first lines), and each line's first row.
        mutable core::Array<CodeFoldRange> m_foldRanges;
        mutable u64 m_foldRangesVersion = ~u64(0);
        bool m_hostFolds = false;
        core::Array<CodeFoldRange> m_hostRanges;
        mutable core::Array<CodeAnchorId> m_folded;
        u64 m_foldStamp = 0;
        mutable core::Array<u32> m_rowStart;
        mutable u64 m_rowsVersion = ~u64(0);
        mutable u64 m_rowsStamp = ~u64(0);
        mutable f32 m_rowsWrap = -1.0f;
        mutable u32 m_rowsTabSize = 0;
        bool m_wrap = false;
        CodeWhitespace m_whitespace = CodeWhitespace::None;
        f32 m_wrapWidth = 0.0f; // the text's width, while wrapping
        bool m_gutterHover = false;
        Text m_foldText; // a folded line's placeholder
        bool m_completedEmpty = false; // opened on no word (a trigger), so an empty word keeps it open
        CodeRange m_hoverWord;     // the word the service last gave hover text for
        u64 m_hoverVersion = ~u64(0);
        core::String m_hoverInfo;
        u32 m_listener = 0; // the token of the editor's change listener on m_doc
        core::RefPtr<CodeFindBar> m_findBar;
        mutable core::Array<CodeRange> m_matches;
        mutable core::String m_matchesQuery;
        mutable u64 m_matchesVersion = ~u64(0);
        const CodeLanguage* m_language = nullptr;
        mutable core::Array<LineLex> m_lex; // per line, tokenized up to the last line asked for
        mutable u32 m_cleanTo = 0;          // lines before this one are known to agree with the lines before them
        mutable u64 m_tokenized = 0;
        core::Array<CodeSelection> m_selections;
        core::Array<f32> m_goals; // each selection's horizontal goal for Up and Down (below 0 when unset)
        usize m_primary = 0;
        mutable core::Array<core::UniquePtr<LineView>> m_views; // per line, made as lines come into view
        mutable f32 m_widest = 0.0f; // the widest line laid out
        f32 m_estimated = 0.0f;
        u64 m_widestVersion = ~u64(0);
        Text m_measure; // ten digits, measuring the gutter's width
        core::Function<void(Text&)> m_styleText;
        core::String m_composition;
        Text m_compositionText;
        u32 m_tabSize = 4;
        bool m_insertSpaces = true;
        bool m_readOnly = false;
        bool m_caretShown = true;
        f32 m_blink = 0.0f;
        bool m_ticking = false;
        // A press's drag: where it began and by what unit (1 a character, 2 a word, 3 a line).
        bool m_dragging = false;
        u32 m_dragUnit = 1;
        CodeRange m_dragOrigin;
        bool m_dragAdds = false;
        f32 m_columnX = 0.0f;                      // the x where it began (in the text's own space)
        u32 m_columnLine = 0;
    };

    inline CodeEditor::LineView& CodeEditor::View(u32 line) const
    {
        while (m_views.Size() < m_doc->LineCount())
            m_views.PushBack(core::UniquePtr<LineView>{});
        core::UniquePtr<LineView>& slot = m_views[line];
        if (!slot)
            slot = core::MakeUnique<LineView>(const_cast<CodeEditor*>(this)->MemoryAllocator());
        LineView& view = *slot;
        const u64 hash = m_doc->LineHash(line);
        u64 stamp = 0;
        if (m_language != nullptr)
        {
            EnsureLexed(line);
            stamp = m_lex[line].Stamp;
        }
        if (view.Laid && view.Hash == hash && view.TabSize == m_tabSize && view.LexStamp == stamp)
            return view;
        view.LexStamp = stamp;
        view.Laid = true;
        view.Hash = hash;
        view.TabSize = m_tabSize;
        view.Shown.Clear();
        view.ShownAt.Clear();
        const core::StringView text = m_doc->LineText(line);
        bool tabs = false;
        for (usize i = 0; i < text.Size() && !tabs; ++i)
            tabs = text[i] == u8'\t';
        const usize cut = core::Min<usize>(text.Size(), kLongestLaidOut);
        if (!tabs)
            view.Shown = core::String(text.SubStr(0, cut));
        else
        {
            u32 visual = 0; // characters so far, for the tab stops
            for (usize i = 0; i < cut; ++i)
            {
                view.ShownAt.PushBack(static_cast<u32>(view.Shown.Size()));
                const char8_t c = text[i];
                if (c == u8'\t')
                {
                    const u32 spaces = m_tabSize - visual % m_tabSize;
                    for (u32 k = 0; k < spaces; ++k)
                        view.Shown.Append(core::StringView(u8" "));
                    visual += spaces;
                    continue;
                }
                view.Shown.Append(text.SubStr(i, 1));
                if ((static_cast<u8>(c) & 0xC0u) != 0x80u)
                    ++visual;
            }
            for (usize i = cut; i <= text.Size(); ++i)
                view.ShownAt.PushBack(static_cast<u32>(view.Shown.Size()));
        }
        if (m_styleText)
            m_styleText(view.Glyphs);
        if (m_wrap)
        {
            view.Glyphs.SetWhiteSpace(TextWhiteSpace::PreWrap);
            view.Glyphs.SetBreakWords(true);
        }
        if (m_language == nullptr)
            view.Glyphs.SetString(view.Shown.AsView());
        else
        {
            // The line's tokens as the core's rich text, each run in its token's colour. A literal "["
            // is written "[[".
            core::String markup;
            const core::StringView shown = view.Shown.AsView();
            const auto append = [&markup](core::StringView piece)
            {
                for (usize i = 0; i < piece.Size(); ++i)
                    markup.Append(piece[i] == u8'[' ? core::StringView(u8"[[") : piece.SubStr(i, 1));
            };
            u32 done = 0;
            for (const CodeSpan& span : m_lex[line].Spans)
            {
                const u32 from = ShownColumn(view, span.Start), to = ShownColumn(view, span.End);
                if (from >= shown.Size())
                    break;
                if (from > done)
                    append(shown.SubStr(done, from - done));
                const core::StringView spanText = shown.SubStr(from, core::Min<usize>(to, shown.Size()) - from);
                if (span.Token == CodeToken::Text)
                    append(spanText);
                else
                {
                    const Color c = TokenColor(span.Token);
                    char8_t tag[] = u8"[color=#rrggbbaa]";
                    const f32 channels[4] = {c.r, c.g, c.b, c.a};
                    constexpr char8_t kHex[] = u8"0123456789abcdef";
                    for (usize k = 0; k < 4; ++k)
                    {
                        const u32 v = static_cast<u32>(core::Clamp(channels[k], 0.0f, 1.0f) * 255.0f + 0.5f);
                        tag[8 + k * 2] = kHex[v >> 4];
                        tag[9 + k * 2] = kHex[v & 15u];
                    }
                    markup.Append(core::StringView(tag, sizeof(tag) - 1));
                    append(spanText);
                    markup.Append(core::StringView(u8"[/color]"));
                }
                done = core::Min<u32>(to, static_cast<u32>(shown.Size()));
            }
            if (done < shown.Size())
                append(shown.SubStr(done, shown.Size() - done));
            view.Glyphs.SetMarkup(markup.AsView());
        }
        m_widest = core::Max(m_widest, view.Glyphs.LayoutFor(Rect{}).Width);
        return view;
    }

    inline void CodeEditor::VisibleLines(u32& first, u32& last) const
    {
        const Rect content = GetContentBounds();
        const f32 top = ScrollShift().y;
        const f32 lh = LineHeight();
        const u32 rows = RowCount();
        first = LineOfRow(core::Min(static_cast<u32>(core::Max(0.0f, top / lh)), rows - 1));
        last = LineOfRow(core::Min(static_cast<u32>(core::Max(0.0f, (top + content.height) / lh)), rows - 1));
    }

    inline CodePosition CodeEditor::PositionAt(core::Float2 local) const
    {
        const Rect content = GetContentBounds();
        const core::Float2 shift = ScrollShift();
        const f32 row = (local.y - content.y + shift.y) / LineHeight();
        if (row < 0.0f)
            return CodePosition{0, 0};
        if (static_cast<u32>(row) >= RowCount())
            return m_doc->End();
        u32 sub = 0;
        const u32 line = LineOfRow(static_cast<u32>(row), &sub);
        return CodePosition{line, ColumnAt(line, sub, local.x - TextLeft() + shift.x)};
    }

    inline Rect CodeEditor::CaretRect(CodePosition at) const
    {
        at = m_doc->Clamp(at);
        const Rect content = GetContentBounds();
        const core::Float2 shift = ScrollShift();
        const f32 lh = LineHeight();
        const Place place = PlaceOf(at);
        return Rect{TextLeft() + place.X - shift.x, content.y + static_cast<f32>(place.Row) * lh - shift.y, 1.5f, lh};
    }

    inline void CodeEditor::ScrollToCaret()
    {
        const Rect content = GetContentBounds();
        if (content.height <= 0.0f)
            return;
        const CodePosition at = m_doc->Clamp(GetPrimary().Head);
        const f32 lh = LineHeight();
        core::Float2 offset = GetScrollOffset();
        const Place place = PlaceOf(at);
        const f32 top = static_cast<f32>(place.Row) * lh;
        if (top < offset.y)
            offset.y = top;
        else if (top + lh > offset.y + content.height)
            offset.y = top + lh - content.height;
        const f32 x = place.X;
        const f32 room = content.width - GutterWidth();
        if (x < offset.x)
            offset.x = core::Max(0.0f, x - room * 0.25f);
        else if (x + DigitWidth() > offset.x + room)
            offset.x = x + DigitWidth() - room + room * 0.25f;
        SetScrollOffset(offset);
    }

    inline bool CodeEditor::Run(TextEditCommand command)
    {
        using C = TextEditCommand;
        const auto extendFor = [](C c)
        {
            return c == C::SelectLeft || c == C::SelectRight || c == C::SelectWordLeft || c == C::SelectWordRight ||
                   c == C::SelectLineStart || c == C::SelectLineEnd || c == C::SelectUp || c == C::SelectDown ||
                   c == C::SelectPageUp || c == C::SelectPageDown || c == C::SelectDocumentStart || c == C::SelectDocumentEnd;
        };
        const bool extend = extendFor(command);
        CodeDocument& doc = *m_doc;
        switch (command)
        {
        case C::MoveLeft:
        case C::SelectLeft:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return !extend && !s.IsEmpty() ? s.Range().Start : doc.PreviousCharacter(s.Head);
            });
            return true;
        case C::MoveRight:
        case C::SelectRight:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return !extend && !s.IsEmpty() ? s.Range().End : doc.NextCharacter(s.Head);
            });
            return true;
        case C::MoveWordLeft:
        case C::SelectWordLeft:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return WordLeft(s.Head);
            });
            return true;
        case C::MoveWordRight:
        case C::SelectWordRight:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return WordRight(s.Head);
            });
            return true;
        case C::MoveLineStart:
        case C::SelectLineStart:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return LineHome(s.Head);
            });
            return true;
        case C::MoveLineEnd:
        case C::SelectLineEnd:
            MoveEach(extend, [&](const CodeSelection& s, f32& goal)
            {
                goal = -1.0f;
                return CodePosition{s.Head.Line, static_cast<u32>(doc.LineText(s.Head.Line).Size())};
            });
            return true;
        case C::MoveUp:
        case C::SelectUp:
        case C::MoveDown:
        case C::SelectDown:
        case C::MovePageUp:
        case C::SelectPageUp:
        case C::MovePageDown:
        case C::SelectPageDown:
        {
            const bool up = command == C::MoveUp || command == C::SelectUp || command == C::MovePageUp || command == C::SelectPageUp;
            const bool page = command == C::MovePageUp || command == C::SelectPageUp || command == C::MovePageDown || command == C::SelectPageDown;
            const i32 lines = (page ? PageLines() : 1) * (up ? -1 : 1);
            MoveEach(extend, [&](const CodeSelection& s, f32& goal) { return Vertical(s.Head, lines, goal); });
            if (page)
            {
                core::Float2 offset = GetScrollOffset();
                offset.y += static_cast<f32>(lines) * LineHeight();
                SetScrollOffset(offset);
                ScrollToCaret();
            }
            return true;
        }
        case C::MoveDocumentStart:
        case C::SelectDocumentStart:
            MoveEach(extend, [&](const CodeSelection&, f32& goal)
            {
                goal = -1.0f;
                return CodePosition{0, 0};
            });
            return true;
        case C::MoveDocumentEnd:
        case C::SelectDocumentEnd:
            MoveEach(extend, [&](const CodeSelection&, f32& goal)
            {
                goal = -1.0f;
                return doc.End();
            });
            return true;
        case C::SelectAll:
        {
            const CodeSelection all{CodePosition{0, 0}, doc.End()};
            SetSelections(core::Span<const CodeSelection>(&all, 1));
            return true;
        }
        case C::DeleteBackward:
        case C::DeleteForward:
        case C::DeleteWordBackward:
        case C::DeleteWordForward:
        case C::DeleteToLineStart:
        case C::DeleteToLineEnd:
            EditEach(CodeEditKind::Deleting, [&](const CodeSelection& s, CodeRange& replaced, core::String&)
            {
                if (!s.IsEmpty())
                    replaced = s.Range();
                else if (command == C::DeleteBackward && DeleteBackwardSpecial(s.Head, replaced))
                    return true;
                else
                {
                    const CodePosition at = s.Head;
                    CodePosition other = at;
                    switch (command)
                    {
                    case C::DeleteBackward: other = doc.PreviousCharacter(at); break;
                    case C::DeleteForward: other = doc.NextCharacter(at); break;
                    case C::DeleteWordBackward: other = WordLeft(at); break;
                    case C::DeleteWordForward: other = WordRight(at); break;
                    case C::DeleteToLineStart: other = CodePosition{at.Line, 0}; break;
                    default: other = CodePosition{at.Line, static_cast<u32>(doc.LineText(at.Line).Size())}; break;
                    }
                    replaced = CodeRange::Ordered(at, other);
                }
                return !replaced.IsEmpty();
            });
            return true;
        case C::InsertNewline: Newline(); return true;
        case C::Undo:
        case C::Redo:
        {
            core::Array<CodeSelection> selections(MemoryAllocator());
            const bool done = command == C::Undo ? doc.Undo(selections) : doc.Redo(selections);
            if (done)
                SetSelections(selections.AsSpan());
            return done;
        }
        case C::Cut: Copy(true); return true;
        case C::Copy: Copy(false); return true;
        case C::Paste: Paste(); return true;
        default: return false;
        }
    }

    inline void CodeEditor::Type(core::StringView text)
    {
        // A single character, in a language that pairs or outdents, follows that language's rules.
        if (m_language != nullptr && text.Size() >= 1 && text.Size() <= 4 && NextGrapheme(text, 0) == text.Size() && text[0] != u8'\n')
            TypeCharacter(text);
        else
            EditEach(CodeEditKind::Typing, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
            {
                replaced = s.Range();
                put = core::String(text);
                return true;
            });
        CompleteAfterTyping(text);
    }

    inline bool CodeEditor::TriggerCompletion(bool invoked, char8_t trigger)
    {
        if (m_service == nullptr || m_readOnly || !GetPrimary().IsEmpty())
            return false;
        const CodePosition at = m_doc->Clamp(GetPrimary().Head);
        const CodePosition start = WordStartBefore(at);
        if (!invoked)
        {
            // Open the list while typing in code (not in a comment or a string), on a word that is a
            // name (not a number), or just after a trigger.
            const CodeToken token = TokenBefore(at);
            if (token == CodeToken::Comment || token == CodeToken::String)
                return false;
            const core::StringView text = m_doc->LineText(at.Line);
            if (trigger == 0 && (start.Column == at.Column || (text[start.Column] >= u8'0' && text[start.Column] <= u8'9')))
                return false;
        }
        CodeCompletionRequest request;
        request.At = at;
        request.WordStart = start;
        request.Invoked = invoked;
        request.Trigger = trigger;
        core::Array<CodeCompletionItem> items(MemoryAllocator());
        m_service->Complete(*m_doc, request, items);
        m_wordStart = start;
        m_completedEmpty = start == at;
        const core::StringView text = m_doc->LineText(at.Line);
        m_completion->SetItems(core::Move(items), text.SubStr(start.Column, at.Column - start.Column));
        if (m_completion->ShownCount() == 0)
        {
            CloseCompletion();
            return false;
        }
        m_completing = true;
        SetKeyContext(core::StringView(u8"codeeditor completing"));
        PlaceCompletion();
        return true;
    }

    inline bool CodeEditor::AcceptCompletion()
    {
        const CodeCompletionItem* item = m_completing ? m_completion->SelectedItem() : nullptr;
        core::StringView word;
        if (item == nullptr || m_readOnly || !TypedWord(word))
        {
            CloseCompletion();
            return false;
        }
        const core::String insert(item->Insert.IsEmpty() ? item->Label.AsView() : item->Insert.AsView());
        const core::String typed(word);
        CloseCompletion();
        m_doc->BreakMerge();
        EditEach(CodeEditKind::Other, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            replaced = s.Range();
            if (s.IsEmpty() && s.Head.Column >= typed.Size())
            {
                const core::StringView line = m_doc->LineText(s.Head.Line);
                const u32 from = s.Head.Column - static_cast<u32>(typed.Size());
                if (s.Head.Column <= line.Size() && line.SubStr(from, typed.Size()) == typed.AsView())
                    replaced.Start.Column = from;
            }
            put = core::String(insert.AsView());
            return true;
        });
        m_doc->BreakMerge();
        return true;
    }

    inline bool CodeEditor::IsFolded(u32 firstLine) const
    {
        for (const CodeAnchorId anchor : m_folded)
            if (m_doc->GetAnchor(anchor).Line == firstLine)
                return RangeStarting(firstLine) != nullptr;
        return false;
    }

    inline bool CodeEditor::Fold(u32 line)
    {
        EnsureFoldRanges();
        const CodeFoldRange* best = nullptr;
        for (const CodeFoldRange& r : m_foldRanges)
            if (r.First <= line && line <= r.Last && !IsFolded(r.First) && (best == nullptr || r.Last - r.First < best->Last - best->First))
                best = &r;
        if (best == nullptr)
            return false;
        FoldRange(*best);
        return true;
    }

    inline bool CodeEditor::Unfold(u32 line)
    {
        EnsureFoldRanges();
        bool any = false;
        for (usize i = m_folded.Size(); i-- > 0;)
        {
            const CodeFoldRange* r = RangeStarting(m_doc->GetAnchor(m_folded[i]).Line);
            if (r != nullptr && r->First <= line && line <= r->Last)
            {
                m_doc->RemoveAnchor(m_folded[i]);
                m_folded.RemoveAt(i);
                any = true;
            }
        }
        if (any)
            FoldsChanged();
        return any;
    }

    inline bool CodeEditor::ToggleFold(u32 line)
    {
        const CodeFoldRange* r = RangeStarting(line);
        if (r == nullptr)
            return false;
        if (!IsFolded(line))
        {
            FoldRange(*r);
            return true;
        }
        for (usize i = m_folded.Size(); i-- > 0;)
            if (m_doc->GetAnchor(m_folded[i]).Line == line)
            {
                m_doc->RemoveAnchor(m_folded[i]);
                m_folded.RemoveAt(i);
            }
        FoldsChanged();
        return true;
    }

    inline void CodeEditor::FoldAll()
    {
        EnsureFoldRanges();
        for (const CodeFoldRange& r : m_foldRanges)
            if (!IsFolded(r.First))
                m_folded.PushBack(m_doc->AddAnchor(CodePosition{r.First, 0}, CodeBias::Right));
        FoldsChanged();
        CaretsOutOfFolds();
    }

    inline void CodeEditor::UnfoldAll()
    {
        for (const CodeAnchorId anchor : m_folded)
            m_doc->RemoveAnchor(anchor);
        m_folded.Clear();
        FoldsChanged();
    }

    inline core::StringView CodeEditor::HoverAt(CodePosition at)
    {
        if (m_service == nullptr || at.Line >= m_doc->LineCount())
            return core::StringView{};
        const core::StringView text = m_doc->LineText(at.Line);
        u32 from = core::Min<u32>(at.Column, static_cast<u32>(text.Size())), to = from;
        while (from > 0 && IsWordByte(text[from - 1]))
            --from;
        while (to < text.Size() && IsWordByte(text[to]))
            ++to;
        const CodeRange word{CodePosition{at.Line, from}, CodePosition{at.Line, to}};
        if (from == to)
            return core::StringView{};
        if (m_hoverVersion != m_doc->GetVersion() || !(m_hoverWord.Start == word.Start) || !(m_hoverWord.End == word.End))
        {
            m_hoverVersion = m_doc->GetVersion();
            m_hoverWord = word;
            m_hoverInfo.Clear();
            if (!m_service->Hover(*m_doc, at, m_hoverInfo))
                m_hoverInfo.Clear();
        }
        return m_hoverInfo.AsView();
    }

    inline void CodeEditor::TypeCharacter(core::StringView text)
    {
        const CodeLanguage& lang = *m_language;
        const CodePair* opens = nullptr;
        const CodePair* closes = nullptr;
        for (const CodePair& pair : lang.AutoClose)
        {
            if (opens == nullptr && pair.Open.AsView() == text)
                opens = &pair;
            if (closes == nullptr && pair.Close.AsView() == text)
                closes = &pair;
        }
        EditEach(CodeEditKind::Typing, [&](const CodeSelection& s, CodeRange& replaced, core::String& put, u32& back)
        {
            replaced = s.Range();
            const CodePosition at = replaced.Start;
            const core::StringView line = m_doc->LineText(at.Line);
            const usize afterAt = replaced.End.Line == at.Line ? replaced.End.Column : line.Size();
            const core::StringView after = line.SubStr(afterAt, line.Size() - afterAt);
            const CodeToken in = TokenBefore(at);
            const bool inText = in == CodeToken::String || in == CodeToken::Comment || in == CodeToken::Escape;
            // Typing the closing character that the pair inserted steps over it.
            if (s.IsEmpty() && closes != nullptr && after.Size() >= text.Size() && after.SubStr(0, text.Size()) == text &&
                !(opens != nullptr && opens->Open == opens->Close && inText && in != CodeToken::String))
            {
                replaced = CodeRange{at, CodePosition{at.Line, at.Column + static_cast<u32>(text.Size())}};
                put = core::String(text);
                return true;
            }
            // An opening character typed over a selection surrounds the selection.
            if (!s.IsEmpty() && opens != nullptr)
            {
                put = core::String(opens->Open);
                put.Append(m_doc->TextIn(replaced).AsView());
                put.Append(opens->Close.AsView());
                back = static_cast<u32>(opens->Close.Size());
                return true;
            }
            // An opening character where its close may follow inserts the pair with the caret
            // between. This does not happen in a string or a comment, nor for a quote right after a
            // name (it's) or right before one.
            if (s.IsEmpty() && opens != nullptr && !inText)
            {
                const bool quote = opens->Open == opens->Close;
                const bool nextFree = after.Size() == 0 || after[0] == u8' ' || after[0] == u8'\t' || after[0] == u8')' ||
                                      after[0] == u8']' || after[0] == u8'}' || after[0] == u8',' || after[0] == u8';';
                const bool afterName = at.Column > 0 && IsNameByte(line[at.Column - 1]);
                if (nextFree && !(quote && afterName))
                {
                    put = core::String(opens->Open);
                    put.Append(opens->Close.AsView());
                    back = static_cast<u32>(opens->Close.Size());
                    return true;
                }
            }
            // A closing word or mark typed first on its line outdents the line to its block's opener.
            core::String before(line.SubStr(0, at.Column));
            before.Append(text);
            const u32 indent = IndentOf(before.AsView());
            const core::StringView first = before.AsView().SubStr(indent, before.Size() - indent);
            if (s.IsEmpty() && !lang.OutdentFirst.IsEmpty() && IsListed(first, lang.OutdentFirst.AsView()) &&
                (after.Size() == 0 || !IsNameByte(after[0])))
            {
                // Take the indent of the nearest line above with text, less one stop unless that line opens
                // the block.
                u32 target = IndentColumns(before.AsView());
                for (u32 up = at.Line; up-- > 0;)
                {
                    const core::StringView above = m_doc->LineText(up);
                    if (IndentOf(above) == above.Size())
                        continue;
                    const u32 aboveIndent = IndentColumns(above);
                    target = EndsWithAny(above, lang.IndentAfter.AsView()) ? aboveIndent : (aboveIndent >= m_tabSize ? aboveIndent - m_tabSize : 0u);
                    break;
                }
                if (target < IndentColumns(before.AsView()))
                {
                    replaced = CodeRange{CodePosition{at.Line, 0}, at};
                    put = IndentText(target);
                    put.Append(first);
                    return true;
                }
            }
            put = core::String(text);
            return true;
        });
    }

    inline void CodeEditor::Newline()
    {
        // Each new line is indented as much as the line it breaks, or one stop more after something
        // that opens a block. Between a bracket pair, the close goes on a line of its own below.
        EditEach(CodeEditKind::Other, [&](const CodeSelection& s, CodeRange& replaced, core::String& put, u32& back)
        {
            replaced = s.Range();
            const core::StringView line = m_doc->LineText(replaced.Start.Line);
            const core::StringView before = line.SubStr(0, replaced.Start.Column);
            const usize afterAt = replaced.End.Line == replaced.Start.Line ? replaced.End.Column : line.Size();
            const core::StringView after = line.SubStr(afterAt, line.Size() - afterAt);
            const core::String indent(line.SubStr(0, core::Min(IndentOf(line), replaced.Start.Column)));
            put = core::String(core::StringView(u8"\n"));
            put.Append(indent.AsView());
            if (m_language == nullptr)
                return true;
            const bool opens = EndsWithAny(before, m_language->IndentAfter.AsView());
            if (!opens)
                return true;
            put.Append(IndentText(m_tabSize).AsView());
            for (const CodePair& pair : m_language->Brackets)
            {
                usize end = before.Size();
                while (end > 0 && (before[end - 1] == u8' ' || before[end - 1] == u8'\t'))
                    --end;
                const bool open = end >= pair.Open.Size() && before.SubStr(end - pair.Open.Size(), pair.Open.Size()) == pair.Open.AsView();
                if (open && after.Size() >= pair.Close.Size() && after.SubStr(0, pair.Close.Size()) == pair.Close.AsView())
                {
                    put.Append(core::StringView(u8"\n"));
                    put.Append(indent.AsView());
                    back = static_cast<u32>(indent.Size() + 1);
                    break;
                }
            }
            return true;
        });
    }

    inline void CodeEditor::Indent()
    {
        // A selection across lines indents those lines; otherwise each caret indents to the next tab
        // stop.
        bool lines = false;
        for (const CodeSelection& sel : m_selections)
            lines = lines || sel.Range().Start.Line != sel.Range().End.Line;
        if (lines)
        {
            IndentLines(false);
            return;
        }
        EditEach(CodeEditKind::Typing, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            replaced = s.Range();
            if (!m_insertSpaces)
            {
                put = core::String(core::StringView(u8"\t"));
                return true;
            }
            const LineView& view = View(replaced.Start.Line);
            const u32 column = ShownColumn(view, replaced.Start.Column);
            put.Clear();
            for (u32 k = m_tabSize - column % m_tabSize; k > 0; --k)
                put.Append(core::StringView(u8" "));
            return true;
        });
    }

    inline void CodeEditor::IndentLines(bool outdent)
    {
        core::Array<u32> firsts(MemoryAllocator()), lasts(MemoryAllocator());
        LineBlocks(firsts, lasts);
        core::Array<Planned> edits(MemoryAllocator());
        for (usize b = 0; b < firsts.Size(); ++b)
            for (u32 line = firsts[b]; line <= lasts[b]; ++line)
            {
                const core::StringView text = m_doc->LineText(line);
                const u32 indent = IndentOf(text);
                if (!outdent)
                {
                    if (indent < text.Size()) // a blank line keeps no indent
                        edits.PushBack(Planned{CodeRange{CodePosition{line, 0}, CodePosition{line, 0}}, IndentText(m_tabSize)});
                    continue;
                }
                // Remove up to one stop's worth of the line's indent.
                u32 columns = 0, bytes = 0;
                while (bytes < indent && columns < m_tabSize)
                {
                    columns = text[bytes] == u8'\t' ? m_tabSize : columns + 1;
                    ++bytes;
                }
                if (bytes > 0)
                    edits.PushBack(Planned{CodeRange{CodePosition{line, 0}, CodePosition{line, bytes}}, core::String{}});
            }
        ApplyPlanned(edits);
    }

    inline void CodeEditor::ToggleComment()
    {
        if (m_language == nullptr)
            return;
        const CodeLanguage& lang = *m_language;
        core::Array<Planned> edits(MemoryAllocator());
        if (lang.LineComment.IsEmpty())
        {
            // The language has only block comments (as markup does). Wrap each selection, or its
            // caret's line, in one; remove it again where a selection already is a block comment.
            const core::StringView open = lang.BlockComment.Open.AsView(), close = lang.BlockComment.Close.AsView();
            if (open.Size() == 0)
                return;
            for (const CodeSelection& sel : m_selections)
            {
                CodeRange r = sel.Range();
                if (r.IsEmpty())
                {
                    const core::StringView text = m_doc->LineText(r.Start.Line);
                    r = CodeRange{CodePosition{r.Start.Line, IndentOf(text)}, CodePosition{r.Start.Line, static_cast<u32>(text.Size())}};
                }
                const core::String inside = m_doc->TextIn(r);
                const core::StringView view = inside.AsView();
                if (view.Size() >= open.Size() + close.Size() && view.SubStr(0, open.Size()) == open && view.SubStr(view.Size() - close.Size(), close.Size()) == close)
                {
                    core::StringView body = view.SubStr(open.Size(), view.Size() - open.Size() - close.Size());
                    if (body.Size() >= 2 && body[0] == u8' ' && body[body.Size() - 1] == u8' ')
                        body = body.SubStr(1, body.Size() - 2);
                    edits.PushBack(Planned{r, core::String(body)});
                }
                else
                {
                    core::String text(open);
                    text.Append(core::StringView(u8" "));
                    text.Append(view);
                    text.Append(core::StringView(u8" "));
                    text.Append(close);
                    edits.PushBack(Planned{r, text});
                }
            }
            ApplyPlanned(edits);
            return;
        }
        const core::StringView mark = lang.LineComment.AsView();
        core::Array<u32> firsts(MemoryAllocator()), lasts(MemoryAllocator());
        LineBlocks(firsts, lasts);
        // Uncomment when every line with text is commented; otherwise comment, at the smallest indent.
        bool all = true;
        u32 least = ~u32(0);
        for (usize b = 0; b < firsts.Size(); ++b)
            for (u32 line = firsts[b]; line <= lasts[b]; ++line)
            {
                const core::StringView text = m_doc->LineText(line);
                const u32 indent = IndentOf(text);
                if (indent == text.Size())
                    continue;
                least = core::Min(least, indent);
                all = all && text.SubStr(indent, core::Min(mark.Size(), text.Size() - indent)) == mark;
            }
        if (least == ~u32(0))
            return; // only blank lines
        for (usize b = 0; b < firsts.Size(); ++b)
            for (u32 line = firsts[b]; line <= lasts[b]; ++line)
            {
                const core::StringView text = m_doc->LineText(line);
                const u32 indent = IndentOf(text);
                if (indent == text.Size())
                    continue;
                if (all)
                {
                    u32 cut = static_cast<u32>(mark.Size());
                    if (indent + cut < text.Size() && text[indent + cut] == u8' ')
                        ++cut;
                    edits.PushBack(Planned{CodeRange{CodePosition{line, indent}, CodePosition{line, indent + cut}}, core::String{}});
                }
                else
                {
                    core::String put(mark);
                    put.Append(core::StringView(u8" "));
                    edits.PushBack(Planned{CodeRange{CodePosition{line, least}, CodePosition{line, least}}, put});
                }
            }
        ApplyPlanned(edits);
    }

    inline void CodeEditor::MoveLines(bool up)
    {
        core::Array<u32> firsts(MemoryAllocator()), lasts(MemoryAllocator());
        LineBlocks(firsts, lasts);
        if (firsts.IsEmpty() || (up && firsts[0] == 0) || (!up && lasts.Back() + 1 >= m_doc->LineCount()))
            return;
        // Swap each block with the line it moves past, as one edit, and move the selections by a
        // line.
        const core::Array<CodeSelection> before = m_selections;
        core::Array<CodeSelection> after = m_selections;
        m_doc->Begin(before.AsSpan());
        for (usize k = 0; k < firsts.Size(); ++k)
        {
            const usize b = up ? k : firsts.Size() - 1 - k;
            const u32 first = firsts[b], last = lasts[b];
            const u32 neighbour = up ? first - 1 : last + 1;
            const CodeRange span{CodePosition{up ? neighbour : first, 0},
                                 CodePosition{up ? last : neighbour, static_cast<u32>(m_doc->LineText(up ? last : neighbour).Size())}};
            const core::String block = m_doc->TextIn(CodeRange{CodePosition{first, 0}, CodePosition{last, static_cast<u32>(m_doc->LineText(last).Size())}});
            const core::String other(m_doc->LineText(neighbour));
            core::String put = up ? block : other;
            put.Append(core::StringView(u8"\n"));
            put.Append(up ? other.AsView() : block.AsView());
            (void)m_doc->Replace(span, put.AsView());
            for (CodeSelection& sel : after)
                if (sel.Range().Start.Line >= first && sel.Range().End.Line <= last + 1)
                {
                    const auto shift = [up](CodePosition p) { return CodePosition{up ? p.Line - 1 : p.Line + 1, p.Column}; };
                    sel = CodeSelection{shift(sel.Anchor), shift(sel.Head)};
                }
        }
        m_doc->End(after.AsSpan());
        SetSelections(after.AsSpan(), m_primary);
    }

    inline void CodeEditor::DuplicateLines(bool up)
    {
        core::Array<u32> firsts(MemoryAllocator()), lasts(MemoryAllocator());
        LineBlocks(firsts, lasts);
        const core::Array<CodeSelection> before = m_selections;
        core::Array<CodeSelection> after = m_selections;
        m_doc->Begin(before.AsSpan());
        for (usize k = firsts.Size(); k-- > 0;)
        {
            const u32 first = firsts[k], last = lasts[k];
            const u32 count = last - first + 1;
            core::String block = m_doc->TextIn(CodeRange{CodePosition{first, 0}, CodePosition{last, static_cast<u32>(m_doc->LineText(last).Size())}});
            // Put the copy after the block's end. Going down, the selections move to the copy; going up,
            // they stay on the original.
            core::String put(core::StringView(u8"\n"));
            put.Append(block.AsView());
            (void)m_doc->Insert(CodePosition{last, static_cast<u32>(m_doc->LineText(last).Size())}, put.AsView());
            for (CodeSelection& sel : after)
            {
                const bool inside = sel.Range().Start.Line >= first && sel.Range().End.Line <= last;
                if (inside && !up)
                    sel = CodeSelection{CodePosition{sel.Anchor.Line + count, sel.Anchor.Column}, CodePosition{sel.Head.Line + count, sel.Head.Column}};
                else if (!inside && sel.Range().Start.Line > last)
                    sel = CodeSelection{CodePosition{sel.Anchor.Line + count, sel.Anchor.Column}, CodePosition{sel.Head.Line + count, sel.Head.Column}};
            }
        }
        m_doc->End(after.AsSpan());
        SetSelections(after.AsSpan(), m_primary);
    }

    inline void CodeEditor::DeleteLines()
    {
        core::Array<u32> firsts(MemoryAllocator()), lasts(MemoryAllocator());
        LineBlocks(firsts, lasts);
        core::Array<Planned> edits(MemoryAllocator());
        for (usize b = 0; b < firsts.Size(); ++b)
        {
            const u32 first = firsts[b], last = lasts[b];
            if (last + 1 < m_doc->LineCount())
                edits.PushBack(Planned{CodeRange{CodePosition{first, 0}, CodePosition{last + 1, 0}}, core::String{}});
            else if (first > 0)
                edits.PushBack(Planned{CodeRange{CodePosition{first - 1, static_cast<u32>(m_doc->LineText(first - 1).Size())},
                                                 CodePosition{last, static_cast<u32>(m_doc->LineText(last).Size())}},
                                       core::String{}});
            else
                edits.PushBack(Planned{CodeRange{CodePosition{0, 0}, m_doc->End()}, core::String{}});
        }
        ApplyPlanned(edits);
    }

    inline bool CodeEditor::AddNextOccurrence()
    {
        CodeSelection primary = GetPrimary();
        if (primary.IsEmpty())
        {
            // First, select the word at the caret.
            const TextRange word = WordRangeAt(m_doc->LineText(primary.Head.Line), primary.Head.Column);
            if (word.Start == word.End)
                return false;
            const CodeSelection one{CodePosition{primary.Head.Line, static_cast<u32>(word.Start)}, CodePosition{primary.Head.Line, static_cast<u32>(word.End)}};
            core::Array<CodeSelection> next = m_selections;
            next[m_primary] = one;
            SetSelections(next.AsSpan(), m_primary);
            return true;
        }
        const core::String needle = m_doc->TextIn(primary.Range());
        // Search from the last selection's end, wrapping to the start, and skip matches already
        // selected.
        CodePosition from = m_selections.Back().Range().End;
        for (usize tries = 0; tries <= m_selections.Size(); ++tries)
        {
            CodeRange found;
            if (!m_doc->Find(needle.AsView(), from, true, false, true, found))
                return false;
            bool taken = false;
            for (const CodeSelection& sel : m_selections)
                taken = taken || sel.Range() == found;
            if (!taken)
            {
                m_selections.PushBack(CodeSelection{found.Start, found.End});
                m_primary = m_selections.Size() - 1;
                Normalize();
                ResetGoals();
                SelectionsChanged();
                return true;
            }
            from = found.End;
        }
        return false;
    }

    inline void CodeEditor::SelectAllOccurrences()
    {
        if (GetPrimary().IsEmpty() && !AddNextOccurrence())
            return;
        const core::String needle = m_doc->TextIn(GetPrimary().Range());
        core::Array<CodeSelection> all(MemoryAllocator());
        CodePosition from{0, 0};
        CodeRange found;
        while (m_doc->Find(needle.AsView(), from, true, false, false, found))
        {
            all.PushBack(CodeSelection{found.Start, found.End});
            from = found.End == found.Start ? m_doc->NextCharacter(found.End) : found.End;
            if (found.End == m_doc->End())
                break;
        }
        if (!all.IsEmpty())
            SetSelections(all.AsSpan());
    }

    inline void CodeEditor::AddCursorVertically(bool up)
    {
        f32 goal = m_goals[m_primary];
        const CodePosition from = GetPrimary().Head;
        if ((up && from.Line == 0) || (!up && from.Line + 1 >= m_doc->LineCount()))
            return;
        const CodePosition at = Vertical(from, up ? -1 : 1, goal);
        m_selections.PushBack(CodeSelection{at, at});
        m_primary = m_selections.Size() - 1;
        Normalize();
        ResetGoals();
        m_goals[m_primary] = goal;
        SelectionsChanged();
    }

    inline bool CodeEditor::FindNext(bool backwards)
    {
        RefreshMatches();
        if (m_matches.IsEmpty())
        {
            UpdateCount();
            return false;
        }
        const CodeRange from = GetPrimary().Range();
        usize pick = 0;
        if (!backwards)
        {
            pick = 0; // wrap to the first when none follows
            for (usize i = 0; i < m_matches.Size(); ++i)
                if (from.End <= m_matches[i].Start && !(m_matches[i] == from))
                {
                    pick = i;
                    break;
                }
        }
        else
        {
            pick = m_matches.Size() - 1; // wrap to the last when none comes before
            for (usize i = m_matches.Size(); i-- > 0;)
                if (m_matches[i].End <= from.Start && !(m_matches[i] == from))
                {
                    pick = i;
                    break;
                }
        }
        const CodeSelection one{m_matches[pick].Start, m_matches[pick].End};
        SetSelections(core::Span<const CodeSelection>(&one, 1));
        UpdateCount();
        return true;
    }

    inline void CodeEditor::ReplaceCurrent()
    {
        if (m_readOnly || CurrentMatch() == 0)
        {
            (void)FindNext(false);
            return;
        }
        core::Array<Planned> edits(MemoryAllocator());
        edits.PushBack(Planned{GetPrimary().Range(), core::String(m_findBar->GetReplacement())});
        ApplyPlanned(edits);
        (void)FindNext(false);
    }

    inline void CodeEditor::ReplaceAll()
    {
        RefreshMatches();
        if (m_readOnly || m_matches.IsEmpty())
            return;
        core::Array<Planned> edits(MemoryAllocator());
        for (const CodeRange& r : m_matches)
            edits.PushBack(Planned{r, core::String(m_findBar->GetReplacement())});
        ApplyPlanned(edits);
        UpdateCount();
    }

    inline void CodeEditor::GoToLine(core::StringView text)
    {
        // Parse `line` or `line:column`, counting from 1.
        u32 line = 0, column = 0;
        usize at = 0;
        while (at < text.Size() && text[at] >= u8'0' && text[at] <= u8'9')
            line = line * 10u + static_cast<u32>(text[at++] - u8'0');
        if (at < text.Size() && text[at] == u8':')
            for (++at; at < text.Size() && text[at] >= u8'0' && text[at] <= u8'9'; ++at)
                column = column * 10u + static_cast<u32>(text[at] - u8'0');
        if (line == 0)
            return;
        m_findBar->Close();
        SetCaret(CodePosition{line - 1, column > 0 ? column - 1 : 0u});
        RequestFocus();
    }

    inline bool CodeEditor::FindMatchingBracket(CodePosition at, CodeRange& bracket, CodeRange& match) const
    {
        if (m_language == nullptr || m_language->Brackets.IsEmpty())
            return false;
        at = m_doc->Clamp(at);
        const auto skip = [](CodeToken t) { return t == CodeToken::String || t == CodeToken::Comment || t == CodeToken::Escape; };
        // Try the bracket after the caret, then the one before it.
        for (int side = 0; side < 2; ++side)
        {
            const core::StringView line = m_doc->LineText(at.Line);
            if (side == 1 && at.Column == 0)
                break;
            for (const CodePair& pair : m_language->Brackets)
                for (int which = 0; which < 2; ++which)
                {
                    const core::StringView mark = which == 0 ? pair.Open.AsView() : pair.Close.AsView();
                    const u32 column = side == 0 ? at.Column : at.Column - static_cast<u32>(core::Min<usize>(mark.Size(), at.Column));
                    if (mark.Size() == 0 || column + mark.Size() > line.Size() || line.SubStr(column, mark.Size()) != mark ||
                        skip(TokenAtColumn(at.Line, column)))
                        continue;
                    bracket = CodeRange{CodePosition{at.Line, column}, CodePosition{at.Line, column + static_cast<u32>(mark.Size())}};
                    // Scan outward from it, counting brackets of its kind and skipping strings and comments, for
                    // a few thousand lines at most.
                    const core::StringView open = pair.Open.AsView(), close = pair.Close.AsView();
                    const bool forward = which == 0;
                    i32 depth = 0;
                    u32 l = at.Line;
                    i64 c = forward ? column : static_cast<i64>(column);
                    for (u32 seen = 0; seen < 4000; ++seen)
                    {
                        const core::StringView text = m_doc->LineText(l);
                        while (forward ? c < static_cast<i64>(text.Size()) : c >= 0)
                        {
                            const u32 col = static_cast<u32>(c);
                            const bool isOpen = col + open.Size() <= text.Size() && text.SubStr(col, open.Size()) == open;
                            const bool isClose = col + close.Size() <= text.Size() && text.SubStr(col, close.Size()) == close;
                            if ((isOpen || isClose) && !skip(TokenAtColumn(l, col)))
                            {
                                depth += (isOpen == forward) ? 1 : -1;
                                if (depth == 0)
                                {
                                    const u32 size = static_cast<u32>(isOpen ? open.Size() : close.Size());
                                    match = CodeRange{CodePosition{l, col}, CodePosition{l, col + size}};
                                    return true;
                                }
                            }
                            c += forward ? 1 : -1;
                        }
                        if (forward ? l + 1 >= m_doc->LineCount() : l == 0)
                            return false;
                        l = forward ? l + 1 : l - 1;
                        c = forward ? 0 : static_cast<i64>(m_doc->LineText(l).Size()) - 1;
                    }
                    return false;
                }
        }
        return false;
    }

    inline void CodeEditor::Copy(bool cut)
    {
        IClipboard* clipboard = Clipboard();
        bool any = false;
        for (const CodeSelection& s : m_selections)
            any = any || !s.IsEmpty();
        core::String text;
        for (usize i = 0; i < m_selections.Size(); ++i)
        {
            const CodeSelection& s = m_selections[i];
            if (any && s.IsEmpty())
                continue;
            if (!text.IsEmpty())
                text.Append(core::StringView(u8"\n"));
            if (any)
                text.Append(m_doc->TextIn(s.Range()).AsView());
            else // no selection: the caret's whole line, with its line feed
            {
                text.Append(m_doc->LineText(s.Head.Line));
                text.Append(core::StringView(u8"\n"));
            }
        }
        if (clipboard != nullptr)
            clipboard->SetText(text.AsView());
        if (!cut || m_readOnly)
            return;
        EditEach(CodeEditKind::Other, [&](const CodeSelection& s, CodeRange& replaced, core::String&)
        {
            if (any)
                replaced = s.Range();
            else if (s.Head.Line + 1 < m_doc->LineCount())
                replaced = CodeRange{CodePosition{s.Head.Line, 0}, CodePosition{s.Head.Line + 1, 0}};
            else
                replaced = CodeRange{CodePosition{s.Head.Line, 0}, CodePosition{s.Head.Line, static_cast<u32>(m_doc->LineText(s.Head.Line).Size())}};
            return !replaced.IsEmpty();
        });
    }

    inline void CodeEditor::Paste()
    {
        IClipboard* clipboard = Clipboard();
        if (clipboard == nullptr || !clipboard->HasText() || m_readOnly)
            return;
        const core::String text = clipboard->GetText();
        // When the clipboard has as many lines as there are carets, paste one line at each caret, in
        // order (copying from several carets and pasting into as many).
        core::Array<core::String> parts(MemoryAllocator());
        {
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
                if (i == text.Size() || text.AsView()[i] == u8'\n')
                {
                    parts.PushBack(core::String(text.AsView().SubStr(start, i - start)));
                    start = i + 1;
                }
        }
        const bool spread = m_selections.Size() > 1 && parts.Size() == m_selections.Size();
        usize index = m_selections.Size();
        EditEach(CodeEditKind::Other, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            --index; // EditEach goes from the last selection
            replaced = s.Range();
            put = spread ? parts[index] : text;
            return true;
        });
    }

    inline void CodeEditor::OnKeyDown(const KeyEvent& event)
    {
        if (!IsTreeEnabled() || event.Target != this)
            return;
        const KeyCode key = static_cast<KeyCode>(event.KeyCode);
        const u32 mods = event.Modifiers & (KeyModCtrl | KeyModAlt | KeyModSuper | KeyModShift);
        if (m_completing && key == KeyCode::Tab && mods == 0)
        {
            (void)AcceptCompletion(); // Tab accepts as Return does; a captured Tab never reaches the keymap
            event.Handled = true;
            return;
        }
        if (key == KeyCode::Escape && mods == 0 && m_selections.Size() > 1 && !m_completing)
        {
            const CodeSelection primary = GetPrimary();
            SetSelections(core::Span<const CodeSelection>(&primary, 1));
            event.Handled = true;
            return;
        }
        if (key == KeyCode::Tab && (mods == 0 || mods == KeyModShift) && !m_readOnly)
        {
            if (mods == KeyModShift)
                IndentLines(true);
            else
                Indent();
            event.Handled = true;
            return;
        }
        // The editor keeps a key that types text; it neither activates anything nor moves focus.
        if ((mods & (KeyModCtrl | KeyModAlt | KeyModSuper)) == 0 && (key == KeyCode::Space || (key >= KeyCode::A && key <= KeyCode::Num9)))
            event.Handled = true;
    }

    inline void CodeEditor::OnMouseDown(const MouseEvent& event)
    {
        if (!IsTreeEnabled() || event.Button != MouseButton::Left || FromCompletion(event))
            return;
        event.Handled = true;
        CloseCompletion();
        RequestFocus();
        const core::Float2 local = ConvertToNodeSpace(event.Position);
        const CodePosition at = PositionAt(local);
        // A click in the marker column goes to the host (to toggle a breakpoint), and the caret stays.
        const Rect content = GetContentBounds();
        if (local.x >= content.x && local.x < content.x + kMarkerColumn)
        {
            if (m_onGutterClick)
                m_onGutterClick(at.Line);
            return;
        }
        // In the fold column, a chevron folds or unfolds its range.
        const f32 foldX = content.x + GutterWidth() - kTextGap - kFoldColumn;
        if (local.x >= foldX && local.x < foldX + kFoldColumn)
        {
            (void)ToggleFold(at.Line);
            return;
        }
        // Clicking a folded line's placeholder unfolds it.
        if (IsFolded(at.Line) && PlaceholderRect(at.Line).Contains(local))
        {
            (void)Unfold(at.Line);
            return;
        }
        m_dragUnit = event.ClickCount >= 3 ? 3u : event.ClickCount == 2 ? 2u : 1u;
        CodeRange unit{at, at};
        if (m_dragUnit == 2)
        {
            const TextRange word = WordRangeAt(m_doc->LineText(at.Line), at.Column);
            unit = CodeRange{CodePosition{at.Line, static_cast<u32>(word.Start)}, CodePosition{at.Line, static_cast<u32>(word.End)}};
        }
        else if (m_dragUnit == 3)
            unit = CodeRange{CodePosition{at.Line, 0},
                             at.Line + 1 < m_doc->LineCount() ? CodePosition{at.Line + 1, 0} : m_doc->End()};
        m_dragAdds = (event.Modifiers & KeyModAlt) != 0;
        m_doc->BreakMerge();
        if ((event.Modifiers & KeyModShift) != 0 && m_dragUnit == 1)
        {
            CodeSelection s = GetPrimary();
            s.Head = at;
            m_dragOrigin = CodeRange{s.Anchor, s.Anchor};
            const CodeSelection one = s;
            SetSelections(core::Span<const CodeSelection>(&one, 1));
        }
        else if (m_dragAdds)
        {
            m_columnX = local.x - TextLeft() + ScrollShift().x;
            m_columnLine = at.Line;
            m_selections.PushBack(CodeSelection{unit.Start, unit.End});
            m_primary = m_selections.Size() - 1;
            Normalize();
            ResetGoals();
            SelectionsChanged();
            m_dragOrigin = unit;
        }
        else
        {
            const CodeSelection one{unit.Start, unit.End};
            SetSelections(core::Span<const CodeSelection>(&one, 1));
            m_dragOrigin = unit;
        }
        m_dragging = true;
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->CapturePointer(*this);
    }

    inline void CodeEditor::OnMouseMove(const MouseEvent& event)
    {
        if (FromCompletion(event))
            return;
        if (!m_dragging)
        {
            // Hovering builds the tooltip: the diagnostics under the pointer (or on its line, in the
            // gutter), then, over a word, the service's text for it.
            const core::Float2 hover = ConvertToNodeSpace(event.Position);
            const CodePosition over = PositionAt(hover);
            if (const bool gutter = hover.x < TextLeft(); gutter != m_gutterHover)
            {
                m_gutterHover = gutter; // shows the folds' chevrons
                Invalidate();
            }
            core::String tip;
            if (hover.x < TextLeft())
            {
                for (const Diagnostic& d : m_diagnostics)
                    if (m_doc->GetAnchor(d.Start).Line <= over.Line && over.Line <= m_doc->GetAnchor(d.End).Line)
                    {
                        if (!tip.IsEmpty())
                            tip.Append(core::StringView(u8"\n"));
                        tip.Append(d.Message.AsView());
                    }
            }
            else
            {
                tip = DiagnosticsAt(over);
                // Only over the word itself, not the space past a line's end.
                const f32 x = hover.x - TextLeft() + ScrollShift().x;
                if (m_service != nullptr && over.Line < m_doc->LineCount() && x <= XOf(m_doc->Clamp(CodePosition{over.Line, ~0u})))
                {
                    const core::StringView info = HoverAt(over);
                    if (info.Size() != 0)
                    {
                        if (!tip.IsEmpty())
                            tip.Append(core::StringView(u8"\n"));
                        tip.Append(info);
                    }
                }
            }
            m_hoverTip = core::Move(tip);
            return;
        }
        const core::Float2 local = ConvertToNodeSpace(event.Position);
        const CodePosition at = PositionAt(local);
        if (m_dragAdds && m_dragUnit == 1 && at.Line != m_columnLine)
        {
            // Alt+drag across lines makes a box selection: it replaces the other selections with
            // one selection on each line between, spanning the same two x positions.
            const f32 x = local.x - TextLeft() + ScrollShift().x;
            core::Array<CodeSelection> next(MemoryAllocator());
            const u32 from = core::Min(m_columnLine, at.Line), to = core::Max(m_columnLine, at.Line);
            for (u32 line = from; line <= to; ++line)
                if (!IsLineHidden(line))
                    next.PushBack(CodeSelection{CodePosition{line, ColumnAtX(line, m_columnX)}, CodePosition{line, ColumnAtX(line, x)}});
            m_selections = core::Move(next);
            m_primary = m_selections.Size() - 1;
            Normalize();
            ResetGoals();
            SelectionsChanged();
            return;
        }
        // Extend from what the press selected to what the pointer is on, by the press's unit.
        CodeRange unit{at, at};
        if (m_dragUnit == 2)
        {
            const TextRange word = WordRangeAt(m_doc->LineText(at.Line), at.Column);
            unit = CodeRange{CodePosition{at.Line, static_cast<u32>(word.Start)}, CodePosition{at.Line, static_cast<u32>(word.End)}};
        }
        else if (m_dragUnit == 3)
            unit = CodeRange{CodePosition{at.Line, 0}, at.Line + 1 < m_doc->LineCount() ? CodePosition{at.Line + 1, 0} : m_doc->End()};
        CodeSelection& s = m_selections[m_primary];
        s = at < m_dragOrigin.Start ? CodeSelection{m_dragOrigin.End, unit.Start} : CodeSelection{m_dragOrigin.Start, unit.End};
        Normalize();
        ResetGoals();
        SelectionsChanged();
    }

    inline void CodeEditor::OnMouseUp(const MouseEvent& event)
    {
        if (!m_dragging || event.Button != MouseButton::Left)
            return;
        m_dragging = false;
        if (EventDispatcher* dispatcher = GetEventDispatcher())
            dispatcher->ReleasePointer(*this);
    }

    inline void CodeEditor::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        UIWidget::OnDraw(ctx, localBounds);
        const Rect content = GetContentBounds();
        const core::Float2 shift = ScrollShift();
        const f32 lh = LineHeight();
        const f32 gutter = GutterWidth();
        const Rect textArea{content.x + gutter, content.y, core::Max(0.0f, content.width - gutter), content.height};
        const auto partColor = [this](const char8_t* name, Color fallback)
        {
            const PartStyle& part = GetPartStyle(name);
            Color color = part.HasBackground ? part.Box.BackgroundColor : fallback;
            color.a *= part.Opacity;
            return color;
        };
        const Color gutterColor = partColor(u8"gutter", Color{0.0f, 0.0f, 0.0f, 0.0f});
        const auto textColor = [this](const char8_t* name, Color fallback)
        {
            const PartStyle& part = GetPartStyle(name);
            return part.HasColor ? part.TextColor : fallback;
        };
        const Color numberColor = textColor(u8"line-number", Color{0.5f, 0.53f, 0.6f, 1.0f});
        const Color currentNumber = textColor(u8"current-line-number", GetTextColor());
        const Color currentLine = partColor(u8"current-line", Color{0.0f, 0.0f, 0.0f, 0.0f}); // from the sheet (none by default)
        const Color selectionColor = partColor(u8"selection", Color{0.25f, 0.45f, 0.8f, 0.45f});
        const Color caretColor = partColor(u8"caret", GetTextColor());

        u32 first = 0, last = 0;
        VisibleLines(first, last);
        const CodePosition primary = m_doc->Clamp(GetPrimary().Head);
        const auto rowTop = [&](u32 line) { return content.y + static_cast<f32>(RowOfLine(line)) * lh - shift.y; };
        const auto topOfRow = [&](u32 row) { return content.y + static_cast<f32>(row) * lh - shift.y; };
        const auto lineEnd = [&](u32 line) { return static_cast<u32>(m_doc->LineText(line).Size()); };
        const f32 textX = textArea.x - shift.x;

        ctx.PushClip(content);
        // Highlight the primary caret's line across the whole view.
        if (GetPrimary().IsEmpty() && primary.Line >= first && primary.Line <= last && currentLine.a > 0.0f)
            ctx.VG().FillRect(Rectangle{content.x, rowTop(primary.Line), content.width, lh * static_cast<f32>(RowsOf(primary.Line))}, currentLine);
        // Highlight the line the debugger is stopped on, across the view.
        for (const Marker& marker : m_markers)
            if (marker.Kind.AsView() == core::StringView(u8"execution"))
            {
                const u32 line = m_doc->GetAnchor(marker.Anchor).Line;
                if (line >= first && line <= last && !IsLineHidden(line))
                    ctx.VG().FillRect(Rectangle{content.x, rowTop(line), content.width, lh * static_cast<f32>(RowsOf(line))},
                                      partColor(u8"execution-line", Color{0.9f, 0.75f, 0.2f, 0.15f}));
            }
        ctx.PushClip(textArea);
        // The find bar's matches in view, with the current one drawn stronger.
        if (m_findBar->IsDisplayed() && m_findBar->GetMode() != CodeFindBar::Mode::GoToLine)
        {
            RefreshMatches();
            const Color matchColor = partColor(u8"find-match", Color{0.85f, 0.65f, 0.2f, 0.3f});
            const Color currentColor = partColor(u8"find-current", Color{0.95f, 0.7f, 0.2f, 0.6f});
            const CodeRange current = GetPrimary().Range();
            for (const CodeRange& r : m_matches)
            {
                if (r.End.Line < first)
                    continue;
                if (r.Start.Line > last)
                    break;
                for (u32 line = core::Max(r.Start.Line, first); line <= core::Min(r.End.Line, last); ++line)
                {
                    if (IsLineHidden(line))
                        continue;
                    ForEachRun(line, line == r.Start.Line ? r.Start.Column : 0, line == r.End.Line ? r.End.Column : lineEnd(line), false,
                               [&](f32 x0, f32 x1, u32 row)
                               {
                                   ctx.VG().FillRect(Rectangle{textX + x0, topOfRow(row), core::Max(1.0f, x1 - x0), lh}, r == current ? currentColor : matchColor);
                               });
                }
            }
        }
        // Draw the selections, then the text over them.
        for (const CodeSelection& s : m_selections)
        {
            if (s.IsEmpty())
                continue;
            const CodeRange r = s.Range();
            for (u32 line = core::Max(r.Start.Line, first); line <= core::Min(r.End.Line, last); ++line)
            {
                if (IsLineHidden(line))
                    continue;
                ForEachRun(line, line == r.Start.Line ? r.Start.Column : 0, line == r.End.Line ? r.End.Column : lineEnd(line), line != r.End.Line,
                           [&](f32 x0, f32 x1, u32 row)
                           {
                               ctx.VG().FillRect(Rectangle{textX + x0, topOfRow(row), core::Max(0.0f, x1 - x0), lh}, selectionColor);
                           });
            }
        }
        // Mark the bracket by the primary caret and its match.
        {
            CodeRange bracket, match;
            if (GetPrimary().IsEmpty() && FindMatchingBracket(primary, bracket, match))
            {
                const Color mark = partColor(u8"bracket-match", Color{0.5f, 0.6f, 0.75f, 0.35f});
                const CodeRange both[] = {bracket, match};
                for (const CodeRange& r : both)
                    if (r.Start.Line >= first && r.Start.Line <= last && !IsLineHidden(r.Start.Line))
                        ForEachRun(r.Start.Line, r.Start.Column, r.End.Column, false, [&](f32 x0, f32 x1, u32 row)
                        {
                            ctx.VG().FillRect(Rectangle{textX + x0, topOfRow(row), x1 - x0, lh}, mark);
                        });
            }
        }
        for (u32 line = first; line <= last; ++line)
        {
            if (IsLineHidden(line))
                continue;
            LineView& view = View(line);
            const TextLayout& layout = LayoutOf(view);
            if (m_whitespace != CodeWhitespace::None)
                DrawWhitespace(ctx, line, view, layout, textX, rowTop(line), textColor(u8"whitespace", numberColor));
            // Draw the line's rows from its first; each row is a line height, which matches the layout's
            // own because it uses the same font.
            const f32 lead = layout.Lines.IsEmpty() ? 0.0f : (lh - layout.Lines[0].Height) * 0.5f;
            view.Glyphs.DrawLayout(ctx, layout, core::Float2{textX, rowTop(line) + lead});
            // Draw a folded line's placeholder after its text.
            if (IsFolded(line))
            {
                const Rect box = PlaceholderRect(line);
                ctx.VG().FillRoundedRect(Rectangle{box.x, box.y, box.width, box.height}, 3.0f,
                                         partColor(u8"fold-placeholder", Color{0.5f, 0.55f, 0.65f, 0.25f}));
                m_foldText.SetColor(numberColor);
                const TextLayout& dots = m_foldText.LayoutFor(Rect{});
                m_foldText.DrawLayout(ctx, dots, core::Float2{box.x + (box.width - dots.Width) * 0.5f, box.y + (box.height - dots.Height) * 0.5f});
            }
        }
        // The diagnostics: a wave under each run, with more severe ones drawn over less severe ones.
        for (u32 pass = 0; pass < 4; ++pass)
            for (const Diagnostic& d : m_diagnostics)
            {
                if (static_cast<u32>(d.Severity) != pass)
                    continue;
                const CodeRange r = MarkedRange(d);
                if (r.End.Line < first || r.Start.Line > last)
                    continue;
                const Color color = partColor(CodeSeverityName(d.Severity).Data(), d.Severity == CodeSeverity::Error ? Color{0.95f, 0.33f, 0.4f, 1.0f}
                                                                                     : Color{0.95f, 0.8f, 0.4f, 1.0f});
                for (u32 line = core::Max(r.Start.Line, first); line <= core::Min(r.End.Line, last); ++line)
                {
                    if (IsLineHidden(line))
                        continue;
                    ForEachRun(line, line == r.Start.Line ? r.Start.Column : 0, line == r.End.Line ? r.End.Column : lineEnd(line), false,
                               [&](f32 left, f32 right, u32 row)
                               {
                                   const f32 x0 = textX + left, x1 = textX + right;
                                   const f32 y = topOfRow(row) + lh - 2.0f;
                                   vg::PathBuilder wave;
                                   wave.MoveTo(x0, y);
                                   bool up = true;
                                   for (f32 x = x0 + 2.0f; x <= core::Max(x1, x0 + 4.0f); x += 2.0f, up = !up)
                                       wave.LineTo(x, up ? y - 1.5f : y);
                                   ctx.VG().StrokePath(wave.ToPath(), color, vg::StrokeStyle(1.0f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
                               });
                }
            }
        // An IME's composition at the primary caret, underlined.
        if (!m_composition.IsEmpty() && IsFocused())
        {
            const Rect caret = CaretRect(primary);
            const f32 width = ComposedWidth();
            ctx.VG().FillRect(Rectangle{caret.x, caret.y, width, lh}, partColor(u8"composition", Color{0.15f, 0.17f, 0.22f, 1.0f}));
            m_compositionText.DrawLayout(ctx, m_compositionText.LayoutFor(Rect{}), core::Float2{caret.x, caret.y});
            ctx.VG().FillRect(Rectangle{caret.x, caret.y + lh - 1.5f, width, 1.0f}, GetTextColor());
        }
        // The carets.
        if (IsFocused() && m_caretShown)
            for (const CodeSelection& s : m_selections)
            {
                const CodePosition at = m_doc->Clamp(s.Head);
                if (at.Line < first || at.Line > last || IsLineHidden(at.Line))
                    continue;
                const Place place = PlaceOf(at);
                ctx.VG().FillRect(Rectangle{textX + place.X + (at == primary ? ComposedWidth() : 0.0f), topOfRow(place.Row), 1.5f, lh}, caretColor);
            }
        ctx.PopClip();
        // The gutter does not scroll across, and its numbers are right aligned.
        if (gutterColor.a > 0.0f)
            ctx.VG().FillRect(Rectangle{content.x, content.y, gutter, content.height}, gutterColor);
        // The marker column: a line's worst diagnostic, then its marks over it, with an execution
        // arrow last.
        for (u32 line = first; line <= last; ++line)
        {
            if (IsLineHidden(line))
                continue;
            const f32 cx = content.x + kMarkerColumn * 0.5f, cy = rowTop(line) + lh * 0.5f;
            const f32 radius = core::Min(lh, kMarkerColumn) * 0.28f;
            CodeSeverity worst = CodeSeverity::Hint;
            if (WorstOn(line, worst) && worst >= CodeSeverity::Info)
                ctx.VG().FillCircle(core::Float2{cx, cy}, radius * 0.75f, partColor(CodeSeverityName(worst).Data(), Color{0.95f, 0.33f, 0.4f, 1.0f}));
            for (int layer = 0; layer < 2; ++layer)
                for (const Marker& marker : m_markers)
                {
                    if (m_doc->GetAnchor(marker.Anchor).Line != line)
                        continue;
                    const core::StringView kind = marker.Kind.AsView();
                    const bool execution = kind == core::StringView(u8"execution");
                    if (execution != (layer == 1))
                        continue;
                    if (kind == core::StringView(u8"breakpoint"))
                        ctx.VG().FillCircle(core::Float2{cx, cy}, radius, partColor(u8"breakpoint", Color{0.9f, 0.33f, 0.3f, 1.0f}));
                    else if (execution)
                    {
                        vg::PathBuilder arrow;
                        arrow.MoveTo(cx - radius, cy - radius);
                        arrow.LineTo(cx + radius, cy);
                        arrow.LineTo(cx - radius, cy + radius);
                        arrow.Close();
                        ctx.VG().FillPath(arrow.ToPath(), partColor(u8"execution", Color{0.95f, 0.8f, 0.3f, 1.0f}));
                    }
                    else if (kind == core::StringView(u8"bookmark"))
                        ctx.VG().FillRect(Rectangle{cx - radius * 0.6f, cy - radius, radius * 1.2f, radius * 2.0f}, partColor(u8"bookmark", Color{0.5f, 0.67f, 1.0f, 1.0f}));
                    else
                        ctx.VG().FillRect(Rectangle{cx - radius * 0.7f, cy - radius * 0.7f, radius * 1.4f, radius * 1.4f}, partColor(u8"marker", Color{0.6f, 0.65f, 0.75f, 1.0f}));
                }
        }
        // The fold column always shows a folded range's chevron. An open range's chevron shows only
        // while the pointer is over the gutter, and is worked out only then.
        const bool chevrons = m_gutterHover || !m_folded.IsEmpty();
        const Color foldColor = textColor(u8"fold", numberColor);
        for (u32 line = first; line <= last && chevrons; ++line)
        {
            if (IsLineHidden(line) || RangeStarting(line) == nullptr)
                continue;
            const bool folded = IsFolded(line);
            if (!folded && !m_gutterHover)
                continue;
            const f32 cx = content.x + gutter - kTextGap - kFoldColumn * 0.5f, cy = rowTop(line) + lh * 0.5f;
            const f32 r = core::Min(lh, kFoldColumn) * 0.2f;
            vg::PathBuilder chevron;
            if (folded)
            {
                chevron.MoveTo(cx - r * 0.5f, cy - r);
                chevron.LineTo(cx + r * 0.5f, cy);
                chevron.LineTo(cx - r * 0.5f, cy + r);
            }
            else
            {
                chevron.MoveTo(cx - r, cy - r * 0.5f);
                chevron.LineTo(cx, cy + r * 0.5f);
                chevron.LineTo(cx + r, cy - r * 0.5f);
            }
            ctx.VG().StrokePath(chevron.ToPath(), foldColor, vg::StrokeStyle(1.25f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        }
        for (u32 line = first; line <= last; ++line)
        {
            if (IsLineHidden(line))
                continue;
            LineView& view = View(line);
            if (view.NumberOf != line)
            {
                view.NumberOf = line;
                if (m_styleText)
                    m_styleText(view.Number);
                view.Number.SetString(core::Format(u8"{}", line + 1).AsView());
            }
            view.Number.SetColor(line == primary.Line ? currentNumber : numberColor);
            const TextLayout& layout = view.Number.LayoutFor(Rect{});
            view.Number.DrawLayout(ctx, layout, core::Float2{content.x + gutter - kTextGap - kFoldColumn - kGutterPad - layout.Width, rowTop(line) + (lh - layout.Height) * 0.5f});
        }
        ctx.PopClip();
    }
}

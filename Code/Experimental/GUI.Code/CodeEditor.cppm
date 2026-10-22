// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - :code_editor partition
//
// The code editor's view (C1 of Specs/gui-lab-evolution-3.md) over a CodeDocument (its own, or one
// its host shares). Each line in view is shaped with the lab's text and kept until its text changes
// (foundation.ui's editor drew every token each frame, fixed pitch, so tabs and wide characters
// misaligned); tabs go to tab stops in what is shown, and the caret and the mouse go by the shaped
// places, so any font serves. Code runs left to right in every language, a string's Arabic still
// shaped. Only the rows in view are drawn; the editor is a scroll container over its lines, the
// core's bars and wheel scrolling it.
//
// Several selections from the start, sorted and apart (zed's): a click places one caret, Shift
// extends it, Alt adds one; a double click selects a word, a triple a line, a drag extends by what
// the press selected. The keys are the keymap's text commands (the "textinput" context it declares:
// the arrows, Home and End, the word and page moves, deleting, undo, the clipboard), each run at
// every caret; typing goes in at each, one undo step that puts every caret back; Return keeps the
// line's indent; Tab indents; Escape leaves one caret. The gutter numbers the lines.
//
// Given a language (C2), each line's tokens are kept with the state it began in and its hash, and a
// line is tokenized again only where it changed or the line before now ends otherwise; tokenizing
// runs to the last line in view. Each token draws in its part's colour (`codeeditor::keyword {
// color: ... }`), so a theme restyles code.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.code:code_editor;

import foundation.core;
import foundation.fonts;
import experimental.gui;
import :code_document;
import :code_language;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    // The code editor's own commands (C3), bound in its "codeeditor" key context.
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

    /// Binds the code editor's keys (VS Code's, ruling 5), rebindable as any: a host calls it once
    /// for a GUI's keymap, after the GUI's own bindings (which then win a tie).
    inline void BindCodeEditorKeys(Keymap& keymap)
    {
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
        };
        for (const Binding& b : kBindings)
            (void)keymap.Bind(core::StringView(b.Keys), core::StringView(b.Command), core::StringView(u8"codeeditor"));
    }

    class CodeEditor : public UIWidget
    {
        RTTI_OBJECT(CodeEditor, UIWidget)
    public:
        static constexpr f32 kBlinkSeconds = 0.5f;
        static constexpr u32 kLongestLaidOut = 4096; ///< a longer line is cut where it is laid out

        CodeEditor() : m_own(MemoryAllocator()), m_selections(MemoryAllocator()), m_goals(MemoryAllocator()), m_views(MemoryAllocator())
        {
            SetTag(core::StringView(u8"codeeditor"));
            SetTabFocusable(true);
            SetTabCaptures(true); // Tab indents
            SetKeyContext(core::StringView(u8"textinput multiline codeeditor"));
            SetClipChildren(true);
            SetScrollsByKeys(false); // the arrows and pages move its carets, through the keymap
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
                (void)OnCommand(core::StringView(name), [run = core::Move(run)](CommandEvent&) mutable { run(); });
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
            Attach(m_own);
            m_selections.PushBack(CodeSelection{});
            m_goals.PushBack(-1.0f);
        }
        ~CodeEditor() override { Detach(); }

        // === The document ===
        /// Edits `document` (shared with its host, which outlives it here); null: its own.
        void SetDocument(CodeDocument* document)
        {
            Detach();
            Attach(document != nullptr ? *document : m_own);
            SetSelections(core::Span<const CodeSelection>{});
        }
        [[nodiscard]] CodeDocument& GetDocument() const noexcept { return *m_doc; }
        /// Replaces the text (undo starts again), one caret at the start.
        void SetText(core::StringView text)
        {
            m_doc->SetText(text);
            SetSelections(core::Span<const CodeSelection>{});
        }
        [[nodiscard]] core::String GetText() const { return m_doc->GetText(); }

        /// The language its code is in (its owner keeps it, Prepared); null: plain text.
        void SetLanguage(const CodeLanguage* language)
        {
            m_language = language;
            m_lex.Clear();
            m_cleanTo = 0;
            m_views.Clear();
            Invalidate();
        }
        [[nodiscard]] const CodeLanguage* GetLanguage() const noexcept { return m_language; }
        /// Line `line`'s tokens (tokenizing up to it).
        [[nodiscard]] core::Span<const CodeSpan> TokensOf(u32 line) const
        {
            if (m_language == nullptr || line >= m_doc->LineCount())
                return {};
            EnsureLexed(line);
            return m_lex[line].Spans.AsSpan();
        }
        /// How many lines have been tokenized since it began (each time a line was), for a test.
        [[nodiscard]] u64 TokenizedCount() const noexcept { return m_tokenized; }

        /// Whether it edits (or only shows, selects and copies).
        void SetReadOnly(bool readOnly) noexcept { m_readOnly = readOnly; }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }
        /// Columns between tab stops (4), and whether Tab puts spaces (true) or a tab.
        void SetTabSize(u32 columns)
        {
            m_tabSize = core::Max(1u, columns);
            m_views.Clear();
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] u32 GetTabSize() const noexcept { return m_tabSize; }
        void SetInsertSpaces(bool spaces) noexcept { m_insertSpaces = spaces; }

        // === Selections ===
        /// The selections, in the document's order, none overlapping.
        [[nodiscard]] core::Span<const CodeSelection> GetSelections() const noexcept { return m_selections.AsSpan(); }
        /// The one the keys and the view follow (the last added).
        [[nodiscard]] const CodeSelection& GetPrimary() const noexcept { return m_selections[m_primary]; }
        /// Sets them (clamped, sorted, those that touch merged); none: one caret at the start.
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
        /// Adds a caret (Alt+click's); it becomes the primary.
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
        /// Runs a text command at every caret (what its keys do); false when it did nothing.
        bool Run(TextEditCommand command);
        /// Types `text` at every caret, replacing what each selects (one undo step while typing goes on).
        /// One character in a language: auto-closed brackets and quotes (typing a close over the one
        /// put skips it), a selection surrounded, a closing word or mark first on its line outdenting it.
        void Type(core::StringView text);

        // === C3's commands (each a command by name too, its keys in BindCodeEditorKeys) ===
        /// Comments the lines the selections cover, or uncomments them when all are (a block comment
        /// round each selection where the language has no line comment).
        void ToggleComment();
        /// Indents (or outdents) the lines the selections cover by a tab stop.
        void IndentLines(bool outdent);
        /// Moves the lines the selections cover up or down past their neighbour.
        void MoveLines(bool up);
        /// Copies the lines the selections cover below (or above) them, the selections going with the copy.
        void DuplicateLines(bool up);
        void DeleteLines();
        /// Selects the primary caret's word, or adds a selection at the next match of the primary's
        /// text (Ctrl+D); false when there is none further.
        bool AddNextOccurrence();
        /// Selects every match of the primary selection's text (Ctrl+Shift+L).
        void SelectAllOccurrences();
        /// Adds a caret a line above (or below) the primary, at its goal across.
        void AddCursorVertically(bool up);
        /// The bracket at or before `at` (not in a string or comment) and the one matching it.
        bool FindMatchingBracket(CodePosition at, CodeRange& bracket, CodeRange& match) const;

        // === Geometry (its own space) ===
        [[nodiscard]] f32 LineHeight() const
        {
            const f32 height = m_measure.GetLineHeight();
            return height > 0.0f ? height : 16.0f;
        }
        /// Where the text starts across: past the gutter and its gap.
        [[nodiscard]] f32 TextLeft() const { return GetContentBounds().x + GutterWidth(); }
        [[nodiscard]] f32 GutterWidth() const
        {
            u32 digits = 1;
            for (u32 n = m_doc->LineCount(); n >= 10; n /= 10)
                ++digits;
            return static_cast<f32>(core::Max(digits, 3u)) * DigitWidth() + 2.0f * kGutterPad + kTextGap;
        }
        /// The position nearest a point.
        [[nodiscard]] CodePosition PositionAt(core::Float2 local) const;
        /// A position's caret: its left, top and the line's height.
        [[nodiscard]] Rect CaretRect(CodePosition at) const;
        /// Scrolls by the least that shows the primary caret.
        void ScrollToCaret();
        /// The lines in view now, first to last (inclusive).
        void VisibleLines(u32& first, u32& last) const;

        // Platform text input while focused, at the primary caret.
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled() && !m_readOnly; }
        [[nodiscard]] bool GetTextInputArea(Rect& area, f32& caretX) const override
        {
            const Rect caret = CaretRect(GetPrimary().Head);
            area = Rect{caret.x, caret.y, core::Max(1.0f, ComposedWidth()), caret.height};
            caretX = caret.x;
            return true;
        }
        /// The current line, to a screen reader.
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
            m_styleText = style;
            m_views.Clear();
            RequestLayout();
            Invalidate();
        }
        // A token's colour changed: its lines are coloured again.
        void OnPartStyleChange(core::StringView part) override
        {
            UIWidget::OnPartStyleChange(part);
            m_views.Clear();
            Invalidate();
        }
        void OnLayout() override
        {
            UIWidget::OnLayout();
            // What it scrolls over: every line down; across, the widest line laid out yet, else each
            // line's length at the width of a digit (an estimate a wider line laid out corrects).
            const f32 digit = DigitWidth();
            if (m_widestVersion != m_doc->GetVersion())
            {
                m_widestVersion = m_doc->GetVersion();
                m_estimated = 0.0f;
                for (u32 line = 0; line < m_doc->LineCount(); ++line)
                    m_estimated = core::Max(m_estimated, static_cast<f32>(core::Min<usize>(m_doc->LineText(line).Size(), kLongestLaidOut)) * digit);
            }
            const Rect content = GetContentBounds();
            const f32 width = GutterWidth() + core::Max(m_estimated, m_widest) + digit * 2.0f;
            const f32 height = static_cast<f32>(m_doc->LineCount()) * LineHeight() + core::Max(0.0f, content.height - LineHeight());
            SetScrollExtent(core::Float2{width, height});
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
            Invalidate();
        }
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseMove(const MouseEvent& event) override;
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
        static constexpr f32 kTextGap = 8.0f;

        struct LineLex
        {
            u64 Hash = 0;
            CodeLexState Entry;
            CodeLexState Exit;
            core::Array<CodeSpan> Spans;
            u64 Stamp = 0; // raised each time it is tokenized
            bool Valid = false;
        };
        struct LineView
        {
            u64 Hash = 0;
            u32 TabSize = 0;
            u64 LexStamp = ~u64(0);
            bool Laid = false;
            core::String Shown;      // the line with its tabs to their stops (cut when very long)
            core::Array<u32> ShownAt; // a byte column's place in Shown (empty: the same)
            Text Glyphs;
            Text Number;
            u32 NumberOf = ~0u;
        };

        void Attach(CodeDocument& document)
        {
            m_doc = &document;
            m_views.Clear();
            m_widestVersion = ~u64(0);
            m_widest = 0.0f;
            CodeEditor* self = this;
            m_doc->SetOnChanged([self](const CodeChange& change) { self->DocumentChanged(change); });
        }
        void Detach()
        {
            if (m_doc != nullptr)
                m_doc->SetOnChanged({});
        }
        void DocumentChanged(const CodeChange& change)
        {
            // The tokens of the lines below move with them; the changed lines' go; what follows a
            // change is checked again from there (its state may differ now).
            const usize lexFirst = core::Min<usize>(change.FirstLine, m_lex.Size());
            const usize lexRemoved = core::Min<usize>(change.RemovedLines, m_lex.Size() - lexFirst);
            for (usize i = 0; i < lexRemoved; ++i)
                m_lex.RemoveAt(lexFirst);
            for (u32 i = 0; i < change.InsertedLines && lexFirst <= m_lex.Size(); ++i)
                m_lex.Insert(lexFirst, LineLex{});
            m_cleanTo = core::Min<u32>(m_cleanTo, change.FirstLine);
            // The views below the change move with their lines; the changed ones go.
            const usize first = core::Min<usize>(change.FirstLine, m_views.Size());
            const usize removed = core::Min<usize>(change.RemovedLines, m_views.Size() - first);
            for (usize i = 0; i < removed; ++i)
                m_views.RemoveAt(first);
            for (u32 i = 0; i < change.InsertedLines && first <= m_views.Size(); ++i)
                m_views.Insert(first, core::UniquePtr<LineView>{});
            RequestLayout();
            Invalidate();
        }
        // A line's view, laid out (made, or remade when its text changed).
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
            // The column whose place is nearest (inside a tab's spaces: the nearer side).
            u32 column = 0;
            while (column + 1 < view.ShownAt.Size() && view.ShownAt[column + 1] <= shown)
                ++column;
            if (column + 1 < view.ShownAt.Size() && shown - view.ShownAt[column] > view.ShownAt[column + 1] - shown)
                ++column;
            return column;
        }
        [[nodiscard]] f32 XOf(CodePosition at) const
        {
            const LineView& view = View(at.Line);
            return view.Glyphs.LayoutFor(Rect{}).CaretX(ShownColumn(view, at.Column));
        }
        [[nodiscard]] u32 ColumnAtX(u32 line, f32 x) const
        {
            const LineView& view = View(line);
            const usize shown = view.Glyphs.LayoutFor(Rect{}).OffsetAt(core::Float2{x, LineHeight() * 0.5f});
            return m_doc->Clamp(CodePosition{line, ByteColumn(view, static_cast<u32>(shown))}).Column;
        }
        [[nodiscard]] f32 ComposedWidth() const
        {
            return m_composition.IsEmpty() ? 0.0f : m_compositionText.LayoutFor(Rect{}).Width;
        }

        // Sorted by start; those that overlap (or start at one place: two carets) as one, the primary kept.
        void Normalize()
        {
            const CodeSelection primary = m_selections[m_primary];
            for (usize i = 1; i < m_selections.Size(); ++i) // insertion sort: few, and nearly sorted
                for (usize j = i; j > 0 && m_selections[j].Range().Start < m_selections[j - 1].Range().Start; --j)
                    core::Swap(m_selections[j], m_selections[j - 1]);
            core::Array<CodeSelection> merged(MemoryAllocator());
            for (const CodeSelection& s : m_selections)
            {
                if (!merged.IsEmpty())
                {
                    CodeSelection& last = merged.Back();
                    const CodeRange a = last.Range(), b = s.Range();
                    if (b.Start < a.End || b.Start == a.Start) // overlapping, or starting at one place
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
            ResetBlink();
            ScrollToCaret();
            Invalidate();
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

        // Each selection's edit, from the last to the first (an edit moves only what follows it),
        // as one undo step; the carets after them where `edit` says, moved by the later edits.
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
                u32 back = 0; // the caret this many bytes before the end of what it put
                if constexpr (requires { edit(m_selections[i], replaced, text, back); })
                {
                    if (!edit(m_selections[i], replaced, text, back))
                        continue;
                }
                else if (!edit(m_selections[i], replaced, text))
                    continue;
                const CodeRange put = m_doc->Replace(replaced, text.AsView(), kind);
                // `back` bytes before the end of what it put (across its line breaks too).
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
        // Each selection's head moved (`extend`: its anchor kept, else collapsed onto it).
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
        // Edits planned against the text as it is (each a range and its new text, none overlapping),
        // applied from the last as one undo step, every selection carried through each: a range's
        // start keeps left, its end and a caret go right.
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
        // The lines the selections cover, as runs [first, last], in order (a selection ending at a
        // line's start leaves that line out).
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
        // A line's indent in columns (its tabs to their stops).
        [[nodiscard]] u32 IndentColumns(core::StringView text) const
        {
            u32 columns = 0;
            for (usize i = 0; i < text.Size() && (text[i] == u8' ' || text[i] == u8'\t'); ++i)
                columns = text[i] == u8'\t' ? (columns / m_tabSize + 1) * m_tabSize : columns + 1;
            return columns;
        }
        // Whether `text` (a line's text before the caret) ends with one of `list`'s (space-separated)
        // words or marks: a word only where a name does not run on before it.
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
        // The token of the character before `at` on its line (what a caret is "in").
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
        // Backspace between a pair it would have put (both go), or in an indent of spaces (back to
        // the stop before).
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
        // Smart Home: the line's first character past its indent, or its start when there already.
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
        // A line up or down (`lines` of them), at the goal across (set from the caret when unset).
        [[nodiscard]] CodePosition Vertical(CodePosition at, i32 lines, f32& goal) const
        {
            if (goal < 0.0f)
                goal = XOf(at);
            const i64 target = static_cast<i64>(at.Line) + lines;
            if (target < 0)
                return CodePosition{0, 0};
            if (target >= static_cast<i64>(m_doc->LineCount()))
                return m_doc->End();
            const u32 line = static_cast<u32>(target);
            return CodePosition{line, ColumnAtX(line, goal)};
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

        // Tokenizes from the first line not known to agree with the one before, up to `line`: a line is
        // tokenized again where its text changed or its entry state did; the rest keep their tokens.
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
        // A token's colour: its part's, else the text's.
        [[nodiscard]] Color TokenColor(CodeToken token) const
        {
            if (token == CodeToken::Text)
                return GetTextColor();
            const PartStyle& part = GetPartStyle(CodeTokenName(token));
            return part.HasColor ? part.TextColor : GetTextColor();
        }

        CodeDocument m_own;
        CodeDocument* m_doc = nullptr;
        const CodeLanguage* m_language = nullptr;
        mutable core::Array<LineLex> m_lex; // by line, tokenized up to the last line asked for
        mutable u32 m_cleanTo = 0;          // lines before it agree with the lines before them
        mutable u64 m_tokenized = 0;
        core::Array<CodeSelection> m_selections;
        core::Array<f32> m_goals; // each selection's goal across, for Up and Down (below 0: none)
        usize m_primary = 0;
        mutable core::Array<core::UniquePtr<LineView>> m_views; // by line, made as lines come into view
        mutable f32 m_widest = 0.0f; // the widest line laid out
        f32 m_estimated = 0.0f;
        u64 m_widestVersion = ~u64(0);
        Text m_measure; // ten digits: the gutter's width
        core::Function<void(Text&)> m_styleText;
        core::String m_composition;
        Text m_compositionText;
        u32 m_tabSize = 4;
        bool m_insertSpaces = true;
        bool m_readOnly = false;
        bool m_caretShown = true;
        f32 m_blink = 0.0f;
        bool m_ticking = false;
        // A press's drag: what it began on and by what (1 a character, 2 a word, 3 a line).
        bool m_dragging = false;
        u32 m_dragUnit = 1;
        CodeRange m_dragOrigin;
        bool m_dragAdds = false;
        f32 m_columnX = 0.0f;                      // where across it began (the text's own space)
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
            u32 visual = 0; // characters before, for the tab stops
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
        if (m_language == nullptr)
            view.Glyphs.SetString(view.Shown.AsView());
        else
        {
            // Its tokens as the core's rich text: each run in its token's colour ("[[" a "[").
            core::String markup;
            const core::StringView shown = view.Shown.AsView();
            const auto append = [&markup](core::StringView text)
            {
                for (usize i = 0; i < text.Size(); ++i)
                    markup.Append(text[i] == u8'[' ? core::StringView(u8"[[") : text.SubStr(i, 1));
            };
            u32 done = 0;
            for (const CodeSpan& span : m_lex[line].Spans)
            {
                const u32 from = ShownColumn(view, span.Start), to = ShownColumn(view, span.End);
                if (from >= shown.Size())
                    break;
                if (from > done)
                    append(shown.SubStr(done, from - done));
                const core::StringView text = shown.SubStr(from, core::Min<usize>(to, shown.Size()) - from);
                if (span.Token == CodeToken::Text)
                    append(text);
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
                    append(text);
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
        const u32 count = m_doc->LineCount();
        first = core::Min(static_cast<u32>(core::Max(0.0f, top / lh)), count - 1);
        last = core::Min(static_cast<u32>(core::Max(0.0f, (top + content.height) / lh)), count - 1);
    }

    inline CodePosition CodeEditor::PositionAt(core::Float2 local) const
    {
        const Rect content = GetContentBounds();
        const core::Float2 shift = ScrollShift();
        const f32 row = (local.y - content.y + shift.y) / LineHeight();
        if (row < 0.0f)
            return CodePosition{0, 0};
        const u32 line = static_cast<u32>(row);
        if (line >= m_doc->LineCount())
            return m_doc->End();
        return CodePosition{line, ColumnAtX(line, local.x - TextLeft() + shift.x)};
    }

    inline Rect CodeEditor::CaretRect(CodePosition at) const
    {
        at = m_doc->Clamp(at);
        const Rect content = GetContentBounds();
        const core::Float2 shift = ScrollShift();
        const f32 lh = LineHeight();
        return Rect{TextLeft() + XOf(at) - shift.x, content.y + static_cast<f32>(at.Line) * lh - shift.y, 1.5f, lh};
    }

    inline void CodeEditor::ScrollToCaret()
    {
        const Rect content = GetContentBounds();
        if (content.height <= 0.0f)
            return;
        const CodePosition at = m_doc->Clamp(GetPrimary().Head);
        const f32 lh = LineHeight();
        core::Float2 offset = GetScrollOffset();
        const f32 top = static_cast<f32>(at.Line) * lh;
        if (top < offset.y)
            offset.y = top;
        else if (top + lh > offset.y + content.height)
            offset.y = top + lh - content.height;
        const f32 x = XOf(at);
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
        // One character, in a language that pairs or outdents: its own rules.
        if (m_language != nullptr && text.Size() >= 1 && text.Size() <= 4 && NextGrapheme(text, 0) == text.Size() && text[0] != u8'\n')
        {
            TypeCharacter(text);
            return;
        }
        EditEach(CodeEditKind::Typing, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            replaced = s.Range();
            put = core::String(text);
            return true;
        });
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
            // Typing the close the pair put: over it.
            if (s.IsEmpty() && closes != nullptr && after.Size() >= text.Size() && after.SubStr(0, text.Size()) == text &&
                !(opens != nullptr && opens->Open == opens->Close && inText && in != CodeToken::String))
            {
                replaced = CodeRange{at, CodePosition{at.Line, at.Column + static_cast<u32>(text.Size())}};
                put = core::String(text);
                return true;
            }
            // A selection and an open: round it.
            if (!s.IsEmpty() && opens != nullptr)
            {
                put = core::String(opens->Open);
                put.Append(m_doc->TextIn(replaced).AsView());
                put.Append(opens->Close.AsView());
                back = static_cast<u32>(opens->Close.Size());
                return true;
            }
            // An open where its close may follow: the pair, the caret between. Not in a string or a
            // comment; a quote not after a name (it's'), nor before one.
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
            // A closing word or mark first on its line: the line outdented to its block's opener.
            core::String before(line.SubStr(0, at.Column));
            before.Append(text);
            const u32 indent = IndentOf(before.AsView());
            const core::StringView first = before.AsView().SubStr(indent, before.Size() - indent);
            if (s.IsEmpty() && !lang.OutdentFirst.IsEmpty() && IsListed(first, lang.OutdentFirst.AsView()) &&
                (after.Size() == 0 || !IsNameByte(after[0])))
            {
                // The nearest line above with text: its indent, less a stop unless it opens the block.
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
        // Each new line as indented as the one it breaks, a stop more after what opens a block; between
        // a bracket pair, the close on a line of its own below.
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
        // A selection across lines indents them; else to the next tab stop at each caret.
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
                // Up to a stop's worth of what indents it.
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
            // Only block comments (markup's): round each selection, or its caret's line; off again
            // where a selection is one already.
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
        // Off when every line with text is commented; else on, at the least indent.
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
        // Each block and the line it passes, swapped as one edit; the selections a line along.
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
            // The copy after the block's end: the selections go to it (down) or stay on the first (up).
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
            // First, the word at the caret.
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
        // From the last selection's end, round to the start; one already selected is skipped.
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

    inline bool CodeEditor::FindMatchingBracket(CodePosition at, CodeRange& bracket, CodeRange& match) const
    {
        if (m_language == nullptr || m_language->Brackets.IsEmpty())
            return false;
        at = m_doc->Clamp(at);
        const auto skip = [](CodeToken t) { return t == CodeToken::String || t == CodeToken::Comment || t == CodeToken::Escape; };
        // The bracket after the caret, else the one before it.
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
                    // Out from it, its kind counted, past strings and comments, a few thousand lines at most.
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
            else // no selection: the caret's whole line, with its line feed (VS Code's)
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
        // As many lines as carets: one to each, in order (several copied, as many pasted).
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
            --index; // EditEach goes from the last
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
        if (key == KeyCode::Escape && mods == 0 && m_selections.Size() > 1)
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
        // A key that types is its own (it neither activates nor moves focus).
        if ((mods & (KeyModCtrl | KeyModAlt | KeyModSuper)) == 0 && (key == KeyCode::Space || (key >= KeyCode::A && key <= KeyCode::Num9)))
            event.Handled = true;
    }

    inline void CodeEditor::OnMouseDown(const MouseEvent& event)
    {
        if (!IsTreeEnabled() || event.Button != MouseButton::Left)
            return;
        event.Handled = true;
        RequestFocus();
        const core::Float2 local = ConvertToNodeSpace(event.Position);
        const CodePosition at = PositionAt(local);
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
        if (!m_dragging)
            return;
        const core::Float2 local = ConvertToNodeSpace(event.Position);
        const CodePosition at = PositionAt(local);
        if (m_dragAdds && m_dragUnit == 1 && at.Line != m_columnLine)
        {
            // Alt+drag across lines: a column, a selection on each line between, at the two places
            // across, in place of the others (VS Code's box selection).
            const f32 x = local.x - TextLeft() + ScrollShift().x;
            core::Array<CodeSelection> next(MemoryAllocator());
            const u32 from = core::Min(m_columnLine, at.Line), to = core::Max(m_columnLine, at.Line);
            for (u32 line = from; line <= to; ++line)
                next.PushBack(CodeSelection{CodePosition{line, ColumnAtX(line, m_columnX)}, CodePosition{line, ColumnAtX(line, x)}});
            m_selections = core::Move(next);
            m_primary = m_selections.Size() - 1;
            Normalize();
            ResetGoals();
            SelectionsChanged();
            return;
        }
        // From what the press selected to what the pointer is on, by the press's unit.
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
        const Color currentLine = partColor(u8"current-line", Color{0.0f, 0.0f, 0.0f, 0.0f}); // the sheet's (none by default)
        const Color selectionColor = partColor(u8"selection", Color{0.25f, 0.45f, 0.8f, 0.45f});
        const Color caretColor = partColor(u8"caret", GetTextColor());

        u32 first = 0, last = 0;
        VisibleLines(first, last);
        const CodePosition primary = m_doc->Clamp(GetPrimary().Head);
        const auto rowTop = [&](u32 line) { return content.y + static_cast<f32>(line) * lh - shift.y; };
        const f32 textX = textArea.x - shift.x;

        ctx.PushClip(content);
        // The primary caret's line, across the whole view.
        if (GetPrimary().IsEmpty() && primary.Line >= first && primary.Line <= last && currentLine.a > 0.0f)
            ctx.VG().FillRect(Rectangle{content.x, rowTop(primary.Line), content.width, lh}, currentLine);
        ctx.PushClip(textArea);
        // The selections, then the text over them.
        for (const CodeSelection& s : m_selections)
        {
            if (s.IsEmpty())
                continue;
            const CodeRange r = s.Range();
            for (u32 line = core::Max(r.Start.Line, first); line <= core::Min(r.End.Line, last); ++line)
            {
                const f32 x0 = line == r.Start.Line ? XOf(r.Start) : 0.0f;
                const f32 x1 = line == r.End.Line ? XOf(r.End) : XOf(CodePosition{line, static_cast<u32>(m_doc->LineText(line).Size())}) + DigitWidth() * 0.5f;
                ctx.VG().FillRect(Rectangle{textX + x0, rowTop(line), core::Max(0.0f, x1 - x0), lh}, selectionColor);
            }
        }
        // The bracket by the primary caret and its match, marked.
        {
            CodeRange bracket, match;
            if (GetPrimary().IsEmpty() && FindMatchingBracket(primary, bracket, match))
            {
                const Color mark = partColor(u8"bracket-match", Color{0.5f, 0.6f, 0.75f, 0.35f});
                const CodeRange both[] = {bracket, match};
                for (const CodeRange& r : both)
                    if (r.Start.Line >= first && r.Start.Line <= last)
                    {
                        const f32 x0 = XOf(r.Start), x1 = XOf(r.End);
                        ctx.VG().FillRect(Rectangle{textX + x0, rowTop(r.Start.Line), x1 - x0, lh}, mark);
                    }
            }
        }
        for (u32 line = first; line <= last; ++line)
        {
            LineView& view = View(line);
            const TextLayout& layout = view.Glyphs.LayoutFor(Rect{});
            view.Glyphs.DrawLayout(ctx, layout, core::Float2{textX, rowTop(line) + (lh - layout.Height) * 0.5f});
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
                if (at.Line < first || at.Line > last)
                    continue;
                ctx.VG().FillRect(Rectangle{textX + XOf(at) + (at == primary ? ComposedWidth() : 0.0f), rowTop(at.Line), 1.5f, lh}, caretColor);
            }
        ctx.PopClip();
        // The gutter: still across, the numbers right aligned.
        if (gutterColor.a > 0.0f)
            ctx.VG().FillRect(Rectangle{content.x, content.y, gutter, content.height}, gutterColor);
        for (u32 line = first; line <= last; ++line)
        {
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
            view.Number.DrawLayout(ctx, layout, core::Float2{content.x + gutter - kTextGap - kGutterPad - layout.Width, rowTop(line) + (lh - layout.Height) * 0.5f});
        }
        ctx.PopClip();
    }
}

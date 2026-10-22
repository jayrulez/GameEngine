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
            SetKeyContext(core::StringView(u8"textinput multiline"));
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
        void Type(core::StringView text);

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
                if (!edit(m_selections[i], replaced, text))
                    continue;
                const CodeRange put = m_doc->Replace(replaced, text.AsView(), kind);
                after[i] = CodeSelection{put.End, put.End};
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
        EditEach(CodeEditKind::Typing, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            replaced = s.Range();
            put = core::String(text);
            return true;
        });
    }

    inline void CodeEditor::Newline()
    {
        // Each new line as indented as the one it breaks (C3 adds the language's rules).
        EditEach(CodeEditKind::Other, [&](const CodeSelection& s, CodeRange& replaced, core::String& put)
        {
            replaced = s.Range();
            const core::StringView line = m_doc->LineText(replaced.Start.Line);
            put = core::String(core::StringView(u8"\n"));
            put.Append(line.SubStr(0, core::Min(IndentOf(line), replaced.Start.Column)));
            return true;
        });
    }

    inline void CodeEditor::Indent()
    {
        // To the next tab stop at each caret (C3 indents a selection's lines).
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
        if (key == KeyCode::Tab && mods == 0 && !m_readOnly)
        {
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
        const CodePosition at = PositionAt(ConvertToNodeSpace(event.Position));
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

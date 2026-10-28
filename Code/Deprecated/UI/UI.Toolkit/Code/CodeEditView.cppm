// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :code_edit_view partition
//
// CodeEditView: the purpose-built code editor widget over the
// :code_document core. Virtualized monospace rendering (only visible lines are drawn; column
// geometry is column * advance), a line-number gutter with clickable markers (breakpoints,
// diagnostics, execution line), full keyboard/mouse editing over CodeDocument's delta undo, and
// the completion seam: composable ICompletionProviders feed a CompletionModel whose popup is
// SELF-DRAWN inside this widget - deliberately NOT PopupLayer::ShowPopup, which pushes/clears
// focus; the editor must keep focus (and IME routing) while the popup routes its keys.
//
// The widget consumes keys it acts on and leaves everything else unhandled so application
// shortcuts (save, palette) keep working while the editor is focused. Tab requires the core
// WantsTabKey opt-in (dispatch-first, traversal fallback - see InputManager::ProcessKeyDown).

module;
#include <cstdio>
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:code_edit_view;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;
import :code_document;
import :code_lexer;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace ui = foundation::ui;

export namespace foundation::ui::toolkit
{

    /// Colors per token kind (CodeTokenKind::Default always draws in the theme's TextColor).
    /// A public field on CodeEditView so pages can restyle; the default set is tuned for the
    /// dark editor themes (keywords lean into the Graphite & Orange accent).
    struct CodeTokenColors
    {
        core::Color keyword{235.0f / 255.0f, 155.0f / 255.0f, 90.0f / 255.0f, 1.0f};
        core::Color type{86.0f / 255.0f, 182.0f / 255.0f, 194.0f / 255.0f, 1.0f};
        core::Color number{220.0f / 255.0f, 190.0f / 255.0f, 120.0f / 255.0f, 1.0f};
        core::Color string{152.0f / 255.0f, 195.0f / 255.0f, 121.0f / 255.0f, 1.0f};
        core::Color comment{110.0f / 255.0f, 120.0f / 255.0f, 130.0f / 255.0f, 1.0f};
        core::Color op{170.0f / 255.0f, 178.0f / 255.0f, 190.0f / 255.0f, 1.0f};
        core::Color punctuation{150.0f / 255.0f, 158.0f / 255.0f, 170.0f / 255.0f, 1.0f};
        core::Color preprocessor{198.0f / 255.0f, 120.0f / 255.0f, 221.0f / 255.0f, 1.0f};
        core::Color tag{97.0f / 255.0f, 175.0f / 255.0f, 239.0f / 255.0f, 1.0f};
        core::Color attribute{229.0f / 255.0f, 192.0f / 255.0f, 123.0f / 255.0f, 1.0f};

        [[nodiscard]] core::Color For(CodeTokenKind kind, core::Color defaultColor) const noexcept
        {
            switch (kind)
            {
                case CodeTokenKind::Keyword:      return keyword;
                case CodeTokenKind::Type:         return type;
                case CodeTokenKind::Number:       return number;
                case CodeTokenKind::String:       return string;
                case CodeTokenKind::Comment:      return comment;
                case CodeTokenKind::Operator:     return op;
                case CodeTokenKind::Punctuation:  return punctuation;
                case CodeTokenKind::Preprocessor: return preprocessor;
                case CodeTokenKind::Tag:          return tag;
                case CodeTokenKind::Attribute:    return attribute;
                default:                          return defaultColor;
            }
        }
    };

    // ---- completion seam ---------------------------------------------------------------------

    struct CompletionCandidate
    {
        String label;      // shown in the popup, matched against the typed prefix
        String insertText; // replaces the prefix on accept (usually == label)
        u8 priority = 0;   // sort tier: lower ranks first (context providers 0, words 100)
    };

    /// A completion source. Providers are COMPOSABLE: the view queries every registered provider
    /// and merges (dedupe by label, sorted). `prefix` is the identifier fragment left of the
    /// cursor at `cursor` (may be empty for explicit Ctrl+Space invocation).
    class ICompletionProvider
    {
    public:
        virtual ~ICompletionProvider() = default;
        virtual void Collect(const CodeDocument& document, CodePosition cursor, StringView prefix,
                             Array<CompletionCandidate>& out) = 0;
    };

    /// The provider every language gets for free: identifiers harvested from the buffer itself.
    class DocumentWordCompletionProvider final : public ICompletionProvider
    {
    public:
        void Collect(const CodeDocument& document, CodePosition cursor, StringView prefix,
                     Array<CompletionCandidate>& out) override
        {
            (void)cursor;
            const Span<const String> words = document.Words();
            for (usize i = 0; i < words.Size(); ++i)
            {
                // The fragment being typed is itself a harvested word - suggesting it back
                // verbatim is noise; longer words that extend it still match.
                if (words[i].AsView() == prefix)
                {
                    continue;
                }
                CompletionCandidate candidate;
                candidate.label = String(words[i].AsView());
                candidate.insertText = String(words[i].AsView());
                candidate.priority = 100; // words rank BELOW context-provider results
                out.PushBack(Move(candidate));
            }
        }
    };

    // ---- the popup model (headless: filter + key routing, no UI) -----------------------------

    enum class CompletionKeyResult : u8
    {
        Ignored,   // not a popup key - the editor handles it normally
        Consumed,  // popup navigation consumed the key
        Accepted,  // commit the selected candidate
        Dismissed, // popup closed; the editor should NOT process the key further
    };

    /// State + key routing for the completion popup. The view feeds keys here FIRST while open
    /// (this is why completion shapes the input design); rendering stays in the view.
    class CompletionModel
    {
    public:
        void Open(CodePosition anchor, Array<CompletionCandidate> candidates, StringView prefix)
        {
            m_anchor = anchor;
            m_all = Move(candidates);
            m_open = true;
            Filter(prefix);
        }

        void Close() noexcept
        {
            m_open = false;
            m_all.Clear();
            m_filtered.Clear();
            m_selected = 0;
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        [[nodiscard]] CodePosition Anchor() const noexcept { return m_anchor; }

        /// Re-filters against the (re)typed prefix: case-insensitive prefix match, exact-case
        /// matches ranked first (stable within each group). Closes when nothing matches, or when
        /// the only match IS the prefix (nothing left to complete).
        void Filter(StringView prefix)
        {
            if (!m_open)
            {
                return;
            }
            m_filtered.Clear();
            for (usize i = 0; i < m_all.Size(); ++i)
            {
                if (StartsWithCaseSensitive(m_all[i].label.AsView(), prefix))
                {
                    m_filtered.PushBack(static_cast<i32>(i));
                }
            }
            for (usize i = 0; i < m_all.Size(); ++i)
            {
                if (!StartsWithCaseSensitive(m_all[i].label.AsView(), prefix) &&
                    StartsWithCaseInsensitive(m_all[i].label.AsView(), prefix))
                {
                    m_filtered.PushBack(static_cast<i32>(i));
                }
            }
            m_selected = 0;
            if (m_filtered.IsEmpty() ||
                (m_filtered.Size() == 1 &&
                 m_all[static_cast<usize>(m_filtered[0])].label.AsView() == prefix))
            {
                Close();
            }
        }

        [[nodiscard]] CompletionKeyResult HandleKey(KeyCode key)
        {
            if (!m_open)
            {
                return CompletionKeyResult::Ignored;
            }
            switch (key)
            {
                case KeyCode::Up:
                    m_selected = m_selected > 0 ? m_selected - 1 : 0;
                    return CompletionKeyResult::Consumed;
                case KeyCode::Down:
                    m_selected = Min(m_selected + 1, static_cast<i32>(m_filtered.Size()) - 1);
                    return CompletionKeyResult::Consumed;
                case KeyCode::Return:
                case KeyCode::Tab:
                    return CompletionKeyResult::Accepted;
                case KeyCode::Escape:
                    Close();
                    return CompletionKeyResult::Dismissed;
                default:
                    return CompletionKeyResult::Ignored;
            }
        }

        [[nodiscard]] i32 ItemCount() const noexcept { return static_cast<i32>(m_filtered.Size()); }
        [[nodiscard]] i32 SelectedIndex() const noexcept { return m_selected; }
        void SetSelectedIndex(i32 index) noexcept
        {
            m_selected = Clamp(index, 0, Max(0, static_cast<i32>(m_filtered.Size()) - 1));
        }

        [[nodiscard]] const CompletionCandidate* Item(i32 index) const
        {
            if (index < 0 || index >= ItemCount())
            {
                return nullptr;
            }
            return &m_all[static_cast<usize>(m_filtered[static_cast<usize>(index)])];
        }
        [[nodiscard]] const CompletionCandidate* Selected() const { return Item(m_selected); }

    private:
        [[nodiscard]] static char8_t ToLowerAscii(char8_t c) noexcept
        {
            return (c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c + 32) : c;
        }
        [[nodiscard]] static bool StartsWithCaseSensitive(StringView s, StringView prefix) noexcept
        {
            return s.StartsWith(prefix);
        }
        [[nodiscard]] static bool StartsWithCaseInsensitive(StringView s,
                                                            StringView prefix) noexcept
        {
            if (prefix.Size() > s.Size())
            {
                return false;
            }
            for (usize i = 0; i < prefix.Size(); ++i)
            {
                if (ToLowerAscii(s[i]) != ToLowerAscii(prefix[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool m_open = false;
        CodePosition m_anchor{};
        Array<CompletionCandidate> m_all;
        Array<i32> m_filtered; // indices into m_all
        i32 m_selected = 0;
    };

    // ---- the widget --------------------------------------------------------------------------

    class CodeEditView : public ViewGroup, public ITooltipProvider
    {
        RTTI_OBJECT(CodeEditView, ViewGroup)

    public:
        // Appearance/behavior knobs (plain fields, read each frame like other toolkit widgets).
        f32 FontSize = 13.0f;
        String FontFamily = String(u8"Mono"); // falls back to the default family when absent
        i32 TabWidth = 4;                     // spaces per indent step (tabs insert spaces)
        bool ShowGutter = true;
        bool ShowLineNumbers = true;
        bool ReadOnly = false;
        bool AllowBreakpoints = true;        // gutter margin click toggles Breakpoint markers
        bool DocumentWordCompletion = true;  // built-in identifier provider
        i32 AutoCompleteMinPrefix = 2;       // identifier chars typed before the popup auto-opens
        bool IndentAfterOpenBrace = true;    // Enter after '{' adds one extra indent step
        // Typing any of these (ASCII) opens completion immediately with an empty prefix -
        // providers read the document left of the cursor for context (member access on '.').
        String CompletionTriggerCharacters = String(u8".");

        Event<void()> OnTextChanged;               // any document mutation (typing, undo, paste)
        Event<void(i32, bool)> OnBreakpointToggled; // (line, nowSet) after a gutter toggle

        /// Hover-value lookup (debugger integration): given the identifier under the mouse,
        /// return its display text, or empty for none. When it yields text, the hover
        /// tooltip shows the VALUE (diagnostics remain the fallback for marked lines).
        Function<String(StringView)> HoverValueProvider;

        CodeEditView()
        {
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            WantsTabKey = true;
            ClipsContent = true;
            Cursor = CursorType::IBeam;
            TooltipPlacement = ui::TooltipPlacement::Pointer; // per-line diagnostics hover

            CodeEditView* self = this;
            m_vBar = MakeRef<ScrollBar>(MemoryAllocator(), false);
            m_vBar->Parent = this;
            m_vBar->OnValueChanged.Add(Event<void(ScrollBar*, f32)>::Handler{
                [self](ScrollBar*, f32 value)
                {
                    self->m_scrollY = value;
                    self->Invalidate();
                }});
            m_hBar = MakeRef<ScrollBar>(MemoryAllocator(), true);
            m_hBar->Parent = this;
            m_hBar->OnValueChanged.Add(Event<void(ScrollBar*, f32)>::Handler{
                [self](ScrollBar*, f32 value)
                {
                    self->m_scrollX = value;
                    self->Invalidate();
                }});

            m_doc.OnLinesChanged = [self](i32 first, i32 removed, i32 added)
            {
                self->m_maxLineDirty = true;
                self->m_highlighter.OnLinesChanged(first, removed, added);
            };
        }

        /// IBeam over the text area only; the gutter is a click-target (breakpoints), so it
        /// gets the arrow. The scrollbars are children carrying their own Arrow cursor.
        [[nodiscard]] CursorType CursorAt(Float2 localPoint) const override
        {
            return localPoint.x < GutterWidth() ? CursorType::Arrow : CursorType::IBeam;
        }

        [[nodiscard]] bool WantsTextInput() const override
        {
            return IsEffectivelyEnabled() && !ReadOnly;
        }

        // ---- diagnostics tooltip (hover a line with an Error/Warning marker) ----

        [[nodiscard]] ITooltipProvider* AsTooltipProvider() override { return this; }
        [[nodiscard]] RefPtr<View> CreateTooltipContent() override
        {
            // A debugger hover-value for the identifier under the mouse wins; a diagnostic
            // on the hovered line is the fallback.
            if (HoverValueProvider)
            {
                const CodeSpan word = m_doc.WordAt(PositionAt(m_lastHover.x, m_lastHover.y));
                if (!word.IsEmpty())
                {
                    const String value = HoverValueProvider(m_doc.TextInSpan(word).AsView());
                    if (!value.IsEmpty())
                    {
                        auto label = MakeRef<Label>(MemoryAllocator(), value.AsView());
                        label->FontSize.SetValue(12.0f);
                        return label;
                    }
                }
            }
            const i32 line =
                static_cast<i32>((m_lastHover.y + m_scrollY - kPadTop) / LineHeight());
            const CodeDiagnostic* diagnostic =
                (line >= 0 && line < m_doc.LineCount()) ? m_doc.DiagnosticOn(line) : nullptr;
            if (diagnostic == nullptr)
            {
                return {};
            }
            auto label = MakeRef<Label>(MemoryAllocator(), diagnostic->message.AsView());
            label->FontSize.SetValue(12.0f);
            return label;
        }

        // ---- document access ----

        [[nodiscard]] CodeDocument& Document() noexcept { return m_doc; }
        [[nodiscard]] const CodeDocument& Document() const noexcept { return m_doc; }

        [[nodiscard]] String Text() const { return m_doc.Text(); }
        void SetText(StringView text)
        {
            m_doc.SetText(text);
            m_cursor = CodePosition{};
            m_anchor = CodePosition{};
            m_desiredColumn = -1;
            m_scrollX = 0;
            m_scrollY = 0;
            m_completion.Close();
            m_maxLineDirty = true;
            Invalidate();
        }

        [[nodiscard]] CodePosition CursorPosition() const noexcept { return m_cursor; }
        void SetCursorPosition(CodePosition pos)
        {
            m_cursor = m_doc.ClampPosition(pos);
            m_anchor = m_cursor;
            m_desiredColumn = -1;
            m_pendingCursorScroll = true;
            ResetBlink();
            Invalidate();
        }

        /// Types `text` at the cursor (replacing any selection) as one discrete undo unit -
        /// the seam for external inserters (API browser, snippet tooling). Paste-kind so it
        /// never coalesces with surrounding keystrokes.
        void InsertAtCursor(StringView text)
        {
            if (text.IsEmpty())
            {
                return;
            }
            InsertText(text, CodeEditKind::Paste);
        }

        [[nodiscard]] bool HasSelection() const noexcept { return !(m_cursor == m_anchor); }
        [[nodiscard]] CodeSpan Selection() const noexcept
        {
            return CodeSpan{m_anchor, m_cursor}.Normalized();
        }
        [[nodiscard]] String SelectedText() const { return m_doc.TextInSpan(Selection()); }

        void SelectAll()
        {
            m_anchor = CodePosition{};
            m_cursor = m_doc.EndPosition();
            Invalidate();
        }

        [[nodiscard]] f32 ScrollY() const noexcept { return m_scrollY; }
        [[nodiscard]] f32 ScrollX() const noexcept { return m_scrollX; }

        /// Scrolls so `line` is visible (roughly centered) and places the cursor on it.
        void ScrollToLine(i32 line)
        {
            line = Clamp(line, 0, m_doc.LineCount() - 1);
            SetCursorPosition(CodePosition{line, 0});
            m_scrollY = Max(0.0f, static_cast<f32>(line) * LineHeight() - m_viewportH * 0.5f);
            ClampScroll();
            Invalidate();
        }

        // ---- completion ----

        /// Registers a provider (borrowed; caller keeps it alive while registered).
        void AddCompletionProvider(ICompletionProvider* provider)
        {
            if (provider != nullptr)
            {
                m_providers.PushBack(provider);
            }
        }

        [[nodiscard]] CompletionModel& Completion() noexcept { return m_completion; }

        /// Opens the popup at the current word (explicit Ctrl+Space path; also used by tests).
        void RequestCompletion() { OpenCompletion(true); }

        // ---- syntax highlighting ----

        /// Colors per token kind; restyle freely (public field like the other knobs).
        CodeTokenColors TokenColors;

        /// Takes ownership. Null disables highlighting (plain single-color text).
        void SetLexer(UniquePtr<ICodeLexer> lexer)
        {
            m_lexer = Move(lexer);
            m_highlighter.SetLexer(m_lexer.Get());
            m_highlighter.Reset(m_doc.LineCount());
            Invalidate();
        }

        [[nodiscard]] CodeHighlighter& Highlighter() noexcept { return m_highlighter; }

        // ---- find / replace / go-to-line ----

        enum class FindBarMode : u8
        {
            Closed,
            Find,
            Replace,
            GoToLine,
        };

        [[nodiscard]] FindBarMode FindBar() const noexcept { return m_findBarMode; }

        /// Opens the in-widget bar (Ctrl+F / Ctrl+H). A single-line selection prefills the
        /// query. Focus moves to the find field; Escape returns it to the editor.
        void OpenFindBar(bool withReplace)
        {
            EnsureFindBar();
            m_findBarMode = withReplace ? FindBarMode::Replace : FindBarMode::Find;
            ApplyFindBarMode();
            const CodeSpan selection = Selection();
            if (HasSelection() && selection.begin.line == selection.end.line)
            {
                m_findField->SetText(m_doc.TextInSpan(selection).AsView());
            }
            RunSearch(); // SetText is a SILENT programmatic setter - search explicitly
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetFocus(m_findField.Get());
            }
            Invalidate();
        }

        /// Opens the bar in go-to-line mode (Ctrl+G): type a 1-based line, Enter jumps.
        void OpenGoToLine()
        {
            EnsureFindBar();
            m_findBarMode = FindBarMode::GoToLine;
            ApplyFindBarMode();
            m_findField->SetText(StringView(u8""));
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetFocus(m_findField.Get());
            }
            Invalidate();
        }

        void CloseFindBar()
        {
            if (m_findBarMode == FindBarMode::Closed)
            {
                return;
            }
            m_findBarMode = FindBarMode::Closed;
            if (m_findBar.Get() != nullptr)
            {
                m_findBar->Visibility = VisibilityValue::Gone;
            }
            m_matches.Clear();
            m_currentMatch = -1;
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetFocus(this);
            }
            Invalidate();
        }

        [[nodiscard]] Span<const CodeSpan> SearchMatches() const noexcept
        {
            return Span<const CodeSpan>(m_matches.Data(), m_matches.Size());
        }
        [[nodiscard]] i32 CurrentMatchIndex() const noexcept { return m_currentMatch; }

        /// Programmatic query/replacement. EditText::SetText is silent (no OnTextChanged),
        /// so the search re-runs explicitly.
        void SetSearchQuery(StringView query)
        {
            EnsureFindBar();
            m_findField->SetText(query);
            RunSearch();
        }
        void SetReplaceText(StringView text)
        {
            EnsureFindBar();
            m_replaceField->SetText(text);
        }

        /// Selects the next/previous match (wraps). F3 / Shift+F3.
        void FindNext() { GotoMatch(1); }
        void FindPrevious() { GotoMatch(-1); }

        /// Replaces the selected match with the replace field's text, then advances.
        void ReplaceCurrent()
        {
            if (ReadOnly || m_currentMatch < 0 ||
                static_cast<usize>(m_currentMatch) >= m_matches.Size())
            {
                FindNext();
                return;
            }
            String replacement;
            if (m_replaceField.Get() != nullptr)
            {
                replacement = String(m_replaceField->Text());
            }
            const CodeSpan span = m_matches[static_cast<usize>(m_currentMatch)];
            const CodeCursorState before{m_cursor, m_anchor};
            m_cursor = m_doc.Edit(span, replacement.AsView(), CodeEditKind::Other, before, Now());
            m_anchor = m_cursor;
            AfterEdit(); // re-runs the search; the current match is now the next one
            if (m_currentMatch >= 0)
            {
                SelectMatch(m_currentMatch);
            }
        }

        /// Replaces every match as ONE undo entry.
        void ReplaceAll()
        {
            if (ReadOnly || m_matches.IsEmpty())
            {
                return;
            }
            String replacement;
            if (m_replaceField.Get() != nullptr)
            {
                replacement = String(m_replaceField->Text());
            }
            const CodeCursorState before{m_cursor, m_anchor};
            m_doc.BeginCompoundEdit();
            for (usize i = m_matches.Size(); i > 0; --i) // back-to-front: spans stay valid
            {
                m_cursor = m_doc.Edit(m_matches[i - 1], replacement.AsView(),
                                      CodeEditKind::Other, before, Now());
            }
            m_doc.EndCompoundEdit();
            m_anchor = m_cursor;
            AfterEdit();
        }

        /// Toggle the language's line comment on the cursor line / every selected line
        /// (Ctrl+/). Uses the lexer's LineCommentPrefix; a language without one (XML) is a
        /// no-op. One undo entry.
        void ToggleLineComment();

        // ---- metrics (fallbacks keep headless tests working without a font service) ----

        [[nodiscard]] f32 LineHeight() const noexcept
        {
            return m_lineHeight > 0.0f ? m_lineHeight : FontSize * 1.35f;
        }
        [[nodiscard]] f32 ColumnAdvance() const noexcept
        {
            return m_advance > 0.0f ? m_advance : FontSize * 0.6f;
        }
        [[nodiscard]] f32 GutterWidth() const
        {
            if (!ShowGutter)
            {
                return 0.0f;
            }
            f32 digitsWidth = 0.0f;
            if (ShowLineNumbers)
            {
                i32 digits = 1;
                for (i32 n = m_doc.LineCount(); n >= 10; n /= 10)
                {
                    ++digits;
                }
                digitsWidth = static_cast<f32>(Max(digits, 3)) * ColumnAdvance() + kNumberPad;
            }
            return kMarkerMargin + digitsWidth + kGutterGap;
        }

        /// Buffer position for a point in view-local coordinates.
        [[nodiscard]] CodePosition PositionAt(f32 localX, f32 localY) const
        {
            const i32 line = static_cast<i32>((localY + m_scrollY - kPadTop) / LineHeight());
            const f32 textX = localX - (GutterWidth() + kPadLeft) + m_scrollX;
            const i32 column = static_cast<i32>((textX / ColumnAdvance()) + 0.5f);
            return m_doc.ClampPosition(CodePosition{line, column});
        }

        // ---- input ----

        /// Capture-phase interplay while a find-bar field is focused: the fields consume most
        /// keys themselves, so Escape/F3/Enter are claimed here BEFORE they reach the field.
        void OnKeyDownCapture(KeyEventArgs& e) override
        {
            if (m_findBarMode == FindBarMode::Closed || Context == nullptr)
            {
                return;
            }
            View* focused = Context->GetFocusManager()->FocusedView();
            if (focused == nullptr || focused == this || !IsInFindBar(focused))
            {
                return;
            }
            switch (e.Key)
            {
                case KeyCode::Escape:
                    CloseFindBar();
                    e.Handled = true;
                    return;
                case KeyCode::F3:
                    if (HasFlag(e.Modifiers, KeyModifiers::Shift))
                    {
                        FindPrevious();
                    }
                    else
                    {
                        FindNext();
                    }
                    e.Handled = true;
                    return;
                case KeyCode::Return:
                    if (m_findBarMode == FindBarMode::GoToLine)
                    {
                        JumpToTypedLine();
                    }
                    else if (focused == m_replaceField.Get())
                    {
                        ReplaceCurrent();
                    }
                    else
                    {
                        FindNext();
                    }
                    e.Handled = true;
                    return;
                default:
                    return;
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            // A focused find-bar field owns the keyboard; anything bubbling up stays its
            // business (the capture handler above already took the interplay keys).
            if (Context != nullptr)
            {
                View* focused = Context->GetFocusManager()->FocusedView();
                if (focused != nullptr && focused != this)
                {
                    return;
                }
            }

            // The popup routes its keys FIRST while open - without stealing focus.
            const CompletionKeyResult routed = m_completion.HandleKey(e.Key);
            if (routed == CompletionKeyResult::Accepted)
            {
                AcceptCompletion();
                e.Handled = true;
                return;
            }
            if (routed == CompletionKeyResult::Consumed || routed == CompletionKeyResult::Dismissed)
            {
                Invalidate();
                e.Handled = true;
                return;
            }

            if (ProcessKey(e.Key, e.Modifiers))
            {
                e.Handled = true;
            }
        }

        void OnTextInput(TextInputEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || ReadOnly || e.Character < 0x20 || e.Character == 0x7F)
            {
                return;
            }
            String text;
            AppendUtf8(text, static_cast<u32>(e.Character));
            InsertText(text.AsView(), CodeEditKind::Typing);

            // Trigger characters ('.') reopen completion with an empty prefix - providers see
            // the receiver word left of the cursor (member completion).
            if (e.Character < 128)
            {
                const StringView triggers = CompletionTriggerCharacters.AsView();
                for (usize i = 0; i < triggers.Size(); ++i)
                {
                    if (static_cast<char32_t>(triggers[i]) == e.Character)
                    {
                        OpenCompletion(true);
                        e.Handled = true;
                        return;
                    }
                }
            }

            // Completion: refilter while open; auto-open once the identifier fragment is long
            // enough (word chars only - punctuation closes below via the empty prefix).
            const StringView prefix = PrefixView();
            if (m_completion.IsOpen())
            {
                if (m_cursor.line != m_completion.Anchor().line || prefix.IsEmpty())
                {
                    m_completion.Close();
                }
                else
                {
                    m_completion.Filter(prefix);
                }
            }
            else if (static_cast<i32>(Utf8Length(prefix)) >= AutoCompleteMinPrefix)
            {
                OpenCompletion(false);
            }
            e.Handled = true;
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetFocus(this);
            }
            m_completion.Close();
            if (e.Button != MouseButton::Left)
            {
                return;
            }

            // Marker-margin click toggles a breakpoint on that line.
            if (ShowGutter && AllowBreakpoints && e.X < kMarkerMargin)
            {
                const i32 line =
                    static_cast<i32>((e.Y + m_scrollY - kPadTop) / LineHeight());
                if (line >= 0 && line < m_doc.LineCount())
                {
                    const bool set = m_doc.ToggleMarker(line, CodeMarkers::Breakpoint);
                    OnBreakpointToggled.Invoke(line, set);
                    Invalidate();
                }
                e.Handled = true;
                return;
            }

            const CodePosition pos = PositionAt(e.X, e.Y);
            if (e.ClickCount >= 3)
            {
                m_anchor = CodePosition{pos.line, 0};
                m_cursor = pos.line + 1 < m_doc.LineCount()
                               ? CodePosition{pos.line + 1, 0}
                               : m_doc.EndPosition();
            }
            else if (e.ClickCount == 2)
            {
                const CodeSpan word = m_doc.WordAt(pos);
                m_anchor = word.begin;
                m_cursor = word.end;
            }
            else
            {
                if (!HasFlag(e.Modifiers, KeyModifiers::Shift))
                {
                    m_anchor = pos;
                }
                m_cursor = pos;
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
            }
            m_desiredColumn = -1;
            ResetBlink();
            Invalidate();
            e.Handled = true;
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            m_lastHover = Float2{e.X, e.Y}; // the diagnostics tooltip reads the hovered line
            if (!m_dragging)
            {
                return;
            }
            m_cursor = PositionAt(e.X, e.Y);
            m_pendingCursorScroll = true;
            Invalidate();
            e.Handled = true;
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_dragging)
            {
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }

        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            const bool horizontal = HasFlag(e.Modifiers, KeyModifiers::Shift);
            if (horizontal)
            {
                m_scrollX -= e.DeltaY * ColumnAdvance() * 6.0f;
            }
            else
            {
                m_scrollY -= e.DeltaY * LineHeight() * 3.0f;
            }
            ClampScroll();
            Invalidate();
            e.Handled = true;
        }

        void OnFocusGained() override { ResetBlink(); }
        void OnFocusLost() override
        {
            m_dragging = false;
            m_completion.Close();
            m_doc.BreakUndoChain();
            Invalidate();
        }

        // ---- layout ----

        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(480.0f),
                                  constraints.ConstrainHeight(320.0f)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            RefreshMetrics();
            RefreshMaxLine();

            const f32 barSize = m_vBar->BarThickness;
            const bool needV = ContentHeight() > height;
            const bool needH = ContentWidth() > width - (needV ? barSize : 0.0f) - GutterWidth();
            m_viewportH = height - (needH ? barSize : 0.0f);
            m_viewportW = width - (needV ? barSize : 0.0f);

            if (m_pendingCursorScroll)
            {
                EnsureCursorVisible();
                m_pendingCursorScroll = false;
            }
            ClampScroll();

            if (Context != nullptr && m_vBar->Context == nullptr)
            {
                Context->AttachView(m_vBar.Get());
            }
            if (Context != nullptr && m_hBar->Context == nullptr)
            {
                Context->AttachView(m_hBar.Get());
            }

            m_vBar->Visibility = needV ? VisibilityValue::Visible : VisibilityValue::Gone;
            m_hBar->Visibility = needH ? VisibilityValue::Visible : VisibilityValue::Gone;
            // Max BEFORE value: SetValue clamps against the bar's current max AND fires
            // OnValueChanged on a clamp - with a stale (smaller) max it would snap m_scrollY
            // back a line right after EnsureCursorVisible advanced it (the half-hidden
            // caret-line bug from the first smoke run).
            if (needV)
            {
                m_vBar->SetMaxValue(MaxScrollY());
                m_vBar->SetViewportSize(m_viewportH);
                m_vBar->SetValue(m_scrollY);
                m_vBar->Measure(BoxConstraints::Tight(barSize, m_viewportH));
                m_vBar->Layout(width - barSize, 0, barSize, m_viewportH);
            }
            if (needH)
            {
                m_hBar->SetMaxValue(MaxScrollX());
                m_hBar->SetViewportSize(m_viewportW - GutterWidth());
                m_hBar->SetValue(m_scrollX);
                m_hBar->Measure(BoxConstraints::Tight(m_viewportW, barSize));
                m_hBar->Layout(0, height - barSize, m_viewportW, barSize);
            }

            // The find bar floats top-right over the text (a logical child; DrawChildren
            // paints it above the content).
            if (m_findBar.Get() != nullptr && m_findBarMode != FindBarMode::Closed)
            {
                m_findBar->Measure(BoxConstraints::Loose(width - GutterWidth() - 16.0f, height));
                const f32 barWidth = m_findBar->MeasuredSize.x;
                const f32 barHeight = m_findBar->MeasuredSize.y;
                const f32 barX = Max(GutterWidth(), width - barWidth - (needV ? barSize : 0.0f) - 6.0f);
                m_findBar->Layout(barX, 2.0f, barWidth, barHeight);
                m_findBarFrame = Rectangle{barX, 2.0f, barWidth, barHeight};
            }
        }

        [[nodiscard]] usize VisualChildCount() const override { return ChildCount() + 2; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            if (index < ChildCount())
            {
                return ViewGroup::GetVisualChild(index);
            }
            if (index == ChildCount())
            {
                return m_vBar.Get();
            }
            if (index == ChildCount() + 1)
            {
                return m_hBar.Get();
            }
            return nullptr;
        }

        // ---- drawing ----

        void OnDraw(UIDrawContext& ctx) override
        {
            RefreshMetrics(ctx.FontService());
            fonts::CachedFont* font =
                ctx.FontService() != nullptr
                    ? ctx.FontService()->GetFont(FontFamily.AsView(), FontSize)
                    : nullptr;

            const f32 width = Width();
            const f32 height = Height();
            const f32 lineH = LineHeight();
            const f32 advance = ColumnAdvance();
            const f32 gutterW = GutterWidth();
            const f32 textLeft = gutterW + kPadLeft - m_scrollX;
            const f32 ascent = font != nullptr ? font->font->Metrics().ascent : lineH * 0.8f;

            const Color background =
                ResolveStyleColor(StyleProperty::Background, Rgb(26, 28, 34));
            const Color textColor =
                ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235));
            const Color dimColor =
                ResolveStyleColor(StyleProperty::TextDimColor, Rgb(120, 128, 144));
            const Color selectionColor =
                ResolveStyleColor(StyleProperty::SelectionColor, Rgb(60, 120, 200, 80));
            const Color cursorColor =
                ResolveStyleColor(StyleProperty::CursorColor, Rgb(220, 225, 235));
            const Color accent = ResolveStyleColor(StyleProperty::AccentColor, Rgb(230, 140, 60));
            // Semantic severity colors (gutter markers, error-line tint) - themed since the
            // consistency pass; the old literals stay as unthemed fallbacks.
            const Color errorColor =
                ResolveStyleColor(StyleProperty::ErrorColor, Rgb(214, 80, 80));
            const Color warningColor =
                ResolveStyleColor(StyleProperty::WarningColor, Rgb(240, 200, 90));

            ctx.VG().FillRect(Rectangle{0, 0, width, height}, background);

            const i32 firstLine =
                Max(0, static_cast<i32>((m_scrollY - kPadTop) / lineH));
            const i32 lastLine = Min(m_doc.LineCount() - 1,
                                     static_cast<i32>((m_scrollY + m_viewportH) / lineH) + 1);
            const CodeSpan selection = Selection();
            if (m_highlighter.HasLexer())
            {
                m_highlighter.EnsureLexed(m_doc, lastLine);
            }

            // Text region, CLIPPED to the right of the gutter: horizontally scrolled text
            // must never bleed under the gutter (first smoke-run finding). The gutter itself
            // is painted afterwards, on top.
            ctx.PushClip(Rectangle{gutterW, 0, width - gutterW, height});
            for (i32 line = firstLine; line <= lastLine; ++line)
            {
                const f32 lineTop = kPadTop + static_cast<f32>(line) * lineH - m_scrollY;
                const CodeMarkers markers = m_doc.MarkersOn(line);

                // Row highlights: execution line wins over the quiet current-line tint.
                if (HasMarker(markers, CodeMarkers::ExecutionLine))
                {
                    ctx.VG().FillRect(Rectangle{gutterW, lineTop, width - gutterW, lineH},
                                      WithAlpha(accent, 0.18f));
                }
                else if (line == m_cursor.line && !HasSelection() && IsFocused())
                {
                    ctx.VG().FillRect(Rectangle{gutterW, lineTop, width - gutterW, lineH},
                                      WithAlpha(textColor, 0.05f));
                }
                if (HasMarker(markers, CodeMarkers::Error))
                {
                    ctx.VG().FillRect(
                        Rectangle{gutterW, lineTop, width - gutterW, lineH},
                        Color{errorColor.r, errorColor.g, errorColor.b, 26.0f / 255.0f});
                }

                // Search matches (under the selection band; the current one pops).
                for (usize m = 0; m < m_matches.Size(); ++m)
                {
                    const CodeSpan& match = m_matches[m];
                    if (match.begin.line != line)
                    {
                        continue;
                    }
                    const bool current = static_cast<i32>(m) == m_currentMatch;
                    ctx.VG().FillRect(
                        Rectangle{textLeft + static_cast<f32>(match.begin.column) * advance,
                                  lineTop,
                                  static_cast<f32>(match.end.column - match.begin.column) *
                                      advance,
                                  lineH},
                        current ? WithAlpha(accent, 0.45f) : WithAlpha(accent, 0.18f));
                }

                // Selection band(s).
                if (HasSelection() && line >= selection.begin.line && line <= selection.end.line)
                {
                    const i32 fromCol =
                        line == selection.begin.line ? selection.begin.column : 0;
                    const f32 toCol =
                        line == selection.end.line
                            ? static_cast<f32>(selection.end.column)
                            : static_cast<f32>(m_doc.LineLength(line)) + 0.4f; // show the newline
                    ctx.VG().FillRect(
                        Rectangle{textLeft + static_cast<f32>(fromCol) * advance, lineTop,
                                  (toCol - static_cast<f32>(fromCol)) * advance, lineH},
                        selectionColor);
                }

                // The text itself: styled token runs when a lexer is set, else one draw.
                if (font != nullptr && !m_doc.Line(line).IsEmpty())
                {
                    const Span<const CodeToken> tokens = m_highlighter.TokensFor(line);
                    if (m_highlighter.HasLexer() && tokens.Size() > 0)
                    {
                        const StringView text = m_doc.Line(line);
                        for (usize t = 0; t < tokens.Size(); ++t)
                        {
                            const CodeToken& token = tokens[t];
                            ctx.VG().DrawText(
                                text.SubStr(token.byteBegin, token.byteEnd - token.byteBegin),
                                font,
                                Float2{textLeft + static_cast<f32>(token.column) * advance,
                                       lineTop + ascent},
                                TokenColors.For(token.kind, textColor));
                        }
                    }
                    else if (!m_highlighter.HasLexer())
                    {
                        ctx.VG().DrawText(m_doc.Line(line), font,
                                          Float2{textLeft, lineTop + ascent}, textColor);
                    }
                }
            }

            // Caret (clipped with the text region).
            if (IsFocused() && !ReadOnly)
            {
                const f32 elapsed =
                    (Context != nullptr ? Context->TotalTime() : 0.0f) - m_blinkReset;
                if ((static_cast<i32>(elapsed / 0.5f) % 2) == 0)
                {
                    const f32 caretX =
                        textLeft + static_cast<f32>(m_cursor.column) * advance;
                    const f32 caretY =
                        kPadTop + static_cast<f32>(m_cursor.line) * lineH - m_scrollY;
                    ctx.VG().FillRect(Rectangle{caretX - 1.0f, caretY, 2.0f, lineH}, cursorColor);
                }
            }

            // Matching-bracket boxes (the pair adjacent to the caret).
            RefreshBracketMatch();
            if (m_bracketValid)
            {
                const CodePosition brackets[2] = {m_bracketA, m_bracketB};
                for (const CodePosition& bracket : brackets)
                {
                    if (bracket.line < firstLine || bracket.line > lastLine)
                    {
                        continue;
                    }
                    const f32 x = textLeft + static_cast<f32>(bracket.column) * advance;
                    const f32 y =
                        kPadTop + static_cast<f32>(bracket.line) * lineH - m_scrollY;
                    ctx.VG().BeginPath();
                    ctx.VG().MoveTo(x - 1.0f, y);
                    ctx.VG().LineTo(x + advance + 1.0f, y);
                    ctx.VG().LineTo(x + advance + 1.0f, y + lineH);
                    ctx.VG().LineTo(x - 1.0f, y + lineH);
                    ctx.VG().LineTo(x - 1.0f, y);
                    ctx.VG().Stroke(WithAlpha(accent, 0.7f), 1.0f);
                }
            }
            ctx.PopClip();

            // The gutter: painted AFTER (over) the text region so nothing bleeds into it.
            if (ShowGutter)
            {
                ctx.VG().FillRect(Rectangle{0, 0, gutterW, height},
                                  Palette::Darken(background, 0.25f));
                for (i32 line = firstLine; line <= lastLine; ++line)
                {
                    const f32 lineTop = kPadTop + static_cast<f32>(line) * lineH - m_scrollY;
                    const CodeMarkers markers = m_doc.MarkersOn(line);
                    const f32 centerY = lineTop + lineH * 0.5f;
                    if (HasMarker(markers, CodeMarkers::Breakpoint))
                    {
                        ctx.VG().FillCircle(Float2{kMarkerMargin * 0.5f, centerY}, 4.5f,
                                            errorColor);
                    }
                    if (HasMarker(markers, CodeMarkers::ExecutionLine))
                    {
                        // Right-pointing arrow in the margin.
                        ctx.VG().BeginPath();
                        ctx.VG().MoveTo(kMarkerMargin * 0.5f - 4.0f, centerY - 4.5f);
                        ctx.VG().LineTo(kMarkerMargin * 0.5f + 4.0f, centerY);
                        ctx.VG().LineTo(kMarkerMargin * 0.5f - 4.0f, centerY + 4.5f);
                        ctx.VG().Stroke(warningColor, 2.0f);
                    }
                    else if (HasMarker(markers, CodeMarkers::Error) ||
                             HasMarker(markers, CodeMarkers::Warning))
                    {
                        const Color c = HasMarker(markers, CodeMarkers::Error)
                                            ? errorColor
                                            : warningColor;
                        ctx.VG().FillRect(
                            Rectangle{kMarkerMargin - 5.0f, centerY - 3.5f, 7.0f, 7.0f}, c);
                    }
                    if (ShowLineNumbers && font != nullptr)
                    {
                        char buffer[16];
                        const int n = std::snprintf(buffer, sizeof(buffer), "%d", line + 1);
                        const StringView number(reinterpret_cast<const char8_t*>(buffer),
                                                n > 0 ? static_cast<usize>(n) : 0u);
                        const f32 numberRight = gutterW - kGutterGap;
                        const f32 numberX =
                            numberRight - static_cast<f32>(number.Size()) * advance;
                        const Color numberColor =
                            line == m_cursor.line ? textColor : dimColor;
                        ctx.VG().DrawText(number, font, Float2{numberX, lineTop + ascent},
                                          numberColor);
                    }
                }
            }

            // Backdrop behind the floating find bar (its controls are drawn by DrawChildren).
            if (m_findBarMode != FindBarMode::Closed && m_findBar.Get() != nullptr)
            {
                ctx.VG().FillRect(m_findBarFrame, Palette::Lighten(background, 0.06f));
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(m_findBarFrame.x, m_findBarFrame.y);
                ctx.VG().LineTo(m_findBarFrame.x + m_findBarFrame.width, m_findBarFrame.y);
                ctx.VG().LineTo(m_findBarFrame.x + m_findBarFrame.width,
                                m_findBarFrame.y + m_findBarFrame.height);
                ctx.VG().LineTo(m_findBarFrame.x, m_findBarFrame.y + m_findBarFrame.height);
                ctx.VG().LineTo(m_findBarFrame.x, m_findBarFrame.y);
                ctx.VG().Stroke(WithAlpha(textColor, 0.25f), 1.0f);
            }

            DrawChildren(ctx); // scrollbars + find bar

            if (m_completion.IsOpen())
            {
                DrawCompletionPopup(ctx, font, textLeft, lineH, advance, ascent, background,
                                    textColor, accent);
            }
        }

    private:
        static constexpr f32 kPadTop = 4.0f;
        static constexpr f32 kPadLeft = 6.0f;
        static constexpr f32 kMarkerMargin = 18.0f;
        static constexpr f32 kNumberPad = 4.0f;
        static constexpr f32 kGutterGap = 8.0f;
        static constexpr i32 kPopupMaxVisible = 8;

        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                         static_cast<f32>(b) / 255.0f, static_cast<f32>(a) / 255.0f};
        }
        [[nodiscard]] static Color WithAlpha(Color c, f32 a) noexcept
        {
            c.a = a;
            return c;
        }

        [[nodiscard]] f32 Now() const noexcept
        {
            return Context != nullptr ? Context->TotalTime() : 0.0f;
        }
        void ResetBlink() noexcept { m_blinkReset = Now(); }

        // ---- metrics ----

        void RefreshMetrics() { RefreshMetrics(Context != nullptr ? Context->FontService() : nullptr); }
        void RefreshMetrics(fonts::IFontService* service)
        {
            if (service == nullptr)
            {
                return;
            }
            fonts::CachedFont* font = service->GetFont(FontFamily.AsView(), FontSize);
            if (font == nullptr || font->font == nullptr)
            {
                return;
            }
            m_lineHeight = font->font->Metrics().lineHeight;
            const fonts::GlyphInfo info = font->font->GetGlyphInfo('M');
            m_advance = info.advanceWidth > 0.0f ? info.advanceWidth
                                                 : font->font->MeasureString(StringView(u8"M"));
        }

        void RefreshMaxLine()
        {
            if (!m_maxLineDirty)
            {
                return;
            }
            m_maxLineLength = 0;
            for (i32 line = 0; line < m_doc.LineCount(); ++line)
            {
                m_maxLineLength = Max(m_maxLineLength, m_doc.LineLength(line));
            }
            m_maxLineDirty = false;
        }

        [[nodiscard]] f32 ContentHeight() const
        {
            return kPadTop * 2.0f + static_cast<f32>(m_doc.LineCount()) * LineHeight();
        }
        [[nodiscard]] f32 ContentWidth() const
        {
            return kPadLeft * 2.0f + static_cast<f32>(m_maxLineLength + 1) * ColumnAdvance();
        }
        [[nodiscard]] f32 MaxScrollY() const
        {
            return Max(0.0f, ContentHeight() - m_viewportH);
        }
        [[nodiscard]] f32 MaxScrollX() const
        {
            return Max(0.0f, ContentWidth() - (m_viewportW - GutterWidth()));
        }
        void ClampScroll()
        {
            m_scrollY = Clamp(m_scrollY, 0.0f, MaxScrollY());
            m_scrollX = Clamp(m_scrollX, 0.0f, MaxScrollX());
        }

        void EnsureCursorVisible()
        {
            const f32 lineH = LineHeight();
            const f32 caretTop = kPadTop + static_cast<f32>(m_cursor.line) * lineH;
            if (caretTop - kPadTop < m_scrollY)
            {
                m_scrollY = caretTop - kPadTop;
            }
            else if (caretTop + lineH > m_scrollY + m_viewportH)
            {
                m_scrollY = caretTop + lineH - m_viewportH;
            }
            const f32 caretX = static_cast<f32>(m_cursor.column) * ColumnAdvance();
            const f32 textViewportW = m_viewportW - GutterWidth() - kPadLeft * 2.0f;
            if (caretX < m_scrollX)
            {
                m_scrollX = Max(0.0f, caretX - ColumnAdvance() * 4.0f);
            }
            else if (caretX > m_scrollX + textViewportW)
            {
                m_scrollX = caretX - textViewportW + ColumnAdvance() * 4.0f;
            }
        }

        // ---- editing core ----

        void AfterEdit()
        {
            m_desiredColumn = -1;
            m_pendingCursorScroll = true;
            m_maxLineDirty = true;
            ResetBlink();
            if (m_findBarMode == FindBarMode::Find || m_findBarMode == FindBarMode::Replace)
            {
                RunSearch(); // spans shift under edits
            }
            Invalidate();
            OnTextChanged.Invoke();
        }

        void InsertText(StringView text, CodeEditKind kind)
        {
            const CodeCursorState before{m_cursor, m_anchor};
            m_cursor = m_doc.Edit(Selection(), text, kind, before, Now());
            m_anchor = m_cursor;
            AfterEdit();
        }

        void DeleteSpan(const CodeSpan& span, CodeEditKind kind)
        {
            const CodeCursorState before{m_cursor, m_anchor};
            m_cursor = m_doc.Edit(span, StringView(u8""), kind, before, Now());
            m_anchor = m_cursor;
            AfterEdit();
        }

        [[nodiscard]] CodePosition LeftOf(CodePosition pos) const
        {
            if (pos.column > 0)
            {
                return CodePosition{pos.line, pos.column - 1};
            }
            if (pos.line > 0)
            {
                return CodePosition{pos.line - 1, m_doc.LineLength(pos.line - 1)};
            }
            return pos;
        }
        [[nodiscard]] CodePosition RightOf(CodePosition pos) const
        {
            if (pos.column < m_doc.LineLength(pos.line))
            {
                return CodePosition{pos.line, pos.column + 1};
            }
            if (pos.line + 1 < m_doc.LineCount())
            {
                return CodePosition{pos.line + 1, 0};
            }
            return pos;
        }

        void MoveCursor(CodePosition pos, bool extendSelection)
        {
            m_cursor = m_doc.ClampPosition(pos);
            if (!extendSelection)
            {
                m_anchor = m_cursor;
            }
            m_doc.BreakUndoChain();
            m_completion.Close();
            m_pendingCursorScroll = true;
            ResetBlink();
            Invalidate();
        }

        [[nodiscard]] i32 FirstNonSpaceColumn(i32 line) const
        {
            const StringView text = m_doc.Line(line);
            i32 column = 0;
            usize i = 0;
            while (i < text.Size() && (text[i] == u8' ' || text[i] == u8'\t'))
            {
                ++i;
                ++column;
            }
            return column;
        }

        /// The identifier fragment immediately left of the cursor (completion prefix).
        [[nodiscard]] StringView PrefixView() const
        {
            const CodeSpan word = m_doc.WordAt(m_cursor);
            if (word.IsEmpty() || word.begin.line != m_cursor.line ||
                word.begin.column >= m_cursor.column)
            {
                return StringView();
            }
            const usize fromByte = m_doc.ColumnToByte(m_cursor.line, word.begin.column);
            const usize toByte = m_doc.ColumnToByte(m_cursor.line, m_cursor.column);
            return m_doc.Line(m_cursor.line).SubStr(fromByte, toByte - fromByte);
        }

        void OpenCompletion(bool explicitRequest)
        {
            const StringView prefix = PrefixView();
            if (!explicitRequest && prefix.IsEmpty())
            {
                return;
            }
            Array<CompletionCandidate> merged;
            if (DocumentWordCompletion)
            {
                m_wordProvider.Collect(m_doc, m_cursor, prefix, merged);
            }
            for (usize i = 0; i < m_providers.Size(); ++i)
            {
                m_providers[i]->Collect(m_doc, m_cursor, prefix, merged);
            }
            DedupeAndSort(merged);
            if (merged.IsEmpty())
            {
                m_completion.Close();
                return;
            }
            const CodePosition anchor{m_cursor.line,
                                      m_cursor.column - static_cast<i32>(Utf8Length(prefix))};
            m_completion.Open(anchor, Move(merged), prefix);
            Invalidate();
        }

        void AcceptCompletion()
        {
            const CompletionCandidate* candidate = m_completion.Selected();
            if (candidate == nullptr)
            {
                m_completion.Close();
                return;
            }
            const String insert = String(candidate->insertText.AsView());
            const CodeSpan replaced{m_completion.Anchor(), m_cursor};
            m_completion.Close();
            const CodeCursorState before{m_cursor, m_anchor};
            m_cursor = m_doc.Edit(replaced, insert.AsView(), CodeEditKind::Other, before, Now());
            m_anchor = m_cursor;
            AfterEdit();
        }

        static void DedupeAndSort(Array<CompletionCandidate>& items)
        {
            // Insertion sort by (priority, label) - context providers rank above document
            // words so member/attribute results are never buried below the fold. Dedupe by
            // label keeps the FIRST (highest-ranked) occurrence.
            const auto ranksBefore = [](const CompletionCandidate& a, const CompletionCandidate& b)
            {
                if (a.priority != b.priority)
                {
                    return a.priority < b.priority;
                }
                return Less(a.label.AsView(), b.label.AsView());
            };
            for (usize i = 1; i < items.Size(); ++i)
            {
                CompletionCandidate value = Move(items[i]);
                usize j = i;
                while (j > 0 && ranksBefore(value, items[j - 1]))
                {
                    items[j] = Move(items[j - 1]);
                    --j;
                }
                items[j] = Move(value);
            }
            // Priority-first ordering scatters equal labels, so dedupe scans all KEPT items
            // (lists are small; the first = highest-ranked occurrence wins).
            usize write = 0;
            for (usize i = 0; i < items.Size(); ++i)
            {
                bool seen = false;
                for (usize k = 0; k < write; ++k)
                {
                    if (items[k].label.AsView() == items[i].label.AsView())
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    if (write != i)
                    {
                        items[write] = Move(items[i]);
                    }
                    ++write;
                }
            }
            while (items.Size() > write)
            {
                items.PopBack();
            }
        }

        [[nodiscard]] static bool Less(StringView a, StringView b) noexcept
        {
            const usize n = Min(a.Size(), b.Size());
            for (usize i = 0; i < n; ++i)
            {
                if (a[i] != b[i])
                {
                    return a[i] < b[i];
                }
            }
            return a.Size() < b.Size();
        }

        // ---- key handling (returns true when the key was consumed) ----

        bool ProcessKey(KeyCode key, KeyModifiers mods)
        {
            const bool shift = HasFlag(mods, KeyModifiers::Shift);
            const bool ctrl = HasFlag(mods, KeyModifiers::Ctrl);
            const bool alt = HasFlag(mods, KeyModifiers::Alt);

            switch (key)
            {
                // -- navigation --
                case KeyCode::Left:
                    if (HasSelection() && !shift && !ctrl)
                    {
                        MoveCursor(Selection().begin, false);
                    }
                    else
                    {
                        MoveCursor(ctrl ? m_doc.PrevWordBoundary(m_cursor) : LeftOf(m_cursor),
                                   shift);
                    }
                    return true;
                case KeyCode::Right:
                    if (HasSelection() && !shift && !ctrl)
                    {
                        MoveCursor(Selection().end, false);
                    }
                    else
                    {
                        MoveCursor(ctrl ? m_doc.NextWordBoundary(m_cursor) : RightOf(m_cursor),
                                   shift);
                    }
                    return true;
                case KeyCode::Up:
                case KeyCode::Down:
                {
                    if (alt && !ReadOnly)
                    {
                        MoveLine(key == KeyCode::Down);
                        return true;
                    }
                    const i32 delta = key == KeyCode::Down ? 1 : -1;
                    const i32 targetLine = m_cursor.line + delta;
                    if (targetLine < 0 || targetLine >= m_doc.LineCount())
                    {
                        MoveCursor(delta < 0 ? CodePosition{0, 0} : m_doc.EndPosition(), shift);
                        return true;
                    }
                    if (m_desiredColumn < 0)
                    {
                        m_desiredColumn = m_cursor.column;
                    }
                    const i32 keepColumn = m_desiredColumn;
                    MoveCursor(CodePosition{targetLine, keepColumn}, shift);
                    m_desiredColumn = keepColumn; // MoveCursor clamps; keep the goal column
                    return true;
                }
                case KeyCode::Home:
                {
                    if (ctrl)
                    {
                        MoveCursor(CodePosition{0, 0}, shift);
                        return true;
                    }
                    // Smart home: first non-space, then hard column 0.
                    const i32 indent = FirstNonSpaceColumn(m_cursor.line);
                    MoveCursor(CodePosition{m_cursor.line,
                                            m_cursor.column == indent ? 0 : indent},
                               shift);
                    return true;
                }
                case KeyCode::End:
                    MoveCursor(ctrl ? m_doc.EndPosition()
                                    : CodePosition{m_cursor.line,
                                                   m_doc.LineLength(m_cursor.line)},
                               shift);
                    return true;
                case KeyCode::PageUp:
                case KeyCode::PageDown:
                {
                    const i32 page =
                        Max(1, static_cast<i32>(m_viewportH / LineHeight()) - 1);
                    const i32 delta = key == KeyCode::PageDown ? page : -page;
                    MoveCursor(CodePosition{m_cursor.line + delta, m_cursor.column}, shift);
                    return true;
                }

                // -- edits --
                case KeyCode::Return:
                    if (ReadOnly)
                    {
                        return false;
                    }
                    {
                        // Newline + copy the current line's leading whitespace; one extra
                        // indent step after an open brace (the language hook).
                        String insert;
                        insert.PushBack(u8'\n');
                        const StringView line = m_doc.Line(m_cursor.line);
                        const usize indentBytes =
                            m_doc.ColumnToByte(m_cursor.line,
                                               Min(FirstNonSpaceColumn(m_cursor.line),
                                                   m_cursor.column));
                        insert.Append(line.SubStr(0, indentBytes));
                        if (IndentAfterOpenBrace && m_cursor.column > 0 &&
                            m_doc.CodepointAt(CodePosition{m_cursor.line,
                                                           m_cursor.column - 1}) == u8'{')
                        {
                            for (i32 i = 0; i < TabWidth; ++i)
                            {
                                insert.PushBack(u8' ');
                            }
                        }
                        InsertText(insert.AsView(), CodeEditKind::Newline);
                    }
                    return true;
                case KeyCode::Backspace:
                    if (ReadOnly)
                    {
                        return false;
                    }
                    if (HasSelection())
                    {
                        DeleteSpan(Selection(), CodeEditKind::Backspace);
                    }
                    else
                    {
                        const CodePosition from =
                            ctrl ? m_doc.PrevWordBoundary(m_cursor) : LeftOf(m_cursor);
                        if (!(from == m_cursor))
                        {
                            DeleteSpan(CodeSpan{from, m_cursor}, CodeEditKind::Backspace);
                        }
                    }
                    if (m_completion.IsOpen())
                    {
                        const StringView prefix = PrefixView();
                        if (prefix.IsEmpty())
                        {
                            m_completion.Close();
                        }
                        else
                        {
                            m_completion.Filter(prefix);
                        }
                    }
                    return true;
                case KeyCode::Delete:
                    if (ReadOnly)
                    {
                        return false;
                    }
                    if (HasSelection())
                    {
                        DeleteSpan(Selection(), CodeEditKind::Delete);
                    }
                    else
                    {
                        const CodePosition to =
                            ctrl ? m_doc.NextWordBoundary(m_cursor) : RightOf(m_cursor);
                        if (!(to == m_cursor))
                        {
                            DeleteSpan(CodeSpan{m_cursor, to}, CodeEditKind::Delete);
                        }
                    }
                    return true;
                case KeyCode::Tab:
                    if (ReadOnly)
                    {
                        return false;
                    }
                    HandleTab(shift);
                    return true;
                case KeyCode::Escape:
                    if (m_findBarMode != FindBarMode::Closed)
                    {
                        CloseFindBar();
                        return true;
                    }
                    if (HasSelection())
                    {
                        m_anchor = m_cursor;
                        Invalidate();
                        return true;
                    }
                    return false;

                case KeyCode::F3:
                    if (shift)
                    {
                        FindPrevious();
                    }
                    else
                    {
                        FindNext();
                    }
                    return true;
                case KeyCode::F:
                    if (ctrl)
                    {
                        OpenFindBar(false);
                        return true;
                    }
                    return false;
                case KeyCode::H:
                    if (ctrl)
                    {
                        OpenFindBar(!ReadOnly);
                        return true;
                    }
                    return false;
                case KeyCode::G:
                    if (ctrl)
                    {
                        OpenGoToLine();
                        return true;
                    }
                    return false;
                case KeyCode::Slash:
                    if (ctrl && !ReadOnly)
                    {
                        ToggleLineComment();
                        return true;
                    }
                    return false;

                // -- chords --
                case KeyCode::A:
                    if (ctrl)
                    {
                        SelectAll();
                        return true;
                    }
                    return false;
                case KeyCode::C:
                    if (ctrl)
                    {
                        CopySelection(false);
                        return true;
                    }
                    return false;
                case KeyCode::X:
                    if (ctrl)
                    {
                        CopySelection(!ReadOnly);
                        return true;
                    }
                    return false;
                case KeyCode::V:
                    if (ctrl && !ReadOnly)
                    {
                        Paste();
                        return true;
                    }
                    return false;
                case KeyCode::Z:
                    if (ctrl && !ReadOnly)
                    {
                        CodeCursorState state;
                        if (shift ? m_doc.Redo(state) : m_doc.Undo(state))
                        {
                            m_cursor = m_doc.ClampPosition(state.cursor);
                            m_anchor = m_doc.ClampPosition(state.anchor);
                            AfterEdit();
                        }
                        return true;
                    }
                    return false;
                case KeyCode::Y:
                    if (ctrl && !ReadOnly)
                    {
                        CodeCursorState state;
                        if (m_doc.Redo(state))
                        {
                            m_cursor = m_doc.ClampPosition(state.cursor);
                            m_anchor = m_doc.ClampPosition(state.anchor);
                            AfterEdit();
                        }
                        return true;
                    }
                    return false;
                case KeyCode::D:
                    if (ctrl && !ReadOnly)
                    {
                        DuplicateLine();
                        return true;
                    }
                    return false;
                case KeyCode::Space:
                    if (ctrl)
                    {
                        OpenCompletion(true);
                        return true;
                    }
                    return false;

                default:
                    return false;
            }
        }

        void HandleTab(bool dedent)
        {
            const CodeSpan selection = Selection();
            const bool multiLine = HasSelection() && selection.begin.line != selection.end.line;
            if (!multiLine && !dedent)
            {
                // Spaces to the next tab stop.
                String spaces;
                const i32 count = TabWidth - (m_cursor.column % TabWidth);
                for (i32 i = 0; i < count; ++i)
                {
                    spaces.PushBack(u8' ');
                }
                InsertText(spaces.AsView(), CodeEditKind::Typing);
                return;
            }

            // Indent/dedent every touched line as ONE undoable replacement.
            const i32 firstLine = selection.begin.line;
            i32 lastLine = selection.end.line;
            if (multiLine && selection.end.column == 0)
            {
                --lastLine; // selection ending at column 0 does not touch that line
            }
            String replacement;
            for (i32 line = firstLine; line <= lastLine; ++line)
            {
                if (line > firstLine)
                {
                    replacement.PushBack(u8'\n');
                }
                const StringView text = m_doc.Line(line);
                if (dedent)
                {
                    usize drop = 0;
                    while (drop < static_cast<usize>(TabWidth) && drop < text.Size() &&
                           text[drop] == u8' ')
                    {
                        ++drop;
                    }
                    replacement.Append(text.SubStr(drop, text.Size() - drop));
                }
                else
                {
                    for (i32 i = 0; i < TabWidth; ++i)
                    {
                        replacement.PushBack(u8' ');
                    }
                    replacement.Append(text);
                }
            }
            const CodeSpan lineSpan{CodePosition{firstLine, 0},
                                    CodePosition{lastLine, m_doc.LineLength(lastLine)}};
            const CodeCursorState before{m_cursor, m_anchor};
            (void)m_doc.Edit(lineSpan, replacement.AsView(), CodeEditKind::Other, before, Now());
            m_anchor = CodePosition{firstLine, 0};
            m_cursor = CodePosition{lastLine, m_doc.LineLength(lastLine)};
            AfterEdit();
        }

        void DuplicateLine()
        {
            const i32 line = m_cursor.line;
            String insert;
            insert.PushBack(u8'\n');
            insert.Append(m_doc.Line(line));
            const CodePosition lineEnd{line, m_doc.LineLength(line)};
            const CodeCursorState before{m_cursor, m_anchor};
            (void)m_doc.Edit(CodeSpan{lineEnd, lineEnd}, insert.AsView(), CodeEditKind::Other,
                             before, Now());
            m_cursor = CodePosition{line + 1, m_cursor.column};
            m_anchor = m_cursor;
            AfterEdit();
        }

        void MoveLine(bool down)
        {
            const i32 line = m_cursor.line;
            const i32 other = down ? line + 1 : line - 1;
            if (other < 0 || other >= m_doc.LineCount())
            {
                return;
            }
            const i32 first = Min(line, other);
            String swapped;
            swapped.Append(m_doc.Line(first + 1));
            swapped.PushBack(u8'\n');
            swapped.Append(m_doc.Line(first));
            const CodeSpan span{CodePosition{first, 0},
                                CodePosition{first + 1, m_doc.LineLength(first + 1)}};
            const CodeCursorState before{m_cursor, m_anchor};
            (void)m_doc.Edit(span, swapped.AsView(), CodeEditKind::Other, before, Now());
            m_cursor = CodePosition{other, m_cursor.column};
            m_anchor = m_cursor;
            AfterEdit();
        }

        void CopySelection(bool cut)
        {
            if (!HasSelection() || Context == nullptr || Context->Clipboard() == nullptr)
            {
                return;
            }
            const String text = SelectedText();
            (void)Context->Clipboard()->SetText(text.AsView());
            if (cut)
            {
                DeleteSpan(Selection(), CodeEditKind::Other);
            }
        }

        void Paste()
        {
            if (Context == nullptr || Context->Clipboard() == nullptr)
            {
                return;
            }
            String text;
            if (!Context->Clipboard()->GetText(text).IsOk() || text.IsEmpty())
            {
                return;
            }
            InsertText(text.AsView(), CodeEditKind::Paste);
        }

        // ---- popup rendering ----

        void DrawCompletionPopup(UIDrawContext& ctx, fonts::CachedFont* font, f32 textLeft,
                                 f32 lineH, f32 advance, f32 ascent, Color background,
                                 Color textColor, Color accent)
        {
            const i32 count = Min(m_completion.ItemCount(), kPopupMaxVisible);
            if (count <= 0)
            {
                return;
            }
            usize maxLabel = 8;
            for (i32 i = 0; i < m_completion.ItemCount(); ++i)
            {
                maxLabel = Max(maxLabel, Utf8Length(m_completion.Item(i)->label.AsView()));
            }
            const f32 popupW =
                Clamp(static_cast<f32>(maxLabel) * advance + 16.0f, 140.0f, 380.0f);
            const f32 popupH = static_cast<f32>(count) * lineH + 6.0f;

            const CodePosition anchor = m_completion.Anchor();
            f32 x = textLeft + static_cast<f32>(anchor.column) * advance - 4.0f;
            f32 y = kPadTop + static_cast<f32>(anchor.line + 1) * lineH - m_scrollY + 2.0f;
            if (y + popupH > Height())
            {
                y = kPadTop + static_cast<f32>(anchor.line) * lineH - m_scrollY - popupH - 2.0f;
            }
            x = Clamp(x, 0.0f, Max(0.0f, Width() - popupW));

            const Color popupBg = Palette::Lighten(background, 0.08f);
            ctx.VG().FillRect(Rectangle{x, y, popupW, popupH}, popupBg);
            ctx.VG().BeginPath();
            ctx.VG().MoveTo(x, y);
            ctx.VG().LineTo(x + popupW, y);
            ctx.VG().LineTo(x + popupW, y + popupH);
            ctx.VG().LineTo(x, y + popupH);
            ctx.VG().LineTo(x, y);
            ctx.VG().Stroke(WithAlpha(textColor, 0.25f), 1.0f);

            // Keep the selected row inside the visible window.
            i32 firstItem = 0;
            if (m_completion.SelectedIndex() >= count)
            {
                firstItem = m_completion.SelectedIndex() - count + 1;
            }
            for (i32 i = 0; i < count; ++i)
            {
                const i32 index = firstItem + i;
                const CompletionCandidate* item = m_completion.Item(index);
                if (item == nullptr)
                {
                    break;
                }
                const f32 rowY = y + 3.0f + static_cast<f32>(i) * lineH;
                if (index == m_completion.SelectedIndex())
                {
                    ctx.VG().FillRect(Rectangle{x + 1.0f, rowY, popupW - 2.0f, lineH},
                                      WithAlpha(accent, 0.3f));
                }
                if (font != nullptr)
                {
                    ctx.VG().DrawText(item->label.AsView(), font,
                                      Float2{x + 8.0f, rowY + ascent}, textColor);
                }
            }

            // Overflow indicator: a slim proportional thumb on the right edge, so a list
            // longer than the visible window is evident (the popup is keyboard-driven; the
            // strip is informational, tracking the selection window as arrows scroll it).
            const i32 total = m_completion.ItemCount();
            if (total > count)
            {
                const f32 trackX = x + popupW - 4.0f;
                const f32 trackTop = y + 2.0f;
                const f32 trackHeight = popupH - 4.0f;
                ctx.VG().FillRect(Rectangle{trackX, trackTop, 3.0f, trackHeight},
                                  WithAlpha(textColor, 0.08f));
                const f32 thumbHeight =
                    Max(8.0f, trackHeight * static_cast<f32>(count) / static_cast<f32>(total));
                const f32 thumbTravel = trackHeight - thumbHeight;
                const f32 thumbY =
                    trackTop + thumbTravel * static_cast<f32>(firstItem) /
                                   static_cast<f32>(Max(1, total - count));
                ctx.VG().FillRect(Rectangle{trackX, thumbY, 3.0f, thumbHeight},
                                  WithAlpha(textColor, 0.35f));
            }
        }

        // ---- find bar machinery ----

        void EnsureFindBar()
        {
            if (m_findBar.Get() != nullptr)
            {
                return;
            }
            CodeEditView* self = this;
            // A vertical stack of two horizontal rows: [find | count | < > aa w | x] over
            // [replace | Replace | All] (the second row shows only in Replace mode).
            m_findBar = MakeRef<FlexLayout>(MemoryAllocator());
            m_findBar->Direction = Orientation::Vertical;
            m_findBar->Spacing = 3.0f;
            m_findBar->Padding = Thickness{6, 4};

            m_findRow = MakeRef<FlexLayout>(MemoryAllocator());
            m_findRow->Direction = Orientation::Horizontal;
            m_findRow->Spacing = 4.0f;
            m_replaceRow = MakeRef<FlexLayout>(MemoryAllocator());
            m_replaceRow->Direction = Orientation::Horizontal;
            m_replaceRow->Spacing = 4.0f;

            m_findField = MakeRef<EditText>(MemoryAllocator());
            m_findField->SetPlaceholder(u8"Find");
            m_findField->OnTextChanged.Add(
                [self](EditText*)
                {
                    if (self->m_findBarMode == FindBarMode::Find ||
                        self->m_findBarMode == FindBarMode::Replace)
                    {
                        self->RunSearch();
                        if (self->m_currentMatch >= 0)
                        {
                            self->SelectMatch(self->m_currentMatch);
                        }
                    }
                });
            {
                LayoutStyle params;
                params.Width = SizeSpec::Fixed(Unit::Dp(170));
                m_findRow->AddView(m_findField.Get(), params);
            }

            m_matchLabel = MakeRef<Label>(MemoryAllocator(), StringView(u8""));
            m_matchLabel->FontSize.SetValue(12.0f);
            m_findRow->AddView(m_matchLabel.Get());

            const auto addButton = [&](FlexLayout& row, RefPtr<Button>& slot,
                                       const char8_t* text, Function<void()> action)
            {
                slot = MakeRef<Button>(MemoryAllocator(), StringView(text));
                slot->FontSize.SetValue(Optional<f32>(12.0f));
                Function<void()> stored = Move(action);
                slot->OnClick.Add([stored = Move(stored)](ButtonBase*) { stored(); });
                row.AddView(slot.Get());
            };
            addButton(*m_findRow, m_prevButton, u8"<", [self] { self->FindPrevious(); });
            addButton(*m_findRow, m_nextButton, u8">", [self] { self->FindNext(); });
            // Real toggle controls (checked state is themed) for the search options.
            const auto addToggle = [&](RefPtr<ToggleButton>& slot, const char8_t* text,
                                       Function<void(bool)> action)
            {
                slot = MakeRef<ToggleButton>(MemoryAllocator(), StringView(text));
                Function<void(bool)> stored = Move(action);
                slot->OnCheckedChanged.Add([stored = Move(stored)](ToggleButton*, bool checked)
                                           { stored(checked); });
                m_findRow->AddView(slot.Get());
            };
            addToggle(m_caseButton, u8"Aa",
                      [self](bool checked)
                      {
                          self->m_searchCaseSensitive = checked;
                          self->RunSearch();
                      });
            addToggle(m_wordButton, u8"W",
                      [self](bool checked)
                      {
                          self->m_searchWholeWord = checked;
                          self->RunSearch();
                      });
            addButton(*m_findRow, m_closeButton, u8"x", [self] { self->CloseFindBar(); });

            m_replaceField = MakeRef<EditText>(MemoryAllocator());
            m_replaceField->SetPlaceholder(u8"Replace");
            {
                LayoutStyle params;
                params.Width = SizeSpec::Fixed(Unit::Dp(170));
                m_replaceRow->AddView(m_replaceField.Get(), params);
            }
            addButton(*m_replaceRow, m_replaceButton, u8"Replace",
                      [self] { self->ReplaceCurrent(); });
            addButton(*m_replaceRow, m_replaceAllButton, u8"All", [self] { self->ReplaceAll(); });

            m_findBar->AddView(m_findRow.Get());
            m_findBar->AddView(m_replaceRow.Get());
            m_findBar->Visibility = VisibilityValue::Gone;
            AddView(m_findBar.Get());
        }

        void ApplyFindBarMode()
        {
            m_findBar->Visibility = VisibilityValue::Visible;
            const bool searching =
                m_findBarMode == FindBarMode::Find || m_findBarMode == FindBarMode::Replace;
            const VisibilityValue searchControls =
                searching ? VisibilityValue::Visible : VisibilityValue::Gone;
            m_prevButton->Visibility = searchControls;
            m_nextButton->Visibility = searchControls;
            m_caseButton->Visibility = searchControls;
            m_wordButton->Visibility = searchControls;
            m_replaceRow->Visibility = (m_findBarMode == FindBarMode::Replace && !ReadOnly)
                                           ? VisibilityValue::Visible
                                           : VisibilityValue::Gone;
            m_findField->SetPlaceholder(m_findBarMode == FindBarMode::GoToLine ? u8"Line"
                                                                               : u8"Find");
            UpdateMatchLabel();
        }

        [[nodiscard]] bool IsInFindBar(const View* view) const
        {
            for (const View* v = view; v != nullptr; v = v->Parent)
            {
                if (v == m_findBar.Get())
                {
                    return true;
                }
            }
            return false;
        }

        void JumpToTypedLine()
        {
            const String text = m_findField->Text();
            i32 line = 0;
            bool any = false;
            for (usize i = 0; i < text.AsView().Size(); ++i)
            {
                const char8_t c = text.AsView()[i];
                if (c < u8'0' || c > u8'9')
                {
                    continue;
                }
                any = true;
                line = line * 10 + (c - u8'0');
                if (line > 100000000)
                {
                    break;
                }
            }
            if (any)
            {
                CloseFindBar();
                ScrollToLine(line - 1); // 1-based entry
            }
        }

        /// Recomputes matches + the current index (first match at/after the cursor). Does not
        /// move the editor selection - callers decide (typing selects, edits keep position).
        void RunSearch()
        {
            m_matches.Clear();
            m_currentMatch = -1;
            if (m_findField.Get() != nullptr &&
                (m_findBarMode == FindBarMode::Find || m_findBarMode == FindBarMode::Replace))
            {
                const String query = m_findField->Text();
                m_doc.FindAll(query.AsView(), m_searchCaseSensitive, m_searchWholeWord,
                              m_matches);
                for (usize i = 0; i < m_matches.Size(); ++i)
                {
                    if (m_cursor <= m_matches[i].begin ||
                        (m_matches[i].begin <= m_cursor && m_cursor <= m_matches[i].end))
                    {
                        m_currentMatch = static_cast<i32>(i);
                        break;
                    }
                }
                if (m_currentMatch < 0 && !m_matches.IsEmpty())
                {
                    m_currentMatch = 0; // wrap
                }
            }
            UpdateMatchLabel();
            Invalidate();
        }

        void SelectMatch(i32 index)
        {
            if (index < 0 || static_cast<usize>(index) >= m_matches.Size())
            {
                return;
            }
            m_currentMatch = index;
            m_anchor = m_matches[static_cast<usize>(index)].begin;
            m_cursor = m_matches[static_cast<usize>(index)].end;
            m_desiredColumn = -1;
            m_pendingCursorScroll = true;
            ResetBlink();
            UpdateMatchLabel();
            Invalidate();
        }

        void GotoMatch(i32 delta)
        {
            if (m_matches.IsEmpty())
            {
                return;
            }
            const i32 count = static_cast<i32>(m_matches.Size());
            i32 target = m_currentMatch;
            if (target < 0)
            {
                target = delta > 0 ? 0 : count - 1;
            }
            else
            {
                target = (target + delta % count + count) % count;
            }
            SelectMatch(target);
        }

        void UpdateMatchLabel()
        {
            if (m_matchLabel.Get() == nullptr)
            {
                return;
            }
            if (m_findBarMode == FindBarMode::GoToLine)
            {
                char buffer[32];
                const int n = std::snprintf(buffer, sizeof(buffer), "1-%d", m_doc.LineCount());
                m_matchLabel->SetText(StringView(reinterpret_cast<const char8_t*>(buffer),
                                                 n > 0 ? static_cast<usize>(n) : 0u));
                return;
            }
            char buffer[32];
            const int n =
                std::snprintf(buffer, sizeof(buffer), "%d/%d",
                              m_currentMatch >= 0 ? m_currentMatch + 1 : 0,
                              static_cast<int>(m_matches.Size()));
            m_matchLabel->SetText(StringView(reinterpret_cast<const char8_t*>(buffer),
                                             n > 0 ? static_cast<usize>(n) : 0u));
        }

        // ---- bracket matching (cached; recomputed on cursor/content change) ----

        void RefreshBracketMatch()
        {
            if (m_bracketVersion == m_doc.Version() && m_bracketCursor == m_cursor &&
                m_bracketAnchor == m_anchor)
            {
                return;
            }
            m_bracketVersion = m_doc.Version();
            m_bracketCursor = m_cursor;
            m_bracketAnchor = m_anchor;
            m_bracketValid = false;
            if (HasSelection())
            {
                return;
            }
            const CodePosition probes[2] = {m_cursor,
                                            CodePosition{m_cursor.line, m_cursor.column - 1}};
            for (const CodePosition& probe : probes)
            {
                if (probe.column < 0)
                {
                    continue;
                }
                CodePosition match{};
                if (m_doc.FindMatchingBracket(probe, match))
                {
                    m_bracketA = probe;
                    m_bracketB = match;
                    m_bracketValid = true;
                    return;
                }
            }
        }

        // ---- state ----

        CodeDocument m_doc;
        CodePosition m_cursor{};
        CodePosition m_anchor{};
        i32 m_desiredColumn = -1; // goal column for vertical motion; -1 = none

        f32 m_scrollX = 0.0f;
        f32 m_scrollY = 0.0f;
        f32 m_viewportW = 0.0f;
        f32 m_viewportH = 0.0f;
        RefPtr<ScrollBar> m_vBar;
        RefPtr<ScrollBar> m_hBar;

        f32 m_lineHeight = 0.0f;
        f32 m_advance = 0.0f;
        i32 m_maxLineLength = 0;
        bool m_maxLineDirty = true;
        bool m_pendingCursorScroll = false;
        bool m_dragging = false;
        f32 m_blinkReset = 0.0f;

        UniquePtr<ICodeLexer> m_lexer;
        CodeHighlighter m_highlighter;

        // Find bar (built lazily; logical child so hit-testing + DrawChildren apply).
        FindBarMode m_findBarMode = FindBarMode::Closed;
        RefPtr<FlexLayout> m_findBar;
        RefPtr<FlexLayout> m_findRow;
        RefPtr<FlexLayout> m_replaceRow;
        RefPtr<EditText> m_findField;
        RefPtr<EditText> m_replaceField;
        RefPtr<Label> m_matchLabel;
        RefPtr<Button> m_prevButton;
        RefPtr<Button> m_nextButton;
        RefPtr<ToggleButton> m_caseButton;
        RefPtr<ToggleButton> m_wordButton;
        RefPtr<Button> m_replaceButton;
        RefPtr<Button> m_replaceAllButton;
        RefPtr<Button> m_closeButton;
        Rectangle m_findBarFrame{};
        bool m_searchCaseSensitive = false;
        bool m_searchWholeWord = false;
        Array<CodeSpan> m_matches;
        i32 m_currentMatch = -1;

        // Bracket-match cache (recomputed when cursor/content change).
        bool m_bracketValid = false;
        CodePosition m_bracketA{};
        CodePosition m_bracketB{};
        u64 m_bracketVersion = static_cast<u64>(-1);
        CodePosition m_bracketCursor{};
        CodePosition m_bracketAnchor{};

        Float2 m_lastHover{}; // for the diagnostics tooltip

        CompletionModel m_completion;
        DocumentWordCompletionProvider m_wordProvider;
        Array<ICompletionProvider*> m_providers; // borrowed
    };

    inline void CodeEditView::ToggleLineComment()
    {
        const StringView prefix =
            m_lexer.Get() != nullptr ? m_lexer->LineCommentPrefix() : StringView(u8"//");
        if (prefix.IsEmpty() || ReadOnly)
        {
            return;
        }

        const CodeSpan selection = Selection();
        const i32 firstLine = selection.begin.line;
        i32 lastLine = selection.end.line;
        if (lastLine > firstLine && selection.end.column == 0)
        {
            --lastLine; // a selection ending at column 0 does not touch that line
        }

        // Uncomment only when EVERY non-blank line already carries the prefix.
        bool allCommented = true;
        bool anyContent = false;
        for (i32 line = firstLine; line <= lastLine; ++line)
        {
            const StringView text = m_doc.Line(line);
            usize i = 0;
            while (i < text.Size() && (text[i] == u8' ' || text[i] == u8'\t'))
            {
                ++i;
            }
            if (i >= text.Size())
            {
                continue; // blank line - ignored by the toggle decision
            }
            anyContent = true;
            if (!text.SubStr(i, text.Size() - i).StartsWith(prefix))
            {
                allCommented = false;
                break;
            }
        }
        if (!anyContent)
        {
            return;
        }

        String replacement;
        for (i32 line = firstLine; line <= lastLine; ++line)
        {
            if (line > firstLine)
            {
                replacement.PushBack(u8'\n');
            }
            const StringView text = m_doc.Line(line);
            usize indent = 0;
            while (indent < text.Size() && (text[indent] == u8' ' || text[indent] == u8'\t'))
            {
                ++indent;
            }
            if (allCommented)
            {
                if (indent < text.Size() &&
                    text.SubStr(indent, text.Size() - indent).StartsWith(prefix))
                {
                    usize drop = indent + prefix.Size();
                    if (drop < text.Size() && text[drop] == u8' ')
                    {
                        ++drop; // the space the toggle itself inserts
                    }
                    replacement.Append(text.SubStr(0, indent));
                    replacement.Append(text.SubStr(drop, text.Size() - drop));
                }
                else
                {
                    replacement.Append(text); // blank line - untouched
                }
            }
            else
            {
                if (indent >= text.Size())
                {
                    replacement.Append(text); // blank line - untouched
                }
                else
                {
                    replacement.Append(text.SubStr(0, indent));
                    replacement.Append(prefix);
                    replacement.PushBack(u8' ');
                    replacement.Append(text.SubStr(indent, text.Size() - indent));
                }
            }
        }

        const CodeSpan lineSpan{CodePosition{firstLine, 0},
                                CodePosition{lastLine, m_doc.LineLength(lastLine)}};
        const CodeCursorState before{m_cursor, m_anchor};
        (void)m_doc.Edit(lineSpan, replacement.AsView(), CodeEditKind::Other, before, Now());
        m_anchor = CodePosition{firstLine, 0};
        m_cursor = CodePosition{lastLine, m_doc.LineLength(lastLine)};
        AfterEdit();
    }

    RTTI_DEFINE_OBJECT(CodeEditView, "rtti::ui::toolkit")
}

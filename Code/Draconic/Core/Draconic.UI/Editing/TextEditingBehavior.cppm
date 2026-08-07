// Draconic UI - :text_editing_behavior partition
//
// Reusable text-editing logic: cursor management, selection, keyboard shortcuts, mouse interaction,
// clipboard, and undo/redo. Operates on CHARACTER indices (not byte offsets), talking to its host
// purely through ITextEditHost - so this partition is View-independent (no cycle). Ported from
// Sedulous.UI/src/Editing/TextEditingBehavior.bf.
//
// Port taxes: Beef `for (c in host.Text.DecodedChars)` codepoint iteration -> foundation::DecodeUtf8
// over the host's UTF-8 Text(); `charStr.Append(char32)` -> foundation::AppendUtf8. The owned heap
// `InputFilter` becomes an Optional<InputFilter> (value type). char32.IsLetterOrDigit is approximated
// (ASCII alnum plus any codepoint >= 0x80 treated as a word char); faithful enough for the ported
// tests, noted for later. The host accessors GetMaxLength()/GetIsReadOnly() are the `Get`-prefixed
// ITextEditHost members (see :itext_edit_host).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:text_editing_behavior;

import draconic.foundation; // String, StringView, Array, Clamp, Min, Max, DecodeUtf8, AppendUtf8, Optional
import :itext_edit_host;
import :undo_stack;
import :input_filter;
import :input_enums; // KeyCode, KeyModifiers, HasFlag

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Type of editing action for undo coalescing.
    enum class EditActionType
    {
        None,
        CharInsert,
        Delete,
        Paste,
        Cut
    };

    class TextEditingBehavior
    {
    public:
        /// Coalesce window for consecutive char inserts into one undo entry (seconds).
        f32 UndoCoalesceTime = 1.0f;
        bool AllowClipboardCopy = true;

        explicit TextEditingBehavior(ITextEditHost* host) : m_host(host) {}

        // === Cursor / selection state ===
        [[nodiscard]] i32 CursorPosition() const noexcept { return m_cursorPos; }
        void SetCursorPosition(i32 value)
        {
            m_cursorPos = Clamp(value, i32{0}, m_host->TextCharCount());
        }
        [[nodiscard]] i32 AnchorPosition() const noexcept { return m_anchorPos; }
        void SetAnchorPosition(i32 value)
        {
            m_anchorPos = Clamp(value, i32{0}, m_host->TextCharCount());
        }

        [[nodiscard]] i32 SelectionStart() const noexcept { return Min(m_anchorPos, m_cursorPos); }
        [[nodiscard]] i32 SelectionEnd() const noexcept { return Max(m_anchorPos, m_cursorPos); }
        [[nodiscard]] i32 SelectionLength() const noexcept
        {
            return SelectionEnd() - SelectionStart();
        }
        [[nodiscard]] bool HasSelection() const noexcept { return m_cursorPos != m_anchorPos; }
        [[nodiscard]] bool IsSelecting() const noexcept { return HasSelection(); }

        void SelectAll()
        {
            m_anchorPos = 0;
            m_cursorPos = m_host->TextCharCount();
        }

        [[nodiscard]] UndoStack& GetUndoStack() noexcept { return m_undoStack; }

        // === Input filter (Optional value type; nullptr = no filter) ===
        [[nodiscard]] InputFilter* Filter()
        {
            return m_inputFilter.HasValue() ? &m_inputFilter.Value() : nullptr;
        }
        void SetFilter(InputFilter filter) { m_inputFilter = Move(filter); }

        // =================================================================
        // Public input handlers
        // =================================================================

        void HandleTextInput(char32_t character)
        {
            if (m_host->GetIsReadOnly())
            {
                return;
            }

            // Filter control characters (allow tab).
            if (character < 32 && character != U'\t')
            {
                return;
            }

            // Apply input filter.
            if (m_inputFilter.HasValue() && !m_inputFilter.Value().Accept(character))
            {
                return;
            }

            // MaxLength check.
            if (m_host->GetMaxLength() > 0)
            {
                const i32 availableChars =
                    m_host->GetMaxLength() - m_host->TextCharCount() + SelectionLength();
                if (availableChars <= 0)
                {
                    return;
                }
            }

            PushUndoIfNeeded(EditActionType::CharInsert);

            if (HasSelection())
            {
                DeleteSelectionText();
            }

            String charStr;
            AppendUtf8(charStr, static_cast<u32>(character));
            m_host->ReplaceText(m_cursorPos, 0, charStr);
            m_cursorPos++;
            m_anchorPos = m_cursorPos;
            m_lastEditTime = m_host->CurrentTime();
            m_host->OnTextModified();
        }

        void HandleKeyDown(KeyCode key, KeyModifiers mods)
        {
            const bool ctrl = HasFlag(mods, KeyModifiers::Ctrl);
            const bool shift = HasFlag(mods, KeyModifiers::Shift);

            switch (key)
            {
            case KeyCode::Left:
                BreakMergeChain();
                if (ctrl)
                {
                    MoveWordLeft(shift);
                }
                else if (!shift && HasSelection())
                {
                    const i32 pos = SelectionStart();
                    m_cursorPos = pos;
                    m_anchorPos = pos;
                }
                else
                {
                    MoveCursor(m_cursorPos - 1, shift);
                }
                break;
            case KeyCode::Right:
                BreakMergeChain();
                if (ctrl)
                {
                    MoveWordRight(shift);
                }
                else if (!shift && HasSelection())
                {
                    const i32 pos = SelectionEnd();
                    m_cursorPos = pos;
                    m_anchorPos = pos;
                }
                else
                {
                    MoveCursor(m_cursorPos + 1, shift);
                }
                break;
            case KeyCode::Up:
                if (m_host->IsMultiline())
                {
                    BreakMergeChain();
                    MoveLineUp(shift);
                }
                break;
            case KeyCode::Down:
                if (m_host->IsMultiline())
                {
                    BreakMergeChain();
                    MoveLineDown(shift);
                }
                break;
            case KeyCode::Return:
                if (m_host->IsMultiline() && !m_host->GetIsReadOnly())
                {
                    PushUndoIfNeeded(EditActionType::CharInsert);
                    if (HasSelection())
                    {
                        DeleteSelectionText();
                    }
                    m_host->ReplaceText(m_cursorPos, 0, u8"\n");
                    m_cursorPos++;
                    m_anchorPos = m_cursorPos;
                    m_lastEditTime = m_host->CurrentTime();
                    m_host->OnTextModified();
                }
                break;
            case KeyCode::Home:
                BreakMergeChain();
                MoveHome(shift);
                break;
            case KeyCode::End:
                BreakMergeChain();
                MoveEnd(shift);
                break;
            case KeyCode::Backspace:
                if (m_host->GetIsReadOnly())
                {
                    return;
                }
                if (HasSelection())
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteSelectionText();
                    m_host->OnTextModified();
                }
                else if (ctrl)
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteWordBackward();
                }
                else
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteBackward();
                }
                break;
            case KeyCode::Delete:
                if (m_host->GetIsReadOnly())
                {
                    return;
                }
                if (HasSelection())
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteSelectionText();
                    m_host->OnTextModified();
                }
                else if (ctrl)
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteWordForward();
                }
                else
                {
                    PushUndoIfNeeded(EditActionType::Delete);
                    DeleteForward();
                }
                break;
            case KeyCode::A:
                if (ctrl)
                {
                    SelectAll();
                }
                break;
            case KeyCode::C:
                if (ctrl)
                {
                    CopyToClipboard();
                }
                break;
            case KeyCode::V:
                if (ctrl && !m_host->GetIsReadOnly())
                {
                    PasteFromClipboard();
                }
                break;
            case KeyCode::X:
                if (ctrl && !m_host->GetIsReadOnly())
                {
                    CutToClipboard();
                }
                break;
            case KeyCode::Z:
                if (ctrl && !shift)
                {
                    PerformUndo();
                }
                else if (ctrl && shift)
                {
                    PerformRedo();
                }
                break;
            case KeyCode::Y:
                if (ctrl)
                {
                    PerformRedo();
                }
                break;
            default:
                break;
            }
        }

        void HandleMouseDown(f32 localX, f32 localY, i32 clickCount, KeyModifiers mods)
        {
            BreakMergeChain();
            const i32 pos = m_host->HitTestPosition(localX, localY);

            if (clickCount == 3)
            {
                SelectAll();
            }
            else if (clickCount == 2)
            {
                SelectWord(pos);
            }
            else if (HasFlag(mods, KeyModifiers::Shift))
            {
                m_cursorPos = pos;
            } // extend selection
            else
            {
                m_cursorPos = pos;
                m_anchorPos = pos;
            } // set cursor, clear selection
        }

        void HandleMouseMove(f32 localX, f32 localY)
        {
            // Extend selection during drag.
            m_cursorPos = m_host->HitTestPosition(localX, localY);
        }

        /// Reset state when text is set programmatically.
        void Reset()
        {
            m_cursorPos = 0;
            m_anchorPos = 0;
            m_undoStack.Clear();
            m_lastActionType = EditActionType::None;
        }

    private:
        // Collect the host text as codepoints (mirrors Beef's DecodedChars materialization).
        void CollectChars(Array<char32_t>& out) const
        {
            const StringView text = m_host->Text();
            usize i = 0;
            while (i < text.Size())
            {
                out.PushBack(static_cast<char32_t>(DecodeUtf8(text, i)));
            }
        }

        // =================================================================
        // Text operations
        // =================================================================

        void DeleteSelectionText()
        {
            if (!HasSelection())
            {
                return;
            }
            const i32 start = SelectionStart();
            const i32 length = SelectionLength();
            m_host->ReplaceText(start, length, u8"");
            m_cursorPos = start;
            m_anchorPos = start;
        }

        void DeleteBackward()
        {
            if (m_cursorPos > 0)
            {
                m_host->ReplaceText(m_cursorPos - 1, 1, u8"");
                m_cursorPos--;
                m_anchorPos = m_cursorPos;
                m_host->OnTextModified();
            }
        }

        void DeleteForward()
        {
            if (m_cursorPos < m_host->TextCharCount())
            {
                m_host->ReplaceText(m_cursorPos, 1, u8"");
                m_anchorPos = m_cursorPos;
                m_host->OnTextModified();
            }
        }

        void DeleteWordBackward()
        {
            if (m_cursorPos > 0)
            {
                const i32 boundary = FindWordBoundaryLeft(m_cursorPos);
                const i32 count = m_cursorPos - boundary;
                m_host->ReplaceText(boundary, count, u8"");
                m_cursorPos = boundary;
                m_anchorPos = m_cursorPos;
                m_host->OnTextModified();
            }
        }

        void DeleteWordForward()
        {
            if (m_cursorPos < m_host->TextCharCount())
            {
                const i32 boundary = FindWordBoundaryRight(m_cursorPos);
                const i32 count = boundary - m_cursorPos;
                m_host->ReplaceText(m_cursorPos, count, u8"");
                m_anchorPos = m_cursorPos;
                m_host->OnTextModified();
            }
        }

        // =================================================================
        // Cursor movement
        // =================================================================

        void MoveCursor(i32 newPos, bool extendSelection)
        {
            m_cursorPos = Clamp(newPos, i32{0}, m_host->TextCharCount());
            if (!extendSelection)
            {
                m_anchorPos = m_cursorPos;
            }
        }

        void MoveWordLeft(bool extendSelection)
        {
            MoveCursor(FindWordBoundaryLeft(m_cursorPos), extendSelection);
        }
        void MoveWordRight(bool extendSelection)
        {
            MoveCursor(FindWordBoundaryRight(m_cursorPos), extendSelection);
        }

        void MoveLineUp(bool extendSelection)
        {
            const f32 curY = m_host->GetCursorYPosition(m_cursorPos);
            const f32 lineH = m_host->LineHeight();
            if (curY < lineH * 0.5f)
            {
                return;
            } // already on the first line
            const f32 curX = m_host->GetCursorXPosition(m_cursorPos);
            const i32 newPos = m_host->HitTestGlyphPosition(curX, curY - lineH * 0.5f);
            MoveCursor(newPos, extendSelection);
        }

        void MoveLineDown(bool extendSelection)
        {
            const f32 curY = m_host->GetCursorYPosition(m_cursorPos);
            const f32 lineH = m_host->LineHeight();

            i32 totalLines = 1;
            {
                const StringView text = m_host->Text();
                usize i = 0;
                while (i < text.Size())
                {
                    if (DecodeUtf8(text, i) == U'\n')
                    {
                        totalLines++;
                    }
                }
            }
            const i32 currentLine = (lineH > 0) ? static_cast<i32>(curY / lineH) : 0;
            if (currentLine >= totalLines - 1)
            {
                return;
            } // already on the last line

            const f32 curX = m_host->GetCursorXPosition(m_cursorPos);
            const i32 newPos = m_host->HitTestGlyphPosition(curX, curY + lineH * 1.5f);
            MoveCursor(newPos, extendSelection);
        }

        void MoveHome(bool extendSelection)
        {
            if (m_host->IsMultiline())
            {
                MoveCursor(GetLineStart(m_cursorPos), extendSelection);
            }
            else
            {
                MoveCursor(0, extendSelection);
            }
        }

        void MoveEnd(bool extendSelection)
        {
            if (m_host->IsMultiline())
            {
                MoveCursor(GetLineEnd(m_cursorPos), extendSelection);
            }
            else
            {
                MoveCursor(m_host->TextCharCount(), extendSelection);
            }
        }

        /// Char index of the start of the line containing charIndex.
        [[nodiscard]] i32 GetLineStart(i32 charIndex) const
        {
            const StringView text = m_host->Text();
            i32 lineStart = 0;
            i32 idx = 0;
            usize i = 0;
            while (i < text.Size())
            {
                if (idx >= charIndex)
                {
                    break;
                }
                if (DecodeUtf8(text, i) == U'\n')
                {
                    lineStart = idx + 1;
                }
                idx++;
            }
            return lineStart;
        }

        /// Char index of the end of the line containing charIndex.
        [[nodiscard]] i32 GetLineEnd(i32 charIndex) const
        {
            const StringView text = m_host->Text();
            i32 idx = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const char32_t c = static_cast<char32_t>(DecodeUtf8(text, i));
                if (idx >= charIndex)
                {
                    if (c == U'\n')
                    {
                        return idx;
                    }
                }
                idx++;
            }
            return m_host->TextCharCount();
        }

        // =================================================================
        // Selection
        // =================================================================

        void SelectWord(i32 position)
        {
            const StringView text = m_host->Text();
            if (text.IsEmpty())
            {
                return;
            }

            const i32 charCount = m_host->TextCharCount();
            const i32 pos = Clamp(position, i32{0}, charCount);

            Array<char32_t> chars;
            CollectChars(chars);

            // Find word start.
            i32 start = (pos > 0 && pos <= charCount) ? pos - 1 : pos;
            if (start < charCount && start >= 0 && IsWordChar(chars[static_cast<usize>(start)]))
            {
                while (start > 0 && IsWordChar(chars[static_cast<usize>(start - 1)]))
                {
                    start--;
                }
            }
            else
            {
                start = pos;
            }

            // Find word end.
            i32 end = start;
            while (end < charCount && IsWordChar(chars[static_cast<usize>(end)]))
            {
                end++;
            }

            m_anchorPos = start;
            m_cursorPos = end;
        }

        // =================================================================
        // Clipboard
        // =================================================================

        void CopyToClipboard()
        {
            if (!HasSelection() || !AllowClipboardCopy)
            {
                return;
            }
            IClipboard* clipboard = m_host->Clipboard();
            if (clipboard == nullptr)
            {
                return;
            }
            String selectedText;
            GetSelectedText(selectedText);
            (void)clipboard->SetText(selectedText);
        }

        void CutToClipboard()
        {
            if (!HasSelection() || m_host->GetIsReadOnly() || !AllowClipboardCopy)
            {
                return;
            }
            CopyToClipboard();
            PushUndoIfNeeded(EditActionType::Cut);
            DeleteSelectionText();
            m_host->OnTextModified();
        }

        void PasteFromClipboard()
        {
            IClipboard* clipboard = m_host->Clipboard();
            if (clipboard == nullptr || !clipboard->HasText())
            {
                return;
            }

            String pasteText;
            if (!clipboard->GetText(pasteText).IsOk())
            {
                return;
            }
            if (pasteText.IsEmpty())
            {
                return;
            }

            // Strip newlines for single-line (newlines are single-byte in UTF-8).
            if (!m_host->IsMultiline())
            {
                for (utf8char& ch : pasteText)
                {
                    if (ch == static_cast<utf8char>('\n') || ch == static_cast<utf8char>('\r'))
                    {
                        ch = static_cast<utf8char>(' ');
                    }
                }
            }

            // Apply input filter to each character.
            if (m_inputFilter.HasValue())
            {
                String filtered;
                const StringView src = pasteText;
                usize i = 0;
                while (i < src.Size())
                {
                    const u32 c = DecodeUtf8(src, i);
                    if (m_inputFilter.Value().Accept(static_cast<char32_t>(c)))
                    {
                        AppendUtf8(filtered, c);
                    }
                }
                pasteText = Move(filtered);
            }
            if (pasteText.IsEmpty())
            {
                return;
            }

            // MaxLength enforcement.
            if (m_host->GetMaxLength() > 0)
            {
                const i32 available =
                    m_host->GetMaxLength() - m_host->TextCharCount() + SelectionLength();
                if (available <= 0)
                {
                    return;
                }

                const i32 pasteCharCount = static_cast<i32>(Utf8Length(pasteText));
                if (pasteCharCount > available)
                {
                    String truncated;
                    const StringView src = pasteText;
                    usize i = 0;
                    i32 count = 0;
                    while (i < src.Size() && count < available)
                    {
                        AppendUtf8(truncated, DecodeUtf8(src, i));
                        count++;
                    }
                    pasteText = Move(truncated);
                }
            }

            PushUndoIfNeeded(EditActionType::Paste);

            if (HasSelection())
            {
                DeleteSelectionText();
            }

            const i32 insertedChars = static_cast<i32>(Utf8Length(pasteText));
            m_host->ReplaceText(m_cursorPos, 0, pasteText);
            m_cursorPos += insertedChars;
            m_anchorPos = m_cursorPos;
            m_host->OnTextModified();
        }

        // =================================================================
        // Undo / Redo
        // =================================================================

        void PushUndoIfNeeded(EditActionType actionType)
        {
            const f32 time = m_host->CurrentTime();
            bool shouldPush = false;

            if (actionType != m_lastActionType)
            {
                shouldPush = true;
            }
            else if (actionType != EditActionType::CharInsert)
            {
                shouldPush = true;
            }
            else if (time - m_lastEditTime > UndoCoalesceTime)
            {
                shouldPush = true;
            }

            if (shouldPush)
            {
                m_undoStack.PushState(m_host->Text(), m_cursorPos, m_anchorPos);
            }

            m_lastActionType = actionType;
            m_lastEditTime = time;
        }

        /// Break the undo merge chain (on navigation).
        void BreakMergeChain() { m_lastActionType = EditActionType::None; }

        void PerformUndo()
        {
            String restoredText;
            i32 restoredCursor = 0;
            i32 restoredAnchor = 0;

            if (m_undoStack.Undo(m_host->Text(), m_cursorPos, m_anchorPos, restoredText,
                                 restoredCursor, restoredAnchor))
            {
                m_host->ReplaceText(0, m_host->TextCharCount(), restoredText);
                m_cursorPos = Clamp(restoredCursor, i32{0}, m_host->TextCharCount());
                m_anchorPos = Clamp(restoredAnchor, i32{0}, m_host->TextCharCount());
                m_lastActionType = EditActionType::None;
                m_host->OnTextModified();
            }
        }

        void PerformRedo()
        {
            String restoredText;
            i32 restoredCursor = 0;
            i32 restoredAnchor = 0;

            if (m_undoStack.Redo(m_host->Text(), m_cursorPos, m_anchorPos, restoredText,
                                 restoredCursor, restoredAnchor))
            {
                m_host->ReplaceText(0, m_host->TextCharCount(), restoredText);
                m_cursorPos = Clamp(restoredCursor, i32{0}, m_host->TextCharCount());
                m_anchorPos = Clamp(restoredAnchor, i32{0}, m_host->TextCharCount());
                m_lastActionType = EditActionType::None;
                m_host->OnTextModified();
            }
        }

        // =================================================================
        // Word boundary helpers
        // =================================================================

        [[nodiscard]] i32 FindWordBoundaryLeft(i32 pos) const
        {
            if (pos <= 0)
            {
                return 0;
            }

            Array<char32_t> chars;
            CollectChars(chars);

            // Skip non-word chars going left (stop at newlines).
            i32 p = pos - 1;
            while (p >= 0 && !IsWordChar(chars[static_cast<usize>(p)]) &&
                   chars[static_cast<usize>(p)] != U'\n')
            {
                p--;
            }

            // If we hit a newline, stop after it.
            if (p >= 0 && chars[static_cast<usize>(p)] == U'\n')
            {
                return p + 1;
            }

            // Skip word chars going left.
            while (p >= 0 && IsWordChar(chars[static_cast<usize>(p)]))
            {
                p--;
            }

            return p + 1;
        }

        [[nodiscard]] i32 FindWordBoundaryRight(i32 pos) const
        {
            const i32 charCount = m_host->TextCharCount();
            if (pos >= charCount)
            {
                return charCount;
            }

            Array<char32_t> chars;
            CollectChars(chars);

            // Skip word chars going right.
            i32 p = pos;
            while (p < charCount && IsWordChar(chars[static_cast<usize>(p)]))
            {
                p++;
            }

            // Skip non-word chars going right (stop at newlines).
            while (p < charCount && !IsWordChar(chars[static_cast<usize>(p)]) &&
                   chars[static_cast<usize>(p)] != U'\n')
            {
                p++;
            }

            return p;
        }

        // Beef used char32.IsLetterOrDigit (full Unicode). Approximation: ASCII alnum plus any
        // codepoint >= 0x80 (treated as a letter). Faithful for the ASCII test corpus.
        [[nodiscard]] static bool IsLetterOrDigit(char32_t c)
        {
            return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') ||
                   (c >= U'A' && c <= U'Z') || c >= 0x80u;
        }
        [[nodiscard]] static bool IsWordChar(char32_t c) { return IsLetterOrDigit(c) || c == U'_'; }

        // =================================================================
        // Helpers
        // =================================================================

        void GetSelectedText(String& outText) const
        {
            if (!HasSelection())
            {
                return;
            }
            const StringView text = m_host->Text();
            const i32 start = SelectionStart();
            const i32 end = SelectionEnd();

            i32 charIdx = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const u32 c = DecodeUtf8(text, i);
                if (charIdx >= start && charIdx < end)
                {
                    AppendUtf8(outText, c);
                }
                if (charIdx >= end)
                {
                    break;
                }
                charIdx++;
            }
        }

        ITextEditHost* m_host;
        i32 m_cursorPos = 0;
        i32 m_anchorPos = 0;
        UndoStack m_undoStack;
        Optional<InputFilter> m_inputFilter;
        f32 m_lastEditTime = 0.0f;
        EditActionType m_lastActionType = EditActionType::None;
    };
}

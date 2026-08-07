// Draconic UI - :undo_stack partition
//
// Fixed-capacity undo/redo stack for text editing. Stores full text snapshots (not deltas).
// Ported from Sedulous.UI/src/Editing/UndoStack.bf. Self-contained (no View dependency).
//
// Port taxes: Beef's `List<UndoEntry*>` + manual `delete`/`DeleteContainerAndItems!` becomes
// Array<UndoEntry> BY VALUE (RAII - entries are owned by the array, no manual lifetime). The
// Beef `out int32` parameters on Undo/Redo become out-reference (`i32&`) parameters, and the
// `String outText` accumulator becomes a `String&` we assign into.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:undo_stack;

import draconic.foundation; // String, StringView, Array, Max

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// A single undo entry capturing full text state.
    struct UndoEntry
    {
        String Text;
        i32 CursorPos = 0;
        i32 AnchorPos = 0;

        UndoEntry() = default;
        UndoEntry(StringView text, i32 cursorPos, i32 anchorPos)
            : Text(text), CursorPos(cursorPos), AnchorPos(anchorPos)
        {
        }
    };

    /// Fixed-capacity undo/redo stack. Snapshots full text for simplicity.
    class UndoStack
    {
    public:
        [[nodiscard]] bool CanUndo() const noexcept { return m_undoList.Size() > 0; }
        [[nodiscard]] bool CanRedo() const noexcept { return m_redoList.Size() > 0; }
        [[nodiscard]] i32 UndoCount() const noexcept { return static_cast<i32>(m_undoList.Size()); }
        [[nodiscard]] i32 RedoCount() const noexcept { return static_cast<i32>(m_redoList.Size()); }

        [[nodiscard]] i32 MaxEntries() const noexcept { return m_maxEntries; }
        void SetMaxEntries(i32 value) noexcept { m_maxEntries = Max(1, value); }

        /// Push current state onto the undo stack. Clears the redo stack.
        void PushState(StringView text, i32 cursorPos, i32 anchorPos)
        {
            ClearRedo();

            // Drop the oldest entry if at capacity.
            if (static_cast<i32>(m_undoList.Size()) >= m_maxEntries)
            {
                m_undoList.RemoveAt(0);
            }

            m_undoList.PushBack(UndoEntry{text, cursorPos, anchorPos});
        }

        /// Undo: pops the previous state, pushing the current state onto the redo stack.
        bool Undo(StringView currentText, i32 currentCursor, i32 currentAnchor, String& outText,
                  i32& outCursor, i32& outAnchor)
        {
            outCursor = 0;
            outAnchor = 0;

            if (m_undoList.Size() == 0)
            {
                return false;
            }

            // Push current state to redo.
            m_redoList.PushBack(UndoEntry{currentText, currentCursor, currentAnchor});

            // Pop from undo.
            UndoEntry entry = Move(m_undoList.Back());
            m_undoList.PopBack();
            outText = Move(entry.Text);
            outCursor = entry.CursorPos;
            outAnchor = entry.AnchorPos;

            return true;
        }

        /// Redo: pops the next state, pushing the current state onto the undo stack.
        bool Redo(StringView currentText, i32 currentCursor, i32 currentAnchor, String& outText,
                  i32& outCursor, i32& outAnchor)
        {
            outCursor = 0;
            outAnchor = 0;

            if (m_redoList.Size() == 0)
            {
                return false;
            }

            // Push current state to undo (without clearing redo).
            if (static_cast<i32>(m_undoList.Size()) >= m_maxEntries)
            {
                m_undoList.RemoveAt(0);
            }
            m_undoList.PushBack(UndoEntry{currentText, currentCursor, currentAnchor});

            // Pop from redo.
            UndoEntry entry = Move(m_redoList.Back());
            m_redoList.PopBack();
            outText = Move(entry.Text);
            outCursor = entry.CursorPos;
            outAnchor = entry.AnchorPos;

            return true;
        }

        /// Clear all undo and redo entries.
        void Clear()
        {
            m_undoList.Clear();
            m_redoList.Clear();
        }

    private:
        void ClearRedo() { m_redoList.Clear(); }

        Array<UndoEntry> m_undoList;
        Array<UndoEntry> m_redoList;
        i32 m_maxEntries = 100;
    };
}

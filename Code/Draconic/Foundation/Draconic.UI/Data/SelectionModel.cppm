// Draconic UI - :selection_model partition
//
// Decoupled selection state: tracks selected indices independently of the data view (multiple views can
// share one). Ported from Sedulous.UI/src/Data/SelectionModel.bf. Self-contained (no View). HashSet<i32>
// backing; Beef `mSelected.Add` -> HashSet::Insert (both return true if newly added); Math.Min/Max ->
// foundation::Min/Max.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:selection_model;

import draconic.foundation; // HashSet, Array, Event? (Event is a UI partition)
import :event;

using namespace draconic::foundation;

export namespace draconic::ui
{
    enum class SelectionMode
    {
        None,
        Single,
        Multiple
    };

    class SelectionModel
    {
    public:
        SelectionMode Mode = SelectionMode::Single;

        Event<void()> OnSelectionChanged;

        [[nodiscard]] usize SelectedCount() const noexcept { return m_selected.Size(); }
        [[nodiscard]] bool IsSelected(i32 index) const noexcept
        {
            return m_selected.Contains(index);
        }

        /// Select an index EXCLUSIVELY (a plain click): the previous selection clears in every
        /// mode - extending is what Toggle (Ctrl) and SelectRange (Shift) are for. (Multiple
        /// mode used to accumulate here, so plain clicks grew the selection forever.)
        void Select(i32 index)
        {
            if (Mode == SelectionMode::None)
            {
                return;
            }
            if (m_selected.Size() == 1 && m_selected.Contains(index))
            {
                return;
            } // already exactly this
            m_selected.Clear();
            m_selected.Insert(index);
            OnSelectionChanged.Invoke();
        }

        /// Deselect an index.
        void Deselect(i32 index)
        {
            if (m_selected.Remove(index))
            {
                OnSelectionChanged.Invoke();
            }
        }

        /// Toggle selection of an index (Ctrl+click).
        void Toggle(i32 index)
        {
            if (Mode == SelectionMode::None)
            {
                return;
            }
            if (IsSelected(index))
            {
                Deselect(index);
            }
            else
            {
                if (Mode == SelectionMode::Single)
                {
                    m_selected.Clear();
                }
                if (m_selected.Insert(index))
                {
                    OnSelectionChanged.Invoke();
                }
            }
        }

        /// Select the inclusive range from..to (Shift+click). Single mode selects only `to`.
        void SelectRange(i32 from, i32 to)
        {
            if (Mode == SelectionMode::None)
            {
                return;
            }
            if (Mode == SelectionMode::Single)
            {
                Select(to);
                return;
            }
            const i32 lo = Min(from, to);
            const i32 hi = Max(from, to);
            m_selected.Clear();
            for (i32 i = lo; i <= hi; ++i)
            {
                m_selected.Insert(i);
            }
            OnSelectionChanged.Invoke();
        }

        /// Clear all selections.
        void ClearSelection()
        {
            if (m_selected.Size() > 0)
            {
                m_selected.Clear();
                OnSelectionChanged.Invoke();
            }
        }

        /// The set of selected indices (borrowed).
        [[nodiscard]] const HashSet<i32>& SelectedPositions() const noexcept { return m_selected; }

        /// The first selected index, or -1.
        [[nodiscard]] i32 FirstSelected() const
        {
            for (i32 idx : m_selected)
            {
                return idx;
            }
            return -1;
        }

        /// Adjust indices when items are inserted/removed at/above startPos. delta > 0 = insertion, < 0 = removal.
        void ShiftIndices(i32 startPos, i32 delta)
        {
            Array<i32> old;
            for (i32 idx : m_selected)
            {
                old.PushBack(idx);
            }
            m_selected.Clear();
            for (i32 idx : old)
            {
                if (idx >= startPos)
                {
                    const i32 newIdx = idx + delta;
                    if (newIdx >= 0)
                    {
                        m_selected.Insert(newIdx);
                    }
                }
                else
                {
                    m_selected.Insert(idx);
                }
            }
        }

    private:
        HashSet<i32> m_selected;
    };
}

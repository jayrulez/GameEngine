// Draconic::EditorCore - :command partition.
//
// The editor undo/redo spine: IEditorCommand + EditorCommandStack. Lumix WorldEditor
// semantics (the best-engineered of the surveyed editors), per docs/design/editor.md §3.4:
//   * every mutation is a command - a failed Execute() means the command is DROPPED, not pushed;
//   * same-type merge against the stack top (a slider drag coalesces into one undo entry);
//   * Begin/EndGroup transactions undo/redo atomically, consecutive same-type groups coalesce
//     unless locked (LockGroup after e.g. "create entity + add component" keeps later identical
//     actions separate).
// Each editor page owns its own stack (see :page); the context routes Edit>Undo/Redo to the
// active page's stack.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

module draconic.editor.core;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::editor
{
    bool EditorCommandStack::Execute(UniquePtr<IEditorCommand> command)
    {
        if (!command || m_locked)
        {
            return false;
        }

        // Same-type merge against the undo top (group markers never match a real TypeId).
        if (m_undoIndex >= 0)
        {
            IEditorCommand& top = *m_stack[static_cast<usize>(m_undoIndex)];
            if (top.TypeId() == command->TypeId() && command->MergeInto(top))
            {
                const bool ok = top.Execute();
                DRACONIC_ASSERT(ok); // re-executing a merged command must not fail
                (void)ok;
                Notify();
                return true;
            }
        }

        if (!command->Execute())
        {
            return false;
        } // dropped, not pushed

        TruncateRedo();
        m_stack.PushBack(Move(command));
        ++m_undoIndex;
        Notify();
        return true;
    }

    bool EditorCommandStack::CanRedo() const noexcept
    {
        return !m_inGroup && m_undoIndex + 1 < static_cast<i64>(m_stack.Size());
    }

    void EditorCommandStack::Undo()
    {
        if (m_locked || !CanUndo())
        {
            return;
        }

        i64 i = m_undoIndex;
        if (m_stack[static_cast<usize>(i)]->TypeId() == detail::kEndGroupTypeId)
        {
            // Unwind the whole group: undo every real command back to the begin marker.
            --i;
            while (i >= 0 && m_stack[static_cast<usize>(i)]->TypeId() != detail::kBeginGroupTypeId)
            {
                m_stack[static_cast<usize>(i)]->Undo();
                --i;
            }
            DRACONIC_ASSERT(i >= 0); // unbalanced group markers
            m_undoIndex = i - 1;     // step past the begin marker
        }
        else
        {
            m_stack[static_cast<usize>(i)]->Undo();
            m_undoIndex = i - 1;
        }
        Notify();
    }

    void EditorCommandStack::Redo()
    {
        if (m_locked || !CanRedo())
        {
            return;
        }

        i64 i = m_undoIndex + 1;
        if (m_stack[static_cast<usize>(i)]->TypeId() == detail::kBeginGroupTypeId)
        {
            // Replay the whole group forward to the end marker.
            ++i;
            while (i < static_cast<i64>(m_stack.Size()) &&
                   m_stack[static_cast<usize>(i)]->TypeId() != detail::kEndGroupTypeId)
            {
                const bool ok = m_stack[static_cast<usize>(i)]->Execute();
                DRACONIC_ASSERT(ok); // replaying a previously-successful command must not fail
                (void)ok;
                ++i;
            }
            DRACONIC_ASSERT(i < static_cast<i64>(m_stack.Size())); // unbalanced group markers
            m_undoIndex = i;                                       // lands on the end marker
        }
        else
        {
            const bool ok = m_stack[static_cast<usize>(i)]->Execute();
            DRACONIC_ASSERT(ok);
            (void)ok;
            m_undoIndex = i;
        }
        Notify();
    }

    void EditorCommandStack::BeginGroup(StringView groupType)
    {
        DRACONIC_ASSERT(!m_inGroup); // no nested groups
        TruncateRedo();

        // Coalesce: if the undo top is an unlocked end marker of the same group type, pop it
        // so new commands append inside that group (its begin marker stays).
        if (m_undoIndex >= 0)
        {
            IEditorCommand& top = *m_stack[static_cast<usize>(m_undoIndex)];
            if (top.TypeId() == detail::kEndGroupTypeId) // no RTTI: marker identity is its TypeId
            {
                auto& end = static_cast<detail::EndGroupCommand&>(top);
                if (!end.locked && end.GroupType() == groupType)
                {
                    m_stack.PopBack();
                    --m_undoIndex;
                    m_inGroup = true;
                    m_groupType = String(groupType);
                    return;
                }
            }
        }

        m_stack.PushBack(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<detail::BeginGroupCommand>(groupType), DefaultAllocator()));
        ++m_undoIndex;
        m_inGroup = true;
        m_groupType = String(groupType);
    }

    void EditorCommandStack::EndGroup()
    {
        DRACONIC_ASSERT(m_inGroup);
        m_stack.PushBack(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<detail::EndGroupCommand>(m_groupType.AsView()),
            DefaultAllocator()));
        ++m_undoIndex;
        m_inGroup = false;
        Notify();
    }

    void EditorCommandStack::LockGroup()
    {
        for (i64 i = m_undoIndex; i >= 0; --i)
        {
            IEditorCommand& cmd = *m_stack[static_cast<usize>(i)];
            if (cmd.TypeId() == detail::kEndGroupTypeId)
            {
                static_cast<detail::EndGroupCommand&>(cmd).locked = true;
                return;
            }
        }
    }

    void EditorCommandStack::Clear()
    {
        DRACONIC_ASSERT(!m_inGroup);
        m_stack.Clear();
        m_undoIndex = -1;
        Notify();
    }

    void EditorCommandStack::TruncateRedo()
    {
        while (static_cast<i64>(m_stack.Size()) > m_undoIndex + 1)
        {
            m_stack.PopBack();
        }
    }

    void EditorCommandStack::Notify()
    {
        if (OnChanged)
        {
            OnChanged();
        }
    }
    bool IEditorCommand::MergeInto(IEditorCommand& previous)
    {
        (void)previous;
        return false;
    }
}

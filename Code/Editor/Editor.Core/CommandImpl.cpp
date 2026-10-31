// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :command partition.
//
// The editor's undo and redo: IEditorCommand and EditorCommandStack.
//   * Every mutation is a command; a command whose Execute() fails is dropped, not pushed.
//   * A command merges into the stack top of the same type (a slider drag becomes one undo entry).
//   * Begin/EndGroup transactions undo and redo atomically, and consecutive groups of the same
//     type coalesce unless locked (LockGroup after, say, creating an entity and adding a component
//     keeps later identical actions separate).
// Each editor page owns its own stack (see :page); the context routes Edit > Undo and Redo to the
// active page's stack.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

module editor.core;

import foundation.core;

using namespace foundation::core;

namespace editor
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
                DIAGNOSTIC_ASSERT(ok); // re-executing a merged command must not fail
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
            DIAGNOSTIC_ASSERT(i >= 0); // unbalanced group markers
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
                DIAGNOSTIC_ASSERT(ok); // replaying a command that succeeded before must not fail
                (void)ok;
                ++i;
            }
            DIAGNOSTIC_ASSERT(i < static_cast<i64>(m_stack.Size())); // unbalanced group markers
            m_undoIndex = i;                                       // lands on the end marker
        }
        else
        {
            const bool ok = m_stack[static_cast<usize>(i)]->Execute();
            DIAGNOSTIC_ASSERT(ok);
            (void)ok;
            m_undoIndex = i;
        }
        Notify();
    }

    void EditorCommandStack::BeginGroup(StringView groupType)
    {
        DIAGNOSTIC_ASSERT(!m_inGroup); // no nested groups
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
            editor::EditorRootAllocator().New<detail::BeginGroupCommand>(groupType), editor::EditorRootAllocator()));
        ++m_undoIndex;
        m_inGroup = true;
        m_groupType = String(groupType);
    }

    void EditorCommandStack::EndGroup()
    {
        DIAGNOSTIC_ASSERT(m_inGroup);
        m_inGroup = false;
        // Nothing ran inside the group, so it must not leave an undo step that does nothing. The
        // begin marker is removed (it is the undo top), and nobody is notified, since nothing
        // changed.
        if (m_undoIndex >= 0 &&
            m_stack[static_cast<usize>(m_undoIndex)]->TypeId() == detail::kBeginGroupTypeId)
        {
            m_stack.PopBack();
            --m_undoIndex;
            return;
        }
        m_stack.PushBack(UniquePtr<IEditorCommand>(
            editor::EditorRootAllocator().New<detail::EndGroupCommand>(m_groupType.AsView()),
            editor::EditorRootAllocator()));
        ++m_undoIndex;
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
        DIAGNOSTIC_ASSERT(!m_inGroup);
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

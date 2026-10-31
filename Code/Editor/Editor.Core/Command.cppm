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

export module editor.core:command;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    // A single reversible edit. TypeId() is the identity for merging and grouping (a stable
    // literal such as "set_property"); commands of different types never merge.
    class IEditorCommand
    {
    public:
        virtual ~IEditorCommand() = default;

        // Apply the edit. Returning false means the edit did nothing (an invalid target, say);
        // the stack discards the command without pushing it.
        [[nodiscard]] virtual bool Execute() = 0;

        // Revert the edit. Only called after a successful Execute().
        virtual void Undo() = 0;

        // Stable identity for merging and group coalescing.
        [[nodiscard]] virtual StringView TypeId() const = 0;

        // Absorb this (newer) command into `previous`, which is already on the stack with the same
        // TypeId; typically this copies the new target value into `previous`. Return true if
        // absorbed: the stack then re-executes `previous` and discards this command.
        [[nodiscard]] virtual bool MergeInto(IEditorCommand& previous);
    };

    namespace detail
    {
        inline constexpr StringView kBeginGroupTypeId = u8"__begin_group";
        inline constexpr StringView kEndGroupTypeId = u8"__end_group";

        // Group brackets: inert markers on the stack; Undo/Redo unwind between them atomically.
        class BeginGroupCommand final : public IEditorCommand
        {
        public:
            explicit BeginGroupCommand(StringView groupType) : m_groupType(groupType) {}
            [[nodiscard]] bool Execute() override { return true; }
            void Undo() override {}
            [[nodiscard]] StringView TypeId() const override { return kBeginGroupTypeId; }
            [[nodiscard]] StringView GroupType() const { return m_groupType.AsView(); }

        private:
            String m_groupType;
        };

        class EndGroupCommand final : public IEditorCommand
        {
        public:
            explicit EndGroupCommand(StringView groupType) : m_groupType(groupType) {}
            [[nodiscard]] bool Execute() override { return true; }
            void Undo() override {}
            [[nodiscard]] StringView TypeId() const override { return kEndGroupTypeId; }
            [[nodiscard]] StringView GroupType() const { return m_groupType.AsView(); }

            bool locked = false; // a locked group never coalesces with the next same-type group

        private:
            String m_groupType;
        };
    }

    // Linear undo stack with an index. Redo re-executes, and any new command truncates the redo
    // tail. Not thread-safe (editor main thread only).
    class EditorCommandStack
    {
    public:
        EditorCommandStack() = default;
        EditorCommandStack(const EditorCommandStack&) = delete;
        EditorCommandStack& operator=(const EditorCommandStack&) = delete;

        /// Fired after any change (execute, undo, redo, clear), for dirty tracking and UI refresh.
        Function<void()> OnChanged;

        /// While locked (the editor's Simulate mode), Execute, Undo and Redo refuse: runtime
        /// mutations do not belong in the edit history, and undoing into entities the simulation
        /// replaced (or the restore on stop recreated) would act on stale guids.
        void SetLocked(bool locked) noexcept { m_locked = locked; }
        [[nodiscard]] bool IsLocked() const noexcept { return m_locked; }

        // Execute `command` and push it. Returns false (command destroyed, stack untouched)
        // if Execute() failed. May merge into the current top instead of pushing.
        bool Execute(UniquePtr<IEditorCommand> command);

        [[nodiscard]] bool CanUndo() const noexcept { return !m_inGroup && m_undoIndex >= 0; }
        [[nodiscard]] bool CanRedo() const noexcept;

        void Undo();

        void Redo();

        // Open a transaction: commands executed until EndGroup() undo/redo as one unit.
        // Consecutive groups of the same type coalesce (the previous group is reopened) unless
        // the previous group was locked. Nesting is not supported.
        void BeginGroup(StringView groupType);

        void EndGroup();

        // Prevent the most recent group from coalescing with the next same-type group.
        void LockGroup();

        void Clear();

        // Entry count including group markers (for diagnostics and tests).
        [[nodiscard]] usize Size() const noexcept { return m_stack.Size(); }
        [[nodiscard]] i64 UndoIndex() const noexcept { return m_undoIndex; }

    private:
        void TruncateRedo();

        void Notify();

        Array<UniquePtr<IEditorCommand>> m_stack;
        bool m_locked = false;
        i64 m_undoIndex = -1; // index of the last executed (undoable) entry
        bool m_inGroup = false;
        String m_groupType;
    };
}

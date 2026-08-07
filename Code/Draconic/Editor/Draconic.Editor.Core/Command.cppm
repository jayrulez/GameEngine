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

export module draconic.editor.core:command;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::editor
{
    // A single reversible edit. TypeId() is the merge/group identity (a stable literal, e.g.
    // "set_property"); commands of different types never merge.
    class IEditorCommand
    {
    public:
        virtual ~IEditorCommand() = default;

        // Apply the edit. Returning false means the edit did nothing (invalid target etc.);
        // the stack discards the command without pushing it.
        [[nodiscard]] virtual bool Execute() = 0;

        // Revert the edit. Only called after a successful Execute().
        virtual void Undo() = 0;

        // Stable identity for merging and group coalescing.
        [[nodiscard]] virtual StringView TypeId() const = 0;

        // Absorb THIS (newer) command into `previous` (already on the stack, same TypeId):
        // typically copy the new target value into `previous`. Return true if absorbed - the
        // stack then re-executes `previous` and discards this command.
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

    // Linear undo stack with an index; redoing re-executes, any new command truncates the redo
    // tail. Not thread-safe (editor main thread only).
    class EditorCommandStack
    {
    public:
        EditorCommandStack() = default;
        EditorCommandStack(const EditorCommandStack&) = delete;
        EditorCommandStack& operator=(const EditorCommandStack&) = delete;

        /// Fired after any change (execute/undo/redo/clear) - dirty tracking / UI refresh hook.
        Function<void()> OnChanged;

        /// While locked (the editor's Simulate mode), Execute/Undo/Redo refuse: runtime
        /// mutations don't belong on the edit-history, and undoing into entities the
        /// simulation replaced (or the stop-restore recreated) is a guid minefield.
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

        // Entry count including group markers (diagnostic / tests).
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

// EditorCommandStack tests: Lumix semantics per docs/design/editor.md §3.4 - execute/undo/redo,
// failed-execute drop, redo-tail truncation, same-type merge (slider drags), group transactions
// (atomic undo/redo, same-type coalescing, LockGroup).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.editor.core;

using namespace draconic::foundation;
using namespace draconic::editor;

namespace
{
    // Adds `delta` to a shared counter. TypeId "add"; merges by summing deltas.
    class AddCommand final : public IEditorCommand
    {
    public:
        AddCommand(i32& target, i32 delta, bool mergeable = false)
            : m_target(&target), m_delta(delta), m_mergeable(mergeable)
        {
        }

        [[nodiscard]] bool Execute() override
        {
            // Merged re-execution must first revert the previously-applied delta; track it.
            if (m_applied != 0)
            {
                *m_target -= m_applied;
            }
            *m_target += m_delta;
            m_applied = m_delta;
            return true;
        }
        void Undo() override
        {
            *m_target -= m_applied;
            m_applied = 0;
        }
        [[nodiscard]] StringView TypeId() const override { return u8"add"; }
        [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
        {
            if (!m_mergeable)
            {
                return false;
            }
            auto& prev = static_cast<AddCommand&>(previous);
            if (!prev.m_mergeable || prev.m_target != m_target)
            {
                return false;
            }
            prev.m_delta = m_delta; // absorb: previous now applies the NEW value
            return true;
        }

    private:
        i32* m_target;
        i32 m_delta;
        i32 m_applied = 0;
        bool m_mergeable;
    };

    class FailCommand final : public IEditorCommand
    {
    public:
        [[nodiscard]] bool Execute() override { return false; }
        void Undo() override {}
        [[nodiscard]] StringView TypeId() const override { return u8"fail"; }
    };

    UniquePtr<IEditorCommand> Add(i32& target, i32 delta, bool mergeable = false)
    {
        return UniquePtr<IEditorCommand>(
            DefaultAllocator().New<AddCommand>(target, delta, mergeable), DefaultAllocator());
    }
}

TEST_CASE("editor-commands: execute, undo, redo")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(!stack.CanUndo());
    CHECK(!stack.CanRedo());

    CHECK(stack.Execute(Add(value, 5)));
    CHECK(stack.Execute(Add(value, 3)));
    CHECK(value == 8);
    CHECK(stack.CanUndo());

    stack.Undo();
    CHECK(value == 5);
    CHECK(stack.CanRedo());

    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo();
    CHECK(value == 5);
    stack.Redo();
    CHECK(value == 8);
    CHECK(!stack.CanRedo());
}

TEST_CASE("editor-commands: failed Execute is dropped, not pushed")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(!stack.Execute(
        UniquePtr<IEditorCommand>(DefaultAllocator().New<FailCommand>(), DefaultAllocator())));
    CHECK(stack.Size() == 1); // only the add
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: new command truncates the redo tail")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    stack.Undo(); // value = 1, redo available
    CHECK(stack.CanRedo());

    CHECK(stack.Execute(Add(value, 10))); // truncates the +2 redo entry
    CHECK(value == 11);
    CHECK(!stack.CanRedo());

    stack.Undo();
    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("editor-commands: same-type merge coalesces a drag into one entry")
{
    EditorCommandStack stack;
    i32 value = 0;

    // A "slider drag": many mergeable sets; only ONE undo entry results.
    CHECK(stack.Execute(Add(value, 1, true)));
    CHECK(stack.Execute(Add(value, 2, true)));
    CHECK(stack.Execute(Add(value, 3, true)));
    CHECK(value == 3); // merged command re-applies the newest value
    CHECK(stack.Size() == 1);

    stack.Undo();
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo();
    CHECK(value == 3);
}

TEST_CASE("editor-commands: non-mergeable commands do not merge")
{
    EditorCommandStack stack;
    i32 value = 0;

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    CHECK(stack.Size() == 2);
}

TEST_CASE("editor-commands: groups undo and redo atomically")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"spawn");
    CHECK(stack.Execute(Add(value, 1)));
    CHECK(stack.Execute(Add(value, 2)));
    CHECK(!stack.CanUndo()); // undo/redo unavailable inside an open group
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo(); // the whole group
    CHECK(value == 0);
    CHECK(!stack.CanUndo());

    stack.Redo(); // the whole group
    CHECK(value == 3);
    CHECK(!stack.CanRedo());
}

TEST_CASE("editor-commands: consecutive same-type groups coalesce")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();

    stack.BeginGroup(u8"move"); // coalesces into the previous "move" group
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo(); // ONE undo reverts both
    CHECK(value == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("editor-commands: LockGroup prevents coalescing")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();
    stack.LockGroup();

    stack.BeginGroup(u8"move"); // locked: stays a separate group
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    CHECK(value == 3);
    stack.Undo();
    CHECK(value == 1); // only the second group reverted
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: different-type groups do not coalesce")
{
    EditorCommandStack stack;
    i32 value = 0;

    stack.BeginGroup(u8"move");
    CHECK(stack.Execute(Add(value, 1)));
    stack.EndGroup();

    stack.BeginGroup(u8"rotate");
    CHECK(stack.Execute(Add(value, 2)));
    stack.EndGroup();

    stack.Undo();
    CHECK(value == 1);
    stack.Undo();
    CHECK(value == 0);
}

TEST_CASE("editor-commands: empty group round-trips undo/redo")
{
    EditorCommandStack stack;
    i32 value = 0;

    // The Lumix play-mode-fence pattern: an empty group as a stack marker.
    stack.BeginGroup(u8"fence");
    stack.EndGroup();
    CHECK(stack.Execute(Add(value, 1)));

    stack.Undo(); // the add
    CHECK(value == 0);
    stack.Undo(); // the empty group (no-op, but index moves)
    CHECK(!stack.CanUndo());

    stack.Redo(); // the empty group
    stack.Redo(); // the add
    CHECK(value == 1);
}

TEST_CASE("editor-commands: OnChanged fires on mutations")
{
    EditorCommandStack stack;
    i32 value = 0;
    i32 changes = 0;
    stack.OnChanged = [&changes]() { ++changes; };

    CHECK(stack.Execute(Add(value, 1)));
    CHECK(changes == 1);
    stack.Undo();
    CHECK(changes == 2);
    stack.Redo();
    CHECK(changes == 3);
    stack.Clear();
    CHECK(changes == 4);
    CHECK(stack.Size() == 0);
    CHECK(!stack.CanUndo());
}

TEST_CASE("command-stack: locked stack refuses execute/undo/redo (Simulate mode)")
{
    EditorCommandStack stack;
    i32 value = 0;
    auto makeSet = [&](i32 target)
    {
        struct SetCommand final : IEditorCommand
        {
            i32* slot;
            i32 to;
            i32 from = 0;
            SetCommand(i32* s, i32 t) : slot(s), to(t) {}
            bool Execute() override
            {
                from = *slot;
                *slot = to;
                return true;
            }
            void Undo() override { *slot = from; }
            [[nodiscard]] StringView TypeId() const override { return u8"test.set"; }
        };
        return UniquePtr<IEditorCommand>(DefaultAllocator().New<SetCommand>(&value, target),
                                         DefaultAllocator());
    };

    REQUIRE(stack.Execute(makeSet(1)));
    CHECK(value == 1);

    stack.SetLocked(true);
    CHECK_FALSE(stack.Execute(makeSet(2))); // refused: no runtime edits on the history
    CHECK(value == 1);
    stack.Undo();
    CHECK(value == 1); // undo refused too

    stack.SetLocked(false);
    stack.Undo();
    CHECK(value == 0); // unlocked: history intact and working
    stack.Redo();
    CHECK(value == 1);
}

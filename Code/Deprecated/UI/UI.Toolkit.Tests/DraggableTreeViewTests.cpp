// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit DraggableTreeView: construct + exercise public methods (drag-enable/item-
// height round-trips, internal TreeView access, adapter wiring) and the IDropTarget path (CanAcceptDrop
// gating on format + adapter.CanMove; OnDrop invokes MoveItem + fires OnItemReordered). No rendering, no
// full drag-manager cycle. A flat 3-item test-double implements IReorderableTreeAdapter minimally.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    // Minimal flat 3-item reorderable adapter. Records the last MoveItem call.
    class FlatReorderAdapter : public IReorderableTreeAdapter
    {
    public:
        [[nodiscard]] i32 RootCount() const override { return 3; }
        [[nodiscard]] i32 GetChildCount(i32) const override { return 0; }
        [[nodiscard]] i32 GetChildId(i32, i32 childIndex) const override { return childIndex; }
        [[nodiscard]] i32 GetDepth(i32) const override { return 0; }
        [[nodiscard]] bool HasChildren(i32) const override { return false; }
        [[nodiscard]] RefPtr<View> CreateView(i32) override
        {
            return MakeRef<Label>(DefaultAllocator());
        }
        void BindView(View*, i32, i32, bool) override {}

        [[nodiscard]] bool CanMove(i32, i32) override { return true; }
        void MoveItem(i32 from, i32 to) override
        {
            LastFrom = from;
            LastTo = to;
            MoveCount++;
        }

        i32 LastFrom = -1;
        i32 LastTo = -1;
        i32 MoveCount = 0;
    };
}

TEST_CASE("toolkit-draggabletreeview: construct + property round-trips")
{
    auto view = core::MakeRef<DraggableTreeView>(core::DefaultAllocator());

    CHECK(view->DragEnabled()); // default true
    view->SetDragEnabled(false);
    CHECK_FALSE(view->DragEnabled());
    view->SetDragEnabled(true);
    CHECK(view->DragEnabled());

    view->SetItemHeight(28.0f);
    CHECK(view->ItemHeight() == doctest::Approx(28.0f));

    REQUIRE(view->InternalTreeView() != nullptr);
}

TEST_CASE("toolkit-draggabletreeview: SetAdapter wires without crash")
{
    auto view = core::MakeRef<DraggableTreeView>(core::DefaultAllocator());
    FlatReorderAdapter adapter;
    view->SetAdapter(&adapter);
    CHECK(view->InternalTreeView()->TreeAdapter == &adapter);
}

TEST_CASE("toolkit-draggabletreeview: CanAcceptDrop gating")
{
    auto view = core::MakeRef<DraggableTreeView>(core::DefaultAllocator());
    FlatReorderAdapter adapter;
    view->SetAdapter(&adapter);
    view->SetItemHeight(20.0f);

    IDropTarget* target = view->AsDropTarget();
    REQUIRE(target != nullptr);

    // Valid tree/reorder payload at a valid localY -> Move (adapter.CanMove true).
    auto dragData = core::MakeRef<TreeDragData>(core::DefaultAllocator(), 0);
    CHECK(target->CanAcceptDrop(dragData.Get(), 5.0f, 25.0f) == DragDropEffects::Move);

    // A non-"tree/reorder" payload -> None.
    auto otherData = core::MakeRef<DragData>(core::DefaultAllocator(), StringView(u8"text/plain"));
    CHECK(target->CanAcceptDrop(otherData.Get(), 5.0f, 25.0f) == DragDropEffects::None);
}

TEST_CASE("toolkit-draggabletreeview: OnDrop invokes MoveItem + fires OnItemReordered")
{
    auto view = core::MakeRef<DraggableTreeView>(core::DefaultAllocator());
    FlatReorderAdapter adapter;
    view->SetAdapter(&adapter);
    view->SetItemHeight(20.0f);

    i32 firedFrom = -1;
    i32 firedTo = -1;
    i32 fireCount = 0;
    view->OnItemReordered.Add(
        Event<void(DraggableTreeView*, i32, i32)>::Handler{[&](DraggableTreeView*, i32 from, i32 to)
                                                           {
                                                               firedFrom = from;
                                                               firedTo = to;
                                                               fireCount++;
                                                           }});

    IDropTarget* target = view->AsDropTarget();
    auto dragData = core::MakeRef<TreeDragData>(core::DefaultAllocator(), 0);

    // localY 45 / itemHeight 20 -> targetPos 2.
    const DragDropEffects effect = target->OnDrop(dragData.Get(), 5.0f, 45.0f);
    CHECK(effect == DragDropEffects::Move);

    CHECK(adapter.MoveCount == 1);
    CHECK(adapter.LastFrom == 0);
    CHECK(adapter.LastTo == 2);

    CHECK(fireCount == 1);
    CHECK(firedFrom == 0);
    CHECK(firedTo == 2);
}

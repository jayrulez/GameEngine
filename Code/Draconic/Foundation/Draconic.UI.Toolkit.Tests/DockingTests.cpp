// Faithful port of Sedulous.UI.Tests/src/DockingTests.bf (12 cases). Beef `scope`/`new` view trees
// become RefPtr-owned views; `===` ref-equality becomes pointer `==` (with .Get()); `Test.Assert` -> CHECK.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;
using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

// new Label("Content") -> a RefPtr<Label>; pass .Get() to AddPanel (the panel/tree adopts a ref).
static RefPtr<Label> MakeLabel(StringView text) { return MakeRef<Label>(DefaultAllocator(), text); }

TEST_CASE("docking: DockablePanel_Title")
{
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"My Panel"));
    CHECK(panel->Title() == StringView(u8"My Panel"));

    panel->SetTitle(StringView(u8"Renamed"));
    CHECK(panel->Title() == StringView(u8"Renamed"));
}

TEST_CASE("docking: DockablePanel_SetContent")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    ctx.AddRootView(root.Get());
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Test"));
    root->AddView(panel.Get());

    auto content = MakeRef<Label>(DefaultAllocator());
    panel->SetContent(content.Get());
    CHECK(panel->ContentView() == content.Get());
}

TEST_CASE("docking: DockSplit_RatioClamping")
{
    auto split = MakeRef<DockSplit>(DefaultAllocator());
    split->SetSplitRatio(-1);
    CHECK(split->SplitRatio() >= 0.05f);

    split->SetSplitRatio(2);
    CHECK(split->SplitRatio() <= 0.95f);
}

TEST_CASE("docking: DockZoneIndicator_AddTargets")
{
    auto indicator = MakeRef<DockZoneIndicator>(DefaultAllocator());
    CHECK(indicator->TargetCount() == 0);

    indicator->AddTarget(DockPosition::Left, Rectangle{0, 0, 100, 400}, nullptr);
    indicator->AddTarget(DockPosition::Right, Rectangle{300, 0, 100, 400}, nullptr);
    CHECK(indicator->TargetCount() == 2);

    indicator->ClearTargets();
    CHECK(indicator->TargetCount() == 0);
}

TEST_CASE("docking: DockZoneIndicator_UpdateHover")
{
    auto indicator = MakeRef<DockZoneIndicator>(DefaultAllocator());
    indicator->AddTarget(DockPosition::Left, Rectangle{0, 0, 100, 400}, nullptr);
    indicator->AddTarget(DockPosition::Right, Rectangle{300, 0, 100, 400}, nullptr);

    indicator->UpdateHover(50, 200);
    CHECK(indicator->HoveredTarget().HasValue());
    CHECK(indicator->HoveredTarget().Value().Position == DockPosition::Left);

    indicator->UpdateHover(350, 200);
    CHECK(indicator->HoveredTarget().Value().Position == DockPosition::Right);

    indicator->UpdateHover(200, 200);
    CHECK(!indicator->HoveredTarget().HasValue());
}

// === DockTabGroup (standalone, no DockManager) ===

TEST_CASE("docking: DockTabGroup_AddRemovePanel")
{
    auto group = MakeRef<DockTabGroup>(DefaultAllocator());
    auto p1 = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Panel 1"));
    auto p2 = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Panel 2"));

    group->AddPanel(p1.Get());
    CHECK(group->PanelCount() == 1);
    CHECK(group->SelectedIndex() == 0);

    group->AddPanel(p2.Get());
    CHECK(group->PanelCount() == 2);

    group->RemovePanel(p1.Get());
    CHECK(group->PanelCount() == 1);
    // Beef `delete p1` dropped: the local RefPtr frees p1 at scope end.
}

TEST_CASE("docking: DockTabGroup_SelectedIndex")
{
    auto group = MakeRef<DockTabGroup>(DefaultAllocator());
    auto p1 = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"A"));
    auto p2 = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"B"));

    group->AddPanel(p1.Get());
    group->AddPanel(p2.Get());

    CHECK(group->SelectedIndex() == 0);
    CHECK(group->SelectedPanel() == p1.Get());

    group->SetSelectedIndex(1);
    CHECK(group->SelectedPanel() == p2.Get());
}

// === DockSplit (standalone) ===

TEST_CASE("docking: DockSplit_SetChildren")
{
    auto split = MakeRef<DockSplit>(DefaultAllocator());
    auto child1 = MakeRef<DockTabGroup>(DefaultAllocator());
    auto child2 = MakeRef<DockTabGroup>(DefaultAllocator());

    split->SetChildren(child1.Get(), child2.Get());
    CHECK(split->First() == child1.Get());
    CHECK(split->Second() == child2.Get());
}

// === DockableWindow (standalone) ===

TEST_CASE("docking: DockableWindow_DetachPanel")
{
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Test"));
    auto dw = MakeRef<DockableWindow>(DefaultAllocator(), panel.Get());

    CHECK(dw->Panel() == panel.Get());

    DockablePanel* detached = dw->DetachPanel();
    CHECK(detached == panel.Get());
    CHECK(dw->Panel() == nullptr);
    // Beef `delete panel` dropped: the local RefPtr frees panel at scope end.
}

// === DockManager (with context) ===

TEST_CASE("docking: DockManager_AddPanel")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);

    DockablePanel* panel = dm->AddPanel(StringView(u8"Test"), MakeLabel(u8"Content").Get());
    CHECK(panel != nullptr);
    CHECK(panel->Title() == StringView(u8"Test"));
    CHECK(panel->DockHost == dm.Get());

    // Dock the panel so it's in the view tree and gets cleaned up.
    dm->DockPanel(panel, DockPosition::Center);
}

TEST_CASE("docking: DockManager_DockPanel_Center")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);

    DockablePanel* panel = dm->AddPanel(StringView(u8"P1"), MakeLabel(u8"Content 1").Get());
    dm->DockPanel(panel, DockPosition::Center);

    CHECK(dm->RootNode() != nullptr);
}

TEST_CASE("docking: DockManager_DockPanel_Split")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());
    ctx.BeginFrame(0.016f);
    root->Measure(BoxConstraints::Tight(800, 600));
    root->Layout(0, 0, 800, 600);

    DockablePanel* p1 = dm->AddPanel(StringView(u8"P1"), MakeLabel(u8"Content 1").Get());
    DockablePanel* p2 = dm->AddPanel(StringView(u8"P2"), MakeLabel(u8"Content 2").Get());

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Right);

    CHECK(Cast<DockSplit>(dm->RootNode()) != nullptr);
}

// DELIBERATE DEVIATION from the Sedulous port (user-approved 2026-07-11): docking a panel into a
// tab group makes it the ACTIVE tab (mainstream-IDE behavior). Upstream keeps the existing
// selection and its editor calls ActivatePanel by hand.
TEST_CASE("docking: DockPanel_Center_ActivatesDockedTab")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"P1"), MakeLabel(u8"Content 1").Get());
    DockablePanel* p2 = dm->AddPanel(StringView(u8"P2"), MakeLabel(u8"Content 2").Get());
    DockablePanel* p3 = dm->AddPanel(StringView(u8"P3"), MakeLabel(u8"Content 3").Get());

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Center); // tabs with p1 - and becomes the active tab

    auto* group = Cast<DockTabGroup>(p2->Parent);
    REQUIRE(group != nullptr);
    CHECK(group->PanelCount() == 2);
    CHECK(group->SelectedPanel() == p2);

    // Same through the relative-to path.
    dm->DockPanelRelativeTo(p3, DockPosition::Center, p1->Parent);
    CHECK(group->SelectedPanel() == p3);

    // Explicit activation still works on a background tab.
    dm->ActivatePanel(p1);
    CHECK(group->SelectedPanel() == p1);
}

// DraggableTreeView drop-zones: the middle band of a row is a drop-INTO target (CanDropInto/
// DropInto, added for the editor hierarchy's reparenting), the edge bands keep the original
// between-rows reorder semantics; each falls back to the other when unsupported.
TEST_CASE("docking: DraggableTreeView_DropIntoZones")
{
    // A 3-root flat tree adapter recording what happened.
    class RecordingAdapter final : public IReorderableTreeAdapter
    {
    public:
        i32 movedFrom = -1, movedTo = -1, intoFrom = -1, intoTo = -1;
        bool allowMove = true, allowInto = true;

        [[nodiscard]] i32 RootCount() const override { return 3; }
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override { return nodeId == -1 ? 3 : 0; }
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
        {
            return parentId == -1 ? childIndex : -1;
        }
        [[nodiscard]] i32 GetDepth(i32) const override { return 0; }
        [[nodiscard]] bool HasChildren(i32) const override { return false; }
        [[nodiscard]] RefPtr<View> CreateView(i32) override
        {
            return MakeRef<Label>(DefaultAllocator(), StringView{});
        }
        void BindView(View*, i32, i32, bool) override {}
        [[nodiscard]] bool CanMove(i32, i32) override { return allowMove; }
        void MoveItem(i32 from, i32 to) override
        {
            movedFrom = from;
            movedTo = to;
        }
        [[nodiscard]] bool CanDropInto(i32, i32) override { return allowInto; }
        void DropInto(i32 from, i32 to) override
        {
            intoFrom = from;
            intoTo = to;
        }
    };

    RecordingAdapter adapter;
    auto tree = MakeRef<DraggableTreeView>(DefaultAllocator());
    tree->SetItemHeight(20.0f);
    tree->SetAdapter(&adapter);
    auto drag = MakeRef<TreeDragData>(DefaultAllocator(), 0);

    // Middle of row 2 (y = 50) = drop-INTO zone.
    CHECK(tree->CanAcceptDrop(drag.Get(), 0, 50.0f) == DragDropEffects::Move);
    CHECK(tree->OnDrop(drag.Get(), 0, 50.0f) == DragDropEffects::Move);
    CHECK(adapter.intoFrom == 0);
    CHECK(adapter.intoTo == 2);
    CHECK(adapter.movedFrom == -1); // reorder path untouched

    // Top edge of row 2 (y = 41) = reorder boundary BEFORE row 2.
    CHECK(tree->OnDrop(drag.Get(), 0, 41.0f) == DragDropEffects::Move);
    CHECK(adapter.movedFrom == 0);
    CHECK(adapter.movedTo == 2);

    // Bottom edge of row 1 (y = 38, frac 0.9) = boundary AFTER row 1 (= before row 2).
    CHECK(tree->OnDrop(drag.Get(), 0, 38.0f) == DragDropEffects::Move);
    CHECK(adapter.movedTo == 2);

    // Below the last row (y = 100, 3 rows x 20px) = end-of-list boundary (count).
    CHECK(tree->OnDrop(drag.Get(), 0, 100.0f) == DragDropEffects::Move);
    CHECK(adapter.movedTo == 3);

    // Reorder unsupported: the edge band falls back to drop-into.
    adapter.allowMove = false;
    adapter.intoTo = -1;
    CHECK(tree->OnDrop(drag.Get(), 0, 41.0f) == DragDropEffects::Move);
    CHECK(adapter.intoTo == 2);

    // Neither supported: refused.
    adapter.allowInto = false;
    CHECK(tree->CanAcceptDrop(drag.Get(), 0, 50.0f) == DragDropEffects::None);
    CHECK(tree->OnDrop(drag.Get(), 0, 50.0f) == DragDropEffects::None);
}

// A press ANYWHERE inside a panel's subtree announces it through OnPanelActivated (capture
// phase - runs before the target handles the click, never consumes it). This is what keeps the
// app's ACTIVE page in sync when side-by-side tab groups make a panel visible without tab
// clicks (its group's SetSelectedIndex early-outs, so tab selection can't re-announce it).
TEST_CASE("docking: press inside panel content activates the panel")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dock = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dock.Get());
    auto contentA = MakeLabel(u8"A");
    auto contentB = MakeLabel(u8"B");
    DockablePanel* a = dock->AddPanel(u8"A", contentA.Get());
    DockablePanel* b = dock->AddPanel(u8"B", contentB.Get());
    // AddPanel only REGISTERS; dock A center, then B RIGHT of A's group - both visible
    // side-by-side, each its own tab group (the shell's idiom).
    dock->DockPanel(a, DockPosition::Center);
    dock->DockPanelRelativeTo(b, DockPosition::Right, a->Parent);

    // Two passes: the dock re-selects tab groups during the first layout; sizes apply next.
    ctx.BeginFrame(0.016f);
    ctx.UpdateRootView(root.Get());
    ctx.BeginFrame(0.016f);
    ctx.UpdateRootView(root.Get());

    DockablePanel* activated = nullptr;
    dock->OnPanelActivated.Add(
        Event<void(DockablePanel*)>::Handler{[&activated](DockablePanel* p) { activated = p; }});

    // Press in the middle of each panel's CONTENT area (not the tab strip).
    auto pressInside = [&](DockablePanel* panel)
    {
        const Float2 screen =
            panel->LocalToScreen(Float2{panel->Width() * 0.5f, panel->Height() * 0.7f});
        (void)ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, screen.x, screen.y, 0.0f);
        (void)ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, screen.x, screen.y);
    };

    pressInside(a);
    CHECK(activated == a);
    pressInside(b);
    CHECK(activated == b);
    pressInside(a);
    CHECK(activated == a);
}

// The close-gesture veto: RequestClose consults the interceptor (the editor prompts on dirty
// pages); OnCloseRequested.Invoke stays the un-vetoed programmatic path (the prompt's own
// Save/Discard buttons use it).
TEST_CASE("docking: close interceptor vetoes gestures but not direct closes")
{
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Doc"));
    i32 closed = 0;
    panel->OnCloseRequested.Add([&closed](DockablePanel*) { ++closed; });

    bool allow = false;
    i32 asked = 0;
    panel->OnCloseInterceptor = [&](DockablePanel*)
    {
        ++asked;
        return allow;
    };

    panel->RequestClose(); // vetoed
    CHECK(asked == 1);
    CHECK(closed == 0);

    allow = true;
    panel->RequestClose(); // allowed through
    CHECK(asked == 2);
    CHECK(closed == 1);

    allow = false;
    panel->OnCloseRequested.Invoke(panel.Get()); // direct: no veto consulted
    CHECK(asked == 2);
    CHECK(closed == 2);
}

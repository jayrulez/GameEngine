// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Faithful port of Sedulous.UI.Tests/src/DockingTests.bf (12 cases). Beef `scope`/`new` view trees
// become RefPtr-owned views; `===` ref-equality becomes pointer `==` (with .Get()); `Test.Assert` -> CHECK.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;

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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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
    UIContext ctx{DefaultAllocator()};
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

// === docking-v2: OS-chromed float windows + drop previews ===

namespace
{
    // Headless IDockableWindowHost: owns created window views (the real host parents them into a
    // RootView) and records the close callbacks so tests can simulate the OS close button.
    class FakeWindowHost : public IDockableWindowHost
    {
    public:
        bool Chrome = false;
        Array<RefPtr<View>> Created;
        Function<void(View*)> LastOnClose;
        i32 Destroyed = 0;

        bool SupportsOSWindows() override { return true; }
        bool UsesOSChrome() override { return Chrome; }
        void CreateDockableWindow(View* view, f32, f32, f32, f32,
                                  Function<void(View*)> onCloseRequested) override
        {
            Created.PushBack(RefPtr<View>(view));
            LastOnClose = Move(onCloseRequested);
        }
        void DestroyDockableWindow(View*) override { ++Destroyed; }
        void MoveDockableWindow(View*, f32, f32) override {}
        void ResizeDockableWindow(View*, f32, f32, f32, f32) override {}
        bool TryGetDockableWindowBounds(View*, f32& x, f32& y, f32& width, f32& height) override
        {
            x = 0;
            y = 0;
            width = 300;
            height = 250;
            return true;
        }
        void GetGlobalMousePosition(f32& globalX, f32& globalY) override
        {
            globalX = 0;
            globalY = 0;
        }
    };
}

// The host's chrome policy propagates to the float window and flips the panel/adorner behavior:
// chromed = OS owns close/resize, the panel hides its X, and a re-dock drag shows the tab-chip
// adorner (the window stays put); borderless = the window itself follows the drag, no adorner.
TEST_CASE("docking: float window inherits host chrome policy")
{
    const bool chromeModes[] = {true, false};
    for (const bool chrome : chromeModes)
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRef<RootView>(DefaultAllocator());
        root->ViewportSize = Float2{800, 600};
        ctx.AddRootView(root.Get());

        auto dm = MakeRef<DockManager>(DefaultAllocator());
        root->AddView(dm.Get());
        FakeWindowHost host;
        host.Chrome = chrome;
        dm->DockableWindowHost = &host;

        DockablePanel* panel = dm->AddPanel(StringView(u8"Scene"), MakeLabel(u8"C").Get());
        dm->FloatPanel(panel, 10, 10);

        REQUIRE(host.Created.Size() == 1);
        auto* fw = Cast<DockableWindow>(host.Created[0].Get());
        REQUIRE(fw != nullptr);
        CHECK(fw->IsOSWindow);
        CHECK(fw->HasOSChrome == chrome);
        CHECK(panel->InChromedOSWindow() == chrome);

        // Chromed: adorner rides the cursor (window stays put). Borderless: window follows,
        // adorner suppressed.
        RefPtr<View> visual = panel->CreateDragVisual(nullptr);
        CHECK((visual.Get() != nullptr) == chrome);

        // Chromed: inner resize edges are the OS's job - edge hit-tests fall through to content.
        fw->Measure(BoxConstraints::Tight(300, 250));
        fw->Layout(0, 0, 300, 250);
        View* edgeHit = fw->HitTest(Float2{2, 125});
        if (chrome)
        {
            CHECK(edgeHit != fw);
        }
        else
        {
            CHECK(edgeHit == fw);
        }
    }
}

// The OS close button (host onCloseRequested) routes through the panel's RequestClose, so the
// close interceptor (dirty-page veto) applies to chromed windows exactly like the drawn X.
TEST_CASE("docking: OS close request respects the panel close interceptor")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());
    FakeWindowHost host;
    host.Chrome = true;
    dm->DockableWindowHost = &host;

    DockablePanel* panel = dm->AddPanel(StringView(u8"Doc"), MakeLabel(u8"C").Get());
    panel->SetPersistenceId(StringView(u8"doc"));
    bool allow = false;
    i32 asked = 0;
    panel->OnCloseInterceptor = [&](DockablePanel*)
    {
        ++asked;
        return allow;
    };

    dm->FloatPanel(panel, 10, 10);
    REQUIRE(host.LastOnClose);

    host.LastOnClose(host.Created[0].Get()); // vetoed: float survives
    CHECK(asked == 1);
    CHECK(host.Destroyed == 0);
    CHECK(dm->FindPanelById(StringView(u8"doc")) == panel);

    allow = true;
    host.LastOnClose(host.Created[0].Get()); // allowed: the window is torn down...
    CHECK(asked == 2);
    CHECK(host.Destroyed == 1);
    // ...and the panel, a TOOL panel (it has a persistence id), hides: registered, out of the
    // tree, alive for a Reset Layout or a layout restore (2026-09-23).
    CHECK(dm->FindPanelById(StringView(u8"doc")) == panel);
    CHECK(panel->Parent == nullptr);
    CHECK(!panel->IsPendingDeletion);
}

// Zone targets carry the would-be dock region; hovering exposes it for the drop-preview overlay.
TEST_CASE("docking: zone indicator carries drop preview rect")
{
    auto indicator = MakeRef<DockZoneIndicator>(DefaultAllocator());
    indicator->AddTarget(DockPosition::Left, Rectangle{8, 180, 40, 40}, nullptr,
                         Rectangle{0, 0, 400, 600});
    indicator->AddTarget(DockPosition::Right, Rectangle{752, 180, 40, 40}, nullptr);

    indicator->UpdateHover(20, 200);
    REQUIRE(indicator->HoveredTarget().HasValue());
    const DockTarget left = indicator->HoveredTarget().Value();
    CHECK(left.PreviewRect.width == 400);
    CHECK(left.PreviewRect.height == 600);

    indicator->UpdateHover(760, 200); // legacy AddTarget: zero-area preview (no overlay)
    REQUIRE(indicator->HoveredTarget().HasValue());
    CHECK(indicator->HoveredTarget().Value().PreviewRect.width == 0);
}

// The editor's Reset Layout crash (RTHomes1, 2026-09-23): the shell borrows raw pointers to its
// tool panels, the user closed Assets, ClosePanel destroyed it, and DockDefaults then read the
// dead panel's Parent. A tool panel (persistence id) now hides on close and stays registered, so
// the default arrangement can be rebuilt over it; a page panel (no id) is still destroyed.
TEST_CASE("docking: closing a tool panel hides it and keeps it registered; the default layout rebuilds over it; a page panel is destroyed")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());
    ctx.BeginFrame(0.016f);

    DockablePanel* welcome = dm->AddPanel(StringView(u8"Welcome"), MakeLabel(u8"w").Get());
    welcome->SetPersistenceId(u8"welcome");
    welcome->SetClosable(false);
    DockablePanel* console = dm->AddPanel(StringView(u8"Console"), MakeLabel(u8"c").Get());
    console->SetPersistenceId(u8"console");
    DockablePanel* assets = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"a").Get());
    assets->SetPersistenceId(u8"assets");
    const auto dockDefaults = [&]()
    {
        // The editor shell's DockDefaults, verbatim.
        dm->DockPanel(welcome, DockPosition::Center);
        dm->DockPanel(console, DockPosition::Bottom);
        dm->DockPanelRelativeTo(assets, DockPosition::Center, console->Parent);
        dm->ActivatePanel(console);
    };
    dockDefaults();
    ctx.BeginFrame(0.016f);
    REQUIRE(Cast<DockTabGroup>(assets->Parent) != nullptr);
    CHECK(dm->RegisteredPanelCount() == 3);

    // The user closes Assets (the close gesture's un-vetoed path): undocked, registered, alive.
    assets->OnCloseRequested.Invoke(assets);
    ctx.BeginFrame(0.016f); // the deferred deletes run here: the panel must not be among them
    ctx.BeginFrame(0.016f);
    CHECK(assets->Parent == nullptr);
    CHECK(dm->RegisteredPanelCount() == 3);
    CHECK(dm->FindPanelById(u8"assets") == assets);
    CHECK(!assets->IsPendingDeletion);

    // View > Reset Layout: the same three pointers, the same sequence - and Assets is back.
    dockDefaults();
    ctx.BeginFrame(0.016f);
    CHECK(Cast<DockTabGroup>(assets->Parent) != nullptr);
    CHECK(Cast<DockTabGroup>(console->Parent) != nullptr);
    CHECK(assets->Parent == console->Parent); // tabbed with the console, as the default says
    dockDefaults(); // and again, with nothing closed: idempotent
    ctx.BeginFrame(0.016f);
    CHECK(dm->RegisteredPanelCount() == 3);
    CHECK(Cast<DockTabGroup>(assets->Parent) != nullptr);

    // A page panel (no persistence id) still dies on close: dropped from the registry.
    DockablePanel* page = dm->AddPanel(StringView(u8"Scene"), MakeLabel(u8"s").Get());
    dm->DockPanelRelativeTo(page, DockPosition::Center, welcome->Parent);
    ctx.BeginFrame(0.016f);
    CHECK(dm->RegisteredPanelCount() == 4);
    RefPtr<DockablePanel> pin(page); // observe the deletion without touching freed memory
    page->OnCloseRequested.Invoke(page);
    CHECK(dm->RegisteredPanelCount() == 3);
    CHECK(pin->IsPendingDeletion);
    ctx.BeginFrame(0.016f);
    ctx.BeginFrame(0.016f);
    CHECK(pin->Parent == nullptr);
}

TEST_CASE("docking: the owner's ActiveMark on a panel makes its group the active one - a click into "
          "another panel does not move it, and only the mark does")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    // Two page groups side by side, and a tool panel below: every group has a selected tab.
    DockablePanel* left = dm->AddPanel(StringView(u8"Scene A"), MakeLabel(u8"A").Get());
    dm->DockPanel(left, DockPosition::Center);
    DockablePanel* right = dm->AddPanel(StringView(u8"Scene B"), MakeLabel(u8"B").Get());
    dm->DockPanel(right, DockPosition::Right);
    DockablePanel* assets = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"assets").Get());
    dm->DockPanel(assets, DockPosition::Bottom);
    auto* leftGroup = Cast<DockTabGroup>(left->Parent);
    auto* rightGroup = Cast<DockTabGroup>(right->Parent);
    auto* toolGroup = Cast<DockTabGroup>(assets->Parent);
    REQUIRE(leftGroup != nullptr);
    REQUIRE(rightGroup != nullptr);
    REQUIRE(toolGroup != nullptr);
    CHECK_FALSE(leftGroup->IsActiveGroup()); // nothing marked yet
    CHECK_FALSE(rightGroup->IsActiveGroup());

    // The owner marks the page a command goes to.
    right->ActiveMark = true;
    CHECK(rightGroup->IsActiveGroup());
    CHECK_FALSE(leftGroup->IsActiveGroup());
    // A click into the tool panel activates it for the DOCK (OnPanelActivated, ActivePanel)
    // but moves no mark: the ring stays on the page.
    DockablePanel* activated = nullptr;
    dm->OnPanelActivated.Add(Event<void(DockablePanel*)>::Handler{
        [&activated](DockablePanel* p) { activated = p; }});
    MouseEventArgs press;
    assets->OnMouseDownCapture(press);
    CHECK(activated == assets);
    CHECK(dm->ActivePanel() == assets);
    CHECK(rightGroup->IsActiveGroup());
    CHECK_FALSE(toolGroup->IsActiveGroup());
    // The dock's active panel follows a tab selection too, and a closed one never stays it.
    DockablePanel* extra = dm->AddPanel(StringView(u8"Scene D"), MakeLabel(u8"D").Get());
    rightGroup->AddPanel(extra);
    rightGroup->SetSelectedIndex(1);
    CHECK(dm->ActivePanel() == extra);
    dm->ClosePanel(extra);
    CHECK(dm->ActivePanel() == right); // the group re-selected its remaining tab
    rightGroup->SetSelectedIndex(0);
    // A click on the tab that is ALREADY selected (the other group's one page) activates it
    // too: the selection does not change, the activation still moves.
    activated = nullptr;
    leftGroup->ActivateSelected(); // its tab is already selected: only the click activates
    CHECK(dm->ActivePanel() == left);
    rightGroup->ActivateSelected();
    CHECK(activated == right);
    CHECK(dm->ActivePanel() == right);
    // The owner moves the mark: the ring follows.
    right->ActiveMark = false;
    left->ActiveMark = true;
    CHECK(leftGroup->IsActiveGroup());
    CHECK_FALSE(rightGroup->IsActiveGroup());
    // A second tab in the marked group, selected: the mark is the panel's, so the group is
    // active only while the marked panel is the selected one.
    DockablePanel* second = dm->AddPanel(StringView(u8"Scene C"), MakeLabel(u8"C").Get());
    leftGroup->AddPanel(second);
    leftGroup->SetSelectedIndex(1);
    CHECK_FALSE(leftGroup->IsActiveGroup());
    leftGroup->SetSelectedIndex(0);
    CHECK(leftGroup->IsActiveGroup());
}

// With no root and no window host (a layout restored headless), nothing but the manager holds a
// floated panel's window: it must live until the panel docks again, then go.
TEST_CASE("docking: with no root or host, a floated panel's window lives until the panel docks again")
{
    auto dm = MakeRef<DockManager>(DefaultAllocator());
    DockablePanel* panel = dm->AddPanel(StringView(u8"Scene"), MakeLabel(u8"C").Get());
    dm->FloatPanel(panel, 10, 10);

    const WeakRefPtr<DockableWindow> window(RefPtr<DockableWindow>(Cast<DockableWindow>(panel->Parent)));
    REQUIRE_FALSE(window.Expired());
    CHECK(window.Lock()->Panel() == panel);

    dm->DockPanel(panel, DockPosition::Left);
    CHECK(window.Expired());
    CHECK(Cast<DockableWindow>(panel->Parent) == nullptr);
}

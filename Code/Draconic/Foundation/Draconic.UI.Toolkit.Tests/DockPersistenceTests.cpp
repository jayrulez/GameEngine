// Faithful port of Sedulous.UI.Tests/src/DockPersistenceTests.bf (19 cases: PersistenceId, ExportLayout,
// ApplyLayout, and Export->Apply round-trips). Beef `scope`/`new` view trees become RefPtr-owned views;
// `===` -> pointer `==`; heap `DockLayoutNode` -> stack value / UniquePtr; `defer delete` dropped (RAII).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;
using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

// new Label("...") -> a RefPtr<Label>; pass .Get() to AddPanel (the panel/tree adopts a ref).
static RefPtr<Label> MakeLabel(StringView text) { return MakeRef<Label>(DefaultAllocator(), text); }

// Beef private helper CountPanels(DockLayoutNode) -> recurse over the value struct via raw pointers.
static int CountPanels(const DockLayoutNode* node)
{
    if (node == nullptr)
    {
        return 0;
    }
    if (node->Type == DockLayoutNodeType::TabGroup)
    {
        return static_cast<int>(node->PanelIds.Size());
    }
    return CountPanels(node->First.Get()) + CountPanels(node->Second.Get());
}

// === PersistenceId ===

TEST_CASE("dock-persistence: PersistenceId_DefaultEmpty")
{
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Test"));
    CHECK(panel->PersistenceId().Size() == 0);
}

TEST_CASE("dock-persistence: PersistenceId_SetAndGet")
{
    auto panel = MakeRef<DockablePanel>(DefaultAllocator(), StringView(u8"Test"));
    panel->SetPersistenceId(StringView(u8"my_panel"));
    CHECK(panel->PersistenceId() == StringView(u8"my_panel"));
}

TEST_CASE("dock-persistence: FindPanelById_Found")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));
    dm->DockPanel(panel, DockPosition::Center);

    DockablePanel* found = dm->FindPanelById(StringView(u8"assets"));
    CHECK(found == panel);
}

TEST_CASE("dock-persistence: FindPanelById_NotFound")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));
    dm->DockPanel(panel, DockPosition::Center);

    CHECK(dm->FindPanelById(StringView(u8"nonexistent")) == nullptr);
}

// === ExportLayout ===

TEST_CASE("dock-persistence: ExportLayout_EmptyTree_ReturnsNull")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    auto layout = dm->ExportLayout();
    CHECK(layout.Get() == nullptr);
}

TEST_CASE("dock-persistence: ExportLayout_SinglePanel")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));
    dm->DockPanel(panel, DockPosition::Center);

    auto layout = dm->ExportLayout();

    CHECK(layout.Get() != nullptr);
    CHECK(layout->Type == DockLayoutNodeType::TabGroup);
    CHECK(layout->PanelIds.Size() == 1);
    CHECK(layout->PanelIds[0] == StringView(u8"assets"));
    CHECK(layout->ActiveTabIndex == 0);
}

TEST_CASE("dock-persistence: ExportLayout_TwoTabs")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"C1").Get());
    p1->SetPersistenceId(StringView(u8"assets"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Console"), MakeLabel(u8"C2").Get());
    p2->SetPersistenceId(StringView(u8"console"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanelRelativeTo(p2, DockPosition::Center, p1->Parent);

    auto layout = dm->ExportLayout();

    CHECK(layout.Get() != nullptr);
    CHECK(layout->Type == DockLayoutNodeType::TabGroup);
    CHECK(layout->PanelIds.Size() == 2);
    CHECK(layout->PanelIds[0] == StringView(u8"assets"));
    CHECK(layout->PanelIds[1] == StringView(u8"console"));
}

TEST_CASE("dock-persistence: ExportLayout_HorizontalSplit")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Left"), MakeLabel(u8"L").Get());
    p1->SetPersistenceId(StringView(u8"left"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Right"), MakeLabel(u8"R").Get());
    p2->SetPersistenceId(StringView(u8"right"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Right);

    auto layout = dm->ExportLayout();

    CHECK(layout.Get() != nullptr);
    CHECK(layout->Type == DockLayoutNodeType::Split);
    CHECK(layout->Direction == Orientation::Horizontal);
    CHECK(layout->First.Get() != nullptr);
    CHECK(layout->Second.Get() != nullptr);
    CHECK(layout->First->Type == DockLayoutNodeType::TabGroup);
    CHECK(layout->Second->Type == DockLayoutNodeType::TabGroup);
    CHECK(layout->First->PanelIds[0] == StringView(u8"left"));
    CHECK(layout->Second->PanelIds[0] == StringView(u8"right"));
}

TEST_CASE("dock-persistence: ExportLayout_PreservesSplitRatio")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Left"), MakeLabel(u8"L").Get());
    p1->SetPersistenceId(StringView(u8"left"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Right"), MakeLabel(u8"R").Get());
    p2->SetPersistenceId(StringView(u8"right"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Right);

    // Adjust split ratio.
    if (auto* split = Cast<DockSplit>(dm->RootNode()))
    {
        split->SetSplitRatio(0.3f);
    }

    auto layout = dm->ExportLayout();

    CHECK(Abs(layout->SplitRatio - 0.3f) < 0.01f);
}

TEST_CASE("dock-persistence: ExportLayout_NestedSplit")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Editor"), MakeLabel(u8"E").Get());
    p1->SetPersistenceId(StringView(u8"editor"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"A").Get());
    p2->SetPersistenceId(StringView(u8"assets"));
    DockablePanel* p3 = dm->AddPanel(StringView(u8"Inspector"), MakeLabel(u8"I").Get());
    p3->SetPersistenceId(StringView(u8"inspector"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Bottom);
    dm->DockPanel(p3, DockPosition::Right);

    auto layout = dm->ExportLayout();

    CHECK(layout.Get() != nullptr);
    CHECK(layout->Type == DockLayoutNodeType::Split);

    // Should be a nested split tree with all 3 panels.
    const int panelCount = CountPanels(layout.Get());
    CHECK(panelCount == 3);
}

// === ApplyLayout ===

TEST_CASE("dock-persistence: ApplyLayout_SinglePanel")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));

    // Build layout manually.
    DockLayoutNode layout;
    layout.Type = DockLayoutNodeType::TabGroup;
    layout.PanelIds.PushBack(String(u8"assets"));
    layout.ActiveTabIndex = 0;

    dm->ApplyLayout(&layout);

    // Panel should be docked.
    CHECK(panel->Parent != nullptr);
    CHECK(dm->RootNode() != nullptr);
}

TEST_CASE("dock-persistence: ApplyLayout_TwoTabs")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"C1").Get());
    p1->SetPersistenceId(StringView(u8"assets"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Console"), MakeLabel(u8"C2").Get());
    p2->SetPersistenceId(StringView(u8"console"));

    DockLayoutNode layout;
    layout.Type = DockLayoutNodeType::TabGroup;
    layout.PanelIds.PushBack(String(u8"assets"));
    layout.PanelIds.PushBack(String(u8"console"));
    layout.ActiveTabIndex = 1;

    dm->ApplyLayout(&layout);

    CHECK(Cast<DockTabGroup>(dm->RootNode()) != nullptr);
    auto* group = Cast<DockTabGroup>(dm->RootNode());
    CHECK(group->PanelCount() == 2);
    CHECK(group->SelectedIndex() == 1);
}

TEST_CASE("dock-persistence: ApplyLayout_Split")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Left"), MakeLabel(u8"L").Get());
    p1->SetPersistenceId(StringView(u8"left"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Right"), MakeLabel(u8"R").Get());
    p2->SetPersistenceId(StringView(u8"right"));

    DockLayoutNode layout;
    layout.Type = DockLayoutNodeType::Split;
    layout.Direction = Orientation::Horizontal;
    layout.SplitRatio = 0.7f;

    auto firstNode = MakeUnique<DockLayoutNode>(DefaultAllocator());
    firstNode->Type = DockLayoutNodeType::TabGroup;
    firstNode->PanelIds.PushBack(String(u8"left"));
    layout.First = Move(firstNode);

    auto secondNode = MakeUnique<DockLayoutNode>(DefaultAllocator());
    secondNode->Type = DockLayoutNodeType::TabGroup;
    secondNode->PanelIds.PushBack(String(u8"right"));
    layout.Second = Move(secondNode);

    dm->ApplyLayout(&layout);

    CHECK(Cast<DockSplit>(dm->RootNode()) != nullptr);
    auto* split = Cast<DockSplit>(dm->RootNode());
    CHECK(split->Orientation() == Orientation::Horizontal);
    CHECK(Abs(split->SplitRatio() - 0.7f) < 0.01f);
    CHECK(Cast<DockTabGroup>(split->First()) != nullptr);
    CHECK(Cast<DockTabGroup>(split->Second()) != nullptr);
}

TEST_CASE("dock-persistence: ApplyLayout_UnknownPanelId_Skipped")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));

    DockLayoutNode layout;
    layout.Type = DockLayoutNodeType::TabGroup;
    layout.PanelIds.PushBack(String(u8"nonexistent"));
    layout.PanelIds.PushBack(String(u8"assets"));

    dm->ApplyLayout(&layout);

    // Only the known panel should be docked.
    CHECK(Cast<DockTabGroup>(dm->RootNode()) != nullptr);
    auto* group = Cast<DockTabGroup>(dm->RootNode());
    CHECK(group->PanelCount() == 1);
}

TEST_CASE("dock-persistence: ApplyLayout_EmptySplitBranch_Collapsed")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Only"), MakeLabel(u8"O").Get());
    panel->SetPersistenceId(StringView(u8"only"));

    // Layout has a split but one side references unknown panels.
    DockLayoutNode layout;
    layout.Type = DockLayoutNodeType::Split;
    layout.Direction = Orientation::Horizontal;
    layout.SplitRatio = 0.5f;

    auto firstNode = MakeUnique<DockLayoutNode>(DefaultAllocator());
    firstNode->Type = DockLayoutNodeType::TabGroup;
    firstNode->PanelIds.PushBack(String(u8"only"));
    layout.First = Move(firstNode);

    auto secondNode = MakeUnique<DockLayoutNode>(DefaultAllocator());
    secondNode->Type = DockLayoutNodeType::TabGroup;
    secondNode->PanelIds.PushBack(String(u8"gone"));
    layout.Second = Move(secondNode);

    dm->ApplyLayout(&layout);

    // Split should collapse since second is empty.
    CHECK(Cast<DockTabGroup>(dm->RootNode()) != nullptr);
}

// === Roundtrip (Export -> Apply) ===

TEST_CASE("dock-persistence: Roundtrip_SinglePanel")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* panel = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"Content").Get());
    panel->SetPersistenceId(StringView(u8"assets"));
    dm->DockPanel(panel, DockPosition::Center);

    auto layout = dm->ExportLayout();

    dm->ApplyLayout(layout.Get());

    CHECK(panel->Parent != nullptr);
    CHECK(dm->RootNode() != nullptr);
}

TEST_CASE("dock-persistence: Roundtrip_ComplexLayout")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Editor"), MakeLabel(u8"E").Get());
    p1->SetPersistenceId(StringView(u8"editor"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"A").Get());
    p2->SetPersistenceId(StringView(u8"assets"));
    DockablePanel* p3 = dm->AddPanel(StringView(u8"Console"), MakeLabel(u8"C").Get());
    p3->SetPersistenceId(StringView(u8"console"));
    DockablePanel* p4 = dm->AddPanel(StringView(u8"Inspector"), MakeLabel(u8"I").Get());
    p4->SetPersistenceId(StringView(u8"inspector"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Bottom);
    dm->DockPanelRelativeTo(p3, DockPosition::Center, p2->Parent); // Tab with Assets
    dm->DockPanel(p4, DockPosition::Right);

    // Export.
    auto layout = dm->ExportLayout();

    const int originalPanelCount = CountPanels(layout.Get());

    // Apply (rebuilds tree).
    dm->ApplyLayout(layout.Get());

    // Export again and verify structure matches.
    auto layout2 = dm->ExportLayout();

    CHECK(layout2.Get() != nullptr);
    CHECK(CountPanels(layout2.Get()) == originalPanelCount);
}

TEST_CASE("dock-persistence: Roundtrip_PreservesSplitRatio")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Left"), MakeLabel(u8"L").Get());
    p1->SetPersistenceId(StringView(u8"left"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Right"), MakeLabel(u8"R").Get());
    p2->SetPersistenceId(StringView(u8"right"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanel(p2, DockPosition::Right);

    if (auto* split = Cast<DockSplit>(dm->RootNode()))
    {
        split->SetSplitRatio(0.35f);
    }

    auto layout = dm->ExportLayout();

    dm->ApplyLayout(layout.Get());

    auto layout2 = dm->ExportLayout();

    CHECK(layout2->Type == DockLayoutNodeType::Split);
    CHECK(Abs(layout2->SplitRatio - 0.35f) < 0.01f);
}

TEST_CASE("dock-persistence: Roundtrip_PreservesActiveTab")
{
    UIContext ctx;
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    auto dm = MakeRef<DockManager>(DefaultAllocator());
    root->AddView(dm.Get());

    DockablePanel* p1 = dm->AddPanel(StringView(u8"Assets"), MakeLabel(u8"A").Get());
    p1->SetPersistenceId(StringView(u8"assets"));
    DockablePanel* p2 = dm->AddPanel(StringView(u8"Console"), MakeLabel(u8"C").Get());
    p2->SetPersistenceId(StringView(u8"console"));

    dm->DockPanel(p1, DockPosition::Center);
    dm->DockPanelRelativeTo(p2, DockPosition::Center, p1->Parent);

    // Select second tab.
    if (auto* group = Cast<DockTabGroup>(dm->RootNode()))
    {
        group->SetSelectedIndex(1);
    }

    auto layout = dm->ExportLayout();

    dm->ApplyLayout(layout.Get());

    if (auto* group = Cast<DockTabGroup>(dm->RootNode()))
    {
        CHECK(group->SelectedIndex() == 1);
    }
}

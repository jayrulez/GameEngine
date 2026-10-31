// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - the DockSpace and its controller. Splits are shown as Splitters and stacks as
// TabViews, kept by node id as the layout changes. A panel's content is made when its tab is first
// shown and is kept by the controller. Closing follows the panel's policy. A panel that is not
// registered shows a placeholder, filled once it is registered. A maximized stack is shown alone.
// What the user does in the widgets is noted back: dragging a separator, and choosing, dragging or
// closing a tab (by its close button or a middle click). A panel wider than its stack moves no
// separator, because the stack clips it. Also covered: a panel that asks before it closes, a panel
// unregistered, a title and tab classes changed while docked, and the active panel following the
// focus.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;
import foundation.gui.docking;

using namespace foundation::gui;
namespace core = foundation::core;
using core::f32;
using core::usize;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        DockController controller{core::DefaultAllocator()};
        core::RefPtr<DockSpace> space = core::MakeRef<DockSpace>(core::DefaultAllocator());
        int made = 0; // contents the factories made

        Bed()
        {
            root->SetSize(core::Float2{800.0f, 600.0f});
            space->EditLayoutStyle([](LayoutStyle& s) {
                s.Width = LayoutLength::Px(800.0f);
                s.Height = LayoutLength::Px(600.0f);
            });
            root->AddChild(space.Get());
            const char8_t* const ids[] = {u8"scene", u8"hierarchy", u8"inspector", u8"console", u8"log"};
            const char8_t* const titles[] = {u8"Scene", u8"Hierarchy", u8"Inspector", u8"Console", u8"Log"};
            for (usize i = 0; i < 5; ++i)
                Register(ids[i], titles[i], DockClosePolicy::Hide);
            space->Show(controller, controller.GetLayout().MainSurface());
        }
        void Register(const char8_t* id, const char8_t* title, DockClosePolicy policy)
        {
            DockPanelDescription description;
            description.Id = core::String(id);
            description.Title = core::String(title);
            description.Close = policy;
            Bed* self = this;
            description.Factory = [self]()
            {
                ++self->made;
                return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator()));
            };
            REQUIRE(controller.Register(core::Move(description)));
        }
        DockNodeId Central() const { return controller.GetLayout().CentralStack(); }
        DockNodeId StackOf(const char8_t* panel) const { return controller.GetLayout().StackOf(SV(panel)); }
        void Lay() { root->UpdateLayout(); }
    };
}

TEST_CASE("dock space: splits are Splitters and stacks TabViews, kept by node id as the layout changes")
{
    Bed bed;
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"scene"), bed.Central()); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"hierarchy"), bed.Central(), DockSide::Start, 0.25f); }).Ok());
    bed.Lay();
    REQUIRE(bed.space->ChildCount() == 1u);
    auto* splitter = Cast<Splitter>(bed.space->GetChildAt(0));
    REQUIRE(splitter != nullptr);
    REQUIRE(splitter->GetPaneCount() == 2u);
    auto* central = Cast<TabView>(bed.space->WidgetOf(bed.Central()));
    REQUIRE(central != nullptr);
    CHECK(splitter->GetPane(1) == central);
    CHECK(central->HasClass(SV(u8"dock-central")));
    CHECK(central->GetGroup() == core::String(u8"dock"));
    CHECK(splitter->GetWeights()[0] == doctest::Approx(0.25f));
    CHECK(bed.space->PageOf(SV(u8"hierarchy"))->GetTitle() == SV(u8"Hierarchy"));
    // One more tab keeps the same widgets and adds a page.
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"inspector"), bed.Central()); }).Ok());
    CHECK(bed.space->GetChildAt(0) == splitter);
    CHECK(bed.space->WidgetOf(bed.Central()) == central);
    CHECK(central->GetPageCount() == 2);
    CHECK(central->GetSelectedIndex() == 1); // the one tabbed in last
}

TEST_CASE("dock space: a panel's content is made when its tab is first shown, and kept by the controller")
{
    Bed bed;
    // With both tabbed in at once, the console, tabbed in last, is the one shown.
    REQUIRE(bed.controller.Edit([&](DockLayout& l)
    {
        const DockOutcome first = l.InsertTab(SV(u8"scene"), bed.Central());
        return first.Ok() ? l.InsertTab(SV(u8"console"), bed.Central()) : first;
    }).Ok());
    bed.Lay();
    CHECK(bed.controller.HasContent(SV(u8"console"))); // selected, so shown
    CHECK_FALSE(bed.controller.HasContent(SV(u8"scene"))); // not shown yet
    CHECK(bed.made == 1);
    auto* central = Cast<TabView>(bed.space->WidgetOf(bed.Central()));
    central->GetBar()->SelectByUser(0); // the user chose the scene's tab
    bed.Lay();
    CHECK(bed.controller.HasContent(SV(u8"scene")));
    CHECK(bed.controller.GetLayout().FindNode(bed.Central())->Active == 0u); // noted
    // Moved to a stack of its own, it keeps the same content.
    const core::RefPtr<Node> scene = bed.controller.Content(SV(u8"scene"));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.MoveBeside(SV(u8"scene"), bed.Central(), DockSide::End); }).Ok());
    bed.Lay();
    CHECK(bed.controller.Content(SV(u8"scene")) == scene);
    CHECK(scene->GetParent() == bed.space->PageOf(SV(u8"scene")));
    CHECK(bed.made == 2);
}

TEST_CASE("dock space: closing by the panel's policy: hide keeps its content, rebuild makes it again, destroy forgets it")
{
    Bed bed;
    bed.Register(u8"preview", u8"Preview", DockClosePolicy::Rebuild);
    bed.Register(u8"search", u8"Search", DockClosePolicy::Destroy);
    const char8_t* const placed[] = {u8"scene", u8"preview", u8"search"};
    for (const char8_t* panel : placed)
    {
        REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(panel), bed.Central(), DockSide::End); }).Ok());
        bed.Lay();
    }
    const core::RefPtr<Node> scene = bed.controller.Content(SV(u8"scene"));
    const core::RefPtr<Node> preview = bed.controller.Content(SV(u8"preview"));
    // Close through the tab's close button, as the user would.
    auto* sceneStack = Cast<TabView>(bed.space->WidgetOf(bed.StackOf(u8"scene")));
    sceneStack->GetBar()->RequestClose(0);
    CHECK(bed.StackOf(u8"scene") == kNoDock);
    CHECK(bed.controller.HasContent(SV(u8"scene"))); // hidden, kept
    REQUIRE(bed.controller.Show(SV(u8"scene")).Ok());
    bed.Lay();
    CHECK(bed.controller.Content(SV(u8"scene")) == scene);

    REQUIRE(bed.controller.Close(SV(u8"preview")).Ok());
    CHECK_FALSE(bed.controller.HasContent(SV(u8"preview"))); // released
    CHECK(bed.controller.GetLayout().FindHidden(SV(u8"preview")) != nullptr); // its place kept
    REQUIRE(bed.controller.Show(SV(u8"preview")).Ok());
    bed.Lay();
    CHECK(bed.controller.Content(SV(u8"preview")) != preview); // made again

    REQUIRE(bed.controller.Close(SV(u8"search")).Ok());
    CHECK_FALSE(bed.controller.HasContent(SV(u8"search")));
    CHECK(bed.controller.GetLayout().FindHidden(SV(u8"search")) == nullptr); // forgotten
    // Shown again, it goes to its default place (the main surface's end).
    REQUIRE(bed.controller.Show(SV(u8"search")).Ok());
    CHECK(bed.StackOf(u8"search") != kNoDock);

    DockPanelDescription fixed;
    fixed.Id = core::String(u8"toolbar");
    fixed.Closable = false;
    REQUIRE(bed.controller.Register(core::Move(fixed)));
    REQUIRE(bed.controller.Show(SV(u8"toolbar")).Ok());
    CHECK_FALSE(bed.controller.Close(SV(u8"toolbar")).Ok());
}

TEST_CASE("dock space: a panel not registered shows a placeholder, filled when it registers")
{
    Bed bed;
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"profiler"), bed.Central()); }).Ok());
    bed.Lay();
    TabPage* page = bed.space->PageOf(SV(u8"profiler"));
    REQUIRE(page != nullptr);
    REQUIRE(page->ChildCount() == 1u);
    CHECK(Cast<Label>(page->GetChildAt(0)) != nullptr);
    CHECK(Cast<Label>(page->GetChildAt(0))->HasClass(SV(u8"dock-placeholder")));
    bed.Register(u8"profiler", u8"Profiler", DockClosePolicy::Hide);
    bed.Lay();
    page = bed.space->PageOf(SV(u8"profiler"));
    REQUIRE(page != nullptr);
    CHECK(page->GetTitle() == SV(u8"Profiler"));
    REQUIRE(page->ChildCount() == 1u);
    CHECK(Cast<Label>(page->GetChildAt(0)) == nullptr);
    CHECK(bed.controller.HasContent(SV(u8"profiler")));
}

TEST_CASE("dock space: a separator dragged and a tab dragged are noted; a maximized stack is shown alone, the rest kept")
{
    Bed bed;
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"hierarchy"), bed.Central(), DockSide::Start, 0.5f); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"log"), bed.Central()); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"console"), bed.Central()); }).Ok());
    bed.Lay();
    auto* splitter = Cast<Splitter>(bed.space->GetChildAt(0));
    REQUIRE(splitter != nullptr);
    splitter->MoveHandleTo(0, 200.0f);
    bed.Lay();
    const DockNode* root = bed.controller.GetLayout().FindNode(bed.controller.GetLayout().FindSurface(bed.controller.GetLayout().MainSurface())->Root);
    CHECK(root->Weights[0] == doctest::Approx(200.0f / 794.0f).epsilon(0.01)); // 800 less the handle
    CHECK(bed.space->GetChildAt(0) == splitter); // kept, not made again

    auto* central = Cast<TabView>(bed.space->WidgetOf(bed.Central()));
    central->GetBar()->MoveTab(1, 0); // console before log
    CHECK(bed.controller.GetLayout().FindNode(bed.Central())->Panels[0] == SV(u8"console"));
    CHECK(bed.space->WidgetOf(bed.Central()) == central);

    const core::RefPtr<Node> held(splitter); // held, so its address is not reused if it is released
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.Maximize(bed.Central()); }).Ok());
    CHECK(bed.space->GetChildAt(0) == central); // alone
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.Restore(l.MainSurface()); }).Ok());
    CHECK(bed.space->GetChildAt(0) == splitter); // kept while it was out of sight
    CHECK(splitter->GetPane(1) == central);
    CHECK(splitter->GetWeights()[0] == doctest::Approx(200.0f / 794.0f).epsilon(0.01));
}

TEST_CASE("dock space: a tab shown with content wider than its stack moves no separator; the stack clips it")
{
    Bed bed;
    DockPanelDescription description;
    description.Id = core::String(u8"wide");
    description.Factory = []()
    {
        core::RefPtr<Panel> wide = core::MakeRef<Panel>(core::DefaultAllocator());
        wide->EditLayoutStyle([](LayoutStyle& s) { s.MinWidth = LayoutLength::Px(700.0f); });
        return core::RefPtr<Node>(wide);
    };
    REQUIRE(bed.controller.Register(core::Move(description)));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"hierarchy"), bed.Central(), DockSide::Start, 0.25f); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"wide"), l.StackOf(SV(u8"hierarchy"))); }).Ok());
    auto* stack = Cast<TabView>(bed.space->WidgetOf(bed.StackOf(u8"hierarchy")));
    REQUIRE(stack != nullptr);
    stack->SetSelectedIndex(0);
    bed.Lay();
    auto* splitter = Cast<Splitter>(bed.space->GetChildAt(0));
    REQUIRE(splitter != nullptr);
    const f32 before = splitter->GetPaneSizes()[0];
    stack->SetSelectedIndex(1); // the wide one
    bed.Lay();
    CHECK(splitter->GetPaneSizes()[0] == doctest::Approx(before));
    CHECK(stack->GetSize().x == doctest::Approx(before).epsilon(0.01)); // snapped to the pixel
    CHECK(stack->GetLayoutStyle().OverflowX == Overflow::Hidden);
    CHECK(stack->GetLayoutStyle().OverflowY == Overflow::Hidden);
}

TEST_CASE("dock space: a closable panel's tab shows its close part, chosen or hovered; it, or a middle click, closes the panel")
{
    Bed bed;
    DockPanelDescription fixed;
    fixed.Id = core::String(u8"fixed");
    fixed.Closable = false;
    fixed.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator())); };
    REQUIRE(bed.controller.Register(core::Move(fixed)));
    const char8_t* const tabbed[] = {u8"fixed", u8"hierarchy", u8"inspector"};
    for (const char8_t* id : tabbed)
        REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(id), bed.Central()); }).Ok());
    bed.Lay();
    TabBar& bar = *Cast<TabView>(bed.space->WidgetOf(bed.Central()))->GetBar();
    REQUIRE(bar.GetTabCount() == 3);
    bar.SelectByUser(2); // the inspector
    bed.Lay();
    CHECK(bar.GetCloseDisplay() == TabCloseDisplay::Hover);
    CHECK_FALSE(bar.GetTab(0)->GetClosePart()->IsDisplayed()); // it cannot be closed
    CHECK(bar.GetTab(1)->GetClosePart()->IsDisplayed());       // its room is kept, and it shows when hovered
    CHECK_FALSE(bar.GetTab(1)->GetClosePart()->IsVisible());
    CHECK(bar.GetTab(2)->GetClosePart()->IsVisible()); // the chosen tab's

    EventDispatcher& d = *bed.root->GetEventDispatcher();
    const Rect close = bar.GetTab(2)->GetClosePart()->GetScreenBounds();
    const core::Float2 at{close.x + close.width * 0.5f, close.y + close.height * 0.5f};
    d.InjectMouseMove(at);
    d.InjectMouseDown(at, MouseButton::Left);
    d.InjectMouseUp(at, MouseButton::Left);
    CHECK(bed.StackOf(u8"inspector") == kNoDock);
    bed.Lay();

    REQUIRE(bar.GetTabCount() == 2);
    const auto middleClick = [&](core::i32 index)
    {
        const Rect tab = bar.GetTab(index)->GetScreenBounds();
        const core::Float2 on{tab.x + tab.width * 0.5f, tab.y + tab.height * 0.5f};
        d.InjectMouseMove(on);
        d.InjectMouseDown(on, MouseButton::Middle);
        d.InjectMouseUp(on, MouseButton::Middle);
        bed.Lay();
    };
    middleClick(0); // the fixed panel: nothing happens
    CHECK(bed.StackOf(u8"fixed") != kNoDock);
    middleClick(1);
    CHECK(bed.StackOf(u8"hierarchy") == kNoDock);
    CHECK(bed.StackOf(u8"fixed") != kNoDock);
}

TEST_CASE("dock space: content taller than the space is clipped; the split keeps the space's size and every strip its height")
{
    Bed bed;
    // A panel far taller than the space (a long tree, a page with a large picture).
    DockPanelDescription tall;
    tall.Id = core::String(u8"tall");
    tall.Title = core::String(u8"Tall");
    tall.Factory = []()
    {
        core::RefPtr<Panel> content = core::MakeRef<Panel>(core::DefaultAllocator());
        content->EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(3000.0f); });
        return core::RefPtr<Node>(content);
    };
    REQUIRE(bed.controller.Register(core::Move(tall)));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"tall"), bed.Central()); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"console"), bed.Central(), DockSide::Bottom, 0.3f); }).Ok());
    bed.Lay();
    REQUIRE(bed.space->ChildCount() >= 1u);
    auto* splitter = Cast<Splitter>(bed.space->GetChildAt(0));
    REQUIRE(splitter != nullptr);
    CHECK(splitter->GetSize().y == doctest::Approx(600.0f)); // the space's height, not 3000 or more
    auto* central = Cast<TabView>(bed.space->WidgetOf(bed.Central()));
    auto* bottom = Cast<TabView>(bed.space->WidgetOf(bed.StackOf(u8"console")));
    REQUIRE(central != nullptr);
    REQUIRE(bottom != nullptr);
    CHECK(bottom->GetPosition().y + bottom->GetSize().y <= doctest::Approx(600.0f)); // in the space
    // The tall page's strip keeps the height of the short one's: the page shrinks, not the strip.
    CHECK(bottom->GetBar()->GetSize().y > 0.0f);
    CHECK(central->GetBar()->GetSize().y == doctest::Approx(bottom->GetBar()->GetSize().y));
}

TEST_CASE("dock space: a panel asks before it closes, by its tab or the controller; CloseNow closes it without asking")
{
    Bed bed;
    int asked = 0;
    bool agrees = false;
    DockPanelDescription document;
    document.Id = core::String(u8"document");
    document.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator())); };
    document.CanClose = [&]()
    {
        ++asked;
        return agrees;
    };
    REQUIRE(bed.controller.Register(core::Move(document)));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"document"), bed.Central()); }).Ok());
    bed.Lay();

    CHECK_FALSE(bed.controller.Close(SV(u8"document")).Ok());
    CHECK(asked == 1);
    CHECK(bed.StackOf(u8"document") != kNoDock);

    // The close button on its tab asks the same way.
    TabBar& bar = *Cast<TabView>(bed.space->WidgetOf(bed.Central()))->GetBar();
    REQUIRE(bar.GetTabCount() == 1);
    const Rect close = bar.GetTab(0)->GetClosePart()->GetScreenBounds();
    const core::Float2 at{close.x + close.width * 0.5f, close.y + close.height * 0.5f};
    EventDispatcher& d = *bed.root->GetEventDispatcher();
    d.InjectMouseMove(at);
    d.InjectMouseDown(at, MouseButton::Left);
    d.InjectMouseUp(at, MouseButton::Left);
    CHECK(asked == 2);
    CHECK(bed.StackOf(u8"document") != kNoDock);

    CHECK(bed.controller.CloseNow(SV(u8"document")).Ok()); // the user agreed elsewhere
    CHECK(asked == 2);
    CHECK(bed.StackOf(u8"document") == kNoDock);

    REQUIRE(bed.controller.Show(SV(u8"document")).Ok());
    agrees = true;
    CHECK(bed.controller.Close(SV(u8"document")).Ok());
    CHECK(asked == 3);
    CHECK(bed.StackOf(u8"document") == kNoDock);
}

TEST_CASE("dock space: a panel unregistered leaves the layout without asking, its content, place and description gone")
{
    Bed bed;
    int asked = 0;
    DockPanelDescription document;
    document.Id = core::String(u8"document");
    document.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator())); };
    document.CanClose = [&]()
    {
        ++asked;
        return false;
    };
    const usize before = bed.controller.Panels().Size();
    REQUIRE(bed.controller.Register(core::Move(document)));
    REQUIRE(bed.controller.Panels().Size() == before + 1u);
    CHECK(bed.controller.Panels()[before].Id == core::String(u8"document")); // in the order they came
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"document"), bed.Central()); }).Ok());
    bed.Lay();
    core::RefPtr<Node> content = bed.controller.Content(SV(u8"document"));
    REQUIRE(content.Get() != nullptr);
    REQUIRE(content->GetParent() != nullptr);

    CHECK(bed.controller.Unregister(SV(u8"document")));
    CHECK(asked == 0);
    CHECK(bed.StackOf(u8"document") == kNoDock);
    CHECK(bed.controller.GetLayout().FindHidden(SV(u8"document")) == nullptr); // no place kept
    CHECK(bed.controller.Describe(SV(u8"document")) == nullptr);
    CHECK(bed.controller.Panels().Size() == before);
    CHECK_FALSE(bed.controller.HasContent(SV(u8"document")));
    CHECK(content->GetParent() == nullptr);
    CHECK_FALSE(bed.controller.Show(SV(u8"document")).Ok());
    CHECK_FALSE(bed.controller.Unregister(SV(u8"document")));
    bed.Lay();
    CHECK(bed.space->PageOf(SV(u8"document")) == nullptr);
}

TEST_CASE("dock space: a docked panel's title and tab classes change where it shows; a panel not registered has none to change")
{
    Bed bed;
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"scene"), bed.Central()); }).Ok());
    bed.Lay();
    TabPage* page = bed.space->PageOf(SV(u8"scene"));
    REQUIRE(page != nullptr);
    REQUIRE(page->GetTab() != nullptr);
    CHECK(page->GetTab()->GetText() == SV(u8"Scene"));

    CHECK(bed.controller.SetTitle(SV(u8"scene"), SV(u8"Scene*")));
    CHECK(bed.controller.SetTabClasses(SV(u8"scene"), SV(u8"dirty active")));
    CHECK(bed.space->PageOf(SV(u8"scene")) == page); // the same page, updated
    CHECK(page->GetTab()->GetText() == SV(u8"Scene*"));
    CHECK(page->GetTab()->HasClass(SV(u8"dirty")));
    CHECK(page->GetTab()->HasClass(SV(u8"active")));
    CHECK(bed.controller.SetTabClasses(SV(u8"scene"), SV(u8"active")));
    CHECK_FALSE(page->GetTab()->HasClass(SV(u8"dirty")));
    CHECK(bed.controller.Describe(SV(u8"scene"))->Title == core::String(u8"Scene*"));

    CHECK_FALSE(bed.controller.SetTitle(SV(u8"nowhere"), SV(u8"Nowhere")));
    CHECK_FALSE(bed.controller.SetTabClasses(SV(u8"nowhere"), SV(u8"dirty")));

    // Shown again elsewhere, it keeps its title and classes.
    REQUIRE(bed.controller.Close(SV(u8"scene")).Ok());
    REQUIRE(bed.controller.Show(SV(u8"scene")).Ok());
    bed.Lay();
    TabPage* again = bed.space->PageOf(SV(u8"scene"));
    REQUIRE(again != nullptr);
    REQUIRE(again->GetTab() != nullptr);
    CHECK(again->GetTab()->GetText() == SV(u8"Scene*"));
    CHECK(again->GetTab()->HasClass(SV(u8"active")));
}

TEST_CASE("dock space: the active panel is the chosen one of the stack the focus went into; it follows a tab chosen there and a close")
{
    Bed bed;
    // Contents that take the focus.
    const char8_t* const ids[] = {u8"first", u8"second", u8"side"};
    for (const char8_t* id : ids)
    {
        DockPanelDescription description;
        description.Id = core::String(id);
        description.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Button>(core::DefaultAllocator())); };
        REQUIRE(bed.controller.Register(core::Move(description)));
    }
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"first"), bed.Central()); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"second"), bed.Central()); }).Ok());
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"side"), bed.Central(), DockSide::Start, 0.25f); }).Ok());
    bed.Lay();
    core::Array<core::String> heard;
    bed.controller.SetOnActiveChanged([&](core::StringView panel) { heard.PushBack(core::String(panel)); });
    CHECK(bed.controller.ActivePanel().IsEmpty()); // the focus was in no stack yet

    TabView& central = *Cast<TabView>(bed.space->WidgetOf(bed.Central()));
    const auto chosen = [&](TabView& view) { return Cast<TabPage>(view.GetPage(view.GetSelectedIndex())); };
    const auto focusIn = [&](const char8_t* panel)
    {
        TabPage* page = bed.space->PageOf(SV(panel));
        REQUIRE(page != nullptr);
        REQUIRE(page->ChildCount() == 1);
        page->GetChildAt(0)->RequestFocus();
    };
    REQUIRE(chosen(central) == bed.space->PageOf(SV(u8"second")));
    focusIn(u8"second");
    CHECK(bed.controller.ActivePanel() == SV(u8"second"));
    REQUIRE(heard.Size() == 1u);

    central.GetBar()->SelectByUser(0); // a tab chosen in the active stack
    bed.Lay();
    CHECK(bed.controller.ActivePanel() == SV(u8"first"));

    focusIn(u8"side"); // into another stack
    CHECK(bed.controller.ActivePanel() == SV(u8"side"));
    central.GetBar()->SelectByUser(1); // a tab chosen in a stack the focus is not in: no change
    bed.Lay();
    CHECK(bed.controller.ActivePanel() == SV(u8"side"));
    REQUIRE(heard.Size() == 3u);

    REQUIRE(bed.controller.Close(SV(u8"side")).Ok()); // its stack went with it, so no panel is active
    CHECK(bed.controller.ActivePanel().IsEmpty());
    REQUIRE(heard.Size() == 4u);
    CHECK(heard[3].IsEmpty());

    bed.Lay();
    focusIn(u8"second");
    CHECK(bed.controller.ActivePanel() == SV(u8"second"));
    REQUIRE(bed.controller.Close(SV(u8"second")).Ok()); // its stack's next chosen panel becomes active
    CHECK(bed.controller.ActivePanel() == SV(u8"first"));
    CHECK(heard.Back() == core::String(u8"first"));
}

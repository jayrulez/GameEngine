// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - tabs (E6): a TabBar of real tab elements and a TabView pairing it with pages. Selection by
// the press, the keys and Ctrl+Tab; closing as a request with a display policy; reordering by a
// drag across a neighbour's midpoint; an overflowing strip that scrolls and lists every tab; pages
// kept alive, or built on first show; markup's pages.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    core::RefPtr<SceneRoot> MakeRoot(float width = 400.0f)
    {
        auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{width, 300.0f});
        return root;
    }

    // Tabs of a known width (the tests have no font to size their text).
    Tab* AddSizedTab(TabBar& bar, const char8_t* text, float width = 80.0f)
    {
        Tab* tab = bar.AddTab(core::StringView(text));
        tab->EditLayoutStyle([width](LayoutStyle& s)
        {
            s.Width = LayoutLength::Px(width);
            s.Height = LayoutLength::Px(24.0f);
        });
        return tab;
    }

    core::Float2 CentreOf(const Node& node)
    {
        const Rect r = node.GetScreenBounds();
        return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    }

    void Press(EventDispatcher& d, core::Float2 at, MouseButton button = MouseButton::Left)
    {
        d.InjectMouseDown(at, button);
        d.InjectMouseUp(at, button);
    }
}

TEST_CASE("tabs: the first tab is selected; a press selects, reporting change, while code's selection reports nothing")
{
    auto root = MakeRoot();
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    Tab* a = AddSizedTab(*bar, u8"One");
    Tab* b = AddSizedTab(*bar, u8"Two");
    AddSizedTab(*bar, u8"Three");
    (void)root->UpdateLayout();
    CHECK(bar->GetTabCount() == 3);
    CHECK(bar->GetSelectedIndex() == 0);
    CHECK(a->IsSelected());
    CHECK((a->GetWidgetStates() & WidgetStateChecked) != 0); // `tab:checked`

    int changes = 0;
    int selectedByUser = -1;
    int anyChange = 0;
    (void)bar->AddEventListener(EventType::Changed, [&](const Event&) { ++changes; });
    bar->SetOnSelected([&](core::i32 index) { selectedByUser = index; });
    bar->SetOnSelectionChanged([&](core::i32) { ++anyChange; });

    EventDispatcher& d = *root->GetEventDispatcher();
    d.InjectMouseDown(CentreOf(*b), MouseButton::Left); // a tab selects on the press
    CHECK(bar->GetSelectedIndex() == 1);
    d.InjectMouseUp(CentreOf(*b), MouseButton::Left);
    CHECK(changes == 1);
    CHECK(selectedByUser == 1);
    CHECK(b->IsFocused());
    CHECK_FALSE(a->IsSelected());

    bar->SetSelectedIndex(2);
    CHECK(bar->GetSelectedIndex() == 2);
    CHECK(changes == 1); // code's selection: no change event
    CHECK(anyChange == 2); // but what shows a page follows both
}

TEST_CASE("tabs: the arrows, Home and End move along the strip; Ctrl+Tab and Ctrl+Shift+Tab cycle round it")
{
    auto root = MakeRoot();
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    const char8_t* const names[] = {u8"A", u8"B", u8"C", u8"D"};
    for (const char8_t* t : names)
        AddSizedTab(*bar, t, 60.0f);
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();
    bar->GetTab(0)->RequestFocus();

    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Right));
    CHECK(bar->GetSelectedIndex() == 1);
    CHECK(bar->GetTab(1)->IsFocused());
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::End));
    CHECK(bar->GetSelectedIndex() == 3);
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Home));
    CHECK(bar->GetSelectedIndex() == 0);
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Left)); // at the start the arrow is navigation's
    CHECK(bar->GetSelectedIndex() == 0);

    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Tab), KeyModCtrl | KeyModShift); // back, round to the last
    CHECK(bar->GetSelectedIndex() == 3);
    CHECK(bar->GetTab(3)->IsFocused());
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Tab), KeyModCtrl); // on, round to the first
    CHECK(bar->GetSelectedIndex() == 0);
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::PageDown), KeyModCtrl);
    CHECK(bar->GetSelectedIndex() == 1);
}

TEST_CASE("tabs: the close display policy shows the close part on never, the selected, the hovered and every tab")
{
    auto root = MakeRoot();
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    Tab* a = AddSizedTab(*bar, u8"A");
    Tab* b = AddSizedTab(*bar, u8"B");
    (void)root->UpdateLayout();

    CHECK_FALSE(a->GetClosePart()->IsDisplayed()); // never, by default: no room for it either
    bar->SetCloseDisplay(TabCloseDisplay::Selected);
    CHECK(a->GetClosePart()->IsDisplayed());
    CHECK(a->GetClosePart()->IsVisible());
    CHECK(b->GetClosePart()->IsDisplayed()); // its room is kept, so showing it grows nothing
    CHECK_FALSE(b->GetClosePart()->IsVisible());

    bar->SetCloseDisplay(TabCloseDisplay::Hover);
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();
    CHECK_FALSE(b->GetClosePart()->IsVisible());
    d.InjectMouseMove(CentreOf(*b));
    CHECK(b->GetClosePart()->IsVisible());
    d.InjectMouseMove(core::Float2{390.0f, 290.0f});
    CHECK_FALSE(b->GetClosePart()->IsVisible());

    bar->SetCloseDisplay(TabCloseDisplay::Always);
    CHECK(b->GetClosePart()->IsVisible());
    b->SetClosable(false);
    CHECK_FALSE(b->GetClosePart()->IsDisplayed());

    bar->SetCloseDisplayName(core::String(u8"hover"));
    CHECK(bar->GetCloseDisplay() == TabCloseDisplay::Hover);
    CHECK(bar->GetCloseDisplayName() == core::StringView(u8"hover"));
}

TEST_CASE("tabs: closing is a request: tabclose is reported, and the tab goes unless a listener takes it")
{
    auto root = MakeRoot();
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    bar->SetCloseDisplay(TabCloseDisplay::Always);
    AddSizedTab(*bar, u8"A");
    Tab* b = AddSizedTab(*bar, u8"B");
    AddSizedTab(*bar, u8"C");
    bar->SetSelectedIndex(1);
    (void)root->UpdateLayout();

    bool keep = true;
    int requests = 0;
    core::i32 asked = -1;
    (void)bar->AddEventListener(EventType::TabClose, [&](const Event& e)
    {
        ++requests;
        asked = static_cast<const TabEvent&>(e).Index;
        if (keep)
            e.Handled = true; // taken: the tab stays (an unsaved document asks first)
    });
    core::i32 removed = 0;
    bar->SetOnTabRemoved([&](Tab&) { ++removed; });

    EventDispatcher& d = *root->GetEventDispatcher();
    Press(d, CentreOf(*b->GetClosePart()));
    CHECK(requests == 1);
    CHECK(asked == 1);
    CHECK(bar->GetTabCount() == 3);
    CHECK(bar->GetSelectedIndex() == 1); // the close part's press does not select either

    keep = false;
    Press(d, CentreOf(*b->GetClosePart()));
    CHECK(requests == 2);
    CHECK(bar->GetTabCount() == 2);
    CHECK(removed == 1);
    // The selected tab went: the one after it takes its place.
    CHECK(bar->GetSelectedIndex() == 1);
    CHECK(bar->GetSelectedTab()->GetText() == core::StringView(u8"C"));

    // A middle click closes too; a tab not closable ignores both.
    Tab* a = bar->GetTab(0);
    a->SetClosable(false);
    (void)root->UpdateLayout();
    Press(d, CentreOf(*a), MouseButton::Middle);
    CHECK(bar->GetTabCount() == 2);
    (void)root->UpdateLayout();
    Press(d, CentreOf(*bar->GetTab(1)), MouseButton::Middle);
    CHECK(bar->GetTabCount() == 1);
    CHECK(bar->GetSelectedIndex() == 0);
}

TEST_CASE("tabs: reorderable, a tab dragged across a neighbour's midpoint changes places with it")
{
    auto root = MakeRoot();
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    Tab* a = AddSizedTab(*bar, u8"A");
    AddSizedTab(*bar, u8"B");
    AddSizedTab(*bar, u8"C");
    core::i32 movedFrom = -1, movedTo = -1;
    bar->SetOnTabMoved([&](core::i32 from, core::i32 to)
    {
        movedFrom = from;
        movedTo = to;
    });
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();
    const core::Float2 start = CentreOf(*a);

    // Not reorderable: a drag moves nothing.
    d.InjectMouseDown(start, MouseButton::Left);
    d.InjectMouseMove(core::Float2{start.x + 100.0f, start.y});
    d.InjectMouseUp(core::Float2{start.x + 100.0f, start.y}, MouseButton::Left);
    CHECK(bar->IndexOf(a) == 0);

    bar->SetReorderable(true);
    d.InjectMouseDown(start, MouseButton::Left);
    d.InjectMouseMove(core::Float2{start.x + 30.0f, start.y}); // short of B's midpoint (82 + 40 from A's left)
    CHECK(bar->IndexOf(a) == 0);
    d.InjectMouseMove(core::Float2{start.x + 90.0f, start.y}); // past it
    CHECK(bar->IndexOf(a) == 1);
    CHECK(movedFrom == 0);
    CHECK(movedTo == 1);
    CHECK(a->HasClass(core::StringView(u8"dragging")));
    (void)root->UpdateLayout();
    d.InjectMouseMove(core::Float2{start.x + 180.0f, start.y}); // past C's
    CHECK(bar->IndexOf(a) == 2);
    (void)root->UpdateLayout();
    d.InjectMouseMove(core::Float2{start.x + 60.0f, start.y}); // back past C's again (now before it)
    CHECK(bar->IndexOf(a) == 1);
    d.InjectMouseUp(core::Float2{start.x + 60.0f, start.y}, MouseButton::Left);
    CHECK_FALSE(a->HasClass(core::StringView(u8"dragging")));
    CHECK(a->IsSelected()); // it stays the selected tab wherever it goes
}

TEST_CASE("tabs: tabs past the strip's end scroll it, a selected one is scrolled to, and a button lists them all")
{
    auto root = MakeRoot(300.0f);
    auto bar = Make<TabBar>();
    root->AddChild(bar.Get());
    AddSizedTab(*bar, u8"A");
    AddSizedTab(*bar, u8"B");
    (void)root->UpdateLayout();
    CHECK_FALSE(bar->GetListButton()->IsDisplayed()); // they fit

    const char8_t* const more[] = {u8"C", u8"D", u8"E", u8"F"};
    for (const char8_t* t : more)
        AddSizedTab(*bar, t);
    (void)root->UpdateLayout();
    CHECK(bar->GetListButton()->IsDisplayed());
    CHECK(bar->GetStrip()->GetScrollRange().x > 0.0f);

    bar->SetSelectedIndex(5);
    (void)root->UpdateLayout();
    const Rect strip = bar->GetStrip()->GetScreenBounds();
    const Rect last = bar->GetTab(5)->GetScreenBounds();
    CHECK(last.x + last.width <= strip.x + strip.width + 0.5f);

    // The wheel turns the strip across (it scrolls on one axis only).
    bar->SetSelectedIndex(0);
    bar->GetStrip()->SetScrollOffset(core::Float2{0.0f, 0.0f});
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();
    d.InjectMouseWheel(CentreOf(*bar->GetTab(0)), core::Float2{0.0f, -1.0f});
    CHECK(bar->GetStrip()->GetScrollOffset().x > 0.0f);

    // The list: every tab, the selected one checked; picking one selects it.
    Press(d, CentreOf(*bar->GetListButton()));
    Menu* menu = bar->GetTabListMenu();
    REQUIRE(menu != nullptr);
    CHECK(menu->ItemCount() == 6);
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Down));
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Down));
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Down));
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Return));
    CHECK(bar->GetSelectedIndex() == 2);
    CHECK(bar->GetTabListMenu() == nullptr);
}

TEST_CASE("tabview: pages pair with tabs; only the selected one shows; hidden ones stay alive; Ctrl+Tab from inside a page")
{
    auto root = MakeRoot();
    auto view = Make<TabView>();
    view->EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(200.0f); });
    root->AddChild(view.Get());
    TabPage* first = view->AddPage(core::StringView(u8"First"));
    TabPage* second = view->AddPage(core::StringView(u8"Second"));
    auto field = Make<TextField>();
    second->AddChild(field.Get());
    for (core::i32 i = 0; i < 2; ++i)
        view->GetBar()->GetTab(i)->EditLayoutStyle([](LayoutStyle& s)
        {
            s.Width = LayoutLength::Px(80.0f);
            s.Height = LayoutLength::Px(24.0f);
        });
    (void)root->UpdateLayout();

    CHECK(view->GetPageCount() == 2);
    CHECK(view->GetBar()->GetTab(1)->GetText() == core::StringView(u8"Second"));
    CHECK(first->GetTab() == view->GetBar()->GetTab(0));
    CHECK(first->IsDisplayed());
    CHECK_FALSE(second->IsDisplayed());
    CHECK(first->GetSize().y > 100.0f); // a page fills what the bar leaves

    second->SetTitle(core::StringView(u8"Renamed")); // its tab follows it
    CHECK(view->GetBar()->GetTab(1)->GetText() == core::StringView(u8"Renamed"));

    int changes = 0;
    (void)view->AddEventListener(EventType::Changed, [&](const Event&) { ++changes; });
    EventDispatcher& d = *root->GetEventDispatcher();
    Press(d, CentreOf(*view->GetBar()->GetTab(1)));
    CHECK(view->GetSelectedIndex() == 1);
    CHECK(changes == 1);
    CHECK_FALSE(first->IsDisplayed());
    CHECK(second->IsDisplayed());
    CHECK(first->GetParent() != nullptr); // kept alive

    // Ctrl+Tab with the focus in a page.
    field->RequestFocus();
    d.InjectKeyDown(static_cast<core::u32>(KeyCode::Tab), KeyModCtrl);
    CHECK(view->GetSelectedIndex() == 0);
    CHECK(changes == 2);
}

TEST_CASE("tabview: a close request reaches the view, and unless taken, the page and its tab go; a drag moves the page too")
{
    auto root = MakeRoot();
    auto view = Make<TabView>();
    root->AddChild(view.Get());
    view->GetBar()->SetCloseDisplay(TabCloseDisplay::Always);
    view->SetReorderable(true);
    const char8_t* const names[] = {u8"A", u8"B", u8"C"};
    for (const char8_t* t : names)
        view->AddPage(core::StringView(t))->GetTab()->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(80.0f); });
    Node* pageA = view->GetPage(0);
    (void)root->UpdateLayout();

    bool keep = false;
    core::i32 asked = -1;
    (void)view->AddEventListener(EventType::TabClose, [&](const Event& e)
    {
        asked = static_cast<const TabEvent&>(e).Index;
        e.Handled = keep;
    });
    EventDispatcher& d = *root->GetEventDispatcher();
    Press(d, CentreOf(*view->GetBar()->GetTab(1)->GetClosePart()));
    CHECK(asked == 1);
    CHECK(view->GetPageCount() == 2);
    CHECK(view->GetChildHost()->ChildCount() == 2);
    CHECK(static_cast<TabPage*>(view->GetPage(1))->GetTitle() == core::StringView(u8"C"));

    keep = true;
    (void)root->UpdateLayout();
    Press(d, CentreOf(*view->GetBar()->GetTab(1)->GetClosePart()));
    CHECK(view->GetPageCount() == 2);

    // A drag of A past C: the pages keep the tabs' order.
    (void)root->UpdateLayout();
    const core::Float2 start = CentreOf(*view->GetBar()->GetTab(0));
    d.InjectMouseDown(start, MouseButton::Left);
    d.InjectMouseMove(core::Float2{start.x + 90.0f, start.y});
    d.InjectMouseUp(core::Float2{start.x + 90.0f, start.y}, MouseButton::Left);
    CHECK(view->GetPage(1) == pageA);
    CHECK(view->GetChildHost()->GetChildAt(1) == pageA);
    CHECK(view->GetSelectedPage() == pageA);

    // Code removing a page takes its tab.
    view->RemovePage(0);
    CHECK(view->GetPageCount() == 1);
    CHECK(view->GetBar()->GetTabCount() == 1);
    CHECK(view->GetSelectedPage() == pageA);
}

TEST_CASE("tabview: a page with a content factory is built the first time it is shown")
{
    auto root = MakeRoot();
    auto view = Make<TabView>();
    root->AddChild(view.Get());
    view->AddPage(core::StringView(u8"Light"));
    TabPage* heavy = view->AddPage(core::StringView(u8"Heavy"));
    int built = 0;
    heavy->SetContentFactory([&](TabPage& page)
    {
        ++built;
        page.AddChild(Make<Label>().Get());
    });
    CHECK(built == 0);
    view->SetSelectedIndex(1);
    CHECK(built == 1);
    CHECK(heavy->ChildCount() == 1);
    view->SetSelectedIndex(0);
    view->SetSelectedIndex(1);
    CHECK(built == 1);
}

TEST_CASE("tabview: markup's pages are its children, with title, icon and closable; its bar's settings are attributes")
{
    RegisterGuiElements();
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, core::DefaultAllocator());
    auto tree = loader.LoadFromString(core::StringView(u8R"(<TabView close-display="hover" reorderable="true" selected="1">
        <TabPage title="General"><Label text="one"/></TabPage>
        <TabPage title="Advanced" closable="false" icon="ui/icons/gear.png"/>
    </TabView>)"));
    REQUIRE(tree.Get() != nullptr);
    auto* view = core::Cast<TabView>(tree.Get());
    REQUIRE(view != nullptr);
    CHECK(view->GetPageCount() == 2);
    CHECK(view->GetBar()->GetTab(0)->GetText() == core::StringView(u8"General"));
    CHECK_FALSE(view->GetBar()->GetTab(1)->IsClosable());
    CHECK(view->GetBar()->GetTab(1)->GetIconSource() == core::StringView(u8"ui/icons/gear.png"));
    CHECK(view->GetBar()->GetCloseDisplay() == TabCloseDisplay::Hover);
    CHECK(view->IsReorderable());
    CHECK(view->GetSelectedIndex() == 1);

    // A bar alone: markup's children are its tabs.
    auto barTree = loader.LoadFromString(core::StringView(u8R"(<TabBar><Tab text="One"/><Tab text="Two" icon="a.png"/></TabBar>)"));
    auto* bar = core::Cast<TabBar>(barTree.Get());
    REQUIRE(bar != nullptr);
    CHECK(bar->GetTabCount() == 2);
    CHECK(bar->GetSelectedIndex() == 0);
    CHECK(bar->GetTab(1)->GetIconSource() == core::StringView(u8"a.png"));
}

namespace
{
    core::RefPtr<TabBar> BarAt(SceneRoot& root, float y, const char8_t* group)
    {
        auto bar = Make<TabBar>();
        bar->EditLayoutStyle([y](LayoutStyle& s)
        {
            s.Placement = Position::Absolute;
            s.Left = LayoutLength::Px(10.0f);
            s.Top = LayoutLength::Px(y);
            s.Width = LayoutLength::Px(380.0f);
        });
        bar->SetReorderable(true);
        bar->SetGroup(core::String(group));
        root.AddChild(bar.Get());
        return bar;
    }
}

TEST_CASE("tabs: a tab dragged out of its strip tears out as a drag that a strip of its group takes, where the mark is")
{
    auto root = MakeRoot();
    auto top = BarAt(*root, 10.0f, u8"docs");
    auto bottom = BarAt(*root, 150.0f, u8"docs");
    auto other = BarAt(*root, 250.0f, u8"tools");
    AddSizedTab(*top, u8"A");
    Tab* b = AddSizedTab(*top, u8"B");
    AddSizedTab(*bottom, u8"X");
    AddSizedTab(*bottom, u8"Y");
    AddSizedTab(*other, u8"T");
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();

    const core::Float2 start = CentreOf(*b);
    d.InjectMouseDown(start, MouseButton::Left);
    d.InjectMouseMove(core::Float2{start.x, start.y + 20.0f}); // not past the tear (32)
    CHECK_FALSE(d.IsDragging());
    d.InjectMouseMove(core::Float2{start.x, start.y + 60.0f});
    REQUIRE(d.IsDragging());
    CHECK(d.GetDragPayload().Type == core::StringView(u8"lab/tab"));
    CHECK(d.GetDragPayload().Label == core::StringView(u8"B"));

    // Over the other group's strip: nothing takes it.
    d.InjectMouseMove(CentreOf(*other->GetTab(0)));
    CHECK(d.GetDropTarget() == nullptr);
    // Over the bottom strip, left of Y's middle: between X and Y.
    const Rect y = bottom->GetTab(1)->GetScreenBounds();
    d.InjectMouseMove(core::Float2{y.x + 10.0f, y.y + 10.0f});
    CHECK(d.GetDropTarget() == bottom.Get());
    CHECK(bottom->GetDropIndex() == 1);
    d.InjectMouseUp(core::Float2{y.x + 10.0f, y.y + 10.0f}, MouseButton::Left);
    CHECK(top->GetTabCount() == 1);
    REQUIRE(bottom->GetTabCount() == 3);
    CHECK(bottom->GetTab(1) == b); // the tab itself moved
    CHECK(bottom->GetSelectedTab() == b);
    CHECK(bottom->GetDropIndex() == -1);
}

TEST_CASE("tabs: a tab torn out of one tab view and dropped on another of its group brings its page")
{
    auto root = MakeRoot();
    const auto place = [](TabView& v, float y)
    {
        v.EditLayoutStyle([y](LayoutStyle& s)
        {
            s.Placement = Position::Absolute;
            s.Left = LayoutLength::Px(10.0f);
            s.Top = LayoutLength::Px(y);
            s.Width = LayoutLength::Px(380.0f);
            s.Height = LayoutLength::Px(100.0f);
        });
    };
    auto left = Make<TabView>();
    auto right = Make<TabView>();
    place(*left, 10.0f);
    place(*right, 160.0f);
    root->AddChild(left.Get());
    root->AddChild(right.Get());
    TabView* const views[] = {left.Get(), right.Get()};
    for (TabView* v : views)
    {
        v->SetReorderable(true);
        v->SetGroup(core::String(u8"docs"));
    }
    left->AddPage(core::StringView(u8"One"));
    TabPage* two = left->AddPage(core::StringView(u8"Two"));
    auto label = Make<Label>();
    two->AddChild(label.Get());
    right->AddPage(core::StringView(u8"Three"));
    for (TabView* v : views)
        for (core::i32 i = 0; i < v->GetPageCount(); ++i)
            v->GetBar()->GetTab(i)->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(80.0f);
                s.Height = LayoutLength::Px(24.0f);
            });
    (void)root->UpdateLayout();
    EventDispatcher& d = *root->GetEventDispatcher();

    const core::Float2 start = CentreOf(*left->GetBar()->GetTab(1));
    d.InjectMouseDown(start, MouseButton::Left);
    d.InjectMouseMove(core::Float2{start.x, start.y + 80.0f});
    REQUIRE(d.IsDragging());
    const Rect three = right->GetBar()->GetTab(0)->GetScreenBounds();
    d.InjectMouseMove(core::Float2{three.x + three.width - 5.0f, three.y + 10.0f}); // after Three
    CHECK(right->GetBar()->GetDropIndex() == 1);
    d.InjectMouseUp(core::Float2{three.x + three.width - 5.0f, three.y + 10.0f}, MouseButton::Left);
    CHECK(left->GetPageCount() == 1);
    REQUIRE(right->GetPageCount() == 2);
    CHECK(right->GetPage(1) == two); // the page itself, what it holds with it
    CHECK(two->ChildCount() == 1);
    CHECK(right->GetSelectedPage() == two);
    CHECK(right->GetBar()->GetTab(1)->GetText() == core::StringView(u8"Two"));
}

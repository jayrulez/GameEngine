// Draconic GUI - Menu tests: opening at a position as a popup, activating an item (runs the
// action + closes), and dismissal (outside click / Escape) via the dispatcher popup support.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
}

TEST_CASE("menu: sizes to its items")
{
    auto menu = Make<Menu>();
    menu->SetWidth(140.0f);
    menu->SetItemHeight(25.0f);
    menu->AddItem(foundation::StringView(u8"Cut"), [] {});
    menu->AddItem(foundation::StringView(u8"Copy"), [] {});
    menu->AddItem(foundation::StringView(u8"Paste"), [] {});

    CHECK(menu->ItemCount() == 3);
    CHECK(menu->GetSize().x == doctest::Approx(140.0f));
    CHECK(menu->GetSize().y == doctest::Approx(75.0f)); // 3 * 25
}

TEST_CASE("menu: Open shows it as the dispatcher popup at the given position")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    anchor->SetSize(foundation::Float2{10.0f, 10.0f});
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"One"), [] {});
    menu->AddItem(foundation::StringView(u8"Two"), [] {});

    menu->Open(*anchor, foundation::Float2{100.0f, 80.0f});
    CHECK(menu->IsOpen());
    CHECK(d->GetPopup() == menu.Get());
    CHECK(menu->GetPosition().x == doctest::Approx(100.0f));
    CHECK(menu->GetPosition().y == doctest::Approx(80.0f));
    CHECK(menu->GetParent() == root.Get()); // attached top-level
}

TEST_CASE("menu: activating an item runs its action and closes")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int cut = 0, paste = 0;
    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"Cut"), [&] { ++cut; });
    menu->AddItem(foundation::StringView(u8"Paste"), [&] { ++paste; });
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    // Item 1 (Paste) spans y in [50 + 26, 50 + 52) = [76, 102); click it.
    d->InjectMouseDown(foundation::Float2{60.0f, 88.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{60.0f, 88.0f}, MouseButton::Left);
    CHECK(paste == 1);
    CHECK(cut == 0);
    CHECK_FALSE(menu->IsOpen());
    CHECK(d->GetPopup() == nullptr);
    CHECK(menu->GetParent() == nullptr); // removed from the tree
}

TEST_CASE("menu: outside click and Escape dismiss it")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->AddItem(foundation::StringView(u8"One"), [] {});
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());

    d->InjectMouseDown(foundation::Float2{300.0f, 300.0f}, MouseButton::Left); // outside
    CHECK_FALSE(menu->IsOpen());

    // Reopen and dismiss with Escape.
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());
    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Escape));
    CHECK_FALSE(menu->IsOpen());
}

TEST_CASE("menu: an outside click dismisses even when the opener covers the click point")
{
    // Regression: the context menu was opened with the full-window panel as its popup owner,
    // so clicks anywhere on the panel counted as "on the owner" and never dismissed. A menu
    // has no persistent owner - any press outside the menu itself must close it.
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto panel = Make<UIWidget>();
    panel->SetSize(foundation::Float2{400.0f, 400.0f}); // covers the whole area (like the sandbox panel)
    root->AddChild(panel.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"One"), [] {});
    menu->Open(*panel, foundation::Float2{50.0f, 50.0f});
    REQUIRE(menu->IsOpen());

    // Left-click on the panel, well away from the menu -> dismissed.
    d->InjectMouseDown(foundation::Float2{300.0f, 300.0f}, MouseButton::Left);
    CHECK_FALSE(menu->IsOpen());
    CHECK(d->GetPopup() == nullptr);
}

TEST_CASE("menu: can be reopened after being dismissed")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto panel = Make<UIWidget>();
    panel->SetSize(foundation::Float2{400.0f, 400.0f});
    root->AddChild(panel.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->AddItem(foundation::StringView(u8"One"), [] {});

    menu->Open(*panel, foundation::Float2{40.0f, 40.0f});
    REQUIRE(menu->IsOpen());
    d->InjectMouseDown(foundation::Float2{300.0f, 300.0f}, MouseButton::Left); // dismiss
    REQUIRE_FALSE(menu->IsOpen());

    // Reopen at a new position - must work and re-register as the popup.
    menu->Open(*panel, foundation::Float2{120.0f, 90.0f});
    CHECK(menu->IsOpen());
    CHECK(d->GetPopup() == menu.Get());
    CHECK(menu->GetParent() == root.Get());
    CHECK(menu->GetPosition().x == doctest::Approx(120.0f));
}

// ===================== Depth: separators / checkable / submenus =====================

TEST_CASE("menu: separators are non-selectable rows that add height")
{
    auto menu = Make<Menu>();
    menu->SetWidth(140.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"Open"), [] {});
    menu->AddSeparator();
    menu->AddItem(foundation::StringView(u8"Quit"), [] {});

    CHECK(menu->RowCount() == 3);
    CHECK(menu->ItemCount() == 2); // the separator is not counted as an item
    CHECK(menu->GetSize().y == doctest::Approx(26.0f + 9.0f + 26.0f)); // item + separator + item
}

TEST_CASE("menu: a checkable item toggles, fires its callback, and closes the menu")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    bool state = false;
    int toggles = 0;
    auto menu = Make<Menu>();
    menu->SetItemHeight(26.0f);
    MenuItem* check = menu->AddCheckItem(foundation::StringView(u8"Bold"), false,
                                         [&](bool on)
                                         {
                                             state = on;
                                             ++toggles;
                                         });
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    CHECK_FALSE(check->IsChecked());
    // Row 0 spans y in [50, 76); click it.
    d->InjectMouseDown(foundation::Float2{60.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{60.0f, 60.0f}, MouseButton::Left);
    CHECK(check->IsChecked());
    CHECK(state == true);
    CHECK(toggles == 1);
    CHECK_FALSE(menu->IsOpen()); // menus close on activation
}

TEST_CASE("menu: a submenu opens to the right as a sibling on hover")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"Top"), [] {});
    Menu* sub = menu->AddSubMenu(foundation::StringView(u8"More"));
    sub->AddItem(foundation::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    CHECK(menu->CurrentSubMenu() == nullptr);
    // Hover the "More" row (row 1: y in [76, 102)).
    d->InjectMouseMove(foundation::Float2{60.0f, 88.0f});
    CHECK(menu->CurrentSubMenu() == sub);
    CHECK(sub->IsOpen());
    CHECK(sub->GetParent() == root.Get());                  // attached as a sibling of the menu
    CHECK(sub->GetPosition().x == doctest::Approx(210.0f)); // menu.x(50) + menu.width(160)
    CHECK(sub->GetPosition().y == doctest::Approx(76.0f));  // menu.y(50) + item.y(26)
}

TEST_CASE("menu: hovering a different row closes the open submenu")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"Top"), [] {});          // row 0
    Menu* sub = menu->AddSubMenu(foundation::StringView(u8"More")); // row 1
    sub->AddItem(foundation::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    d->InjectMouseMove(foundation::Float2{60.0f, 88.0f}); // open submenu (hover "More")
    REQUIRE(menu->CurrentSubMenu() == sub);
    d->InjectMouseMove(foundation::Float2{60.0f, 60.0f}); // hover "Top" (row 0)
    CHECK(menu->CurrentSubMenu() == nullptr);
    CHECK(sub->GetParent() == nullptr); // submenu removed from the tree
}

TEST_CASE(
    "menu: clicking inside an open submenu does not dismiss; activating closes the whole chain")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int deep = 0;
    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    menu->AddItem(foundation::StringView(u8"Top"), [] {});
    Menu* sub = menu->AddSubMenu(foundation::StringView(u8"More"));
    sub->AddItem(foundation::StringView(u8"Deep"), [&] { ++deep; });
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    d->InjectMouseMove(foundation::Float2{60.0f, 88.0f}); // open submenu
    REQUIRE(sub->IsOpen());

    // The submenu sits at x in [210, 370], its "Deep" row at y in [76, 102). A press there is
    // inside the chain -> the root menu must stay open (the popup 'contains' predicate).
    d->InjectMouseDown(foundation::Float2{260.0f, 88.0f}, MouseButton::Left);
    CHECK(menu->IsOpen());
    d->InjectMouseUp(foundation::Float2{260.0f, 88.0f}, MouseButton::Left);
    CHECK(deep == 1);
    CHECK_FALSE(menu->IsOpen()); // the whole chain closed
    CHECK(menu->GetParent() == nullptr);
    CHECK(sub->GetParent() == nullptr);
    CHECK(d->GetPopup() == nullptr);
}

TEST_CASE("menu: an outside click dismisses the whole open submenu chain")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{600.0f, 600.0f});
    auto anchor = Make<UIWidget>();
    root->AddChild(anchor.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    auto menu = Make<Menu>();
    menu->SetWidth(160.0f);
    menu->SetItemHeight(26.0f);
    Menu* sub = menu->AddSubMenu(foundation::StringView(u8"More"));
    sub->AddItem(foundation::StringView(u8"Deep"), [] {});
    menu->Open(*anchor, foundation::Float2{50.0f, 50.0f});

    d->InjectMouseMove(foundation::Float2{60.0f, 60.0f}); // "More" is row 0 (y in [50,76)) -> open sub
    REQUIRE(sub->IsOpen());

    d->InjectMouseDown(foundation::Float2{500.0f, 500.0f}, MouseButton::Left); // outside everything
    CHECK_FALSE(menu->IsOpen());
    CHECK(menu->GetParent() == nullptr);
    CHECK(sub->GetParent() == nullptr);
    CHECK(d->GetPopup() == nullptr);
}

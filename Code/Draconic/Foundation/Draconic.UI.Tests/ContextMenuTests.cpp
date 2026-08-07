// Ported from Sedulous.UI.Tests/src/ContextMenuTests.bf (faithful). Beef `delegate void()` -> Function<
// void()>; Beef nullable String -> empty String check; MenuItem.CreateSeparator returns UniquePtr; menu
// item/submenu structure only (no popup, no font).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<ContextMenu> MakeMenu()
{
    return foundation::MakeRef<ContextMenu>(foundation::DefaultAllocator());
}

TEST_CASE("context-menu: MenuItem_Properties")
{
    MenuItem item(u8"Test", []() {}, true);
    CHECK(!item.Label.IsEmpty());
    CHECK(item.Label == u8"Test");
    CHECK(item.Enabled == true);
    CHECK(!item.IsSeparator);
    CHECK(!item.Submenu);
}

TEST_CASE("context-menu: MenuItem_CreateSeparator")
{
    auto item = MenuItem::CreateSeparator();
    CHECK(item->IsSeparator);
    CHECK(item->Label.IsEmpty());
    CHECK(!item->Action);
}

TEST_CASE("context-menu: AddItem_IncreasesCount")
{
    auto menu = MakeMenu();
    CHECK(menu->ItemCount() == 0);

    menu->AddItem(u8"Item 1", []() {});
    CHECK(menu->ItemCount() == 1);

    menu->AddItem(u8"Item 2", []() {});
    CHECK(menu->ItemCount() == 2);
}

TEST_CASE("context-menu: AddSeparator_IncreasesCount")
{
    auto menu = MakeMenu();
    menu->AddItem(u8"Item 1", []() {});
    menu->AddSeparator();
    menu->AddItem(u8"Item 2", []() {});
    CHECK(menu->ItemCount() == 3);
}

TEST_CASE("context-menu: AddSubmenu_CreatesItemWithSubmenu")
{
    auto menu = MakeMenu();
    MenuItem* sub = menu->AddSubmenu(u8"More");
    CHECK(sub != nullptr);
    CHECK(sub->Submenu);
    CHECK(!sub->Label.IsEmpty());
    CHECK(sub->Label == u8"More");
    CHECK(menu->ItemCount() == 1);
}

TEST_CASE("context-menu: IsFocusable")
{
    auto menu = MakeMenu();
    CHECK(menu->IsFocusable);
}

TEST_CASE("context-menu: Show resets the stale hover highlight")
{
    // Menus are retained views (a menu bar reuses its instances): hover from the previous
    // open must NOT survive into the next Show - the regression was "Close Project" still
    // highlighted when reopening the File menu after a project close/reopen.
    auto menu = MakeMenu();
    menu->AddItem(u8"Open", {});
    menu->AddItem(u8"Close Project", {});

    MouseEventArgs move;
    move.X = 10.0f;
    move.Y = 10.0f; // inside the first item's band
    menu->OnMouseMove(move);
    REQUIRE(menu->HoveredIndex() >= 0);

    UIContext ctx; // no active root: Show early-outs, but AFTER clearing the hover
    menu->Show(&ctx, 0.0f, 0.0f);
    CHECK(menu->HoveredIndex() == -1);
}

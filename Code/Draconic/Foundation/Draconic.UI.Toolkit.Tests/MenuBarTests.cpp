// Smoke test for MenuBar: constructs, adds menus, tracks count, returns usable ContextMenus.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-menubar: AddsMenus")
{
    auto bar = foundation::MakeRef<MenuBar>(foundation::DefaultAllocator());
    CHECK(bar->MenuCount() == 0u);

    ContextMenu* fileMenu = bar->AddMenu(u8"File");
    REQUIRE(fileMenu != nullptr);
    ContextMenu* editMenu = bar->AddMenu(u8"Edit");
    REQUIRE(editMenu != nullptr);

    CHECK(bar->MenuCount() == 2u);
    CHECK(fileMenu != editMenu);

    // The returned menus are live and accept items (no popup shown here).
    bool ran = false;
    fileMenu->AddItem(u8"Open", [&]() { ran = true; });
    fileMenu->AddSeparator();
    editMenu->AddItem(u8"Undo", []() {});
    CHECK(ran == false); // action not invoked merely by adding

    // MenuBar is its own popup owner.
    CHECK(bar->OwnerView() == bar.Get());
}

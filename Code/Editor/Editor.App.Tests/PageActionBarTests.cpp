// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A page's bar bound to the actions over that page: a click runs the action with the page as its
// subject (not the active page); Sync shows the action's label and keys in the tooltip, enabled and
// checked as the action answers over the page, two pages' bars each their own.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    class BarPage final : public EditorPage
    {
    public:
        BarPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"bar"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        bool ready = false;
        bool on = false;
        u32 runs = 0;
    };
}

TEST_CASE("page action bar: a click runs the action over its own page; Sync shows its label, keys, enabled and checked")
{
    EditorActionRegistry actions;
    EditorActionDeclaration toggle;
    toggle.id = String(u8"page.toggle");
    toggle.label = String(u8"Toggle");
    toggle.kind = EditorActionKind::Toggle;
    toggle.shortcut = EditorShortcut::Of(u8"ctrl-t");
    toggle.enabled = [](EditorPage* page) { return page != nullptr && static_cast<BarPage*>(page)->ready; };
    toggle.checked = [](EditorPage* page) { return page != nullptr && static_cast<BarPage*>(page)->on; };
    toggle.execute = [](EditorPage* page)
    {
        auto* bar = static_cast<BarPage*>(page);
        bar->on = !bar->on;
        ++bar->runs;
    };
    REQUIRE(actions.Register(Move(toggle)));

    BarPage first;
    BarPage second;
    auto firstButton = MakeRef<gui::ToolButton>(DefaultAllocator());
    firstButton->SetToggle(true);
    auto secondButton = MakeRef<gui::ToolButton>(DefaultAllocator());
    secondButton->SetToggle(true);
    app::PageActionBar firstBar(DefaultAllocator(), actions, first);
    app::PageActionBar secondBar(DefaultAllocator(), actions, second);
    firstBar.Bind(firstButton.Get(), u8"page.toggle");
    secondBar.Bind(secondButton.Get(), u8"page.toggle");
    firstBar.Bind(nullptr, u8"page.toggle"); // no such button in the markup: nothing bound
    CHECK(firstBar.Count() == 1u);

    CHECK(firstButton->GetTooltipText() == StringView(u8"Toggle (Ctrl+T)"));
    CHECK_FALSE(firstButton->IsEnabled()); // its page is not ready

    first.ready = true;
    firstBar.Sync();
    secondBar.Sync();
    CHECK(firstButton->IsEnabled());
    CHECK_FALSE(secondButton->IsEnabled()); // the other page's answer

    CHECK(firstButton->HandleActivate());
    CHECK(first.runs == 1u);
    CHECK(second.runs == 0u);
    firstBar.Sync();
    CHECK(firstButton->IsChecked());
    CHECK_FALSE(secondButton->IsChecked());
}

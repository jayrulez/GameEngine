// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorDock: the shared panels by the ids saved layouts and the MCP tools name, the
// default layout (Project central, Assets and Console under it side by side), the Project panel's
// content the open project's page or the welcome wherever the panel shows, Reset Layout, and the
// Profiler page presented in the page area.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.docking;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
using editor::app::EditorDock;

namespace
{
    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Panel> into = MakeRef<gui::Panel>(DefaultAllocator());
        EditorDock dock{DefaultAllocator()};
        RefPtr<gui::Label> welcome = MakeRef<gui::Label>(DefaultAllocator());

        Bed()
        {
            root->SetSize(Float2{800.0f, 600.0f});
            into->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(800.0f);
                s.Height = gui::LayoutLength::Px(600.0f);
            });
            root->AddChild(into.Get());
            EditorDock::Contents contents;
            gui::Label* made = welcome.Get();
            contents.welcome = [made]() { return RefPtr<gui::Node>(made); };
            dock.Open(*into, nullptr, Move(contents));
            (void)root->UpdateLayout();
        }
        [[nodiscard]] const gui::DockLayout& Layout() { return dock.Controller().GetLayout(); }
    };
}

TEST_CASE("editor dock: the shared panels by their ids, the Project panel central and fixed, Assets and Console under it side by side")
{
    Bed bed;
    REQUIRE(bed.dock.Area() != nullptr);
    CHECK(bed.dock.Area()->GetParent() == bed.into.Get());
    CHECK_FALSE(bed.dock.FloatsInWindows()); // no host to open windows

    const gui::DockLayout& layout = bed.Layout();
    CHECK(layout.StackOf(editor::app::kPanelProject) == layout.CentralStack());
    const gui::DockNodeId assets = layout.StackOf(editor::app::kPanelAssets);
    const gui::DockNodeId console = layout.StackOf(editor::app::kPanelConsole);
    REQUIRE(assets != gui::kNoDock);
    REQUIRE(console != gui::kNoDock);
    CHECK(assets != console); // side by side, both shown
    CHECK(assets != layout.CentralStack());

    const gui::DockPanelDescription* project = bed.dock.Controller().Describe(editor::app::kPanelProject);
    REQUIRE(project != nullptr);
    CHECK(project->Title == String(u8"Project"));
    CHECK_FALSE(project->Closable);
    CHECK_FALSE(bed.dock.Controller().Close(editor::app::kPanelProject).Ok());
    CHECK(bed.dock.Controller().Describe(editor::app::kPanelConsole)->Closable);
}

TEST_CASE("editor dock: the Project panel shows the welcome, the open project's page while one is open, the welcome again after")
{
    Bed bed;
    RefPtr<gui::Node> holder = bed.dock.Controller().Content(editor::app::kPanelProject);
    REQUIRE(holder.Get() != nullptr);
    REQUIRE(holder->ChildCount() == 1u);
    CHECK(holder->GetChildAt(0) == bed.welcome.Get());

    RefPtr<gui::Label> page = MakeRef<gui::Label>(DefaultAllocator());
    bed.dock.SetProjectContent(page);
    REQUIRE(holder->ChildCount() == 1u);
    CHECK(holder->GetChildAt(0) == page.Get());
    // Moved to another stack, the panel keeps its content: the same holder.
    REQUIRE(bed.dock.Controller().Edit([](gui::DockLayout& l)
    {
        const gui::DockDrop beside{gui::DockDropKind::Tab, l.StackOf(editor::app::kPanelAssets), gui::kNoDock, 0, gui::DockSide::Start};
        return gui::ApplyDrop(l, editor::app::kPanelProject, beside);
    }).Ok());
    CHECK(bed.Layout().StackOf(editor::app::kPanelProject) == bed.Layout().StackOf(editor::app::kPanelAssets));
    CHECK(bed.dock.Controller().Content(editor::app::kPanelProject).Get() == holder.Get());

    bed.dock.SetProjectContent({});
    REQUIRE(holder->ChildCount() == 1u);
    CHECK(holder->GetChildAt(0) == bed.welcome.Get());
}

TEST_CASE("editor dock: Reset Layout takes the default again, a closed panel shown where it belongs")
{
    Bed bed;
    REQUIRE(bed.dock.Controller().Close(editor::app::kPanelConsole).Ok());
    CHECK(bed.Layout().StackOf(editor::app::kPanelConsole) == gui::kNoDock);
    bed.dock.ResetLayout();
    CHECK(bed.Layout().StackOf(editor::app::kPanelConsole) != gui::kNoDock);
    CHECK(bed.Layout().StackOf(editor::app::kPanelProject) == bed.Layout().CentralStack());
}

TEST_CASE("editor dock: the Profiler is a page, out of the default layout until presented, then in the page area and to the front")
{
    Bed bed;
    const gui::DockPanelDescription* profiler = bed.dock.Controller().Describe(editor::app::kPanelProfiler);
    REQUIRE(profiler != nullptr);
    CHECK(profiler->Title == String(u8"Profiler"));
    CHECK(profiler->Default.Central);
    CHECK(bed.Layout().StackOf(editor::app::kPanelProfiler) == gui::kNoDock);

    bed.dock.Present(editor::app::kPanelProfiler);
    CHECK(bed.Layout().StackOf(editor::app::kPanelProfiler) == bed.Layout().CentralStack());

    // Presented again behind the Project panel's tab: to the front.
    REQUIRE(bed.dock.Controller().Edit([](gui::DockLayout& layout) { return layout.Activate(editor::app::kPanelProject); }).Ok());
    bed.dock.Present(editor::app::kPanelProfiler);
    const gui::DockNode* central = bed.Layout().FindNode(bed.Layout().CentralStack());
    REQUIRE(central != nullptr);
    CHECK(central->Panels[central->Active] == String(editor::app::kPanelProfiler));
}

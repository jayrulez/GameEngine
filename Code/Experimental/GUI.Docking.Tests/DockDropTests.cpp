// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - drag to dock within one window (D4 of Specs/gui-lab-evolution-3.md): a tab torn
// from a stack's strip carries its panel; over a strip it goes in at a tab's place, over a stack's
// middle into it, over its edge band beside it, over the surface's band along the whole edge. Each
// drop is the layout's move, tried on a copy first: the preview is where the panel would be, and a
// move the layout refuses shows its reason and does nothing.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;
import experimental.gui.docking;

using namespace experimental::gui;
namespace core = foundation::core;
using core::f32;
using core::i32;
using core::usize;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // Scene and inspector tabbed in the central stack, the hierarchy beside it on the left (a
    // quarter), the console under the central stack.
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        DockController controller{core::DefaultAllocator()};
        core::RefPtr<DockSpace> space = core::MakeRef<DockSpace>(core::DefaultAllocator());

        Bed()
        {
            root->SetSize(core::Float2{800.0f, 600.0f});
            space->EditLayoutStyle([](LayoutStyle& s) {
                s.Width = LayoutLength::Px(800.0f);
                s.Height = LayoutLength::Px(600.0f);
            });
            root->AddChild(space.Get());
            const char8_t* const ids[] = {u8"scene", u8"inspector", u8"hierarchy", u8"console", u8"log"};
            for (const char8_t* id : ids)
            {
                DockPanelDescription description;
                description.Id = core::String(id);
                description.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator())); };
                REQUIRE(controller.Register(core::Move(description)));
            }
            space->Show(controller, controller.GetLayout().MainSurface());
            const DockNodeId central = controller.GetLayout().CentralStack();
            REQUIRE(controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"scene"), central); }).Ok());
            REQUIRE(controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"inspector"), central); }).Ok());
            REQUIRE(controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"hierarchy"), central, DockSide::Start, 0.25f); }).Ok());
            REQUIRE(controller.Edit([&](DockLayout& l) { return l.InsertBeside(SV(u8"console"), central, DockSide::Bottom, 0.3f); }).Ok());
            Lay();
        }
        void Lay()
        {
            root->UpdateLayout();
            // The tabs a size of their own: with no font their text measures nothing.
            const char8_t* const ids[] = {u8"scene", u8"inspector", u8"hierarchy", u8"console"};
            for (const char8_t* id : ids)
                if (Tab* tab = TabOf(id))
                    tab->EditLayoutStyle([](LayoutStyle& s) {
                        s.Width = LayoutLength::Px(80.0f);
                        s.Height = LayoutLength::Px(24.0f);
                    });
            root->UpdateLayout();
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        DockNodeId StackOf(const char8_t* panel) const { return controller.GetLayout().StackOf(SV(panel)); }
        TabView* ViewOf(const char8_t* panel) const { return Cast<TabView>(space->WidgetOf(StackOf(panel))); }
        Tab* TabOf(const char8_t* panel) const
        {
            TabView* view = ViewOf(panel);
            TabPage* page = space->PageOf(SV(panel));
            if (view == nullptr || page == nullptr)
                return nullptr;
            for (i32 i = 0; i < view->GetPageCount(); ++i)
                if (view->GetPage(i) == page)
                    return view->GetBar()->GetTab(i);
            return nullptr;
        }
        static core::Float2 Centre(const Rect& r) { return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f}; }
        static Rect Bounds(const Node& n) { return n.GetScreenBounds(); }
        // A tab torn out of its strip (pulled down into its stack).
        void Tear(const char8_t* panel)
        {
            Tab* tab = TabOf(panel);
            REQUIRE(tab != nullptr);
            const core::Float2 at = Centre(Bounds(*tab));
            D().InjectMouseDown(at, MouseButton::Left);
            D().InjectMouseMove(core::Float2{at.x, at.y + 50.0f});
            D().InjectMouseMove(core::Float2{at.x, at.y + 60.0f});
            REQUIRE(D().IsDragging());
        }
        void Over(core::Float2 at) { D().InjectMouseMove(at); }
        void Release(core::Float2 at) { D().InjectMouseUp(at, MouseButton::Left); }
        bool Preview(Rect& out) const { return space->GetPreviewBounds(out); }
    };
}

TEST_CASE("dock drop: over a stack's middle the panel tabs into it; the preview is the stack")
{
    Bed bed;
    const DockNodeId hierarchy = bed.StackOf(u8"hierarchy");
    bed.Tear(u8"inspector");
    const Rect stack = Bed::Bounds(*bed.space->WidgetOf(hierarchy));
    const core::Float2 middle = Bed::Centre(stack);
    bed.Over(middle);
    CHECK(bed.D().GetDropTarget() == bed.space->WidgetOf(hierarchy));
    Rect preview;
    REQUIRE(bed.Preview(preview));
    CHECK(preview.x == doctest::Approx(stack.x).epsilon(0.02));
    CHECK(preview.width == doctest::Approx(stack.width).epsilon(0.05));
    CHECK(bed.controller.GetLayout().StackOf(SV(u8"inspector")) != hierarchy); // nothing moved yet
    bed.Release(middle);
    CHECK(bed.StackOf(u8"inspector") == hierarchy);
    CHECK_FALSE(bed.Preview(preview));
    CHECK(bed.space->PageOf(SV(u8"inspector")) != nullptr);
}

TEST_CASE("dock drop: over a stack's edge band the panel goes beside it, the preview its half")
{
    Bed bed;
    const DockNodeId console = bed.StackOf(u8"console");
    bed.Tear(u8"inspector");
    const Rect stack = Bed::Bounds(*bed.space->WidgetOf(console));
    const core::Float2 right{stack.x + stack.width - 30.0f, stack.y + stack.height * 0.5f}; // in its band (64)
    bed.Over(right);
    Rect preview;
    REQUIRE(bed.Preview(preview));
    CHECK(preview.x == doctest::Approx(stack.x + stack.width * 0.5f).epsilon(0.03));
    CHECK(preview.width == doctest::Approx(stack.width * 0.5f).epsilon(0.05));
    bed.Release(right);
    const DockNodeId inspector = bed.StackOf(u8"inspector");
    REQUIRE(inspector != kNoDock);
    CHECK(inspector != console);
    const DockNode* split = bed.controller.GetLayout().FindNode(bed.controller.GetLayout().FindNode(inspector)->Parent);
    REQUIRE(split != nullptr);
    CHECK(split->Axis == DockAxis::Horizontal);
    CHECK(split->Children[0] == console);
    CHECK(split->Children[1] == inspector);
}

TEST_CASE("dock drop: over the surface's band the panel goes along the whole edge")
{
    Bed bed;
    bed.Tear(u8"inspector");
    const core::Float2 top{400.0f, 8.0f}; // in the surface's band (20), over the central strip
    // Down the left side, over the hierarchy's body: the surface's band too.
    bed.Over(core::Float2{6.0f, 300.0f});
    CHECK(bed.D().GetDropTarget() == bed.space.Get());
    bed.Over(top);
    CHECK(bed.D().GetDropTarget() != nullptr); // the strip under it, which leaves the band its due
    Rect preview;
    REQUIRE(bed.Preview(preview));
    CHECK(preview.y == doctest::Approx(0.0f));
    CHECK(preview.width == doctest::Approx(800.0f));
    CHECK(preview.height == doctest::Approx(150.0f).epsilon(0.05)); // a quarter
    bed.Release(top);
    const DockLayout& layout = bed.controller.GetLayout();
    const DockNode* root = layout.FindNode(layout.FindSurface(layout.MainSurface())->Root);
    REQUIRE(root != nullptr);
    CHECK(root->Axis == DockAxis::Vertical);
    CHECK(root->Children[0] == bed.StackOf(u8"inspector"));
}

TEST_CASE("dock drop: over a strip the panel goes in at a tab's place, counted as the tabs are")
{
    Bed bed;
    // Into the console's strip, before the console.
    bed.Tear(u8"scene");
    const Rect consoleTab = Bed::Bounds(*bed.TabOf(u8"console"));
    const core::Float2 before{consoleTab.x + 10.0f, consoleTab.y + 10.0f};
    bed.Over(before);
    CHECK(bed.D().GetDropTarget() == bed.ViewOf(u8"console")->GetBar());
    bed.Release(before);
    const DockNode* stack = bed.controller.GetLayout().FindNode(bed.StackOf(u8"console"));
    REQUIRE(stack->Panels.Size() == 2u);
    CHECK(stack->Panels[0] == SV(u8"scene"));
    CHECK(stack->Panels[1] == SV(u8"console"));
    bed.Lay();
    // Within its own strip, torn and dropped after the console: last.
    bed.Tear(u8"scene");
    const Rect after = Bed::Bounds(*bed.TabOf(u8"console"));
    const core::Float2 end{after.x + after.width - 5.0f, after.y + 10.0f};
    bed.Over(end);
    bed.Release(end);
    stack = bed.controller.GetLayout().FindNode(bed.StackOf(u8"console"));
    CHECK(stack->Panels[0] == SV(u8"console"));
    CHECK(stack->Panels[1] == SV(u8"scene"));
}

TEST_CASE("dock drop: a move the layout refuses shows its reason, no preview, and does nothing; Escape leaves it as it was")
{
    Bed bed;
    const DockNodeId hierarchy = bed.StackOf(u8"hierarchy");
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.SetStackFlags(hierarchy, DockStackNoTabs); }).Ok());
    bed.Lay();
    bed.Tear(u8"inspector");
    const core::Float2 middle = Bed::Centre(Bed::Bounds(*bed.space->WidgetOf(hierarchy)));
    bed.Over(middle);
    CHECK(bed.D().GetDropTarget() == nullptr);
    CHECK_FALSE(bed.D().GetDropReason().IsEmpty());
    Rect preview;
    CHECK_FALSE(bed.Preview(preview));
    bed.Release(middle);
    CHECK(bed.StackOf(u8"inspector") == bed.controller.GetLayout().CentralStack());

    bed.Tear(u8"inspector");
    const core::Float2 console = Bed::Centre(Bed::Bounds(*bed.space->WidgetOf(bed.StackOf(u8"console"))));
    bed.Over(console);
    REQUIRE(bed.Preview(preview));
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK_FALSE(bed.D().IsDragging());
    CHECK_FALSE(bed.Preview(preview));
    CHECK(bed.StackOf(u8"inspector") == bed.controller.GetLayout().CentralStack());
}

TEST_CASE("dock drop: the bands are the sheet's, through the theme's tokens")
{
    Bed bed;
    StyleManager styles;
    const core::String css = core::Format(u8"{} :root {{ --dock-edge: 40px; --dock-surface-edge: 12px; }}", DockingSheetCSS());
    styles.SetStyleSheet(CSSParser::Parse(css.AsView()));
    styles.ApplyTree(*bed.root.Get());
    CHECK(bed.space->GetEdgeBand() == doctest::Approx(40.0f));
    CHECK(bed.space->GetSurfaceBand() == doctest::Approx(12.0f));
}

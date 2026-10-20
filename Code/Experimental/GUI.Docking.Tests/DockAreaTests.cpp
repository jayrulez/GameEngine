// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - floating surfaces in one window (D5 of Specs/gui-lab-evolution-3.md): a DockArea
// shows the main surface and each floating one as a window over it, holding a dock space of its
// own; the windows follow the layout (made, placed, titled, ended) and the layout follows what the
// user does to them (moved or resized, closed); a float is docked into like the main surface; a tab
// dragged with Shift, Move to's Float and dock.float float a panel; a float dragged by its title
// docks whole over a target, is only moved with Shift, and goes back on Escape.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;
import experimental.gui.docking;

using namespace experimental::gui;
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
        core::RefPtr<DockArea> area = core::MakeRef<DockArea>(core::DefaultAllocator());

        Bed()
        {
            root->SetSize(core::Float2{800.0f, 600.0f});
            area->EditLayoutStyle([](LayoutStyle& s) {
                s.Width = LayoutLength::Px(800.0f);
                s.Height = LayoutLength::Px(600.0f);
            });
            root->AddChild(area.Get());
            const char8_t* const ids[] = {u8"scene", u8"profiler", u8"memory", u8"console"};
            const char8_t* const titles[] = {u8"Scene", u8"Profiler", u8"Memory", u8"Console"};
            for (usize i = 0; i < 4; ++i)
            {
                DockPanelDescription description;
                description.Id = core::String(ids[i]);
                description.Title = core::String(titles[i]);
                description.Closable = i != 3; // the console may not close
                description.Factory = []() { return core::RefPtr<Node>(core::MakeRef<Panel>(core::DefaultAllocator())); };
                REQUIRE(controller.Register(core::Move(description)));
            }
            area->Show(controller);
            REQUIRE(controller.Edit([](DockLayout& l) { return l.InsertTab(SV(u8"scene"), l.CentralStack()); }).Ok());
            Lay();
        }
        void Lay()
        {
            root->UpdateLayout();
            // The tabs a size of their own: with no font their text measures nothing.
            const char8_t* const ids[] = {u8"scene", u8"profiler", u8"memory", u8"console"};
            for (const char8_t* id : ids)
                if (Tab* tab = TabOf(id))
                    tab->EditLayoutStyle([](LayoutStyle& s) {
                        s.Width = LayoutLength::Px(80.0f);
                        s.Height = LayoutLength::Px(24.0f);
                    });
            root->UpdateLayout();
        }
        // The dock space showing a panel: the main one or a float's.
        DockSpace* SpaceOf(const char8_t* panel) const
        {
            if (area->GetMainSpace()->PageOf(SV(panel)) != nullptr)
                return area->GetMainSpace();
            for (usize i = 0; i < area->GetFloatCount(); ++i)
                if (area->GetFloat(i)->GetSpace()->PageOf(SV(panel)) != nullptr)
                    return area->GetFloat(i)->GetSpace();
            return nullptr;
        }
        Tab* TabOf(const char8_t* panel) const
        {
            DockSpace* space = SpaceOf(panel);
            if (space == nullptr)
                return nullptr;
            TabView* view = Cast<TabView>(space->WidgetOf(controller.GetLayout().StackOf(SV(panel))));
            TabPage* page = space->PageOf(SV(panel));
            if (view == nullptr)
                return nullptr;
            for (core::i32 i = 0; i < view->GetPageCount(); ++i)
                if (view->GetPage(i) == page)
                    return view->GetBar()->GetTab(i);
            return nullptr;
        }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        DockSurfaceId Float(const char8_t* panel, core::Rectangle bounds)
        {
            DockSurfaceId made = kNoDock;
            REQUIRE(controller.Edit([&](DockLayout& l) { return l.Float(SV(panel), bounds, &made); }).Ok());
            Lay();
            return made;
        }
    };
}

TEST_CASE("dock area: a floating surface is a window over the main one, at its bounds, its panels in a dock space of its own")
{
    Bed bed;
    CHECK(bed.area->GetFloatCount() == 0u);
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{100.0f, 80.0f, 300.0f, 200.0f});
    REQUIRE(bed.area->GetFloatCount() == 1u);
    DockFloat* window = bed.area->FloatOf(floating);
    REQUIRE(window != nullptr);
    CHECK(window->HasClass(SV(u8"dock-float")));
    CHECK(window->GetPosition().x == doctest::Approx(100.0f));
    CHECK(window->GetSize().x == doctest::Approx(300.0f));
    CHECK(window->GetTitle() == SV(u8"Profiler"));
    REQUIRE(window->GetSpace() != nullptr);
    CHECK(window->GetSpace()->GetSurface() == floating);
    CHECK(window->GetSpace()->PageOf(SV(u8"profiler")) != nullptr);
    CHECK(bed.area->GetMainSpace()->PageOf(SV(u8"profiler")) == nullptr);
    CHECK(bed.area->GetMainSpace()->GetSize().x == doctest::Approx(800.0f)); // the main surface fills the area

    // Docked into like the main one: a tab, then the title counts it.
    const DockNodeId stack = bed.controller.GetLayout().StackOf(SV(u8"profiler"));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.InsertTab(SV(u8"memory"), stack); }).Ok());
    bed.Lay();
    CHECK(window->GetSpace()->PageOf(SV(u8"memory")) != nullptr);
    CHECK(window->GetTitle() == SV(u8"Memory +1")); // the shown one first

    // Docked away whole: the window ends, its panels' contents kept and shown in the main surface.
    const core::RefPtr<Node> content = bed.controller.Content(SV(u8"profiler"));
    REQUIRE(bed.controller.Edit([&](DockLayout& l) { return l.DockSurfaceInto(floating, l.CentralStack()); }).Ok());
    bed.Lay();
    CHECK(bed.area->GetFloatCount() == 0u);
    REQUIRE(bed.area->GetMainSpace()->PageOf(SV(u8"profiler")) != nullptr);
    CHECK(bed.controller.Content(SV(u8"profiler")) == content);
}

TEST_CASE("dock area: a float moved (Shift on its title) or resized by the user writes its bounds back, kept where its title can be reached")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{100.0f, 80.0f, 300.0f, 200.0f});
    DockFloat* window = bed.area->FloatOf(floating);
    REQUIRE(window != nullptr);
    const core::Float2 at{window->GetScreenBounds().x + 150.0f, window->GetScreenBounds().y + 10.0f};
    bed.D().InjectMouseDown(at, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{at.x + 20.0f, at.y + 15.0f}, KeyModShift);
    bed.D().InjectMouseMove(core::Float2{at.x + 40.0f, at.y + 30.0f}, KeyModShift);
    bed.D().InjectMouseUp(core::Float2{at.x + 40.0f, at.y + 30.0f}, MouseButton::Left, KeyModShift);
    bed.Lay();
    const core::Rectangle moved = bed.controller.GetLayout().FindSurface(floating)->Bounds;
    CHECK(moved.x == doctest::Approx(140.0f));
    CHECK(moved.y == doctest::Approx(110.0f));
    CHECK(moved.width == doctest::Approx(300.0f));

    // Far off the area: its title bar kept in reach, and that is what is noted.
    // Its grip: the size noted too.
    const Rect box = window->GetScreenBounds();
    const core::Float2 grip{box.x + box.width - 7.0f, box.y + box.height - 7.0f};
    bed.D().InjectMouseDown(grip, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{grip.x + 50.0f, grip.y + 20.0f});
    bed.D().InjectMouseUp(core::Float2{grip.x + 50.0f, grip.y + 20.0f}, MouseButton::Left);
    bed.Lay();
    CHECK(bed.controller.GetLayout().FindSurface(floating)->Bounds.width == doctest::Approx(350.0f));
    CHECK(bed.controller.GetLayout().FindSurface(floating)->Bounds.height == doctest::Approx(220.0f));

    const core::Float2 from{window->GetScreenBounds().x + 150.0f, window->GetScreenBounds().y + 10.0f};
    bed.D().InjectMouseDown(from, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{from.x + 20.0f, from.y}, KeyModShift);
    bed.D().InjectMouseMove(core::Float2{from.x + 2000.0f, from.y - 500.0f}, KeyModShift);
    bed.D().InjectMouseUp(core::Float2{from.x + 2000.0f, from.y - 500.0f}, MouseButton::Left, KeyModShift);
    bed.Lay();
    const core::Rectangle kept = bed.controller.GetLayout().FindSurface(floating)->Bounds;
    CHECK(kept.x == doctest::Approx(800.0f - DockArea::kKeepInView));
    CHECK(kept.y == doctest::Approx(0.0f));
    CHECK(window->GetPosition().x == doctest::Approx(kept.x));
}

TEST_CASE("dock area: closing a float closes its panels as their policies say; one that may not close keeps it")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{100.0f, 80.0f, 300.0f, 200.0f});
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertTab(SV(u8"memory"), l.StackOf(SV(u8"profiler"))); }).Ok());
    bed.Lay();
    DockFloat* window = bed.area->FloatOf(floating);
    REQUIRE(window != nullptr);
    window->RequestClose(); // what its close button does
    bed.Lay();
    CHECK(bed.area->GetFloatCount() == 0u);
    CHECK(bed.controller.GetLayout().FindHidden(SV(u8"profiler")) != nullptr); // hidden, its place kept
    CHECK(bed.controller.GetLayout().FindHidden(SV(u8"memory")) != nullptr);

    const DockSurfaceId other = bed.Float(u8"console", core::Rectangle{50.0f, 50.0f, 300.0f, 200.0f});
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertTab(SV(u8"memory"), l.StackOf(SV(u8"console"))); }).Ok());
    bed.Lay();
    bed.area->FloatOf(other)->RequestClose();
    bed.Lay();
    REQUIRE(bed.area->FloatOf(other) != nullptr); // the console stays, and its window
    CHECK(bed.controller.GetLayout().StackOf(SV(u8"console")) != kNoDock);
    CHECK(bed.controller.GetLayout().StackOf(SV(u8"memory")) == kNoDock);
}

TEST_CASE("dock area: floats with no bounds of their own cascade, and a restored layout's floats come back as windows")
{
    Bed bed;
    (void)bed.Float(u8"profiler", core::Rectangle{0.0f, 0.0f, 0.0f, 0.0f});
    (void)bed.Float(u8"memory", core::Rectangle{0.0f, 0.0f, 0.0f, 0.0f});
    REQUIRE(bed.area->GetFloatCount() == 2u);
    const core::Float2 first = bed.area->GetFloat(0)->GetPosition();
    const core::Float2 second = bed.area->GetFloat(1)->GetPosition();
    CHECK(second.x > first.x);
    CHECK(bed.area->GetFloat(0)->GetSize().x == doctest::Approx(DockArea::kFloatSize.x));
    CHECK(bed.controller.GetLayout().FindSurface(bed.area->GetFloat(0)->GetSurface())->Bounds.width == doctest::Approx(DockArea::kFloatSize.x));

    // The same layout adopted again (a restore): the windows stay one per float.
    const DockLayout saved = bed.controller.GetLayout();
    REQUIRE(bed.controller.Adopt(saved).Ok());
    bed.Lay();
    CHECK(bed.area->GetFloatCount() == 2u);
}

TEST_CASE("dock area: a tab dragged with Shift held floats at the pointer: the dock targets pass it on, the preview is the window")
{
    Bed bed;
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertTab(SV(u8"profiler"), l.CentralStack()); }).Ok());
    bed.Lay();
    Tab* tab = bed.TabOf(u8"profiler");
    REQUIRE(tab != nullptr);
    const Rect from = tab->GetScreenBounds();
    const core::Float2 press{from.x + 10.0f, from.y + 10.0f};
    bed.D().InjectMouseDown(press, MouseButton::Left);
    const core::Float2 over{400.0f, 300.0f}; // the central stack's middle
    bed.D().InjectMouseMove(core::Float2{press.x + 10.0f, press.y});
    bed.D().InjectMouseMove(over);
    REQUIRE(bed.D().IsDragging());
    CHECK(bed.D().GetDropTarget() != bed.area.Get()); // docking: into the stack
    core::Rectangle preview;
    CHECK_FALSE(bed.area->GetFloatPreview(preview));

    bed.D().InjectMouseMove(over, KeyModShift); // Shift: a float
    CHECK(bed.D().GetDropTarget() == bed.area.Get());
    REQUIRE(bed.area->GetFloatPreview(preview));
    CHECK(preview.x < over.x);
    CHECK(preview.x + preview.width > over.x);
    CHECK(preview.y == doctest::Approx(over.y - 14.0f)); // its title bar under the pointer
    Rect docked;
    CHECK_FALSE(bed.area->GetMainSpace()->GetPreviewBounds(docked)); // the dock's preview gone

    bed.D().InjectMouseUp(over, MouseButton::Left, KeyModShift); // let go with Shift still held
    bed.Lay();
    REQUIRE(bed.area->GetFloatCount() == 1u);
    DockFloat* window = bed.area->GetFloat(0);
    CHECK(window->GetSpace()->PageOf(SV(u8"profiler")) != nullptr);
    CHECK(window->GetPosition().x == doctest::Approx(preview.x));
    CHECK(window->GetPosition().y == doctest::Approx(preview.y));
    CHECK_FALSE(bed.area->GetFloatPreview(preview));
}

TEST_CASE("dock area: Move to's Float and dock.float float a panel over its stack")
{
    Bed bed;
    DefineDockingCommands(bed.root->GetContext());
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertBeside(SV(u8"profiler"), l.CentralStack(), DockSide::End, 0.4f); }).Ok());
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertTab(SV(u8"memory"), l.StackOf(SV(u8"profiler"))); }).Ok());
    bed.Lay();
    DockSpace* main = bed.area->GetMainSpace();
    Tab* tab = bed.TabOf(u8"memory");
    REQUIRE(tab != nullptr);
    Menu* menu = main->OpenMoveTo(SV(u8"memory"), *tab, core::Float2{0.0f, 0.0f});
    REQUIRE(menu != nullptr);
    MenuItem* floatItem = nullptr;
    for (usize i = 0; i < menu->ChildCount(); ++i)
        if (auto* item = Cast<MenuItem>(menu->GetChildAt(i)); item != nullptr && item->GetText() == SV(u8"Float"))
            floatItem = item;
    REQUIRE(floatItem != nullptr);
    CHECK(floatItem->IsEnabled());
    const Rect stack = main->WidgetOf(bed.controller.GetLayout().StackOf(SV(u8"memory")))->GetScreenBounds();
    floatItem->HandleActivate();
    bed.Lay();
    REQUIRE(bed.area->GetFloatCount() == 1u);
    const DockFloat* window = bed.area->GetFloat(0);
    CHECK(window->GetSpace()->PageOf(SV(u8"memory")) != nullptr);
    CHECK(window->GetPosition().x == doctest::Approx(stack.x + 24.0f).epsilon(0.01)); // over its stack, a little in
    CHECK(window->GetPosition().y == doctest::Approx(stack.y + 24.0f).epsilon(0.01));

    // From the keys: the focused stack's panel.
    bed.TabOf(u8"profiler")->RequestFocus();
    CHECK(bed.root->RunCommand(SV(u8"dock.float")));
    bed.Lay();
    CHECK(bed.area->GetFloatCount() == 2u);
    CHECK(bed.SpaceOf(u8"profiler") != main);
}

namespace
{
    // A float's title bar pressed and dragged to `to` (in steps), the window following.
    void DragTitle(Bed& bed, DockFloat& window, core::Float2 to, core::u32 modifiers = 0)
    {
        const Rect at = window.GetScreenBounds();
        const core::Float2 press{at.x + 60.0f, at.y + 10.0f};
        bed.D().InjectMouseDown(press, MouseButton::Left);
        bed.D().InjectMouseMove(core::Float2{press.x + 10.0f, press.y}, modifiers);
        bed.D().InjectMouseMove(to, modifiers);
    }
}

TEST_CASE("dock area: a float dragged by its title follows the pointer and docks whole where it is let go over a target")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{420.0f, 300.0f, 300.0f, 200.0f});
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertBeside(SV(u8"memory"), l.StackOf(SV(u8"profiler")), DockSide::End); }).Ok());
    bed.Lay();
    DockFloat* window = bed.area->FloatOf(floating);
    REQUIRE(window != nullptr);
    const core::Float2 middle{300.0f, 250.0f}; // the central stack's middle
    DragTitle(bed, *window, middle);
    REQUIRE(bed.D().IsDragging());
    CHECK(bed.D().GetDragPreview() == nullptr); // the window is what follows
    CHECK(window->IsBeingDragged());
    CHECK(window->HasClass(SV(u8"dock-dragging")));
    CHECK(window->GetPosition().x == doctest::Approx(middle.x - 60.0f)); // held where it was taken
    CHECK(window->GetPosition().y == doctest::Approx(middle.y - 10.0f));
    Rect preview;
    REQUIRE(bed.area->GetMainSpace()->GetPreviewBounds(preview)); // the target under it answered
    CHECK(preview.width == doctest::Approx(800.0f).epsilon(0.02)); // into the central stack: all of it
    bed.D().InjectMouseUp(middle, MouseButton::Left);
    bed.Lay();
    CHECK(bed.area->GetFloatCount() == 0u);
    const DockNode& central = *bed.controller.GetLayout().FindNode(bed.controller.GetLayout().CentralStack());
    CHECK(central.Panels.Size() == 3u); // scene, then the float's two
    CHECK(bed.area->GetMainSpace()->PageOf(SV(u8"memory")) != nullptr);
}

TEST_CASE("dock area: a float let go in a stack's edge band docks beside it, its own split kept")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{420.0f, 300.0f, 300.0f, 200.0f});
    REQUIRE(bed.controller.Edit([](DockLayout& l) { return l.InsertBeside(SV(u8"memory"), l.StackOf(SV(u8"profiler")), DockSide::Bottom); }).Ok());
    bed.Lay();
    const core::Float2 right{770.0f, 250.0f}; // in the central stack's right band (64), not the surface's (20)
    DragTitle(bed, *bed.area->FloatOf(floating), right);
    Rect preview;
    REQUIRE(bed.area->GetMainSpace()->GetPreviewBounds(preview));
    CHECK(preview.x == doctest::Approx(400.0f).epsilon(0.03)); // its half
    bed.D().InjectMouseUp(right, MouseButton::Left);
    bed.Lay();
    const DockLayout& layout = bed.controller.GetLayout();
    const DockNode& root = *layout.FindNode(layout.FindSurface(layout.MainSurface())->Root);
    REQUIRE(root.Axis == DockAxis::Horizontal);
    REQUIRE(root.Children.Size() == 2u);
    const DockNode& moved = *layout.FindNode(root.Children[1]);
    CHECK(moved.Axis == DockAxis::Vertical); // the float's split, kept
    CHECK(layout.SurfaceOf(layout.StackOf(SV(u8"memory"))) == layout.MainSurface());
}

TEST_CASE("dock area: a float dragged with Shift is only moved; Escape puts it back")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{100.0f, 100.0f, 300.0f, 200.0f});
    DockFloat* window = bed.area->FloatOf(floating);
    const core::Float2 to{400.0f, 300.0f};
    DragTitle(bed, *window, to, KeyModShift);
    CHECK(bed.D().GetDropTarget() == bed.area.Get());
    Rect preview;
    CHECK_FALSE(bed.area->GetMainSpace()->GetPreviewBounds(preview));
    bed.D().InjectMouseUp(to, MouseButton::Left, KeyModShift);
    bed.Lay();
    REQUIRE(bed.area->FloatOf(floating) == window);
    const core::Rectangle bounds = bed.controller.GetLayout().FindSurface(floating)->Bounds;
    CHECK(bounds.x == doctest::Approx(to.x - 60.0f));
    CHECK(bounds.y == doctest::Approx(to.y - 10.0f));
    CHECK_FALSE(window->IsBeingDragged());

    DragTitle(bed, *window, core::Float2{600.0f, 500.0f});
    CHECK(window->GetPosition().x != doctest::Approx(bounds.x));
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    bed.D().InjectMouseUp(core::Float2{600.0f, 500.0f}, MouseButton::Left);
    bed.Lay();
    CHECK(window->GetPosition().x == doctest::Approx(bounds.x));
    CHECK(window->GetPosition().y == doctest::Approx(bounds.y));
    CHECK(bed.controller.GetLayout().FindSurface(floating) != nullptr); // nothing docked
}

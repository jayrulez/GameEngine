// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - floating surfaces in one window (D5 of Specs/gui-lab-evolution-3.md): a DockArea
// shows the main surface and each floating one as a window over it, holding a dock space of its
// own; the windows follow the layout (made, placed, titled, ended) and the layout follows what the
// user does to them (moved or resized, closed); a float is docked into like the main surface.
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
        void Lay() { root->UpdateLayout(); }
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

TEST_CASE("dock area: a float moved or resized by the user writes its bounds back, kept where its title can be reached")
{
    Bed bed;
    const DockSurfaceId floating = bed.Float(u8"profiler", core::Rectangle{100.0f, 80.0f, 300.0f, 200.0f});
    DockFloat* window = bed.area->FloatOf(floating);
    REQUIRE(window != nullptr);
    const core::Float2 at{window->GetScreenBounds().x + 150.0f, window->GetScreenBounds().y + 10.0f};
    bed.D().InjectMouseDown(at, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{at.x + 40.0f, at.y + 30.0f});
    bed.D().InjectMouseUp(core::Float2{at.x + 40.0f, at.y + 30.0f}, MouseButton::Left);
    bed.Lay();
    const core::Rectangle moved = bed.controller.GetLayout().FindSurface(floating)->Bounds;
    CHECK(moved.x == doctest::Approx(140.0f));
    CHECK(moved.y == doctest::Approx(110.0f));
    CHECK(moved.width == doctest::Approx(300.0f));

    // Far off the area: its title bar kept in reach, and that is what is noted.
    const core::Float2 from{window->GetScreenBounds().x + 150.0f, window->GetScreenBounds().y + 10.0f};
    bed.D().InjectMouseDown(from, MouseButton::Left);
    bed.D().InjectMouseMove(core::Float2{from.x + 2000.0f, from.y - 500.0f});
    bed.D().InjectMouseUp(core::Float2{from.x + 2000.0f, from.y - 500.0f}, MouseButton::Left);
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

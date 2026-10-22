// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the graph view (T6 of Specs/gui-lab-evolution-3.md): a source's nodes as widgets made
// while in view and reused as it moves (a node never seen made once to learn its size); a click or a
// box selecting, Shift or Ctrl adding or taking away; a drag moving the selected nodes as one gesture
// through the source, on the grid where it snaps, Escape cancelling it; read only moving nothing; a
// double click the application's; the wheel zooming about the pointer within limits and a middle
// drag panning; framing every node.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;
import experimental.gui.toolkit;

using namespace experimental::gui;
namespace core = foundation::core;
using core::f32;
using core::usize;

namespace
{
    GraphPinInfo Pin(const char8_t* name, GraphPinDirection direction)
    {
        GraphPinInfo pin;
        pin.Name = core::String(name);
        pin.Direction = direction;
        return pin;
    }
    GraphNodeInfo Node(const char8_t* title, core::Float2 at)
    {
        GraphNodeInfo node;
        node.Title = core::String(title);
        node.Position = at;
        node.Pins.PushBack(Pin(u8"in", GraphPinDirection::Input));
        node.Pins.PushBack(Pin(u8"out", GraphPinDirection::Output));
        return node;
    }

    // A 600 by 400 view at the root's top-left, the graph's origin at its top-left at the zoom of 1:
    // A at (0, 0) and B at (300, 0) in view, C at (1200, 800) out of it.
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<GraphView> view = core::MakeRef<GraphView>(core::DefaultAllocator());
        GraphSet graph;
        core::Array<GraphChange> changes{core::DefaultAllocator()};
        GraphId a = 0, b = 0, c = 0;

        Bed()
        {
            a = graph.AddNode(Node(u8"A", core::Float2{0.0f, 0.0f}));
            b = graph.AddNode(Node(u8"B", core::Float2{300.0f, 0.0f}));
            c = graph.AddNode(Node(u8"C", core::Float2{1200.0f, 800.0f}));
            graph.SetOnChange([this](const GraphChange& change) { changes.PushBack(change); });
            root->SetSize(core::Float2{800.0f, 600.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            view->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(600.0f);
                s.Height = LayoutLength::Px(400.0f);
            });
            root->AddChild(view.Get());
            view->SetSource(&graph);
            (void)root->UpdateLayout();
        }
        ~Bed() { view->SetSource(nullptr); }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        [[nodiscard]] core::Float2 Middle(GraphId node) const
        {
            const Rect r = view->NodeRect(node);
            return core::Float2{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
        }
        [[nodiscard]] core::Float2 Where(GraphId node) const { return graph.GetNode(node)->Position; }
        void Press(core::Float2 at, core::u32 modifiers = 0)
        {
            D().InjectMouseMove(at, modifiers);
            D().InjectMouseDown(at, MouseButton::Left, modifiers);
        }
        void Drag(core::Float2 from, core::Float2 to, core::u32 modifiers = 0)
        {
            Press(from, modifiers);
            D().InjectMouseMove(core::Float2{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f}, modifiers);
            D().InjectMouseMove(to, modifiers);
            D().InjectMouseUp(to, MouseButton::Left, modifiers);
        }
        void Click(core::Float2 at, core::u32 modifiers = 0)
        {
            Press(at, modifiers);
            D().InjectMouseUp(at, MouseButton::Left, modifiers);
        }
        void Pause() // a later press is not the second of a double
        {
            D().InjectMouseMove(core::Float2{790.0f, 590.0f});
            D().InjectMouseDown(core::Float2{790.0f, 590.0f}, MouseButton::Left);
            D().InjectMouseUp(core::Float2{790.0f, 590.0f}, MouseButton::Left);
        }
        [[nodiscard]] usize Selected() const { return view->GetSelectedNodes().Size(); }
    };
}

TEST_CASE("GraphView: nodes are widgets made while in view, reused as it pans, and zoomed with it")
{
    Bed bed;
    CHECK(bed.view->MadeNodeCount() == 2);
    REQUIRE(bed.view->GetNodeWidget(bed.a) != nullptr);
    REQUIRE(bed.view->GetNodeWidget(bed.b) != nullptr);
    CHECK(bed.view->GetNodeWidget(bed.c) == nullptr);
    CHECK(bed.view->GetNodeWidget(bed.a)->GetTitleLabel().GetText() == core::StringView(u8"A"));
    CHECK(bed.view->GetNodeWidget(bed.a)->PinRowCount() == 2);
    // C was made once to learn its size, then went back to the pool.
    CHECK(bed.view->PooledNodeCount() == 1);
    CHECK(bed.view->NodeSize(bed.c).x == doctest::Approx(bed.view->NodeSize(bed.a).x));
    CHECK(bed.view->NodeSize(bed.c).y == doctest::Approx(bed.view->NodeSize(bed.a).y));

    // Panned to C: its widget is one reused, and A's and B's go back to the pool.
    bed.view->SetPan(core::Float2{1100.0f, 700.0f});
    (void)bed.root->UpdateLayout();
    CHECK(bed.view->MadeNodeCount() == 1);
    REQUIRE(bed.view->GetNodeWidget(bed.c) != nullptr);
    CHECK(bed.view->GetNodeWidget(bed.c)->GetTitleLabel().GetText() == core::StringView(u8"C"));
    CHECK(bed.view->GetNodeWidget(bed.a) == nullptr);
    CHECK(bed.view->MadeNodeCount() + bed.view->PooledNodeCount() == 3);
    const Rect atC = bed.view->NodeRect(bed.c);
    CHECK(atC.x == doctest::Approx(100.0f));
    CHECK(atC.y == doctest::Approx(100.0f));

    // Zoomed out, a widget is drawn at the zoom, its rect following.
    bed.view->SetPan(core::Float2{0.0f, 0.0f});
    bed.view->SetZoom(0.5f);
    (void)bed.root->UpdateLayout();
    REQUIRE(bed.view->GetNodeWidget(bed.b) != nullptr);
    CHECK(bed.view->GetNodeWidget(bed.b)->GetScale().x == doctest::Approx(0.5f));
    CHECK(bed.view->NodeRect(bed.b).x == doctest::Approx(150.0f));
    CHECK(bed.view->NodeRect(bed.b).width == doctest::Approx(bed.view->NodeSize(bed.b).x * 0.5f));
    // A pin is drawn on its node's edge: an input at the start, an output at the end.
    GraphNodeWidget& node = *bed.view->GetNodeWidget(bed.b);
    core::Float2 in{}, out{};
    REQUIRE(node.PinPoint(node.PinRowAt(0).GetPinId(), in));
    REQUIRE(node.PinPoint(node.PinRowAt(1).GetPinId(), out));
    CHECK(in.x == doctest::Approx(0.0f));
    CHECK(out.x == doctest::Approx(node.GetSize().x));
    CHECK(in.y > node.GetHeader().GetSize().y);
}

TEST_CASE("GraphView: a click or a box selects; Shift or Ctrl adds or takes away")
{
    Bed bed;
    int told = 0;
    bed.view->SetOnSelectionChanged([&told]() { ++told; });
    bed.Click(bed.Middle(bed.a));
    REQUIRE(bed.Selected() == 1);
    CHECK(bed.view->IsNodeSelected(bed.a));
    CHECK(bed.view->GetNodeWidget(bed.a)->HasClass(core::StringView(u8"selected")));
    CHECK(told == 1);
    bed.Pause();
    bed.Click(core::Float2{500.0f, 350.0f});
    CHECK(bed.Selected() == 0); // a click on nothing selects nothing
    bed.Pause();
    bed.Click(bed.Middle(bed.a));
    bed.Pause();
    bed.Click(bed.Middle(bed.b), KeyModShift);
    CHECK(bed.Selected() == 2);
    bed.Pause();
    bed.Click(bed.Middle(bed.a), KeyModShift);
    bed.Click(bed.Middle(bed.a), KeyModCtrl); // a second press soon after: still a toggle
    CHECK(bed.Selected() == 2);
    bed.Pause();
    bed.Click(bed.Middle(bed.b), KeyModCtrl);
    REQUIRE(bed.Selected() == 1);
    CHECK(bed.view->IsNodeSelected(bed.a));
    CHECK_FALSE(bed.view->GetNodeWidget(bed.b)->HasClass(core::StringView(u8"selected")));

    // A box from nothing over both; with Shift it adds to what is selected.
    bed.Pause();
    bed.Drag(core::Float2{590.0f, 390.0f}, core::Float2{10.0f, 10.0f});
    CHECK(bed.Selected() == 2);
    bed.Pause();
    bed.Click(bed.Middle(bed.b));
    REQUIRE(bed.Selected() == 1);
    const Rect atA = bed.view->NodeRect(bed.a);
    bed.Drag(core::Float2{atA.Right() + 20.0f, 390.0f}, core::Float2{atA.x + 10.0f, atA.y + 10.0f}, KeyModShift);
    CHECK(bed.Selected() == 2);

    // A node the source takes away (an undo) leaves the selection.
    REQUIRE(bed.graph.DeleteNodes(core::Span<const GraphId>(&bed.a, 1)));
    REQUIRE(bed.Selected() == 1);
    CHECK(bed.view->IsNodeSelected(bed.b));
}

TEST_CASE("GraphView: a drag moves the selected nodes as one gesture, on the grid, and Escape cancels it")
{
    Bed bed;
    bed.Click(bed.Middle(bed.a));
    bed.Pause();
    bed.Click(bed.Middle(bed.b), KeyModShift);
    bed.Pause();
    // 37 by 21 on a grid of 16: the pressed node lands at (32, 16), the other keeps its place to it.
    const core::Float2 from = bed.Middle(bed.a);
    bed.Drag(from, core::Float2{from.x + 37.0f, from.y + 21.0f});
    CHECK(bed.Where(bed.a).x == doctest::Approx(32.0f));
    CHECK(bed.Where(bed.a).y == doctest::Approx(16.0f));
    CHECK(bed.Where(bed.b).x == doctest::Approx(332.0f));
    CHECK(bed.Where(bed.b).y == doctest::Approx(16.0f));
    REQUIRE(bed.changes.Size() == 1); // one change for the whole drag
    CHECK(bed.Selected() == 2);
    bed.graph.Apply(bed.changes[0], true);
    CHECK(bed.Where(bed.a).x == doctest::Approx(0.0f));

    // Not snapping, it moves as the pointer did (over the zoom).
    bed.Pause();
    bed.view->SetSnapToGrid(false);
    bed.view->SetZoom(0.5f);
    (void)bed.root->UpdateLayout();
    bed.Click(bed.Middle(bed.a));
    bed.Pause();
    const core::Float2 at = bed.Middle(bed.a);
    bed.Drag(at, core::Float2{at.x + 10.0f, at.y + 5.0f});
    CHECK(bed.Where(bed.a).x == doctest::Approx(20.0f));
    CHECK(bed.Where(bed.a).y == doctest::Approx(10.0f));
    CHECK(bed.Where(bed.b).x == doctest::Approx(300.0f)); // not selected
    REQUIRE(bed.changes.Size() == 2);

    // Escape mid-drag puts it back, and nothing is told.
    bed.Pause();
    const core::Float2 again = bed.Middle(bed.a);
    bed.Press(again);
    bed.D().InjectMouseMove(core::Float2{again.x + 40.0f, again.y + 40.0f});
    CHECK(bed.Where(bed.a).x == doctest::Approx(100.0f));
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK(bed.Where(bed.a).x == doctest::Approx(20.0f));
    bed.D().InjectMouseUp(core::Float2{again.x + 40.0f, again.y + 40.0f}, MouseButton::Left);
    CHECK(bed.Where(bed.a).x == doctest::Approx(20.0f));
    CHECK(bed.changes.Size() == 2);

    // Read only, a drag selects and moves nothing.
    bed.Pause();
    bed.view->SetReadOnly(true);
    const core::Float2 still = bed.Middle(bed.a);
    bed.Drag(still, core::Float2{still.x + 50.0f, still.y});
    CHECK(bed.Where(bed.a).x == doctest::Approx(20.0f));
    CHECK(bed.changes.Size() == 2);
    CHECK(bed.view->IsNodeSelected(bed.a));
}

TEST_CASE("GraphView: the wheel zooms about the pointer within its limits; a middle drag pans")
{
    Bed bed;
    const core::Float2 at{250.0f, 150.0f};
    const core::Float2 under = bed.view->ViewToGraph(at);
    bed.D().InjectMouseMove(at);
    bed.D().InjectMouseWheel(at, core::Float2{0.0f, 1.0f});
    CHECK(bed.view->GetZoom() == doctest::Approx(1.15f));
    CHECK(bed.view->ViewToGraph(at).x == doctest::Approx(under.x));
    CHECK(bed.view->ViewToGraph(at).y == doctest::Approx(under.y));
    for (int i = 0; i < 20; ++i)
        bed.D().InjectMouseWheel(at, core::Float2{0.0f, 1.0f});
    CHECK(bed.view->GetZoom() == doctest::Approx(bed.view->ZoomMax()));
    for (int i = 0; i < 40; ++i)
        bed.D().InjectMouseWheel(at, core::Float2{0.0f, -1.0f});
    CHECK(bed.view->GetZoom() == doctest::Approx(bed.view->ZoomMin()));

    bed.view->SetZoom(0.5f);
    const core::Float2 pan = bed.view->GetPan();
    bed.D().InjectMouseDown(at, MouseButton::Middle);
    bed.D().InjectMouseMove(core::Float2{at.x + 50.0f, at.y + 30.0f});
    bed.D().InjectMouseUp(core::Float2{at.x + 50.0f, at.y + 30.0f}, MouseButton::Middle);
    CHECK(bed.view->GetPan().x == doctest::Approx(pan.x - 100.0f));
    CHECK(bed.view->GetPan().y == doctest::Approx(pan.y - 60.0f));
    CHECK(bed.changes.IsEmpty());
}

TEST_CASE("GraphView: a double click on a node is the application's; framing fits the nodes in view")
{
    Bed bed;
    GraphId opened = 0;
    bed.view->SetOnNodeDoubleClick([&opened](GraphId node) { opened = node; });
    bed.Click(bed.Middle(bed.b));
    bed.Click(bed.Middle(bed.b));
    CHECK(opened == bed.b);
    CHECK(bed.changes.IsEmpty());

    bed.view->FrameAll();
    (void)bed.root->UpdateLayout();
    const Rect box = bed.view->GetContentBounds();
    const GraphId all[] = {bed.a, bed.b, bed.c};
    for (const GraphId node : all)
        CHECK(box.Contains(bed.view->NodeRect(node)));
    CHECK(bed.view->MadeNodeCount() == 3);

    // The selection alone, at no more than the zoom of 1.
    bed.view->SetSelectedNodes(core::Span<const GraphId>(&bed.c, 1));
    bed.view->FrameSelection();
    CHECK(bed.view->GetZoom() == doctest::Approx(1.0f));
    CHECK(box.Contains(bed.view->NodeRect(bed.c)));
    CHECK_FALSE(box.Intersects(bed.view->NodeRect(bed.a)));
}

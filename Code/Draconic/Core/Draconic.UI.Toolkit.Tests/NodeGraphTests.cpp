// Verbatim port of Sedulous.UI.Tests/src/NodeGraphTests.bf (SedulousEngine).
// Data-model + add/remove/connection/selection/transform/auto-size/custom-validator coverage for the
// toolkit NodeGraphCanvas. Beef `scope NodeGraphNode()` (heap, owned) -> the canvas stores nodes as
// UniquePtr<NodeGraphNode>, so tests build a node via MakeUnique, keep the raw pointer for later
// assertions (the canvas keeps a stable address), and Move the owner into AddNode.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    // Beef byte `Color(r,g,b,a)` literal -> float foundation::Color.
    [[nodiscard]] foundation::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255)
    {
        return foundation::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    // Build an owned node with an optional title.
    [[nodiscard]] UniquePtr<NodeGraphNode> MakeNode(StringView title = StringView{})
    {
        auto node = MakeUnique<NodeGraphNode>(DefaultAllocator());
        if (title.Size() > 0)
        {
            node->Title = String(title);
        }
        return node;
    }

    [[nodiscard]] NodeGraphPort MakePort(PortDirection dir)
    {
        NodeGraphPort p;
        p.Direction = dir;
        return p;
    }

    [[nodiscard]] NodeGraphConnection Conn(i32 sn, i32 sp, i32 dn, i32 dp)
    {
        NodeGraphConnection c;
        c.SourceNodeIndex = sn;
        c.SourcePortIndex = sp;
        c.DestNodeIndex = dn;
        c.DestPortIndex = dp;
        return c;
    }
}

// === Data Model ===

TEST_CASE("nodegraph: NodeGraphNode_Defaults")
{
    NodeGraphNode node;
    CHECK(node.UserHandle == 0);
    CHECK(node.IsMovable == true);
    CHECK(node.IsDeletable == true);
    CHECK(node.IsSelected == false);
    CHECK(node.InputPorts.Size() == 0);
    CHECK(node.OutputPorts.Size() == 0);
    CHECK(node.Size.x >= 120); // NodeMinWidth default.
}

TEST_CASE("nodegraph: NodeGraphPortType_Untyped")
{
    const NodeGraphPortType untyped = NodeGraphPortType::Untyped();
    CHECK(untyped.TypeId == 0);
}

// === Add / Remove Nodes ===

TEST_CASE("nodegraph: AddNode_ReturnsIndex")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    const i32 idx = canvas->AddNode(MakeNode(u8"Test"));
    CHECK(idx == 0);
    CHECK(canvas->NodeCount() == 1);
}

TEST_CASE("nodegraph: AddMultipleNodes")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    CHECK(canvas->AddNode(MakeNode(u8"A")) == 0);
    CHECK(canvas->AddNode(MakeNode(u8"B")) == 1);
    CHECK(canvas->AddNode(MakeNode(u8"C")) == 2);
    CHECK(canvas->NodeCount() == 3);
}

TEST_CASE("nodegraph: RemoveNode_DecreasesCount")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    canvas->AddNode(MakeNode(u8"A"));
    canvas->AddNode(MakeNode(u8"B"));

    canvas->RemoveNode(0);
    CHECK(canvas->NodeCount() == 1);
    CHECK(canvas->GetNode(0)->Title == StringView(u8"B"));
}

TEST_CASE("nodegraph: RemoveNode_RemovesConnections")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode(u8"A");
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode(u8"B");
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(canvas->ConnectionCount() == 1);

    canvas->RemoveNode(0);
    CHECK(canvas->ConnectionCount() == 0);
}

TEST_CASE("nodegraph: RemoveNode_RemapsConnectionIndices")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());

    auto n0 = MakeNode(u8"A");
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode(u8"B");
    n1->OutputPorts.PushBack(MakePort(PortDirection::Output));
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    auto n2 = MakeNode(u8"C");
    n2->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n2));

    // Connection: B(1) -> C(2).
    canvas->AddConnection(Conn(1, 0, 2, 0));

    // Remove A(0) - B becomes 0, C becomes 1.
    canvas->RemoveNode(0);
    CHECK(canvas->ConnectionCount() == 1);
    const NodeGraphConnection conn = canvas->GetConnection(0);
    CHECK(conn.SourceNodeIndex == 0); // was 1, now 0
    CHECK(conn.DestNodeIndex == 1);   // was 2, now 1
}

// === Connections ===

TEST_CASE("nodegraph: AddConnection_Valid")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    const i32 idx = canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(idx == 0);
    CHECK(canvas->ConnectionCount() == 1);
}

TEST_CASE("nodegraph: AddConnection_RejectsSelfConnection")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    n0->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n0));

    const i32 idx = canvas->AddConnection(Conn(0, 0, 0, 0));
    CHECK(idx == -1);
    CHECK(canvas->ConnectionCount() == 0);
}

TEST_CASE("nodegraph: AddConnection_RejectsDuplicate")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    canvas->AddConnection(Conn(0, 0, 1, 0));
    const i32 dup = canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(dup == -1);
    CHECK(canvas->ConnectionCount() == 1);
}

TEST_CASE("nodegraph: AddConnection_RejectsInvalidIndices")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    // Dest node doesn't exist.
    const i32 idx = canvas->AddConnection(Conn(0, 0, 5, 0));
    CHECK(idx == -1);
}

TEST_CASE("nodegraph: AddConnection_TypeValidation_SameType")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());

    const NodeGraphPortType audioType(1, Rgb(100, 200, 255, 255));
    const NodeGraphPortType floatType(2, Rgb(100, 255, 100, 255));

    auto n0 = MakeNode();
    NodeGraphPort out0 = MakePort(PortDirection::Output);
    out0.PortType = audioType;
    n0->OutputPorts.PushBack(out0);
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    NodeGraphPort in1 = MakePort(PortDirection::Input);
    in1.PortType = floatType; // Different type.
    n1->InputPorts.PushBack(in1);
    canvas->AddNode(Move(n1));

    // Mismatched types should be rejected.
    const i32 idx = canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(idx == -1);
}

TEST_CASE("nodegraph: AddConnection_TypeValidation_UntypedConnectsToAnything")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());

    const NodeGraphPortType audioType(1, Rgb(100, 200, 255, 255));

    auto n0 = MakeNode();
    NodeGraphPort out0 = MakePort(PortDirection::Output);
    out0.PortType = NodeGraphPortType::Untyped();
    n0->OutputPorts.PushBack(out0);
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    NodeGraphPort in1 = MakePort(PortDirection::Input);
    in1.PortType = audioType;
    n1->InputPorts.PushBack(in1);
    canvas->AddNode(Move(n1));

    const i32 idx = canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(idx >= 0);
}

TEST_CASE("nodegraph: RemoveConnection")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    canvas->AddConnection(Conn(0, 0, 1, 0));
    canvas->RemoveConnection(0);
    CHECK(canvas->ConnectionCount() == 0);
}

// === Selection ===

TEST_CASE("nodegraph: SelectNode")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0u = MakeNode(u8"A");
    NodeGraphNode* n0 = n0u.Get();
    canvas->AddNode(Move(n0u));
    auto n1u = MakeNode(u8"B");
    NodeGraphNode* n1 = n1u.Get();
    canvas->AddNode(Move(n1u));

    canvas->SelectNode(0);
    CHECK(n0->IsSelected);
    CHECK(!n1->IsSelected);

    // Selecting another clears previous.
    canvas->SelectNode(1);
    CHECK(!n0->IsSelected);
    CHECK(n1->IsSelected);
}

TEST_CASE("nodegraph: SelectNode_AddToSelection")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0u = MakeNode(u8"A");
    NodeGraphNode* n0 = n0u.Get();
    canvas->AddNode(Move(n0u));
    auto n1u = MakeNode(u8"B");
    NodeGraphNode* n1 = n1u.Get();
    canvas->AddNode(Move(n1u));

    canvas->SelectNode(0);
    canvas->SelectNode(1, /*addToSelection*/ true);
    CHECK(n0->IsSelected);
    CHECK(n1->IsSelected);
}

TEST_CASE("nodegraph: ClearSelection")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0u = MakeNode(u8"A");
    NodeGraphNode* n0 = n0u.Get();
    canvas->AddNode(Move(n0u));

    canvas->SelectNode(0);
    CHECK(n0->IsSelected);

    canvas->ClearSelection();
    CHECK(!n0->IsSelected);
}

TEST_CASE("nodegraph: GetSelectedNodes")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    canvas->AddNode(MakeNode(u8"A"));
    canvas->AddNode(MakeNode(u8"B"));
    canvas->AddNode(MakeNode(u8"C"));

    canvas->SelectNode(0);
    canvas->SelectNode(2, /*addToSelection*/ true);

    Array<i32> selected;
    canvas->GetSelectedNodes(selected);
    CHECK(selected.Size() == 2);
    bool has0 = false, has2 = false;
    for (usize i = 0; i < selected.Size(); ++i)
    {
        if (selected[i] == 0)
        {
            has0 = true;
        }
        if (selected[i] == 2)
        {
            has2 = true;
        }
    }
    CHECK(has0);
    CHECK(has2);
}

// === Clear ===

TEST_CASE("nodegraph: Clear_RemovesEverything")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto n0 = MakeNode();
    n0->OutputPorts.PushBack(MakePort(PortDirection::Output));
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    n1->InputPorts.PushBack(MakePort(PortDirection::Input));
    canvas->AddNode(Move(n1));

    canvas->AddConnection(Conn(0, 0, 1, 0));

    canvas->Clear();
    CHECK(canvas->NodeCount() == 0);
    CHECK(canvas->ConnectionCount() == 0);
}

// === Coordinate Transforms ===

TEST_CASE("nodegraph: ScreenToCanvas_Identity")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    const Float2 result = canvas->ScreenToCanvas(Float2{100, 200});
    CHECK(Abs(result.x - 100) < 0.01f);
    CHECK(Abs(result.y - 200) < 0.01f);
}

TEST_CASE("nodegraph: CanvasToScreen_Identity")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    const Float2 result = canvas->CanvasToScreen(Float2{100, 200});
    CHECK(Abs(result.x - 100) < 0.01f);
    CHECK(Abs(result.y - 200) < 0.01f);
}

TEST_CASE("nodegraph: ScreenToCanvas_Roundtrip")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    const Float2 original{150, 250};
    const Float2 screen = canvas->CanvasToScreen(original);
    const Float2 back = canvas->ScreenToCanvas(screen);
    CHECK(Abs(back.x - original.x) < 0.01f);
    CHECK(Abs(back.y - original.y) < 0.01f);
}

// === Auto-sizing ===

TEST_CASE("nodegraph: AutoSize_ExpandsForPorts")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    auto node = MakeNode(u8"Multi-port");
    node->Size = Float2{160, 40}; // Intentionally small.

    for (i32 i = 0; i < 5; i++)
    {
        NodeGraphPort port = MakePort(PortDirection::Input);
        port.Label = String(u8"In");
        node->InputPorts.PushBack(port);
    }

    NodeGraphNode* raw = node.Get();
    canvas->AddNode(Move(node)); // AddNode calls AutoSizeNode.
    CHECK(raw->Size.y > 40);     // Should have expanded for 5 ports.
}

// === Custom Connection Validator ===

TEST_CASE("nodegraph: CustomValidator_AllowsAll")
{
    auto canvas = MakeRef<NodeGraphCanvas>(DefaultAllocator());
    canvas->ConnectionValidator = [](NodeGraphPortType, NodeGraphPortType)
    { return true; }; // Allow everything.

    const NodeGraphPortType typeA(1, Rgb(255, 0, 0, 255));
    const NodeGraphPortType typeB(2, Rgb(0, 255, 0, 255));

    auto n0 = MakeNode();
    NodeGraphPort out0 = MakePort(PortDirection::Output);
    out0.PortType = typeA;
    n0->OutputPorts.PushBack(out0);
    canvas->AddNode(Move(n0));

    auto n1 = MakeNode();
    NodeGraphPort in1 = MakePort(PortDirection::Input);
    in1.PortType = typeB;
    n1->InputPorts.PushBack(in1);
    canvas->AddNode(Move(n1));

    // Different types but custom validator allows it.
    const i32 idx = canvas->AddConnection(Conn(0, 0, 1, 0));
    CHECK(idx >= 0);
}

// === State-machine mode (ConnectionStyle::StraightNodeToNode) ===

TEST_CASE("nodegraph: EdgeStyle defaults to BezierPorts; highlight defaults off")
{
    NodeGraphCanvas canvas;
    CHECK(canvas.EdgeStyle == ConnectionStyle::BezierPorts);
    NodeGraphNode node;
    CHECK(node.IsHighlighted == false);
}

TEST_CASE("nodegraph: straight mode connects PORT-LESS nodes and allows parallel duplicates")
{
    NodeGraphCanvas canvas;
    canvas.EdgeStyle = ConnectionStyle::StraightNodeToNode;
    const i32 a = canvas.AddNode(MakeNode(u8"Idle"));
    const i32 b = canvas.AddNode(MakeNode(u8"Run"));

    // Port-less edges connect (BezierPorts would refuse: no ports to validate).
    CHECK(canvas.AddConnection(Conn(a, 0, b, 0)) >= 0);
    // A second A->B transition is LEGAL (state machines draw them in offset lanes).
    CHECK(canvas.AddConnection(Conn(a, 0, b, 0)) >= 0);
    // The reverse direction too.
    CHECK(canvas.AddConnection(Conn(b, 0, a, 0)) >= 0);
    CHECK(canvas.ConnectionCount() == 3);

    // Self edges still refuse.
    CHECK(canvas.AddConnection(Conn(a, 0, a, 0)) == -1);
    // Out-of-bounds nodes still refuse.
    CHECK(canvas.AddConnection(Conn(a, 0, 99, 0)) == -1);
}

TEST_CASE("nodegraph: bezier mode still refuses port-less connections (regression guard)")
{
    NodeGraphCanvas canvas; // default BezierPorts
    const i32 a = canvas.AddNode(MakeNode(u8"A"));
    const i32 b = canvas.AddNode(MakeNode(u8"B"));
    CHECK(canvas.AddConnection(Conn(a, 0, b, 0)) == -1); // no ports -> invalid
}

TEST_CASE("nodegraph: StartLinkFrom ignores invalid indices and ReadOnly canvases")
{
    NodeGraphCanvas canvas;
    canvas.EdgeStyle = ConnectionStyle::StraightNodeToNode;
    const i32 a = canvas.AddNode(MakeNode(u8"Idle"));
    bool fired = false;
    canvas.OnNodeLinkRequested.Add([&fired](i32, i32) { fired = true; });

    canvas.StartLinkFrom(-1); // out of range: no-op
    canvas.StartLinkFrom(5);
    canvas.ReadOnly = true;
    canvas.StartLinkFrom(a); // read-only: no-op
    CHECK_FALSE(fired);      // no gesture ever started, so nothing can fire
}

// Draconic GUI - Node draw tests: the tree renders through the DrawContext/VG seam.
// GPU-free (VGContext tessellates into a CPU vertex batch), so we assert geometry is
// produced and that visibility/alpha gate drawing.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

namespace
{
    foundation::RefPtr<Node> MakeNode() { return foundation::MakeRef<Node>(foundation::DefaultAllocator()); }

    foundation::RefPtr<Node> MakePanel(foundation::Float2 size, foundation::Color color)
    {
        auto node = MakeNode();
        node->SetSize(size);
        node->SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), color));
        return node;
    }
}

TEST_CASE("node-draw: background renders through VG")
{
    auto panel = MakePanel(foundation::Float2{100.0f, 50.0f}, foundation::Color::Blue);
    vg::VGContext ctx;
    DrawContext dc{ctx};
    panel->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("node-draw: empty node with no background draws nothing")
{
    auto node = MakeNode();
    node->SetSize(foundation::Float2{100.0f, 50.0f});
    vg::VGContext ctx;
    DrawContext dc{ctx};
    node->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

TEST_CASE("node-draw: invisible node draws nothing")
{
    auto panel = MakePanel(foundation::Float2{100.0f, 50.0f}, foundation::Color::Blue);
    panel->SetVisible(false);
    vg::VGContext ctx;
    DrawContext dc{ctx};
    panel->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

TEST_CASE("node-draw: fully transparent node draws nothing")
{
    auto panel = MakePanel(foundation::Float2{100.0f, 50.0f}, foundation::Color::Blue);
    panel->SetAlpha(0.0f);
    vg::VGContext ctx;
    DrawContext dc{ctx};
    panel->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

TEST_CASE("node-draw: nested tree renders parent and children")
{
    auto root = MakePanel(foundation::Float2{200.0f, 200.0f}, foundation::Color::White);
    auto child = MakePanel(foundation::Float2{50.0f, 50.0f}, foundation::Color::Red);
    child->SetPosition(foundation::Float2{20.0f, 20.0f});
    root->AddChild(child.Get());

    vg::VGContext ctx;
    DrawContext dc{ctx};
    root->Draw(dc);
    // Both the root background and the child background contribute geometry.
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("node-draw: hidden child is skipped while parent still draws")
{
    auto root = MakePanel(foundation::Float2{100.0f, 100.0f}, foundation::Color::White);
    auto child = MakePanel(foundation::Float2{40.0f, 40.0f}, foundation::Color::Red);
    root->AddChild(child.Get());

    vg::VGContext ctxBoth;
    DrawContext dcBoth{ctxBoth};
    root->Draw(dcBoth);
    const foundation::usize withChild = ctxBoth.GetBatch().vertices.Size();

    child->SetVisible(false);
    vg::VGContext ctxRootOnly;
    DrawContext dcRootOnly{ctxRootOnly};
    root->Draw(dcRootOnly);
    const foundation::usize withoutChild = ctxRootOnly.GetBatch().vertices.Size();

    CHECK(withoutChild > 0);
    CHECK(withoutChild < withChild); // the hidden child contributed no geometry
}

TEST_CASE("node-draw: clip children does not crash and still draws")
{
    auto root = MakePanel(foundation::Float2{100.0f, 100.0f}, foundation::Color::White);
    root->SetClipChildren(true);
    auto child = MakePanel(foundation::Float2{200.0f, 200.0f}, foundation::Color::Red); // overflows
    root->AddChild(child.Get());

    vg::VGContext ctx;
    DrawContext dc{ctx};
    root->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("node-draw: foreground draws over children")
{
    auto node = MakePanel(foundation::Float2{100.0f, 100.0f}, foundation::Color::White);
    node->SetForeground(foundation::MakeRef<BorderDrawable>(foundation::DefaultAllocator(),
                                                      foundation::Color{0.0f, 0.0f, 0.0f, 1.0f}, 2.0f));
    vg::VGContext ctx;
    DrawContext dc{ctx};
    node->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

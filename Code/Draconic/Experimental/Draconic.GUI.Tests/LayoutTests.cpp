// Draconic GUI - GridLayout + RelativeLayout tests: children are positioned by the layout
// rules from the padding-inset content box, and re-laid-out on size/child changes.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }

    foundation::RefPtr<UIWidget> Cell(float w, float h)
    {
        auto n = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
        n->SetSize(foundation::Float2{w, h});
        return n;
    }
}

TEST_CASE("grid: flows children into columns and wraps rows")
{
    auto grid = Make<GridLayout>();
    grid->SetSize(foundation::Float2{100.0f, 100.0f}); // no padding -> content 100x100
    grid->SetColumns(2);
    grid->SetSpacing(0.0f, 0.0f); // cellW = 50

    auto a = Cell(20.0f, 10.0f);
    auto b = Cell(20.0f, 30.0f);
    auto c = Cell(20.0f, 10.0f);
    grid->AddChild(a.Get());
    grid->AddChild(b.Get());
    grid->AddChild(c.Get()); // wraps to row 2

    CHECK(grid->CellWidth() == doctest::Approx(50.0f));
    // Row 0: a at col 0 (x=0), b at col 1 (x=50); row height = max(10,30) = 30.
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(50.0f));
    CHECK(b->GetPosition().y == doctest::Approx(0.0f));
    // Row 1: c at col 0, y advanced by row-0 height (30).
    CHECK(c->GetPosition().x == doctest::Approx(0.0f));
    CHECK(c->GetPosition().y == doctest::Approx(30.0f));
}

TEST_CASE("grid: spacing splits the content width and offsets columns")
{
    auto grid = Make<GridLayout>();
    grid->SetSize(foundation::Float2{100.0f, 100.0f});
    grid->SetColumns(2);
    grid->SetSpacing(10.0f, 4.0f); // cellW = (100 - 10) / 2 = 45

    auto a = Cell(10.0f, 10.0f);
    auto b = Cell(10.0f, 10.0f);
    grid->AddChild(a.Get());
    grid->AddChild(b.Get());

    CHECK(grid->CellWidth() == doctest::Approx(45.0f));
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(55.0f)); // 45 + 10 spacing
}

TEST_CASE("grid: respects padding and skips hidden children")
{
    auto grid = Make<GridLayout>();
    grid->SetSize(foundation::Float2{120.0f, 120.0f});
    grid->SetPadding(Thickness{10.0f}); // content origin (10,10), width 100
    grid->SetColumns(2);

    auto a = Cell(10.0f, 10.0f);
    auto hidden = Cell(10.0f, 10.0f);
    hidden->SetVisible(false);
    auto b = Cell(10.0f, 10.0f);
    grid->AddChild(a.Get());
    grid->AddChild(hidden.Get());
    grid->AddChild(b.Get());

    // 'hidden' is skipped, so b takes the second cell of row 0.
    // content: origin (10,10), width 100; cellW = 100/2 = 50.
    CHECK(a->GetPosition().x == doctest::Approx(10.0f));
    CHECK(a->GetPosition().y == doctest::Approx(10.0f));
    CHECK(b->GetPosition().x == doctest::Approx(60.0f)); // 10 + cellW(50) + spacing(0)
    CHECK(b->GetPosition().y == doctest::Approx(10.0f));
}

TEST_CASE("grid: columns clamp to at least one")
{
    auto grid = Make<GridLayout>();
    grid->SetSize(foundation::Float2{60.0f, 100.0f});
    grid->SetColumns(0); // clamps to 1
    CHECK(grid->GetColumns() == 1);

    auto a = Cell(10.0f, 10.0f);
    auto b = Cell(10.0f, 20.0f);
    grid->AddChild(a.Get());
    grid->AddChild(b.Get()); // single column -> stacks vertically

    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(b->GetPosition().y == doctest::Approx(10.0f)); // below a (row height 10)
}

TEST_CASE("relative: anchors pin children to edges, corners, and center")
{
    auto rel = Make<RelativeLayout>();
    rel->SetSize(foundation::Float2{100.0f, 100.0f}); // content 100x100

    auto tl = Cell(20.0f, 20.0f); // default top-left
    auto br = Cell(20.0f, 20.0f);
    auto ctr = Cell(20.0f, 20.0f);
    rel->AddChild(tl.Get());
    rel->AddChild(br.Get());
    rel->AddChild(ctr.Get());

    rel->SetAnchor(br.Get(), AnchorRight | AnchorBottom);
    rel->SetAnchor(ctr.Get(), AnchorCenter);

    CHECK(tl->GetPosition().x == doctest::Approx(0.0f));
    CHECK(tl->GetPosition().y == doctest::Approx(0.0f));
    CHECK(br->GetPosition().x == doctest::Approx(80.0f)); // 100 - 20
    CHECK(br->GetPosition().y == doctest::Approx(80.0f));
    CHECK(ctr->GetPosition().x == doctest::Approx(40.0f)); // (100 - 20) / 2
    CHECK(ctr->GetPosition().y == doctest::Approx(40.0f));
}

TEST_CASE("relative: mixed horizontal/vertical anchors and re-layout on resize")
{
    auto rel = Make<RelativeLayout>();
    rel->SetSize(foundation::Float2{200.0f, 100.0f});

    auto topRight = Cell(40.0f, 10.0f);
    rel->AddChild(topRight.Get());
    rel->SetAnchor(topRight.Get(), AnchorRight | AnchorTop);
    CHECK(topRight->GetPosition().x == doctest::Approx(160.0f)); // 200 - 40
    CHECK(topRight->GetPosition().y == doctest::Approx(0.0f));

    // Growing the layout re-anchors (OnSizeChange -> PerformLayout).
    rel->SetSize(foundation::Float2{300.0f, 100.0f});
    CHECK(topRight->GetPosition().x == doctest::Approx(260.0f)); // 300 - 40
}

TEST_CASE("relative: clearing an anchor returns a child to the top-left")
{
    auto rel = Make<RelativeLayout>();
    rel->SetSize(foundation::Float2{100.0f, 100.0f});
    auto c = Cell(20.0f, 20.0f);
    rel->AddChild(c.Get());

    rel->SetAnchor(c.Get(), AnchorCenter);
    CHECK(c->GetPosition().x == doctest::Approx(40.0f));
    CHECK(rel->GetAnchor(c.Get()) == static_cast<foundation::u32>(AnchorCenter));

    rel->SetAnchor(c.Get(), AnchorNone); // clears
    CHECK(rel->GetAnchor(c.Get()) == static_cast<foundation::u32>(AnchorNone));
    CHECK(c->GetPosition().x == doctest::Approx(0.0f));
    CHECK(c->GetPosition().y == doctest::Approx(0.0f));
}

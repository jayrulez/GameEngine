// Draconic GUI - LinearLayout tests: children stacked in a row/column with spacing, re-run
// on add and on size/orientation/spacing changes, skipping hidden children.
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

    foundation::RefPtr<UIWidget> Child(foundation::Float2 size)
    {
        auto w = Make<UIWidget>();
        w->SetSize(size);
        return w;
    }
}

TEST_CASE("linear-layout: vertical stacks children on add")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{100.0f, 200.0f});
    layout->SetSpacing(5.0f);

    auto a = Child(foundation::Float2{80.0f, 20.0f});
    auto b = Child(foundation::Float2{80.0f, 30.0f});
    layout->AddChild(a.Get());
    layout->AddChild(b.Get());

    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(b->GetPosition().y == doctest::Approx(25.0f)); // 20 + 5 spacing
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(0.0f));
}

TEST_CASE("linear-layout: horizontal stacks along x")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{300.0f, 50.0f});
    layout->SetOrientation(Orientation::Horizontal);
    layout->SetSpacing(10.0f);

    auto a = Child(foundation::Float2{40.0f, 40.0f});
    auto b = Child(foundation::Float2{60.0f, 40.0f});
    layout->AddChild(a.Get());
    layout->AddChild(b.Get());

    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(50.0f)); // 40 + 10
}

TEST_CASE("linear-layout: padding offsets the start")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{100.0f, 200.0f});
    layout->SetPadding(Thickness{8.0f});

    auto a = Child(foundation::Float2{50.0f, 20.0f});
    layout->AddChild(a.Get());
    CHECK(a->GetPosition().x == doctest::Approx(8.0f));
    CHECK(a->GetPosition().y == doctest::Approx(8.0f));
}

TEST_CASE("linear-layout: hidden children are skipped")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{100.0f, 200.0f});

    auto a = Child(foundation::Float2{50.0f, 20.0f});
    auto b = Child(foundation::Float2{50.0f, 30.0f});
    auto c = Child(foundation::Float2{50.0f, 40.0f});
    layout->AddChild(a.Get());
    layout->AddChild(b.Get());
    layout->AddChild(c.Get());

    b->SetVisible(false);
    layout->PerformLayout(); // re-run after visibility change

    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(c->GetPosition().y == doctest::Approx(20.0f)); // b (hidden) contributes no offset
}

TEST_CASE("linear-layout: relayouts on orientation change")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{200.0f, 200.0f});
    auto a = Child(foundation::Float2{30.0f, 30.0f});
    auto b = Child(foundation::Float2{30.0f, 30.0f});
    layout->AddChild(a.Get());
    layout->AddChild(b.Get());
    CHECK(b->GetPosition().y == doctest::Approx(30.0f)); // vertical default

    layout->SetOrientation(Orientation::Horizontal);
    CHECK(b->GetPosition().x == doctest::Approx(30.0f));
    CHECK(b->GetPosition().y == doctest::Approx(0.0f));
}

TEST_CASE("linear-layout: wrap-content sizes the layout to its stacked children + padding")
{
    auto layout = Make<LinearLayout>();
    layout->SetSize(foundation::Float2{100.0f, 500.0f}); // starts tall
    layout->SetSpacing(5.0f);
    layout->SetPadding(Thickness{10.0f}); // 10 on every side
    layout->SetWrapContent(true);

    layout->AddChild(Child(foundation::Float2{40.0f, 20.0f}).Get());
    layout->AddChild(Child(foundation::Float2{40.0f, 30.0f}).Get());

    // Height = padding(10) + 20 + spacing(5) + 30 + padding(10) = 75; width unchanged.
    CHECK(layout->GetSize().y == doctest::Approx(75.0f));
    CHECK(layout->GetSize().x == doctest::Approx(100.0f));
}

TEST_CASE("linear-layout: horizontal wrap-content sizes width to children")
{
    auto layout = Make<LinearLayout>();
    layout->SetOrientation(Orientation::Horizontal);
    layout->SetSize(foundation::Float2{500.0f, 40.0f});
    layout->SetSpacing(4.0f);
    layout->SetWrapContent(true);

    layout->AddChild(Child(foundation::Float2{30.0f, 20.0f}).Get());
    layout->AddChild(Child(foundation::Float2{50.0f, 20.0f}).Get());

    // Width = 30 + spacing(4) + 50 = 84 (no padding); height unchanged.
    CHECK(layout->GetSize().x == doctest::Approx(84.0f));
    CHECK(layout->GetSize().y == doctest::Approx(40.0f));
}

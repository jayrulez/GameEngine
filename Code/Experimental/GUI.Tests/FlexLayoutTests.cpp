// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - FlexLayout tests: main-axis distribution (justify-content + flex-grow), cross-
// axis alignment (align-items), gap, and row/column direction.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    core::RefPtr<UIWidget> Child(core::Float2 size)
    {
        auto w = Make<UIWidget>();
        w->SetSize(size);
        return w;
    }

    // A flex row of three 40x20 children in a 300x100 box (gap 0 unless set).
    core::RefPtr<FlexLayout> Row3(core::RefPtr<UIWidget>& a, core::RefPtr<UIWidget>& b,
                                  core::RefPtr<UIWidget>& c)
    {
        auto flex = Make<FlexLayout>();
        flex->SetSize(core::Float2{300.0f, 100.0f});
        flex->SetDirection(FlexDirection::Row);
        a = Child(core::Float2{40.0f, 20.0f});
        b = Child(core::Float2{40.0f, 20.0f});
        c = Child(core::Float2{40.0f, 20.0f});
        flex->AddChild(a.Get());
        flex->AddChild(b.Get());
        flex->AddChild(c.Get());
        return flex;
    }
}

TEST_CASE("flex: row start with gap")
{
    core::RefPtr<UIWidget> a, b, c;
    auto flex = Row3(a, b, c);
    flex->SetGap(10.0f); // 40 + 10 stride
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(50.0f));
    CHECK(c->GetPosition().x == doctest::Approx(100.0f));
}

TEST_CASE("flex: justify-content distributes the leftover space")
{
    core::RefPtr<UIWidget> a, b, c;
    auto flex = Row3(a, b, c); // free = 300 - 120 = 180

    flex->SetJustifyContent(JustifyContent::Center); // start += 90
    CHECK(a->GetPosition().x == doctest::Approx(90.0f));
    CHECK(c->GetPosition().x == doctest::Approx(170.0f));

    flex->SetJustifyContent(JustifyContent::End); // start += 180
    CHECK(a->GetPosition().x == doctest::Approx(180.0f));
    CHECK(c->GetPosition().x == doctest::Approx(260.0f));

    flex->SetJustifyContent(JustifyContent::SpaceBetween); // between = 180/2 = 90
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(130.0f));
    CHECK(c->GetPosition().x == doctest::Approx(260.0f));

    flex->SetJustifyContent(JustifyContent::SpaceAround); // unit = 60; start 30; between 60
    CHECK(a->GetPosition().x == doctest::Approx(30.0f));
    CHECK(b->GetPosition().x == doctest::Approx(130.0f));

    flex->SetJustifyContent(JustifyContent::SpaceEvenly); // unit = 45; start 45; between 45
    CHECK(a->GetPosition().x == doctest::Approx(45.0f));
    CHECK(b->GetPosition().x == doctest::Approx(130.0f));
}

TEST_CASE("flex: flex-grow shares leftover main-axis space")
{
    core::RefPtr<UIWidget> a, b, c;
    auto flex = Row3(a, b, c); // free = 180

    flex->SetChildGrow(a.Get(), 2.0f);
    flex->SetChildGrow(b.Get(), 1.0f);
    // a += 2/3*180 = 120 -> 160; b += 1/3*180 = 60 -> 100; c stays 40.
    CHECK(a->GetSize().x == doctest::Approx(160.0f));
    CHECK(b->GetSize().x == doctest::Approx(100.0f));
    CHECK(c->GetSize().x == doctest::Approx(40.0f));
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));
    CHECK(b->GetPosition().x == doctest::Approx(160.0f));
    CHECK(c->GetPosition().x == doctest::Approx(260.0f));
}

TEST_CASE("flex: align-items positions/stretches on the cross axis")
{
    core::RefPtr<UIWidget> a, b, c;
    auto flex = Row3(a, b, c); // cross = 100, child height 20

    flex->SetAlignItems(AlignItems::Center);
    CHECK(a->GetPosition().y == doctest::Approx(40.0f)); // (100-20)/2

    flex->SetAlignItems(AlignItems::End);
    CHECK(a->GetPosition().y == doctest::Approx(80.0f)); // 100-20

    flex->SetAlignItems(AlignItems::Stretch);
    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(a->GetSize().y == doctest::Approx(100.0f)); // stretched to the cross size
}

TEST_CASE("flex: column direction stacks on the y (main) axis")
{
    auto flex = Make<FlexLayout>();
    flex->SetSize(core::Float2{100.0f, 300.0f});
    flex->SetDirection(FlexDirection::Column);
    flex->SetGap(5.0f);
    auto a = Child(core::Float2{40.0f, 30.0f});
    auto b = Child(core::Float2{40.0f, 30.0f});
    flex->AddChild(a.Get());
    flex->AddChild(b.Get());
    CHECK(a->GetPosition().y == doctest::Approx(0.0f));
    CHECK(b->GetPosition().y == doctest::Approx(35.0f)); // 30 + 5 gap
    CHECK(a->GetPosition().x == doctest::Approx(0.0f));  // cross axis start
}

TEST_CASE("flex: markup structural attributes configure the layout")
{
    RegisterGuiElements();
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, foundation::core::DefaultAllocator());
    auto root = loader.LoadFromString(core::StringView(
        u8R"(<FlexLayout direction="column" justify-content="center" align-items="stretch" gap="6"/>)"));
    REQUIRE(root.Get() != nullptr);
    auto* flex = core::Cast<FlexLayout>(root.Get());
    REQUIRE(flex != nullptr);
    CHECK(flex->GetDirection() == FlexDirection::Column);
    CHECK(flex->GetJustifyContent() == JustifyContent::Center);
    CHECK(flex->GetAlignItems() == AlignItems::Stretch);
    CHECK(flex->GetGap() == doctest::Approx(6.0f));
}

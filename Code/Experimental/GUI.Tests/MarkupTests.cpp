// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - MarkupLoader tests: inflate a widget tree from XML - element -> widget,
// nesting -> children, id/class -> identity, claimed attributes (text/orientation) structural,
// and everything else applied as inline CSS through the StyleApplier.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    core::RefPtr<Node> Load(const char8_t* markup)
    {
        RegisterGuiElements();
        static const ElementSet elements = ElementSet::All();
        MarkupLoader loader(elements, foundation::core::DefaultAllocator());
        return loader.LoadFromString(core::StringView(markup));
    }
}

TEST_CASE("markup: inflates a nested tree with identity + structural attributes")
{
    auto root = Load(u8R"(
        <LinearLayout orientation="vertical" spacing="6" id="root">
            <Label text="Title" text-align="center"/>
            <Button text="OK" class="accent primary"/>
        </LinearLayout>
    )");
    REQUIRE(root.Get() != nullptr);

    auto* layout = core::Cast<LinearLayout>(root.Get());
    REQUIRE(layout != nullptr);
    CHECK(layout->GetId() == SV(u8"root"));
    CHECK(layout->GetOrientation() == Orientation::Vertical);
    CHECK(layout->GetSpacing() == doctest::Approx(6.0f));
    REQUIRE(layout->ChildCount() == 2);

    auto* title = core::Cast<Label>(layout->GetChildAt(0));
    REQUIRE(title != nullptr);
    CHECK(title->GetText() == SV(u8"Title"));
    CHECK(title->GetTextAlignH() == TextHAlign::Center);

    auto* button = core::Cast<Button>(layout->GetChildAt(1));
    REQUIRE(button != nullptr);
    CHECK(button->GetText() == SV(u8"OK"));
    CHECK(button->HasClass(SV(u8"accent")));
    CHECK(button->HasClass(SV(u8"primary")));
}

TEST_CASE("markup: unclaimed attributes are applied as inline CSS")
{
    auto root = Load(
        u8R"(<Label text="Hi" width="120" height="30" background-color="#ff0000" padding="4"/>)");
    REQUIRE(root.Get() != nullptr);
    auto* label = core::Cast<Label>(root.Get());
    REQUIRE(label != nullptr);

    CHECK(label->GetSize().x == doctest::Approx(120.0f));
    CHECK(label->GetSize().y == doctest::Approx(30.0f));
    CHECK(label->GetPadding().Left == doctest::Approx(4.0f));
    auto* bg = core::Cast<RectangleDrawable>(label->GetBackground());
    REQUIRE(bg != nullptr);
    CHECK(bg->GetColor().r == doctest::Approx(1.0f));
}

TEST_CASE("markup: CSS length units work in markup (via the applier)")
{
    RegisterGuiElements();
    static const ElementSet elements = ElementSet::All();
    LengthContext ctx;
    ctx.RootFontSize = 10.0f; // 2rem -> 20
    MarkupLoader loader(elements, foundation::core::DefaultAllocator(), nullptr, nullptr, ctx);
    auto root = loader.LoadFromString(SV(u8R"(<Button width="2rem"/>)"));
    REQUIRE(root.Get() != nullptr);
    CHECK(core::Cast<Button>(root.Get())->GetSize().x == doctest::Approx(20.0f));
}

TEST_CASE("markup: an unknown root element yields null; unknown children are skipped")
{
    CHECK(Load(u8R"(<NotAWidget/>)").Get() == nullptr);

    auto root =
        Load(u8R"(<LinearLayout><Label text="a"/><Bogus/><Label text="b"/></LinearLayout>)");
    REQUIRE(root.Get() != nullptr);
    CHECK(root->ChildCount() == 2); // the <Bogus/> child was skipped
}

TEST_CASE("markup: a parse error yields null")
{
    CHECK(Load(u8R"(<LinearLayout><Label text="x")").Get() == nullptr); // unterminated
}

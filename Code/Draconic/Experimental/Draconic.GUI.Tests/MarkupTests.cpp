// Draconic GUI - MarkupLoader tests: inflate a widget tree from XML - element -> widget,
// nesting -> children, id/class -> identity, claimed attributes (text/orientation) structural,
// and everything else applied as inline CSS through the StyleApplier.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }

    foundation::RefPtr<Node> Load(const char8_t* markup)
    {
        static WidgetFactory factory = DefaultWidgetFactory();
        MarkupLoader loader(factory);
        return loader.LoadFromString(foundation::StringView(markup));
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

    auto* layout = foundation::Cast<LinearLayout>(root.Get());
    REQUIRE(layout != nullptr);
    CHECK(layout->GetId() == SV(u8"root"));
    CHECK(layout->GetOrientation() == Orientation::Vertical);
    CHECK(layout->GetSpacing() == doctest::Approx(6.0f));
    REQUIRE(layout->ChildCount() == 2);

    auto* title = foundation::Cast<Label>(layout->GetChildAt(0));
    REQUIRE(title != nullptr);
    CHECK(title->GetText() == SV(u8"Title"));
    CHECK(title->GetTextAlignH() == TextHAlign::Center);

    auto* button = foundation::Cast<Button>(layout->GetChildAt(1));
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
    auto* label = foundation::Cast<Label>(root.Get());
    REQUIRE(label != nullptr);

    CHECK(label->GetSize().x == doctest::Approx(120.0f));
    CHECK(label->GetSize().y == doctest::Approx(30.0f));
    CHECK(label->GetPadding().Left == doctest::Approx(4.0f));
    auto* bg = foundation::Cast<RectangleDrawable>(label->GetBackground());
    REQUIRE(bg != nullptr);
    CHECK(bg->GetColor().r == doctest::Approx(1.0f));
}

TEST_CASE("markup: CSS length units work in markup (via the applier)")
{
    static WidgetFactory factory = DefaultWidgetFactory();
    LengthContext ctx;
    ctx.RootFontSize = 10.0f; // 2rem -> 20
    MarkupLoader loader(factory, nullptr, nullptr, ctx);
    auto root = loader.LoadFromString(SV(u8R"(<Button width="2rem"/>)"));
    REQUIRE(root.Get() != nullptr);
    CHECK(foundation::Cast<Button>(root.Get())->GetSize().x == doctest::Approx(20.0f));
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

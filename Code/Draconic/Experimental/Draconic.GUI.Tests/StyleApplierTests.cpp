// Draconic GUI - CSS value parsers + typed property application: parse value strings into
// Color/length/bool/Thickness, and apply a resolved (or parsed) stylesheet onto a widget.
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
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }

    foundation::RefPtr<UIWidget> Widget(const char8_t* tag)
    {
        auto w = Make<UIWidget>();
        w->SetTag(SV(tag));
        return w;
    }
}

// === value parsers ===

TEST_CASE("css-values: ParseLength")
{
    CHECK(ParseLength(SV(u8"4")).Value() == doctest::Approx(4.0f));
    CHECK(ParseLength(SV(u8"2.5")).Value() == doctest::Approx(2.5f));
    CHECK(ParseLength(SV(u8"10px")).Value() == doctest::Approx(10.0f)); // unit ignored
    CHECK(ParseLength(SV(u8"-3")).Value() == doctest::Approx(-3.0f));
    CHECK_FALSE(ParseLength(SV(u8"abc")).HasValue());
    CHECK_FALSE(ParseLength(SV(u8"")).HasValue());
}

TEST_CASE("css-values: ParseColor hex / named / rgb")
{
    CHECK(ParseColor(SV(u8"#ff0000")).Value().r == doctest::Approx(1.0f));
    CHECK(ParseColor(SV(u8"#00ff00")).Value().g == doctest::Approx(1.0f));
    CHECK(ParseColor(SV(u8"#f00")).Value().r == doctest::Approx(1.0f));                 // shorthand
    CHECK(ParseColor(SV(u8"#0000ff80")).Value().a == doctest::Approx(128.0f / 255.0f)); // alpha
    CHECK(ParseColor(SV(u8"white")).Value().r == doctest::Approx(1.0f));
    CHECK(ParseColor(SV(u8"blue")).Value().b == doctest::Approx(1.0f));
    CHECK(ParseColor(SV(u8"rgb(0,0,255)")).Value().b == doctest::Approx(1.0f));
    CHECK(ParseColor(SV(u8"rgba(255,0,0,0.5)")).Value().a == doctest::Approx(0.5f));
    CHECK_FALSE(ParseColor(SV(u8"#zz")).HasValue());
    CHECK_FALSE(ParseColor(SV(u8"notacolor")).HasValue());
}

TEST_CASE("css-values: ParseBool")
{
    CHECK(ParseBool(SV(u8"true")).Value() == true);
    CHECK(ParseBool(SV(u8"false")).Value() == false);
    CHECK(ParseBool(SV(u8"1")).Value() == true);
    CHECK(ParseBool(SV(u8"0")).Value() == false);
    CHECK_FALSE(ParseBool(SV(u8"maybe")).HasValue());
}

TEST_CASE("css-values: ParseThickness shorthand (top/right/bottom/left)")
{
    Thickness one = ParseThickness(SV(u8"4")).Value();
    CHECK(one.Left == 4.0f);
    CHECK(one.Top == 4.0f);
    CHECK(one.Right == 4.0f);
    CHECK(one.Bottom == 4.0f);

    Thickness two = ParseThickness(SV(u8"2 6")).Value(); // vertical=2, horizontal=6
    CHECK(two.Top == 2.0f);
    CHECK(two.Bottom == 2.0f);
    CHECK(two.Left == 6.0f);
    CHECK(two.Right == 6.0f);

    Thickness four = ParseThickness(SV(u8"1 2 3 4")).Value(); // top right bottom left
    CHECK(four.Top == 1.0f);
    CHECK(four.Right == 2.0f);
    CHECK(four.Bottom == 3.0f);
    CHECK(four.Left == 4.0f);
}

// === application ===

TEST_CASE("style-applier: background-color / padding / opacity")
{
    StyleSheet sheet =
        CSSParser::Parse(SV(u8"button { background-color: #0000ff; padding: 4; opacity: 0.5; }"));
    auto w = Widget(u8"button");
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get()));

    REQUIRE(w->GetBackground() != nullptr);
    auto* bg = foundation::Cast<RectangleDrawable>(w->GetBackground());
    REQUIRE(bg != nullptr);
    CHECK(bg->GetColor().b == doctest::Approx(1.0f)); // blue
    CHECK(w->GetPadding().Left == doctest::Approx(4.0f));
    CHECK(w->GetAlpha() == doctest::Approx(0.5f));
}

TEST_CASE("style-applier: width/height, enabled, visibility, margin")
{
    StyleSheet sheet = CSSParser::Parse(SV(u8"button { width: 120; height: 40; enabled: false; "
                                           u8"visibility: hidden; margin: 1 2 3 4; }"));
    auto w = Widget(u8"button");
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get(), MediaContext{}, /*applyPseudo*/ false));

    CHECK(w->GetSize().x == doctest::Approx(120.0f));
    CHECK(w->GetSize().y == doctest::Approx(40.0f));
    CHECK_FALSE(w->IsEnabled());
    CHECK_FALSE(w->IsVisible());
    CHECK(w->GetMargin().Top == doctest::Approx(1.0f));
    CHECK(w->GetMargin().Left == doctest::Approx(4.0f));
}

TEST_CASE("style-applier: cascade drives the applied value")
{
    StyleSheet sheet = CSSParser::Parse(
        SV(u8"button { background-color: black; } .primary { background-color: red; }"));
    auto w = Widget(u8"button");
    w->AddClass(SV(u8"primary"));
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get()));

    auto* bg = foundation::Cast<RectangleDrawable>(w->GetBackground());
    REQUIRE(bg != nullptr);
    CHECK(bg->GetColor().r == doctest::Approx(1.0f)); // .primary (red) beats button (black)
    CHECK(bg->GetColor().g == doctest::Approx(0.0f));
}

TEST_CASE("style-applier: text-align / vertical-align reach a label")
{
    StyleSheet sheet =
        CSSParser::Parse(SV(u8"label { text-align: center; vertical-align: bottom; }"));
    auto l = Make<Label>(); // Label's ctor tags it "label"
    ApplyStyle(*l.Get(), sheet.Resolve(*l.Get(), MediaContext{}, /*applyPseudo*/ false));
    CHECK(l->GetTextAlignH() == TextHAlign::Center);
    CHECK(l->GetTextAlignV() == TextVAlign::Bottom);

    StyleSheet right = CSSParser::Parse(SV(u8"label { text-align: right; }"));
    ApplyStyle(*l.Get(), right.Resolve(*l.Get(), MediaContext{}, false));
    CHECK(l->GetTextAlignH() == TextHAlign::Right);
    CHECK(l->GetTextAlignV() == TextVAlign::Bottom); // unchanged (only H specified)
}

TEST_CASE("style-applier: min/max-width/height clamp the applied size")
{
    StyleSheet sheet = CSSParser::Parse(SV(u8"box { min-width: 50; max-width: 100; min-height: 20; "
                                           u8"max-height: 80; width: 200; height: 5; }"));
    auto w = Widget(u8"box");
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get(), MediaContext{}, /*applyPseudo*/ false));
    CHECK(w->GetSize().x == doctest::Approx(100.0f)); // 200 clamped down to max 100
    CHECK(w->GetSize().y == doctest::Approx(20.0f));  // 5 clamped up to min 20
    CHECK(w->GetMinSize().x == doctest::Approx(50.0f));
    CHECK(w->GetMaxSize().y == doctest::Approx(80.0f));
}

TEST_CASE("node: SetSize clamps to min/max size constraints")
{
    auto w = Make<UIWidget>();
    w->SetMaxSize(foundation::Float2{100.0f, 100.0f});
    w->SetSize(foundation::Float2{200.0f, 50.0f});
    CHECK(w->GetSize().x == doctest::Approx(100.0f)); // clamped to max
    CHECK(w->GetSize().y == doctest::Approx(50.0f));  // within bounds

    w->SetMinSize(foundation::Float2{60.0f, 60.0f});
    CHECK(w->GetSize().y == doctest::Approx(60.0f)); // re-clamped up when the min grows
}

TEST_CASE("css-values: ParseCornerRadii (1 and 4 values)")
{
    auto one = ParseCornerRadii(SV(u8"6"));
    REQUIRE(one.HasValue());
    CHECK(one.Value().topLeft == doctest::Approx(6.0f));
    CHECK(one.Value().bottomRight == doctest::Approx(6.0f));

    auto four = ParseCornerRadii(SV(u8"1 2 3 4"));
    REQUIRE(four.HasValue());
    CHECK(four.Value().topLeft == doctest::Approx(1.0f));
    CHECK(four.Value().topRight == doctest::Approx(2.0f));
    CHECK(four.Value().bottomRight == doctest::Approx(3.0f));
    CHECK(four.Value().bottomLeft == doctest::Approx(4.0f));
}

TEST_CASE("css-values: ParseBorder (width + style + color, any order)")
{
    auto b = ParseBorder(SV(u8"2 solid #ff0000"));
    REQUIRE(b.HasValue());
    CHECK(b.Value().Width == doctest::Approx(2.0f));
    CHECK(b.Value().LineColor.r == doctest::Approx(1.0f));

    auto reordered = ParseBorder(SV(u8"red 3"));
    REQUIRE(reordered.HasValue());
    CHECK(reordered.Value().Width == doctest::Approx(3.0f));
    CHECK(reordered.Value().LineColor.r == doctest::Approx(1.0f));

    auto none = ParseBorder(SV(u8"none"));
    REQUIRE(none.HasValue());
    CHECK(none.Value().Width == doctest::Approx(0.0f));
}

TEST_CASE("style-applier: border + border-radius round the background and set a border foreground")
{
    StyleSheet sheet = CSSParser::Parse(
        SV(u8"box { background-color: #ffffff; border-radius: 6; border: 2 solid #ff0000; }"));
    auto w = Widget(u8"box");
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get(), MediaContext{}, /*applyPseudo*/ false));

    // Background rounded.
    auto* bg = foundation::Cast<RectangleDrawable>(w->GetBackground());
    REQUIRE(bg != nullptr);
    CHECK(bg->GetCornerRadii().topLeft == doctest::Approx(6.0f));

    // Border foreground: width + color + radii.
    auto* border = foundation::Cast<BorderDrawable>(w->GetForeground());
    REQUIRE(border != nullptr);
    CHECK(border->GetWidth() == doctest::Approx(2.0f));
    CHECK(border->GetColor().r == doctest::Approx(1.0f));
    CHECK(border->GetCornerRadii().topLeft == doctest::Approx(6.0f));
}

TEST_CASE("style-applier: border-color / border-width overrides without the shorthand")
{
    StyleSheet sheet = CSSParser::Parse(SV(u8"box { border-width: 4; border-color: blue; }"));
    auto w = Widget(u8"box");
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get(), MediaContext{}, /*applyPseudo*/ false));
    auto* border = foundation::Cast<BorderDrawable>(w->GetForeground());
    REQUIRE(border != nullptr);
    CHECK(border->GetWidth() == doctest::Approx(4.0f));
    CHECK(border->GetColor().b == doctest::Approx(1.0f));
}

TEST_CASE("css-values: ParseLengthValue recognizes units")
{
    CHECK(ParseLengthValue(SV(u8"12")).Value().Unit == LengthUnit::Px);
    CHECK(ParseLengthValue(SV(u8"12px")).Value().Value == doctest::Approx(12.0f));
    CHECK(ParseLengthValue(SV(u8"1.5rem")).Value().Unit == LengthUnit::Rem);
    CHECK(ParseLengthValue(SV(u8"2em")).Value().Unit == LengthUnit::Em);
    CHECK(ParseLengthValue(SV(u8"50vw")).Value().Unit == LengthUnit::Vw);
    CHECK(ParseLengthValue(SV(u8"80vh")).Value().Unit == LengthUnit::Vh);
    CHECK(ParseLengthValue(SV(u8"25%")).Value().Unit == LengthUnit::Percent);
    CHECK_FALSE(ParseLengthValue(SV(u8"10pt")).HasValue()); // unknown unit
}

TEST_CASE("css-values: ResolveLength resolves units against a context")
{
    LengthContext ctx;
    ctx.RootFontSize = 10.0f;
    ctx.ElementFontSize = 20.0f;
    ctx.ViewportWidth = 1000.0f;
    ctx.ViewportHeight = 400.0f;

    CHECK(ResolveLength(SV(u8"3rem"), ctx).Value() == doctest::Approx(30.0f));  // 3 * root(10)
    CHECK(ResolveLength(SV(u8"2em"), ctx).Value() == doctest::Approx(40.0f));   // 2 * elem(20)
    CHECK(ResolveLength(SV(u8"50vw"), ctx).Value() == doctest::Approx(500.0f)); // 50% of 1000
    CHECK(ResolveLength(SV(u8"25vh"), ctx).Value() == doctest::Approx(100.0f)); // 25% of 400
    CHECK(ResolveLength(SV(u8"50%"), ctx, /*percentBase*/ 200.0f).Value() ==
          doctest::Approx(100.0f));
    CHECK(ResolveLength(SV(u8"12"), ctx).Value() == doctest::Approx(12.0f)); // px passthrough
}

TEST_CASE("style-applier: width/height resolve rem and vh via the length context")
{
    StyleSheet sheet = CSSParser::Parse(SV(u8"box { width: 2rem; height: 50vh; }"));
    auto w = Widget(u8"box");
    LengthContext ctx;
    ctx.RootFontSize = 10.0f;    // 2rem -> 20
    ctx.ViewportHeight = 200.0f; // 50vh -> 100
    ApplyStyle(*w.Get(), sheet.Resolve(*w.Get(), MediaContext{}, /*applyPseudo*/ false), nullptr,
               nullptr, ctx);
    CHECK(w->GetSize().x == doctest::Approx(20.0f));
    CHECK(w->GetSize().y == doctest::Approx(100.0f));
}

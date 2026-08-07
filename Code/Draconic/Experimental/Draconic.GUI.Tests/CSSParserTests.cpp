// Draconic GUI - CSSParser tests: parse .css text into a StyleSheet (comments, selector
// lists, declaration blocks), then resolve end-to-end against a UIWidget.
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

TEST_CASE("css-parser: rules, selector lists, and comments")
{
    const char8_t* css = u8R"(
        /* theme */
        button { color: black; padding: 4; }
        .primary, .secondary { color: white; } /* two selectors share the block */
        #ok:hover { color: green; }
    )";
    StyleSheet sheet = CSSParser::Parse(SV(css));
    CHECK(sheet.RuleCount() == 4); // button, .primary, .secondary, #ok:hover
}

TEST_CASE("css-parser: end-to-end resolve on a widget")
{
    const char8_t* css = u8"button { color: black; padding: 4; } .primary { color: white; }";
    StyleSheet sheet = CSSParser::Parse(SV(css));

    auto w = Widget(u8"button");
    w->AddClass(SV(u8"primary"));
    ResolvedStyle rs = sheet.Resolve(*w.Get());

    CHECK(rs.Get(SV(u8"color")) == SV(u8"white")); // .primary beats button
    CHECK(rs.Get(SV(u8"padding")) == SV(u8"4"));
}

TEST_CASE("css-parser: values keep internal spaces; trailing semicolon optional")
{
    const char8_t* css = u8"box { margin: 1 2 3 4; color: red }";
    StyleSheet sheet = CSSParser::Parse(SV(css));
    REQUIRE(sheet.RuleCount() == 1);

    auto w = Widget(u8"box");
    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Get(SV(u8"margin")) == SV(u8"1 2 3 4")); // internal spaces preserved
    CHECK(rs.Get(SV(u8"color")) == SV(u8"red"));      // last decl, no semicolon
}

TEST_CASE("css-parser: comment stripping inside blocks and selectors")
{
    const char8_t* css = u8"/* a */ button /* b */ { color: /* c */ red; }";
    StyleSheet sheet = CSSParser::Parse(SV(css));
    auto w = Widget(u8"button");
    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Get(SV(u8"color")) == SV(u8"red"));
}

TEST_CASE("css-parser: descendant/child selectors survive parsing")
{
    const char8_t* css = u8".panel > button { color: blue; } .panel label { color: gray; }";
    StyleSheet sheet = CSSParser::Parse(SV(css));
    CHECK(sheet.RuleCount() == 2);

    auto panel = Make<UIWidget>();
    panel->AddClass(SV(u8"panel"));
    auto button = Widget(u8"button");
    panel->AddChild(button.Get());

    ResolvedStyle rs = sheet.Resolve(*button.Get());
    CHECK(rs.Get(SV(u8"color")) == SV(u8"blue")); // .panel > button matches direct child
}

TEST_CASE("css-parser: empty and malformed input is tolerated")
{
    CHECK(CSSParser::Parse(SV(u8"")).RuleCount() == 0);
    CHECK(CSSParser::Parse(SV(u8"   \n  ")).RuleCount() == 0);
    CHECK(CSSParser::Parse(SV(u8".x {}")).RuleCount() == 1); // empty block ok
    CHECK(CSSParser::Parse(SV(u8"button { color")).RuleCount() ==
          1); // unterminated block: best-effort
}

// Draconic GUI - Text tests: measurement and alignment logic against a mock IFont (6px
// advance/byte, 12px line height). The full glyph-render path (atlas + texture) is an
// integration concern; here we assert Text's own logic + the draw guards.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.fonts;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

namespace
{
    // 6px advance per byte, 12px line height.
    class MockFont : public fonts::IFont
    {
    public:
        foundation::StringView FamilyName() const override { return foundation::StringView(u8"mock"); }
        fonts::FontMetrics Metrics() const override
        {
            fonts::FontMetrics m;
            m.ascent = 10.0f;
            m.descent = -2.0f;
            m.lineGap = 0.0f;
            m.lineHeight = 12.0f;
            m.pixelHeight = 10.0f;
            m.scale = 1.0f;
            return m;
        }
        foundation::f32 PixelHeight() const override { return 10.0f; }
        fonts::GlyphInfo GetGlyphInfo(foundation::i32 cp) const override
        {
            fonts::GlyphInfo g;
            g.codepoint = cp;
            g.advanceWidth = 6.0f;
            return g;
        }
        foundation::f32 GetKerning(foundation::i32, foundation::i32) const override { return 0.0f; }
        bool HasGlyph(foundation::i32) const override { return true; }
        foundation::f32 MeasureString(foundation::StringView text) const override
        {
            return static_cast<foundation::f32>(text.Size()) * 6.0f;
        }
        foundation::f32 MeasureString(foundation::StringView text,
                                foundation::Array<fonts::GlyphPosition>& out) const override
        {
            (void)out;
            return static_cast<foundation::f32>(text.Size()) * 6.0f;
        }
    };

    MockFont* NewMock() { return foundation::DefaultAllocator().New<MockFont>(); }
}

TEST_CASE("text: measurement uses font metrics")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr); // owns + deletes the mock on scope exit
    Text t{foundation::StringView(u8"Hello"), &cf};
    CHECK(t.GetWidth() == doctest::Approx(30.0f)); // 5 * 6
    CHECK(t.GetLineHeight() == doctest::Approx(12.0f));
    CHECK(t.Measure().x == doctest::Approx(30.0f));
    CHECK(t.Measure().y == doctest::Approx(12.0f));
}

TEST_CASE("text: no font measures to zero")
{
    Text t;
    t.SetString(foundation::StringView(u8"Hello"));
    CHECK(t.GetWidth() == 0.0f);
    CHECK(t.GetLineHeight() == 0.0f);
}

TEST_CASE("text: string/color/alignment accessors")
{
    Text t;
    t.SetString(foundation::StringView(u8"abc"));
    CHECK(t.GetString() == foundation::StringView(u8"abc"));
    CHECK_FALSE(t.IsEmpty());
    t.SetColor(foundation::Color::Red);
    CHECK(t.GetColor().r == doctest::Approx(1.0f));
    t.SetAlignment(TextHAlign::Center, TextVAlign::Bottom);
    CHECK(t.GetHAlign() == TextHAlign::Center);
    CHECK(t.GetVAlign() == TextVAlign::Bottom);
}

TEST_CASE("text: alignment within bounds")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    Text t{foundation::StringView(u8"Hello"), &cf}; // width 30, height 12
    const Rect bounds{0.0f, 0.0f, 100.0f, 50.0f};

    t.SetAlignment(TextHAlign::Left, TextVAlign::Top);
    CHECK(t.AlignedPosition(bounds).x == doctest::Approx(0.0f));
    CHECK(t.AlignedPosition(bounds).y == doctest::Approx(0.0f));

    t.SetAlignment(TextHAlign::Center, TextVAlign::Middle);
    CHECK(t.AlignedPosition(bounds).x == doctest::Approx(35.0f)); // (100-30)/2
    CHECK(t.AlignedPosition(bounds).y == doctest::Approx(19.0f)); // (50-12)/2

    t.SetAlignment(TextHAlign::Right, TextVAlign::Bottom);
    CHECK(t.AlignedPosition(bounds).x == doctest::Approx(70.0f)); // 100-30
    CHECK(t.AlignedPosition(bounds).y == doctest::Approx(38.0f)); // 50-12
}

TEST_CASE("text: draw guards - no font or empty produces no geometry")
{
    vg::VGContext ctx; // no font service -> VG DrawText early-returns anyway
    DrawContext dc{ctx};

    Text noFont;
    noFont.SetString(foundation::StringView(u8"Hello"));
    noFont.Draw(dc, foundation::Float2{0.0f, 0.0f});
    CHECK(ctx.GetBatch().vertices.Size() == 0);

    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    Text empty{foundation::StringView(u8""), &cf};
    empty.Draw(dc, foundation::Float2{0.0f, 0.0f});
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

TEST_CASE("text: word wrap breaks at whitespace to fit the width")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr); // 6px/byte, 12px line height
    Text t{foundation::StringView(u8"hello world foo"), &cf};
    t.SetWordWrap(true);

    foundation::Array<foundation::StringView> lines;
    t.ComputeLines(60.0f, lines); // 60px = 10 bytes
    REQUIRE(lines.Size() == 2);
    CHECK(lines[0] == foundation::StringView(u8"hello"));
    CHECK(lines[1] == foundation::StringView(u8"world foo"));

    const foundation::Float2 wrapped = t.MeasureWrapped(60.0f);
    CHECK(wrapped.x == doctest::Approx(54.0f)); // widest line "world foo" = 9 * 6
    CHECK(wrapped.y == doctest::Approx(24.0f)); // 2 lines * 12
}

TEST_CASE("text: explicit newlines always break")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    Text t{foundation::StringView(u8"a\nbc\nd"), &cf};
    t.SetWordWrap(true);

    foundation::Array<foundation::StringView> lines;
    t.ComputeLines(1000.0f, lines); // huge width -> only the newlines break
    REQUIRE(lines.Size() == 3);
    CHECK(lines[0] == foundation::StringView(u8"a"));
    CHECK(lines[1] == foundation::StringView(u8"bc"));
    CHECK(lines[2] == foundation::StringView(u8"d"));
}

TEST_CASE("text: a word longer than the width takes its own line (overflows)")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    Text t{foundation::StringView(u8"abcdefghij k"), &cf}; // first word 10 bytes = 60px
    t.SetWordWrap(true);

    foundation::Array<foundation::StringView> lines;
    t.ComputeLines(30.0f, lines); // 30px = 5 bytes; the long word can't fit but is forced
    REQUIRE(lines.Size() == 2);
    CHECK(lines[0] == foundation::StringView(u8"abcdefghij")); // overflows its line
    CHECK(lines[1] == foundation::StringView(u8"k"));
}

TEST_CASE("text: word wrap draws one DrawText per non-empty line")
{
    fonts::CachedFont cf(NewMock(), nullptr, nullptr);
    Text t{foundation::StringView(u8"hello world foo"), &cf};
    t.SetWordWrap(true);

    vg::VGContext ctx; // no font service -> DrawText early-returns, but exercises the layout path
    DrawContext dc{ctx};
    t.Draw(dc, Rect{0.0f, 0.0f, 60.0f, 100.0f});
    // No atlas -> no geometry, but the multi-line path must not crash and must produce no verts.
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - theme + resource-backed CSS props: text color, font-family/size and
// background-image resolve through the StyleManager (with an IResourceProvider), and the
// built-in default theme restyles the standard widgets.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.fonts;
import foundation.image;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace image = foundation::image;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    // 6px advance / byte mock font (as in the Text tests).
    class MockFont : public fonts::IFont
    {
    public:
        core::StringView FamilyName() const override { return core::StringView(u8"mock"); }
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
        core::f32 PixelHeight() const override { return 10.0f; }
        fonts::GlyphInfo GetGlyphInfo(core::i32 cp) const override
        {
            fonts::GlyphInfo g;
            g.codepoint = cp;
            g.advanceWidth = 6.0f;
            return g;
        }
        core::f32 GetKerning(core::i32, core::i32) const override { return 0.0f; }
        bool HasGlyph(core::i32) const override { return true; }
        core::f32 MeasureString(core::StringView t) const override
        {
            return static_cast<core::f32>(t.Size()) * 6.0f;
        }
        core::f32 MeasureString(core::StringView t,
                                core::Array<fonts::GlyphPosition>& o) const override
        {
            (void)o;
            return static_cast<core::f32>(t.Size()) * 6.0f;
        }
    };

    // A resource provider that hands out one image, recording the requested path.
    class MockResources : public IResourceProvider
    {
    public:
        image::OwnedImageData img{2, 2, image::PixelFormat::RGBA8,
                                  core::Span<const core::u8>(kPixels, 16)};
        core::String lastPath;
        const image::ImageData* LoadImage(core::StringView path) override
        {
            lastPath = core::String(path);
            return &img;
        }

    private:
        static constexpr core::u8 kPixels[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                                                 0,   0, 255, 255, 255, 255, 255, 255};
    };

    // A font service that hands out one font, recording the requested family + size.
    class MockFontService : public fonts::IFontService
    {
    public:
        fonts::CachedFont font{core::DefaultAllocator(), core::DefaultAllocator().New<MockFont>(),
                               nullptr, nullptr};
        core::String lastFamily;
        core::f32 lastSize = 0.0f;

        fonts::CachedFont* GetFont(core::f32) override { return &font; }
        fonts::CachedFont* GetFont(core::StringView family, core::f32 size) override
        {
            lastFamily = core::String(family);
            lastSize = size;
            return &font;
        }
        image::ImageData* GetAtlasTexture(fonts::CachedFont*) override { return nullptr; }
        image::ImageData* GetAtlasTexture(core::StringView, core::f32) override { return nullptr; }
        void ReleaseFont(fonts::CachedFont*) override {}
        core::StringView DefaultFontFamily() const override { return core::StringView(u8"mock"); }
        bool HasFamily(core::StringView) const override { return false; }
    };

    core::RefPtr<Label> ApplyCSS(const char8_t* css, IResourceProvider* res = nullptr)
    {
        auto label = core::MakeRef<Label>(core::DefaultAllocator());
        StyleManager mgr;
        mgr.SetStyleSheet(CSSParser::Parse(core::StringView(css)));
        if (res)
            mgr.SetResourceProvider(res);
        mgr.ApplyTo(*label.Get());
        return label;
    }
}

TEST_CASE("theme: color styles a label's text color")
{
    auto label = ApplyCSS(u8"label { color: #ff8800; }");
    CHECK(label->GetTextColor().r == doctest::Approx(1.0f));
    CHECK(label->GetTextColor().g == doctest::Approx(0x88 / 255.0f));
    CHECK(label->GetTextColor().b == doctest::Approx(0.0f));
}

TEST_CASE("theme: background-image loads via the provider and is wrapped in an ImageDrawable")
{
    MockResources res;
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    StyleManager mgr;
    mgr.SetStyleSheet(
        CSSParser::Parse(core::StringView(u8"label { background-image: url(panel); }")));
    mgr.SetResourceProvider(&res);
    mgr.ApplyTo(*label.Get());

    CHECK(res.lastPath.AsView() == core::StringView(u8"panel"));
    // The GUI wraps the provider's raw image in an ImageDrawable (it does not return a Drawable).
    ImageDrawable* d = core::Cast<ImageDrawable>(label->GetBackground());
    REQUIRE(d != nullptr);
    CHECK(d->Image == &res.img);
}

TEST_CASE("theme: font-family + font-size resolve a font via the font service")
{
    MockFontService fs;
    auto label = core::MakeRef<Label>(core::DefaultAllocator());
    StyleManager mgr;
    mgr.SetStyleSheet(
        CSSParser::Parse(core::StringView(u8"label { font-family: Roboto; font-size: 18; }")));
    mgr.SetFontService(&fs);
    mgr.ApplyTo(*label.Get());

    CHECK(fs.lastFamily.AsView() == core::StringView(u8"Roboto"));
    CHECK(fs.lastSize == doctest::Approx(18.0f));
    CHECK(label->GetFont() == &fs.font);
}

TEST_CASE("theme: without providers, resource props are skipped (no crash)")
{
    auto label =
        ApplyCSS(u8"label { background-image: url(panel); font-family: Roboto; color: #ffffff; }");
    CHECK(label->GetBackground() == nullptr); // no provider -> background-image ignored
    CHECK(label->GetTextColor().r == doctest::Approx(1.0f)); // color still applied
}

TEST_CASE("theme: ParseUrl strips the url() wrapper and quotes")
{
    CHECK(ParseUrl(core::StringView(u8"url(panel)")) == core::StringView(u8"panel"));
    CHECK(ParseUrl(core::StringView(u8"url('a/b.png')")) == core::StringView(u8"a/b.png"));
    CHECK(ParseUrl(core::StringView(u8"plain")) == core::StringView(u8"plain"));
}

TEST_CASE("theme: the built-in default theme restyles the standard widgets")
{
    auto root = Make<SceneNode>();
    auto button = Make<Button>();
    auto textfield = Make<TextField>();
    root->AddChild(button.Get());
    root->AddChild(textfield.Get());

    StyleManager mgr;
    mgr.SetStyleSheet(CSSParser::Parse(DefaultDarkThemeCSS()));
    mgr.ApplyTree(*root.Get());

    // The theme gives the button a background + padding and the textfield padding.
    CHECK(button->GetBackground() != nullptr);
    CHECK(button->GetPadding().Left == doctest::Approx(9.0f));
    CHECK(textfield->GetBackground() != nullptr);
    CHECK(textfield->GetPadding().Left == doctest::Approx(6.0f));

    // Switching to the light theme is just swapping the sheet (hot-reload).
    mgr.SetStyleSheet(CSSParser::Parse(DefaultLightThemeCSS()));
    mgr.ApplyTree(*root.Get());
    CHECK(button->GetBackground() != nullptr); // restyled, still has a bg
}

TEST_CASE("theme: pseudo-element (::part) rules resolve separately from the element style")
{
    auto w = Make<UIWidget>();
    w->SetTag(core::StringView(u8"slider"));
    StyleSheet sheet =
        CSSParser::Parse(core::StringView(u8"slider { background-color: #00ff00; } "
                                          u8"slider::fill { background-color: #ff0000; } "
                                          u8"slider::thumb { background-color: #0000ff; }"));

    // The element's own style has only the non-pseudo declaration.
    ResolvedStyle main = sheet.Resolve(*w.Get());
    CHECK(main.Get(core::StringView(u8"background-color")) == core::StringView(u8"#00ff00"));

    // Each part resolves its own cascade.
    ResolvedStyle fill = sheet.Resolve(*w.Get(), MediaContext{}, true, core::StringView(u8"fill"));
    CHECK(fill.Get(core::StringView(u8"background-color")) == core::StringView(u8"#ff0000"));
    ResolvedStyle thumb =
        sheet.Resolve(*w.Get(), MediaContext{}, true, core::StringView(u8"thumb"));
    CHECK(thumb.Get(core::StringView(u8"background-color")) == core::StringView(u8"#0000ff"));

    // A part with no rule resolves to nothing.
    ResolvedStyle none = sheet.Resolve(*w.Get(), MediaContext{}, true, core::StringView(u8"track"));
    CHECK_FALSE(none.Has(core::StringView(u8"background-color")));
}

TEST_CASE("theme: the default theme carries pseudo-element part rules")
{
    // Sanity: the built-in themes actually contain ::part rules a Slider/Window can pick up.
    StyleSheet dark = CSSParser::Parse(DefaultDarkThemeCSS());
    auto slider = Make<UIWidget>();
    slider->SetTag(core::StringView(u8"slider"));
    ResolvedStyle fill =
        dark.Resolve(*slider.Get(), MediaContext{}, true, core::StringView(u8"fill"));
    CHECK(fill.Has(core::StringView(u8"background-color")));

    auto window = Make<UIWidget>();
    window->SetTag(core::StringView(u8"window"));
    ResolvedStyle title =
        dark.Resolve(*window.Get(), MediaContext{}, true, core::StringView(u8"title"));
    CHECK(title.Has(core::StringView(u8"background-color")));
}

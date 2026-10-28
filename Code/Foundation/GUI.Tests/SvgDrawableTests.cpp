// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - the SVG drawable: an SVG picture as image content, from its markup (an icon set compiled
// into the binary, owning its document) or by a path through the resource provider (borrowing
// the provider's); its intrinsic size is the document's, it draws paths at the size it lands, and
// a tint recolours it whole. PictureDrawable picks SVG or raster by the source (an SVG by a .svg
// ending or written inline as a data: URL, which the provider parses once and keeps), and an
// Image's src, a menu command's icon and a rich text's inline icon take either.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import foundation.image;
import foundation.vg.svg;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.gui;

using namespace foundation::gui;
namespace core = foundation::core;
namespace vg = foundation::vg;
namespace image = foundation::image;
namespace fonts = foundation::fonts;

namespace
{
    // A red 24 by 12 rectangle.
    constexpr const char8_t* kBar = u8R"(<svg width="24" height="12"><rect width="24" height="12" fill="#ff0000"/></svg>)";

    // An SVG and a raster, each counted when asked for.
    class Pictures : public IResourceProvider
    {
    public:
        Pictures() : IResourceProvider(core::DefaultAllocator()) {}
        static inline core::u8 kPixels[40 * 20 * 4]{};
        image::OwnedImageData img{40, 20, image::PixelFormat::RGBA8, core::Span<const core::u8>(kPixels, sizeof(kPixels))};
        foundation::vg::svg::SVGDocument svg = foundation::vg::svg::SVGLoader::Load(core::StringView(kBar)).Value();
        int images = 0, svgs = 0;
        const image::ImageData* LoadImage(core::StringView path) override
        {
            ++images;
            return path == core::StringView(u8"missing.png") || path == core::StringView(u8"{vector}") ? nullptr : &img;
        }
        bool LoadText(core::StringView, core::String&) override { return false; }
        const foundation::vg::svg::SVGDocument* LoadSvg(core::StringView path) override
        {
            ++svgs;
            return path == core::StringView(u8"missing.svg") || path == core::StringView(u8"missing.png") ? nullptr : &svg;
        }
    };

    bool AllRed(vg::VGContext& ctx)
    {
        for (const vg::VGVertex& v : ctx.GetBatch().vertices)
            if (v.color.r < 0.99f || v.color.g > 0.01f)
                return false;
        return true;
    }
}

TEST_CASE("svg-drawable: from its markup, the document's size, and the paths it draws as authored")
{
    core::RefPtr<SvgDrawable> d = SvgDrawable::FromText(core::DefaultAllocator(), core::StringView(kBar));
    REQUIRE(d.Get() != nullptr);
    REQUIRE(d->Document() != nullptr);
    REQUIRE(d->IntrinsicSize().HasValue());
    CHECK(d->IntrinsicSize().Value().x == doctest::Approx(24.0f));
    CHECK(d->IntrinsicSize().Value().y == doctest::Approx(12.0f));
    vg::VGContext ctx;
    DrawContext dc{ctx};
    d->Draw(dc, Rect{0.0f, 0.0f, 48.0f, 24.0f});
    REQUIRE(ctx.GetBatch().vertices.Size() > 0);
    CHECK(AllRed(ctx)); // white draws it as authored
    for (const vg::VGVertex& v : ctx.GetBatch().vertices)
    {
        CHECK(v.position.x >= -1.0f); // scaled into the rectangle it lands in
        CHECK(v.position.x <= 49.0f);
        CHECK(v.position.y <= 25.0f);
    }
}

TEST_CASE("svg-drawable: a tint recolours it whole; markup that does not parse makes none")
{
    core::RefPtr<SvgDrawable> d = SvgDrawable::FromText(core::DefaultAllocator(), core::StringView(kBar));
    REQUIRE(d.Get() != nullptr);
    vg::VGContext ctx;
    DrawContext dc{ctx};
    d->DrawTinted(dc, Rect{0.0f, 0.0f, 24.0f, 12.0f}, core::Color::Blue);
    REQUIRE(ctx.GetBatch().vertices.Size() > 0);
    for (const vg::VGVertex& v : ctx.GetBatch().vertices)
    {
        CHECK(v.color.b == doctest::Approx(1.0f));
        CHECK(v.color.r == doctest::Approx(0.0f));
    }
    CHECK(SvgDrawable::FromText(core::DefaultAllocator(), core::StringView(u8"not svg")).Get() == nullptr);
}

TEST_CASE("svg-drawable: a picture by its path is an SVG by its ending, a raster otherwise, none when missing")
{
    Pictures pictures;
    core::RefPtr<Drawable> svg = PictureDrawable(pictures, core::StringView(u8"Icons/bar.SVG"), core::DefaultAllocator());
    REQUIRE(svg.Get() != nullptr);
    auto* asSvg = core::Cast<SvgDrawable>(svg.Get());
    REQUIRE(asSvg != nullptr);
    CHECK(asSvg->Document() == &pictures.svg); // borrowed, not copied
    core::RefPtr<Drawable> raster = PictureDrawable(pictures, core::StringView(u8"Icons/bar.png"), core::DefaultAllocator());
    REQUIRE(raster.Get() != nullptr);
    CHECK(core::Cast<ImageDrawable>(raster.Get()) != nullptr);
    CHECK(pictures.svgs == 1);
    CHECK(pictures.images == 1);
    CHECK(PictureDrawable(pictures, core::StringView(u8"missing.svg"), core::DefaultAllocator()).Get() == nullptr);
    CHECK(PictureDrawable(pictures, core::StringView(u8"missing.png"), core::DefaultAllocator()).Get() == nullptr);
    CHECK(pictures.svgs == 2); // a missing raster named by its file is not asked for as an SVG
}

TEST_CASE("svg-drawable: a picture by an id is whichever the provider has, a raster first")
{
    // An engine names its pictures by id ("{guid}"): no ending says the kind, the provider does.
    Pictures pictures;
    core::RefPtr<Drawable> vector = PictureDrawable(pictures, core::StringView(u8"{vector}"), core::DefaultAllocator());
    REQUIRE(vector.Get() != nullptr);
    CHECK(core::Cast<SvgDrawable>(vector.Get()) != nullptr);
    core::RefPtr<Drawable> raster = PictureDrawable(pictures, core::StringView(u8"{texture}"), core::DefaultAllocator());
    REQUIRE(raster.Get() != nullptr);
    CHECK(core::Cast<ImageDrawable>(raster.Get()) != nullptr);
    CHECK(pictures.svgs == 1); // the raster was found first
    CHECK_FALSE(HasFileEnding(core::StringView(u8"{6faf9764-c33a-444f-8126-91cc8eade4f5}")));
    CHECK_FALSE(HasFileEnding(core::StringView(u8"v1.2/icon")));
    CHECK(HasFileEnding(core::StringView(u8"Icons/a.png")));
}

TEST_CASE("svg-drawable: an Image shows an SVG by its src or from markup, sized as the document")
{
    Pictures pictures;
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 200.0f});
    root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
    root->GetContext().SetResourceProvider(&pictures);
    auto bySource = core::MakeRef<Image>(core::DefaultAllocator());
    bySource->SetSource(core::String(u8"bar.svg"));
    auto fromText = core::MakeRef<Image>(core::DefaultAllocator());
    fromText->SetDrawable(SvgDrawable::FromText(core::DefaultAllocator(), core::StringView(kBar)));
    root->AddChild(bySource.Get());
    root->AddChild(fromText.Get());
    (void)root->UpdateLayout();
    CHECK(pictures.svgs == 1);
    CHECK(pictures.images == 0);
    CHECK(bySource->GetSize().x == doctest::Approx(24.0f));
    CHECK(bySource->GetSize().y == doctest::Approx(12.0f));
    CHECK(fromText->GetSize().x == doctest::Approx(24.0f));
    CHECK(fromText->GetSize().y == doctest::Approx(12.0f));
}

TEST_CASE("svg-drawable: a rich text's inline icon by an .svg path is drawn as its paths")
{
    fonts::TrueTypeFonts::Initialize();
    fonts::TrueTypeFontService service{core::DefaultAllocator()};
    const core::String data = foundation::vfs::FindDataRoot();
    REQUIRE(service.LoadFont(u8"Roboto", foundation::vfs::DataPath(data.AsView(), u8"Engine/Fonts/roboto/Roboto-Regular.ttf").AsView())
            == fonts::FontLoadResult::Success);
    Pictures pictures;
    Text text;
    text.SetFontService(&service);
    text.SetFont(service.GetFont(u8"Roboto", 16.0f));
    text.SetResourceProvider(&pictures);
    text.SetMarkup(core::StringView(u8"[icon=bar.svg]"));
    vg::VGContext ctx;
    DrawContext dc{ctx};
    text.Draw(dc, core::Float2{0.0f, 0.0f});
    CHECK(pictures.svgs == 1);
    CHECK(pictures.images == 0);
    REQUIRE(ctx.GetBatch().vertices.Size() > 0);
    CHECK(AllRed(ctx)); // the icon alone, as authored
}

TEST_CASE("svg-drawable: a menu command's icon by an .svg path")
{
    Pictures pictures;
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->GetContext().SetResourceProvider(&pictures);
    auto item = core::MakeRef<MenuItem>(core::DefaultAllocator());
    root->AddChild(item.Get());
    item->OnBoundCommandText(core::StringView(u8"Bar"), core::StringView(u8"bar.svg"), core::StringView());
    CHECK(pictures.svgs == 1);
    REQUIRE(item->GetIcon() != nullptr);
    CHECK(core::Cast<SvgDrawable>(item->GetIcon()) != nullptr);
}

namespace
{
    constexpr const char8_t* kBarBase64 =
        u8"PHN2ZyB3aWR0aD0iMjQiIGhlaWdodD0iMTIiPjxyZWN0IHdpZHRoPSIyNCIgaGVpZ2h0PSIxMiIgZmlsbD0iI2ZmMDAwMCIvPjwvc3ZnPg==";
    constexpr const char8_t* kBarPercent =
        u8"data:image/svg+xml,%3Csvg%20width%3D%2224%22%20height%3D%2212%22%3E%3Crect%20width%3D%2224%22%20height%3D%2212%22"
        u8"%20fill%3D%22%23ff0000%22%2F%3E%3C%2Fsvg%3E";
}

TEST_CASE("svg-drawable: an SVG written inline decodes from a data: URL, plain, percent-encoded or base64")
{
    core::String out;
    REQUIRE(DecodeSvgDataUrl(core::StringView(kBarPercent), out));
    CHECK(out.AsView() == core::StringView(kBar));
    core::String base64(u8"data:image/svg+xml;charset=utf-8;base64,");
    base64.Append(core::StringView(kBarBase64));
    REQUIRE(DecodeSvgDataUrl(base64.AsView(), out));
    CHECK(out.AsView() == core::StringView(kBar));
    REQUIRE(DecodeSvgDataUrl(core::StringView(u8"DATA:image/SVG+xml;utf8,<svg/>"), out)); // the media type in any case
    CHECK(out.AsView() == core::StringView(u8"<svg/>"));
    CHECK_FALSE(DecodeSvgDataUrl(core::StringView(u8"data:image/svg+xml;base64,not*base64"), out));
    CHECK_FALSE(DecodeSvgDataUrl(core::StringView(u8"data:image/png;base64,AAAA"), out));
    CHECK_FALSE(DecodeSvgDataUrl(core::StringView(u8"data:image/svg+xmlfoo,<svg/>"), out));

    CHECK(IsSvgSource(core::StringView(kBarPercent)));
    CHECK(IsSvgSource(core::StringView(u8"Icons/a.svg")));
    CHECK_FALSE(IsSvgSource(core::StringView(u8"Icons/a.png")));
    CHECK_FALSE(IsSvgSource(core::StringView(u8"data:image/png;base64,AAAA")));
}

TEST_CASE("svg-drawable: the provider parses an inline SVG once and keeps it; a path still goes to LoadSvg")
{
    Pictures pictures;
    const foundation::vg::svg::SVGDocument* first = pictures.Svg(core::StringView(kBarPercent));
    REQUIRE(first != nullptr);
    CHECK(first->width == doctest::Approx(24.0f));
    CHECK(pictures.Svg(core::StringView(kBarPercent)) == first); // the same document, not parsed again
    CHECK(pictures.svgs == 0);
    CHECK(pictures.Svg(core::StringView(u8"data:image/svg+xml,not svg")) == nullptr);
    CHECK(pictures.Svg(core::StringView(u8"data:image/svg+xml,not svg")) == nullptr); // remembered as none
    CHECK(pictures.Svg(core::StringView(u8"bar.svg")) == &pictures.svg);
    CHECK(pictures.svgs == 1);
}

TEST_CASE("svg-drawable: an inline SVG as a sheet's background, an Image's src and a rich text's icon")
{
    Pictures pictures;
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{200.0f, 200.0f});
    root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
    root->GetContext().SetResourceProvider(&pictures);
    core::RefPtr<Panel> panel = core::MakeRef<Panel>(core::DefaultAllocator());
    panel->AddClass(core::StringView(u8"card"));
    root->AddChild(panel.Get());
    StyleManager styles;
    styles.SetResourceProvider(&pictures);
    styles.SetStyleSheet(CSSParser::Parse(core::StringView(
        u8".card { background-image: url(\"data:image/svg+xml,<svg width='24' height='12'><rect width='24' height='12'"
        u8" fill='%23ff0000'/></svg>\"); }")));
    styles.ApplyTree(*root.Get());
    REQUIRE(panel->GetBox().Layers.Size() == 1u);
    REQUIRE(panel->GetBox().Layers[0].Svg != nullptr);
    CHECK(panel->GetBox().Layers[0].Svg->width == doctest::Approx(24.0f));

    RegisterGuiElements();
    static const ElementSet elements = ElementSet::All();
    MarkupLoader loader(elements, core::DefaultAllocator());
    core::String markup(u8"<Image src=\"");
    markup.Append(core::StringView(kBarPercent));
    markup.Append(core::StringView(u8"\"/>"));
    core::RefPtr<Node> image = loader.LoadFromString(markup.AsView());
    REQUIRE(image.Get() != nullptr);
    root->AddChild(image.Get());
    (void)root->UpdateLayout();
    CHECK(image->GetSize().x == doctest::Approx(24.0f));
    CHECK(image->GetSize().y == doctest::Approx(12.0f));
    CHECK(pictures.svgs == 0); // neither asked the provider for a file

    fonts::TrueTypeFonts::Initialize();
    fonts::TrueTypeFontService service{core::DefaultAllocator()};
    const core::String data = foundation::vfs::FindDataRoot();
    REQUIRE(service.LoadFont(u8"Roboto", foundation::vfs::DataPath(data.AsView(), u8"Engine/Fonts/roboto/Roboto-Regular.ttf").AsView())
            == fonts::FontLoadResult::Success);
    Text text;
    text.SetFontService(&service);
    text.SetFont(service.GetFont(u8"Roboto", 16.0f));
    text.SetResourceProvider(&pictures);
    core::String rich(u8"[icon=");
    rich.Append(core::StringView(kBarPercent));
    rich.Append(core::StringView(u8"]"));
    text.SetMarkup(rich.AsView());
    vg::VGContext ctx;
    DrawContext dc{ctx};
    text.Draw(dc, core::Float2{0.0f, 0.0f});
    REQUIRE(ctx.GetBatch().vertices.Size() > 0);
    CHECK(AllRed(ctx));
    CHECK(pictures.svgs == 0);
}

namespace
{
    // An atlas that has a picture the second time it is asked for it, and says what it was asked.
    class AtlasBaker : public IPictureBaker
    {
    public:
        image::ImageDataRef page{512, 512};
        int asked = 0;
        core::u32 width = 0, height = 0;
        bool mask = false;
        const image::ImageData* Baked(const foundation::vg::svg::SVGDocument&, core::u32, core::u32) override { return nullptr; }
        core::Optional<PictureRegion> AtlasRegion(const foundation::vg::svg::SVGDocument&, core::u32 w, core::u32 h, bool m) override
        {
            ++asked;
            width = w;
            height = h;
            mask = m;
            if (asked == 1)
                return {};
            return PictureRegion{&page, core::Rectangle{100.0f, 40.0f, static_cast<core::f32>(w), static_cast<core::f32>(h)}};
        }
        core::u64 Generation() const override { return 0; }
    };
}

TEST_CASE("svg drawable: with a baker it is asked for in its atlas at its device size, drawn as paths until it is there, then as one quad")
{
    core::RefPtr<SvgDrawable> icon = SvgDrawable::FromText(core::DefaultAllocator(), core::StringView(kBar));
    REQUIRE(icon);
    AtlasBaker baker;
    const core::Color tint{0.2f, 0.4f, 1.0f, 1.0f};
    const auto draw = [&](vg::VGContext& ctx, core::Color with)
    {
        DrawContext dc{ctx};
        dc.SetPictureBaker(&baker);
        ctx.SetTransform(core::Float4x4::Scale(core::Float3{2.0f, 2.0f, 1.0f})); // two device pixels a unit
        icon->DrawTinted(dc, Rect{0.0f, 0.0f, 12.0f, 6.0f}, with);
    };
    {
        vg::VGContext ctx;
        draw(ctx, tint);
        CHECK(baker.asked == 1);
        CHECK(baker.width == 24u); // its size on the device
        CHECK(baker.height == 12u);
        CHECK(baker.mask); // tinted: baked white, to take the tint
        for (const vg::VGCommand& cmd : ctx.GetBatch().commands)
            CHECK(cmd.drawMode != vg::VGDrawMode::Layer); // paths meanwhile
    }
    {
        vg::VGContext ctx;
        draw(ctx, tint);
        const vg::VGBatch& batch = ctx.GetBatch();
        REQUIRE(batch.CommandCount() == 1u);
        CHECK(batch.GetCommand(0).drawMode == vg::VGDrawMode::Layer);
        CHECK(batch.textures[static_cast<core::usize>(batch.GetCommand(0).textureIndex)] == &baker.page);
        CHECK(batch.GetCommand(0).indexCount == 6); // one quad
        CHECK(batch.vertices[0].color.b == doctest::Approx(1.0f)); // the tint, on the quad
        CHECK(batch.vertices[0].color.r == doctest::Approx(0.2f));
    }
    {
        vg::VGContext ctx;
        draw(ctx, core::Color::White);
        CHECK_FALSE(baker.mask); // white: as authored
    }
}

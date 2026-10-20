// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - border-image and @spritesheet (E13 of Specs/gui-lab-evolution-2.md): a nine-slice skin cut
// from an image (or a sprite's region of an atlas, or an SVG, its pieces drawn as paths) drawn in
// place of the border, and sprites named as images, written as their atlas and region.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
import foundation.vg;
import foundation.vg.svg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace image = foundation::image;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    // A 64 by 32 atlas, whatever path is asked for (the last remembered).
    class Atlas : public IResourceProvider
    {
    public:
        Atlas() : IResourceProvider(core::DefaultAllocator()) {}
        static inline core::u8 kPixels[64 * 32 * 4]{};
        image::OwnedImageData img{64, 32, image::PixelFormat::RGBA8, core::Span<const core::u8>(kPixels, sizeof(kPixels))};
        core::Array<core::String> asked;
        const image::ImageData* LoadImage(core::StringView path) override
        {
            asked.PushBack(core::String(path));
            return &img;
        }
        bool LoadText(core::StringView, core::String&) override { return false; }
        const foundation::vg::svg::SVGDocument* LoadSvg(core::StringView) override { return nullptr; }
    };
    struct Bed
    {
        Atlas atlas;
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<Panel> panel = core::MakeRef<Panel>(core::DefaultAllocator());
        StyleManager styles;
        explicit Bed(const char8_t* css)
        {
            panel->AddClass(SV(u8"skin"));
            root->AddChild(panel.Get());
            styles.SetResourceProvider(&atlas);
            styles.SetStyleSheet(CSSParser::Parse(SV(css)));
            styles.ApplyTree(*root.Get());
        }
    };
}

TEST_CASE("border-image: slices, widths, fill and repeats from the shorthand")
{
    Bed bed(u8".skin { border-width: 4px; border-style: solid; border-image: url(frame.png) 8 12 fill / 2 6px round repeat; }");
    const BorderImage& skin = bed.panel->GetBox().Skin;
    REQUIRE(skin.Image == &bed.atlas.img);
    CHECK(skin.Slice[0] == doctest::Approx(8.0f));  // top
    CHECK(skin.Slice[1] == doctest::Approx(12.0f)); // right
    CHECK(skin.Slice[2] == doctest::Approx(8.0f));  // bottom, as top
    CHECK(skin.Slice[3] == doctest::Approx(12.0f)); // left, as right
    CHECK(skin.Fill);
    CHECK(skin.Width[0] == doctest::Approx(8.0f)); // 2 times the 4px border
    CHECK(skin.Width[1] == doctest::Approx(6.0f));
    CHECK(skin.RepeatX == BorderImageRepeat::Round);
    CHECK(skin.RepeatY == BorderImageRepeat::Repeat);
    CHECK_FALSE(bed.panel->GetBox().IsEmpty());
}

TEST_CASE("border-image: a percentage slice is of the image, and no slice is all of it")
{
    Bed bed(u8".skin { border-image: url(frame.png) 25% stretch; }");
    const BorderImage& skin = bed.panel->GetBox().Skin;
    CHECK(skin.Slice[0] == doctest::Approx(8.0f));  // 25% of 32
    CHECK(skin.Slice[1] == doctest::Approx(16.0f)); // 25% of 64
    CHECK(skin.Width[0] < 0.0f);                    // no width: the border's
    CHECK(skin.RepeatX == BorderImageRepeat::Stretch);
}

TEST_CASE("spritesheet: a sprite named as an image is its atlas and region, in a border image and a background")
{
    Bed bed(u8"@spritesheet ui { src: url(theme/atlas.png); button: 0px 0px 32px 16px; star: 32 16 16 16; }"
            u8".skin { border-image: url(button) 4 fill; background-image: url(\"star\"); background-size: auto; }");
    const NodeBox& box = bed.panel->GetBox();
    REQUIRE(box.Skin.Image != nullptr);
    CHECK(box.Skin.Source.width == doctest::Approx(32.0f));
    CHECK(box.Skin.Source.height == doctest::Approx(16.0f));
    REQUIRE(box.Layers.Size() == 1u);
    CHECK(box.Layers[0].Source.x == doctest::Approx(32.0f));
    CHECK(box.Layers[0].Source.y == doctest::Approx(16.0f));
    for (const core::String& path : bed.atlas.asked)
    {
        CHECK(path == SV(u8"theme/atlas.png")); // the atlas, not the sprite's name
    }
    // Its own size is the region's.
    const core::Rectangle placed = PlaceImageLayer(box.Layers[0], core::Rectangle{0.0f, 0.0f, 100.0f, 100.0f}, 16.0f, 16.0f);
    CHECK(placed.width == doctest::Approx(16.0f));
}

TEST_CASE("border-style: dashed and dotted sides are recorded; the rest are solid")
{
    Bed bed(u8".skin { border-width: 2px; border-color: red; border-style: dashed dotted solid double; }");
    const NodeBox& box = bed.panel->GetBox();
    CHECK(box.BorderLines[0] == BorderLine::Dashed);
    CHECK(box.BorderLines[1] == BorderLine::Dotted);
    CHECK(box.BorderLines[2] == BorderLine::Solid);
    CHECK(box.BorderLines[3] == BorderLine::Solid); // double: drawn solid
    CHECK(box.BorderWidths.top == doctest::Approx(2.0f));
}

TEST_CASE("border-image: an SVG is sliced in its own units, each piece drawn as paths clipped to its place")
{
    // 30 by 30: a red, a green and a blue band, 10 wide each, sliced 10 from every side.
    Bed bed(u8".skin { border-width: 10px; border-style: solid; border-image: url(\"data:image/svg+xml,<svg width='30' height='30'>"
            u8"<rect width='10' height='30' fill='red'/><rect x='10' width='10' height='30' fill='lime'/>"
            u8"<rect x='20' width='10' height='30' fill='blue'/></svg>\") 10 fill; }");
    const NodeBox& box = bed.panel->GetBox();
    REQUIRE(box.Skin.Svg != nullptr);
    CHECK(box.Skin.Image == nullptr);
    CHECK(box.Skin.Slice[1] == doctest::Approx(10.0f)); // in its units
    CHECK_FALSE(box.IsEmpty());

    foundation::vg::VGContext ctx;
    DrawContext dc{ctx};
    box.DrawUnder(dc, Rect{0.0f, 0.0f, 100.0f, 40.0f});
    const foundation::vg::VGBatch& batch = ctx.GetBatch();
    core::usize clipped = 0, red = 0, green = 0, blue = 0;
    for (const foundation::vg::VGCommand& command : batch.commands)
    {
        if (!command.scissored && command.clipMode == foundation::vg::VGClipMode::None)
            continue;
        ++clipped;
        const core::Rectangle clip = command.clipRect;
        for (core::i32 k = 0; k < command.indexCount; ++k)
        {
            const foundation::vg::VGVertex& v = batch.vertices[batch.indices[static_cast<core::usize>(command.startIndex + k)]];
            if (v.position.x < clip.x - 0.01f || v.position.x > clip.x + clip.width + 0.01f)
                continue; // cut off by its piece's clip
            if (v.coverage < 0.99f)
                continue; // an edge's anti-aliasing fringe, half a pixel out
            if (v.color.r > 0.5f)
            {
                ++red;
                CHECK(v.position.x <= 10.1f); // the left column alone
            }
            else if (v.color.g > 0.5f)
            {
                ++green;
                CHECK(v.position.x >= 9.9f); // the middle, stretched across
                CHECK(v.position.x <= 90.1f);
            }
            else if (v.color.b > 0.5f)
            {
                ++blue;
                CHECK(v.position.x >= 89.9f); // the right column alone
            }
        }
    }
    CHECK(clipped >= 9u); // a clip per piece
    CHECK(red > 0u);
    CHECK(green > 0u);
    CHECK(blue > 0u);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - border-image and @spritesheet (E13 of Specs/gui-lab-evolution-2.md): a nine-slice skin cut
// from an image (or a sprite's region of an atlas) drawn in place of the border, and sprites named
// as images, written as their atlas and region.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
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

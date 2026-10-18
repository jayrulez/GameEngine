// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - background-size and background-position (E13 of Specs/gui-lab-evolution-2.md): an image
// layer fills its box unless sized (cover, contain, lengths, auto), and is placed by percentages of
// the room left over or by lengths from the box's edge.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace image = foundation::image;

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    // A 40 by 20 picture.
    class Pictures : public IResourceProvider
    {
    public:
        static inline core::u8 kPixels[40 * 20 * 4]{};
        image::OwnedImageData img{40, 20, image::PixelFormat::RGBA8, core::Span<const core::u8>(kPixels, sizeof(kPixels))};
        const image::ImageData* LoadImage(core::StringView) override { return &img; }
        bool LoadText(core::StringView, core::String&) override { return false; }
    };
    void CheckRect(const core::Rectangle& r, core::f32 x, core::f32 y, core::f32 w, core::f32 h)
    {
        CHECK(r.x == doctest::Approx(x));
        CHECK(r.y == doctest::Approx(y));
        CHECK(r.width == doctest::Approx(w));
        CHECK(r.height == doctest::Approx(h));
    }
}

TEST_CASE("background: an image layer sized and placed in its box")
{
    const core::Rectangle box{10.0f, 10.0f, 100.0f, 100.0f};
    BoxLayer layer;
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), 10.0f, 10.0f, 100.0f, 100.0f); // no size: filled
    layer.Size = LayerSize::Cover; // 5x: 200 by 100, from the top left
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), 10.0f, 10.0f, 200.0f, 100.0f);
    layer.X = LayerLength{0.0f, 50.0f, false}; // centred: half the room (-100) left over
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), -40.0f, 10.0f, 200.0f, 100.0f);
    layer.Size = LayerSize::Contain; // 2.5x: 100 by 50
    layer.Y = LayerLength{0.0f, 100.0f, false};
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), 10.0f, 60.0f, 100.0f, 50.0f);
    layer.Size = LayerSize::Lengths; // 50% wide, its height from the aspect
    layer.Width = LayerLength{0.0f, 50.0f, false};
    layer.Height = LayerLength{0.0f, 0.0f, true};
    layer.X = LayerLength{5.0f, 0.0f, false};
    layer.Y = LayerLength{0.0f, 0.0f, false};
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), 15.0f, 10.0f, 50.0f, 25.0f);
    layer.Size = LayerSize::Auto;
    CheckRect(PlaceImageLayer(layer, box, 40.0f, 20.0f), 15.0f, 10.0f, 40.0f, 20.0f);
}

TEST_CASE("background: background-size and background-position from a sheet, a comma list per layer")
{
    Pictures pictures;
    core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    core::RefPtr<Panel> panel = core::MakeRef<Panel>(core::DefaultAllocator());
    panel->AddClass(SV(u8"card"));
    root->AddChild(panel.Get());
    StyleManager styles;
    styles.SetResourceProvider(&pictures);
    styles.SetStyleSheet(CSSParser::Parse(SV(u8".card { background-image: url(a.png), url(b.png), url(c.png);"
                                             u8" background-size: cover, 50% auto; background-position: right bottom, 10px center; }")));
    styles.ApplyTree(*root.Get());
    const auto& layers = panel->GetBox().Layers;
    REQUIRE(layers.Size() == 3u);
    CHECK(layers[0].Size == LayerSize::Cover);
    CHECK(layers[0].X.Percent == doctest::Approx(100.0f));
    CHECK(layers[0].Y.Percent == doctest::Approx(100.0f));
    CHECK(layers[1].Size == LayerSize::Lengths);
    CHECK(layers[1].Width.Percent == doctest::Approx(50.0f));
    CHECK(layers[1].Height.Auto);
    CHECK(layers[1].X.Px == doctest::Approx(10.0f));
    CHECK(layers[1].Y.Percent == doctest::Approx(50.0f));
    CHECK(layers[2].Size == LayerSize::Lengths); // past the list: the last again
    CHECK(layers[2].X.Px == doctest::Approx(10.0f));
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The picture on the image pages (ImageInspectView). It fits its view until zoomed or panned, the
// wheel zooms about the pointer, and a drag pans. A pixel is found under a point and read, a single
// channel shows as grey, and the readout describes a pixel.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.image;
import foundation.gui;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace image = foundation::image;

namespace
{
    // A 40x20 picture: every pixel is (10, 20, 30, 200) except (5, 2), which is (255, 128, 0, 64).
    image::Image Picture()
    {
        image::Image picture(40, 20, image::PixelFormat::RGBA8);
        Span<u8> data = picture.PixelDataMut();
        for (usize i = 0; i < data.Size(); i += 4)
        {
            data[i] = 10;
            data[i + 1] = 20;
            data[i + 2] = 30;
            data[i + 3] = 200;
        }
        const usize at = (2u * 40u + 5u) * 4u;
        data[at] = 255;
        data[at + 1] = 128;
        data[at + 2] = 0;
        data[at + 3] = 64;
        return picture;
    }

    // The view at 400x400 in a root.
    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<editor::app::ImageInspectView> view = MakeRef<editor::app::ImageInspectView>(DefaultAllocator());

        Bed()
        {
            view->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(400.0f);
                s.Height = gui::LayoutLength::Px(400.0f);
            });
            root->AddChild(view.Get());
            view->SetImage(Picture());
            (void)root->UpdateLayout();
        }
    };
}

TEST_CASE("image inspect: it fits its view, centred, until zoomed or panned")
{
    Bed bed;
    editor::app::ImageInspectView& view = *bed.view;
    CHECK(view.Fitting());
    CHECK(view.Zoom() == doctest::Approx(10.0f)); // 40 wide in 400
    CHECK(view.Origin().x == doctest::Approx(0.0f));
    CHECK(view.Origin().y == doctest::Approx(100.0f)); // 200 high, centred in 400

    // The wheel zooms about the point under it: the pixel there stays there.
    i32 x = 0;
    i32 y = 0;
    REQUIRE(view.PixelAt(Float2{55.0f, 125.0f}, x, y));
    CHECK(x == 5);
    CHECK(y == 2);
    view.WheelAt(Float2{55.0f, 125.0f}, 2.0f);
    CHECK_FALSE(view.Fitting());
    CHECK(view.Zoom() == doctest::Approx(10.0f * 1.25f * 1.25f));
    REQUIRE(view.PixelAt(Float2{55.0f, 125.0f}, x, y));
    CHECK(x == 5);
    CHECK(y == 2);

    // A drag pans and Fit puts it back. 1:1 shows one image pixel per screen pixel, about the middle.
    const Float2 before = view.Origin();
    view.DragBy(Float2{10.0f, 10.0f}, Float2{40.0f, 0.0f});
    CHECK(view.Origin().x == doctest::Approx(before.x + 30.0f));
    CHECK(view.Origin().y == doctest::Approx(before.y - 10.0f));
    view.Fit();
    CHECK(view.Fitting());
    CHECK(view.Zoom() == doctest::Approx(10.0f));
    view.ActualSize();
    CHECK(view.Zoom() == doctest::Approx(1.0f));
    CHECK(view.Origin().x == doctest::Approx(180.0f));
    CHECK_FALSE(view.PixelAt(Float2{10.0f, 10.0f}, x, y)); // off the picture

    // A picture of another size fits again.
    view.SetImage(image::Image(8, 8, image::PixelFormat::RGBA8));
    CHECK(view.Fitting());
    CHECK(view.Zoom() == doctest::Approx(50.0f));
}

TEST_CASE("image inspect: a pixel read and said; a channel shown on its own")
{
    Bed bed;
    editor::app::ImageInspectView& view = *bed.view;
    editor::app::ImagePixel pixel;
    REQUIRE(view.ValueAt(5, 2, pixel));
    CHECK(pixel.r == 255);
    CHECK(pixel.g == 128);
    CHECK(pixel.b == 0);
    CHECK(pixel.a == 64);
    CHECK_FALSE(view.ValueAt(40, 0, pixel));
    CHECK(editor::app::DescribePixel(5, 2, &pixel) == StringView(u8"x 5  y 2  ·  R 255 G 128 B 0 A 64"));
    CHECK(editor::app::DescribePixel(-1, -1, nullptr).IsEmpty());

    // Choosing a channel leaves the picture's pixels untouched; the channel is drawn as grey.
    view.SetChannel(editor::app::ImageChannel::Green);
    CHECK(view.Channel() == editor::app::ImageChannel::Green);
    REQUIRE(view.ValueAt(5, 2, pixel));
    CHECK(pixel.g == 128);
    CHECK(pixel.r == 255);
}

TEST_CASE("image inspect: a new picture of the same size is a new image, so the renderer draws it and not the last one's texture")
{
    Bed bed;
    editor::app::ImageInspectView& view = *bed.view;
    const u64 first = view.Pixels().InstanceId();
    view.SetImage(Picture()); // a font rebaked at the same size
    CHECK(view.Pixels().InstanceId() != first);
    CHECK(view.Fitting()); // the same size: the fit is kept as it was
}

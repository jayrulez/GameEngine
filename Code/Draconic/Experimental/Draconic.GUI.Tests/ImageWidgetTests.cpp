// Draconic GUI - Image widget tests: the scale modes compute the right destination rect
// (aspect-preserving Fit/Fill, centered Center, natural None, box Stretch) from a drawable's
// intrinsic size.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.image;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;
namespace image = draconic::image;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }

    // A 4x2 (aspect 2:1) RGBA image.
    image::OwnedImageData MakeImage()
    {
        static const foundation::u8 pixels[32] = {
            255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
            255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
        };
        return image::OwnedImageData(4, 2, image::PixelFormat::RGBA8,
                                     foundation::Span<const foundation::u8>(pixels, 32));
    }
}

TEST_CASE("image: stretch fills the content box")
{
    image::OwnedImageData img = MakeImage();
    auto drawable = Make<ImageDrawable>();
    drawable->Image = &img;

    auto w = Make<Image>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    w->SetDrawable(drawable);
    w->SetScaleMode(ImageScaleMode::Stretch);

    const Rect r = w->DrawnBounds();
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(100.0f));
    CHECK(r.height == doctest::Approx(100.0f));
}

TEST_CASE("image: fit preserves aspect inside the box, centered")
{
    image::OwnedImageData img = MakeImage(); // 4x2 -> aspect 2:1
    auto drawable = Make<ImageDrawable>();
    drawable->Image = &img;

    auto w = Make<Image>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    w->SetDrawable(drawable);
    w->SetScaleMode(ImageScaleMode::Fit); // scale = min(100/4, 100/2) = 25 -> 100x50

    const Rect r = w->DrawnBounds();
    CHECK(r.width == doctest::Approx(100.0f));
    CHECK(r.height == doctest::Approx(50.0f));
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(25.0f)); // centered vertically
}

TEST_CASE("image: fill covers the box, centered (overflow negative)")
{
    image::OwnedImageData img = MakeImage(); // 4x2
    auto drawable = Make<ImageDrawable>();
    drawable->Image = &img;

    auto w = Make<Image>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    w->SetDrawable(drawable);
    w->SetScaleMode(ImageScaleMode::Fill); // scale = max(25, 50) = 50 -> 200x100

    const Rect r = w->DrawnBounds();
    CHECK(r.width == doctest::Approx(200.0f));
    CHECK(r.height == doctest::Approx(100.0f));
    CHECK(r.x == doctest::Approx(-50.0f)); // centered -> overflows horizontally
    CHECK(r.y == doctest::Approx(0.0f));
}

TEST_CASE("image: center uses natural size centered; none is natural at top-left")
{
    image::OwnedImageData img = MakeImage(); // 4x2
    auto drawable = Make<ImageDrawable>();
    drawable->Image = &img;

    auto w = Make<Image>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    w->SetDrawable(drawable);

    w->SetScaleMode(ImageScaleMode::Center);
    Rect r = w->DrawnBounds();
    CHECK(r.width == doctest::Approx(4.0f));
    CHECK(r.height == doctest::Approx(2.0f));
    CHECK(r.x == doctest::Approx(48.0f)); // (100-4)/2
    CHECK(r.y == doctest::Approx(49.0f)); // (100-2)/2

    w->SetScaleMode(ImageScaleMode::None);
    r = w->DrawnBounds();
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.width == doctest::Approx(4.0f));
    CHECK(r.height == doctest::Approx(2.0f));
}

TEST_CASE("image: no drawable falls back to the content box and draws nothing")
{
    auto w = Make<Image>();
    w->SetSize(foundation::Float2{60.0f, 40.0f});
    w->SetScaleMode(ImageScaleMode::Fit);

    const Rect r = w->DrawnBounds(); // no intrinsic -> the box
    CHECK(r.width == doctest::Approx(60.0f));
    CHECK(r.height == doctest::Approx(40.0f));
    CHECK_FALSE(w->IntrinsicSize().HasValue());

    vg::VGContext ctx;
    DrawContext dc{ctx};
    w->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() == 0); // nothing drawn
}

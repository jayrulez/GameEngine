// Ported from Sedulous.Images.Tests/ImageDataTests.bf - OwnedImageData,
// NineSlice, ImageAtlasBuilder, PixelFormat. Mirrors the Sedulous assertions so
// the ported lib inherits that suite's coverage. (Test.Assert -> CHECK; Beef
// scope/new -> stack/Array; nullable RectangleI -> const RectI*.)
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.image;

using namespace draconic::foundation;
using namespace draconic::image;

// ============================================================ OwnedImageData

TEST_CASE("image.owned: construct from span")
{
    u8 pixels[16];
    for (i32 i = 0; i < 16; ++i)
        pixels[i] = static_cast<u8>(i);

    OwnedImageData img(2, 2, PixelFormat::RGBA8, Span<const u8>(pixels, 16));
    CHECK(img.Width() == 2u);
    CHECK(img.Height() == 2u);
    CHECK(img.Format() == PixelFormat::RGBA8);
    CHECK(img.PixelData().Size() == 16u);
    CHECK(img.PixelData().Data()[0] == 0);
    CHECK(img.PixelData().Data()[4] == 4);
}

TEST_CASE("image.owned: construct from array (move)")
{
    Array<u8> data;
    data.Resize(8);
    data[0] = 255;
    data[7] = 128;
    OwnedImageData img(2, 1, PixelFormat::RGBA8, Move(data));
    CHECK(img.Width() == 2u);
    CHECK(img.Height() == 1u);
    CHECK(img.PixelData().Data()[0] == 255);
    CHECK(img.PixelData().Data()[7] == 128);
}

TEST_CASE("image.owned: R8 format")
{
    Array<u8> data;
    data.Resize(4);
    OwnedImageData img(2, 2, PixelFormat::R8, Move(data));
    CHECK(img.Format() == PixelFormat::R8);
    CHECK(img.PixelData().Size() == 4u);
}

// ==================================================================== NineSlice

TEST_CASE("image.nineslice: construct four values")
{
    const NineSlice ns(4, 6, 8, 10);
    CHECK(ns.left == 4);
    CHECK(ns.top == 6);
    CHECK(ns.right == 8);
    CHECK(ns.bottom == 10);
}

TEST_CASE("image.nineslice: construct uniform")
{
    const NineSlice ns(5.0f);
    CHECK(ns.left == 5);
    CHECK(ns.top == 5);
    CHECK(ns.right == 5);
    CHECK(ns.bottom == 5);
}

TEST_CASE("image.nineslice: construct symmetric")
{
    const NineSlice ns(3, 7);
    CHECK(ns.left == 3);
    CHECK(ns.right == 3);
    CHECK(ns.top == 7);
    CHECK(ns.bottom == 7);
}

TEST_CASE("image.nineslice: horizontal border")
{
    const NineSlice ns(4, 0, 6, 0);
    CHECK(ns.HorizontalBorder() == 10);
}

TEST_CASE("image.nineslice: vertical border")
{
    const NineSlice ns(0, 3, 0, 5);
    CHECK(ns.VerticalBorder() == 8);
}

TEST_CASE("image.nineslice: IsValid non-zero")
{
    const NineSlice ns(1, 0, 0, 0);
    CHECK(ns.IsValid());
}

TEST_CASE("image.nineslice: IsValid all-zero")
{
    const NineSlice ns(0, 0, 0, 0);
    CHECK_FALSE(ns.IsValid());
}

// =========================================================== ImageAtlasBuilder

namespace
{
    OwnedImageData MakeImage(u32 w, u32 h, u8 fill)
    {
        Array<u8> data;
        data.Resize(static_cast<usize>(w) * h * 4);
        for (usize i = 0; i < data.Size(); ++i)
            data[i] = fill;
        return OwnedImageData(w, h, PixelFormat::RGBA8, Move(data));
    }

    bool Overlaps(const RectI& a, const RectI& b)
    {
        return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height &&
               a.y + a.height > b.y;
    }

    bool IsPow2(u32 v) { return v > 0 && (v & (v - 1)) == 0; }
}

TEST_CASE("image.atlas: empty build")
{
    ImageAtlasBuilder builder;
    CHECK(builder.Build());
    REQUIRE(builder.Atlas() != nullptr);
    CHECK(builder.Atlas()->Width() >= 1u);
    CHECK(builder.Atlas()->Height() >= 1u);
}

TEST_CASE("image.atlas: single image")
{
    const OwnedImageData img = MakeImage(32, 32, 128);

    ImageAtlasBuilder builder;
    builder.AddImage(u8"test", &img);
    CHECK(builder.Build());

    const RectI* region = builder.GetRegion(u8"test");
    REQUIRE(region != nullptr);
    CHECK(region->width == 32);
    CHECK(region->height == 32);
}

TEST_CASE("image.atlas: multiple images do not overlap")
{
    const OwnedImageData img1 = MakeImage(64, 64, 100);
    const OwnedImageData img2 = MakeImage(32, 32, 200);
    const OwnedImageData img3 = MakeImage(48, 48, 150);

    ImageAtlasBuilder builder;
    builder.AddImage(u8"a", &img1);
    builder.AddImage(u8"b", &img2);
    builder.AddImage(u8"c", &img3);
    CHECK(builder.Build());

    const RectI* r1 = builder.GetRegion(u8"a");
    const RectI* r2 = builder.GetRegion(u8"b");
    const RectI* r3 = builder.GetRegion(u8"c");
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);
    REQUIRE(r3 != nullptr);

    CHECK_FALSE(Overlaps(*r1, *r2));
    CHECK_FALSE(Overlaps(*r1, *r3));
    CHECK_FALSE(Overlaps(*r2, *r3));
}

TEST_CASE("image.atlas: dimensions are powers of two")
{
    const OwnedImageData img = MakeImage(100, 100, 0);

    ImageAtlasBuilder builder;
    builder.AddImage(u8"big", &img);
    CHECK(builder.Build());

    CHECK(IsPow2(builder.Atlas()->Width()));
    CHECK(IsPow2(builder.Atlas()->Height()));
}

TEST_CASE("image.atlas: grows when needed")
{
    ImageAtlasBuilder builder(256, 4096);
    Array<OwnedImageData> images;
    images.Reserve(20);
    for (i32 i = 0; i < 20; ++i)
        images.PushBack(MakeImage(64, 64, static_cast<u8>(i)));

    // Names must outlive Build(); build them up front.
    for (i32 i = 0; i < 20; ++i)
    {
        String name = Format(u8"img{}", i);
        builder.AddImage(name.AsView(), &images[static_cast<usize>(i)]);
    }

    CHECK(builder.Build());
    // 20 x 64x64 = 81920 px; 256x256=65536 too small -> should have grown.
    CHECK((builder.Atlas()->Width() >= 512u || builder.Atlas()->Height() >= 512u));
}

TEST_CASE("image.atlas: GetRegion missing")
{
    ImageAtlasBuilder builder;
    builder.Build();
    CHECK(builder.GetRegion(u8"nonexistent") == nullptr);
}

TEST_CASE("image.atlas: pixel data copied")
{
    const OwnedImageData img = MakeImage(4, 4, 42);

    ImageAtlasBuilder builder(256);
    builder.AddImage(u8"px", &img);
    CHECK(builder.Build());

    const RectI* region = builder.GetRegion(u8"px");
    REQUIRE(region != nullptr);
    const ImageData* atlas = builder.Atlas();
    const u32 stride = atlas->Width() * 4;

    const usize offset = static_cast<usize>(region->y) * stride + static_cast<usize>(region->x) * 4;
    CHECK(atlas->PixelData().Data()[offset] == 42);
}

TEST_CASE("image.atlas: large image (bigger than min size)")
{
    const OwnedImageData img = MakeImage(300, 300, 77);

    ImageAtlasBuilder builder(256, 4096);
    builder.AddImage(u8"large", &img);
    CHECK(builder.Build());

    const RectI* region = builder.GetRegion(u8"large");
    REQUIRE(region != nullptr);
    CHECK(region->width == 300);
    CHECK(region->height == 300);
    CHECK(builder.Atlas()->Width() >= 302u); // 300 + padding
}

// =================================================================== PixelFormat

TEST_CASE("image.pixelformat: distinct values")
{
    CHECK(PixelFormat::R8 != PixelFormat::RGBA8);
    CHECK(PixelFormat::RGBA8 != PixelFormat::BGRA8);
}

TEST_CASE("image.data: InstanceId is a per-construction identity")
{
    const u8 px[4] = {1, 2, 3, 4};
    const OwnedImageData a(1, 1, PixelFormat::RGBA8, Span<const u8>(px, 4));
    const OwnedImageData b(1, 1, PixelFormat::RGBA8, Span<const u8>(px, 4));
    CHECK(a.InstanceId() != 0);
    CHECK(a.InstanceId() != b.InstanceId()); // every construction is a new identity

    const OwnedImageData c(a);
    CHECK(c.InstanceId() != a.InstanceId()); // copies are new identities

    // Assignment keeps the target's identity (caches keyed on it stay coherent).
    OwnedImageData d(1, 1, PixelFormat::RGBA8, Span<const u8>(px, 4));
    const u64 dId = d.InstanceId();
    d = a;
    CHECK(d.InstanceId() == dId);
}

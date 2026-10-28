// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/ThemeAtlasTests.bf (faithful; Beef `scope`/`new`/`defer
// ReleaseRef` -> stack values / RefPtr, tuple state span -> StateImageEntry[]).
// NOTE: Sedulous NineSlice exposes PascalCase `.Left`; the engine image::NineSlice uses lowercase
// `.left` (the image module's field convention), so `Slices.left` here is the faithful equivalent.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
import foundation.ui;

using namespace foundation::ui;
namespace core = foundation::core;
namespace image = foundation::image;
using namespace foundation::core;

static image::OwnedImageData MakeTestImage(u32 w, u32 h, u8 r, u8 g, u8 b)
{
    core::Array<u8> data;
    data.Resize(static_cast<usize>(w) * h * 4);
    for (u32 i = 0; i < w * h; ++i)
    {
        data[i * 4] = r;
        data[i * 4 + 1] = g;
        data[i * 4 + 2] = b;
        data[i * 4 + 3] = 255;
    }
    return image::OwnedImageData(w, h, image::PixelFormat::RGBA8, core::Move(data));
}

// === ThemeAtlas ===

TEST_CASE("theme-atlas: ThemeAtlas_CreateImageDrawable")
{
    core::RefPtr<ThemeAtlas> atlasRef = core::MakeRef<ThemeAtlas>(DefaultAllocator());
    ThemeAtlas& atlas = *atlasRef;
    image::OwnedImageData img = MakeTestImage(32, 32, 255, 0, 0);

    atlas.AddImage(u8"button", &img);
    CHECK(atlas.Build());

    core::RefPtr<AtlasImageDrawable> drawable = atlas.CreateImageDrawable(u8"button");
    CHECK(drawable);
    CHECK(drawable->AtlasImage != nullptr);
}

TEST_CASE("theme-atlas: ThemeAtlas_CreateNineSliceDrawable")
{
    core::RefPtr<ThemeAtlas> atlasRef = core::MakeRef<ThemeAtlas>(DefaultAllocator());
    ThemeAtlas& atlas = *atlasRef;
    image::OwnedImageData img = MakeTestImage(32, 32, 255, 0, 0);

    atlas.AddImage(u8"panel", &img);
    CHECK(atlas.Build());

    image::NineSlice slices(4, 4, 4, 4);
    core::RefPtr<AtlasNineSliceDrawable> drawable =
        atlas.CreateNineSliceDrawable(u8"panel", slices);
    CHECK(drawable);
    CHECK(drawable->Slices.left == 4);
}

TEST_CASE("theme-atlas: ThemeAtlas_CreateDrawable_BeforeBuild_ReturnsNull")
{
    core::RefPtr<ThemeAtlas> atlasRef = core::MakeRef<ThemeAtlas>(DefaultAllocator());
    ThemeAtlas& atlas = *atlasRef;
    core::RefPtr<AtlasImageDrawable> drawable = atlas.CreateImageDrawable(u8"missing");
    CHECK(!drawable);
}

TEST_CASE("theme-atlas: ThemeAtlas_CreateStateDrawable")
{
    core::RefPtr<ThemeAtlas> atlasRef = core::MakeRef<ThemeAtlas>(DefaultAllocator());
    ThemeAtlas& atlas = *atlasRef;
    image::OwnedImageData imgNormal = MakeTestImage(16, 16, 200, 200, 200);
    image::OwnedImageData imgHover = MakeTestImage(16, 16, 220, 220, 220);

    atlas.AddImage(u8"btn_normal", &imgNormal);
    atlas.AddImage(u8"btn_hover", &imgHover);
    CHECK(atlas.Build());

    const StateImageEntry states[2] = {{ControlState::Normal, u8"btn_normal"},
                                       {ControlState::Hover, u8"btn_hover"}};
    core::RefPtr<StateListDrawable> stateDrawable =
        atlas.CreateStateDrawable(core::Span<const StateImageEntry>(states, 2));
    CHECK(stateDrawable);
}

TEST_CASE("theme-atlas: ThemeAtlas_MultipleImages_AllPackable")
{
    core::RefPtr<ThemeAtlas> atlasRef = core::MakeRef<ThemeAtlas>(DefaultAllocator());
    ThemeAtlas& atlas = *atlasRef;
    image::OwnedImageData img1 = MakeTestImage(64, 64, 255, 0, 0);
    image::OwnedImageData img2 = MakeTestImage(32, 32, 0, 255, 0);
    image::OwnedImageData img3 = MakeTestImage(48, 48, 0, 0, 255);

    atlas.AddImage(u8"red", &img1);
    atlas.AddImage(u8"green", &img2);
    atlas.AddImage(u8"blue", &img3);
    CHECK(atlas.Build());

    core::RefPtr<AtlasImageDrawable> d1 = atlas.CreateImageDrawable(u8"red");
    core::RefPtr<AtlasImageDrawable> d2 = atlas.CreateImageDrawable(u8"green");
    core::RefPtr<AtlasImageDrawable> d3 = atlas.CreateImageDrawable(u8"blue");

    CHECK(d1);
    CHECK(d2);
    CHECK(d3);
}

// === ThemeImageSet ===

TEST_CASE("theme-atlas: ThemeImageSet_AddImage")
{
    ThemeImageSet set;
    image::OwnedImageData img = MakeTestImage(16, 16, 255, 0, 0);

    set.AddImage(u8"button:Background", &img);
    core::Optional<ThemeImageEntry> entry = set.GetEntry(u8"button:Background");
    CHECK(entry.HasValue());
    CHECK(!entry.Value().IsNineSlice);
}

TEST_CASE("theme-atlas: ThemeImageSet_AddImage_NineSlice")
{
    ThemeImageSet set;
    image::OwnedImageData img = MakeTestImage(32, 32, 255, 0, 0);

    set.AddImage(u8"panel:Background", &img, image::NineSlice(4, 4, 4, 4));
    core::Optional<ThemeImageEntry> entry = set.GetEntry(u8"panel:Background");
    CHECK(entry.HasValue());
    CHECK(entry.Value().IsNineSlice);
    CHECK(entry.Value().Slices.left == 4);
}

TEST_CASE("theme-atlas: ThemeImageSet_AddStateImages")
{
    ThemeImageSet set;
    image::OwnedImageData normal = MakeTestImage(16, 16, 200, 200, 200);
    image::OwnedImageData hover = MakeTestImage(16, 16, 220, 220, 220);

    set.AddStateImages(u8"button:Background", &normal, &hover);

    // State images are added with internal keys.
    core::Optional<ThemeImageEntry> normalEntry = set.GetEntry(u8"button:Background_Normal");
    CHECK(normalEntry.HasValue());
    core::Optional<ThemeImageEntry> hoverEntry = set.GetEntry(u8"button:Background_Hover");
    CHECK(hoverEntry.HasValue());
}

TEST_CASE("theme-atlas: ThemeImageSet_NullImage_Ignored")
{
    ThemeImageSet set;
    set.AddImage(u8"key", nullptr);
    CHECK(!set.GetEntry(u8"key").HasValue());
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full CPU-image asset pipeline: author an ImageAsset (an image file), cook it with
// ImageAssetBuilder into an output content DB, and load the cooked ImageResource
// through the ResourceManager (device-free). PNG round-trips RGBA8.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.image;
import foundation.image.io;
import foundation.image.resource;
import image.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::image;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_imgpipe_src.png");
        FileDelete(u8"scratch_imgpipe_out_db/icon.rasset");
        FileDelete(u8"scratch_imgpipe_out_db/icon.pixels.bin");
        RemoveDirectory(u8"scratch_imgpipe_out_db");
    }
}

TEST_CASE("image.pipeline: ImageAsset -> cook -> ImageResource round-trips")
{
    RegisterImageResource();
    RegisterImageAsset();
    RemoveTree();

    // A known 2x2 RGBA source image on disk (the art the asset references).
    {
        Image src(2, 2, PixelFormat::RGBA8);
        Span<u8> px = src.PixelDataMut();
        for (usize i = 0; i < px.Size(); ++i)
        {
            px.Data()[i] = static_cast<u8>(i * 7);
        }
        REQUIRE(foundation::image::io::SaveImage(src, u8"scratch_imgpipe_src.png",
                                               foundation::image::io::ImageFileFormat::PNG)
                    .IsOk());
    }

    NativeFileSystem outMount(u8"scratch_imgpipe_out_db", DefaultAllocator());

    // --- cook (tooling): ImageAsset into ImageResource in the output DB ---
    Guid id;
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"icon", ImageResource::StaticType());
        id = inst->Id();

        ImageAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"scratch_imgpipe_src.png");
        asset.colorSpace = ImageColorSpace::Srgb;

        ImageAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // --- runtime load (device-free): cooked ImageResource via the manager ---
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    ImageFactory factory;
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<ImageResource> img = manager.Bind<ImageResource>(id);
    REQUIRE(img);
    CHECK(img->width == 2u);
    CHECK(img->height == 2u);
    CHECK(img->format == PixelFormat::RGBA8);
    CHECK(img->colorSpace == ImageColorSpace::Srgb);

    REQUIRE(img->Pixels().Size() == 2u * 2u * 4u);
    bool match = true;
    for (usize i = 0; i < img->Pixels().Size(); ++i)
    {
        if (img->Pixels().Data()[i] != static_cast<u8>(i * 7))
        {
            match = false;
            break;
        }
    }
    CHECK(match);

    // The IImageData view points at the resource's pixels.
    ImageDataRef view = img->View();
    CHECK(view.Width() == 2u);
    CHECK(view.PixelData().Data() == img->Pixels().Data());

    RemoveTree();
}

TEST_CASE("image.pipeline: builder fails on a missing source file")
{
    RegisterImageResource();
    RegisterImageAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_imgpipe_out_db", DefaultAllocator());
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"icon", ImageResource::StaticType());

    ImageAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"does_not_exist_xyz.png");
    ImageAssetBuilder builder;
    NativeFileSystem srcMount2(u8".", DefaultAllocator());
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &srcMount2;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    RemoveTree();
}

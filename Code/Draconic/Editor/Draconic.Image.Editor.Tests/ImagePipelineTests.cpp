// Full CPU-image asset pipeline: author an ImageAsset (image file) -> cook with
// ImageAssetBuilder into an output content DB -> load the cooked ImageResource
// through the ResourceManager (device-free, model B). PNG round-trips RGBA8.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.image;
import draconic.image.io;
import draconic.image.resource;
import draconic.image.editor;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::image;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"draconic_imgpipe_src.png");
        FileDelete(u8"draconic_imgpipe_out_db/icon.rasset");
        FileDelete(u8"draconic_imgpipe_out_db/icon.pixels.bin");
        RemoveDirectory(u8"draconic_imgpipe_out_db");
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
        REQUIRE(draconic::image::io::SaveImage(src, u8"draconic_imgpipe_src.png",
                                               draconic::image::io::ImageFileFormat::PNG)
                    .IsOk());
    }

    NativeFileSystem outMount(u8"draconic_imgpipe_out_db");

    // --- cook (tooling): ImageAsset -> ImageResource in the output DB ---
    Guid id;
    {
        draconic::content::ContentDatabase outDb(
            outMount, draconic::foundation::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"icon", ImageResource::StaticType());
        id = inst->Id();

        ImageAsset asset;
        asset.fileName = draconic::vfs::SourcePath(u8"draconic_imgpipe_src.png");
        asset.colorSpace = ImageColorSpace::Srgb;

        ImageAssetBuilder builder;
        NativeFileSystem srcMount(u8".");
        draconic::editor::AssetBuildContext ctx;
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // --- runtime load (device-free): cooked ImageResource via the manager ---
    draconic::content::ContentDatabase outDb(outMount, draconic::foundation::BinarySerializerFactory(),
                                             u8".rasset");
    ImageFactory factory;
    ResourceManager manager(outDb);
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
    NativeFileSystem outMount(u8"draconic_imgpipe_out_db");
    draconic::content::ContentDatabase outDb(outMount, draconic::foundation::BinarySerializerFactory(),
                                             u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"icon", ImageResource::StaticType());

    ImageAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"does_not_exist_xyz.png");
    ImageAssetBuilder builder;
    NativeFileSystem srcMount2(u8".");
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &srcMount2;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    RemoveTree();
}

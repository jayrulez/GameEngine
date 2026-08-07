// Full font asset pipeline: author a FontAsset over a real TTF -> cook with
// FontAssetBuilder into an output content DB -> load the cooked FontResource through the
// ResourceManager with the device-free FontFactory and verify the rasterizer-free product.
// Covers both bake modes (coverage size ramp + msdfgen MSDF) and the importer's file claim.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.editor.core;
import draconic.fonts;
import draconic.image;
import draconic.fonts.resource;
import draconic.fonts.editor;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::fonts;

namespace
{
    constexpr StringView kSourceFont = u8"draconic_fontpipe_src.ttf";

    void RemoveTree()
    {
        FileDelete(kSourceFont);
        FileDelete(u8"draconic_fontpipe_out_db/uifont.rasset");
        FileDelete(u8"draconic_fontpipe_out_db/uifont.data.bin");
        RemoveDirectory(u8"draconic_fontpipe_out_db");
    }

    // Copy the repo's DejaVu Mono beside the test (the sources mount is the CWD).
    bool StageSourceFont()
    {
        return FileCopyPreserving(
            StringView(reinterpret_cast<const utf8char*>(DRACONIC_TEST_FONT_PATH)), kSourceFont);
    }
}

TEST_CASE("font.pipeline: FontAsset raster ramp -> cook -> rasterizer-free Font")
{
    RegisterFontAsset();
    RemoveTree();
    REQUIRE(StageSourceFont());

    NativeFileSystem outMount(u8"draconic_fontpipe_out_db");
    Guid id;
    {
        draconic::content::ContentDatabase outDb(
            outMount, draconic::foundation::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());
        id = inst->Id();

        FontAsset asset;
        asset.fileName = draconic::vfs::SourcePath(kSourceFont);
        asset.family = String(u8"TestMono");
        asset.sizes.Clear();
        asset.sizes.PushBack(12.0f);
        asset.sizes.PushBack(24.0f);
        asset.firstCodepoint = 32;
        asset.lastCodepoint = 126; // ASCII keeps the bake fast
        asset.atlasWidth = 512;
        asset.atlasHeight = 512;

        FontAssetBuilder builder;
        REQUIRE(builder.AssetType() == &FontAsset::StaticType());
        REQUIRE(builder.ProductType() == &FontResource::StaticType());
        NativeFileSystem srcMount(u8".");
        draconic::editor::AssetBuildContext ctx;
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    draconic::content::ContentDatabase outDb(outMount, draconic::foundation::BinarySerializerFactory(),
                                             u8".rasset");
    FontFactory factory;
    ResourceManager manager(outDb);
    manager.AddFactory(&factory);

    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);
    CHECK(font->Family() == u8"TestMono");
    REQUIRE(font->EntryCount() == 2u);

    const Font::Entry* entry = font->ClosestEntry(12.0f);
    REQUIRE(entry != nullptr);
    CHECK(entry->pixelHeight == doctest::Approx(12.0f));
    REQUIRE(entry->font);
    CHECK(entry->font->HasGlyph('A'));
    CHECK(entry->font->GetGlyphInfo('A').advanceWidth > 0.0f);
    CHECK(entry->font->Metrics().ascent > 0.0f);
    REQUIRE(entry->atlas);
    CHECK(entry->atlas->Mode() == AtlasMode::Coverage);
    CHECK(entry->atlas->Contains('A'));
    GlyphQuad quad;
    f32 cursor = 0.0f;
    CHECK(entry->atlas->GetGlyphQuad('A', cursor, 0.0f, quad));
    CHECK(cursor > 0.0f); // the advance moved
    REQUIRE(entry->atlasImage);
    CHECK(entry->atlasImage->Width() == 512u);
    CHECK(entry->atlasImage->PixelData().Size() == 512u * 512u * 4u); // RGBA expansion

    // A monospace face measures a string as advance * count.
    const f32 one = entry->font->GetGlyphInfo('M').advanceWidth;
    CHECK(entry->font->MeasureString(u8"MM") == doctest::Approx(one * 2.0f).epsilon(0.01));

    RemoveTree();
}

TEST_CASE("font.pipeline: MSDF bake cooks a DistanceField resource")
{
    RegisterFontAsset();
    RemoveTree();
    REQUIRE(StageSourceFont());

    NativeFileSystem outMount(u8"draconic_fontpipe_out_db");
    Guid id;
    {
        draconic::content::ContentDatabase outDb(
            outMount, draconic::foundation::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());
        id = inst->Id();

        FontAsset asset;
        asset.fileName = draconic::vfs::SourcePath(kSourceFont);
        asset.mode = FontBakeMode::DistanceField;
        asset.dfSize = 32.0f;
        asset.firstCodepoint = 'A';
        asset.lastCodepoint = 'Z'; // a small range keeps msdfgen quick
        asset.atlasWidth = 256;
        asset.atlasHeight = 256;

        FontAssetBuilder builder;
        NativeFileSystem srcMount(u8".");
        draconic::editor::AssetBuildContext ctx;
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    draconic::content::ContentDatabase outDb(outMount, draconic::foundation::BinarySerializerFactory(),
                                             u8".rasset");
    FontFactory factory;
    ResourceManager manager(outDb);
    manager.AddFactory(&factory);

    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);
    REQUIRE(font->EntryCount() == 1u);
    const Font::Entry& entry = font->EntryAt(0);
    REQUIRE(entry.atlas);
    CHECK(entry.atlas->Mode() == AtlasMode::DistanceField);
    CHECK(entry.atlas->DistanceFieldRange() > 0.0f);
    CHECK(entry.atlas->Contains('Q'));
    REQUIRE(entry.atlasImage);
    CHECK(entry.atlasImage->ColorSpace() == draconic::image::ImageColorSpace::Linear);

    RemoveTree();
}

TEST_CASE("font.importer: claims font extensions only")
{
    FontAssetImporter importer;
    CHECK(importer.Accepts(u8"ttf"));
    CHECK(importer.Accepts(u8"otf"));
    CHECK(importer.Accepts(u8"ttc"));
    CHECK_FALSE(importer.Accepts(u8"png"));
    CHECK_FALSE(importer.Accepts(u8"xasset"));
}

TEST_CASE("font.pipeline: builder fails on a missing source file")
{
    RegisterFontAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"draconic_fontpipe_out_db");
    draconic::content::ContentDatabase outDb(outMount, draconic::foundation::BinarySerializerFactory(),
                                             u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());

    FontAsset asset;
    asset.fileName = draconic::vfs::SourcePath(u8"does_not_exist_xyz.ttf");
    FontAssetBuilder builder;
    NativeFileSystem srcMount(u8".");
    draconic::editor::AssetBuildContext ctx;
    ctx.sources = &srcMount;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());
    RemoveTree();
}

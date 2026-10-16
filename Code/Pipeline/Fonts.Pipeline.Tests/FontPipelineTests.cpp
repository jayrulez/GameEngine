// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full font asset pipeline: author a FontAsset over a real TTF -> cook with
// FontAssetBuilder into an output content DB -> load the cooked FontResource through the
// ResourceManager with the device-free FontFactory and verify the rasterizer-free product.
// Covers both bake modes (coverage size ramp + msdfgen MSDF) and the importer's file claim.
#include <doctest/doctest.h>
#include <cstring>
#include <string>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import editor.core;
import pipeline.importer;
import foundation.fonts;
import foundation.image;
import foundation.fonts.resource;
import fonts.pipeline;
import foundation.xml.serialization; // XmlSerializerFactory: the legacy-payload test rewrites a keyed file

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::fonts;

namespace
{
    constexpr StringView kSourceFont = u8"scratch_fontpipe_src.ttf";

    void RemoveTree()
    {
        FileDelete(kSourceFont);
        FileDelete(u8"scratch_fontpipe_out_db/uifont.rasset");
        FileDelete(u8"scratch_fontpipe_out_db/uifont.data.bin");
        RemoveDirectory(u8"scratch_fontpipe_out_db");
    }

    // Copy the data root's DejaVu Mono beside the test (the sources mount is the CWD).
    bool StageSourceFont()
    {
        const String root = FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return FileCopyPreserving(
            DataPath(root.AsView(), u8"Assets/fonts/dejavu/DejaVuSansMono.ttf").AsView(),
            kSourceFont);
    }
}

TEST_CASE("font.pipeline: FontAsset raster ramp -> cook -> rasterizer-free Font")
{
    RegisterFontAsset();
    RemoveTree();
    REQUIRE(StageSourceFont());

    NativeFileSystem outMount(u8"scratch_fontpipe_out_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());
        id = inst->Id();

        FontAsset asset;
        asset.fileName = foundation::vfs::SourcePath(kSourceFont);
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
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    FontFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);
    CHECK(font->Family() == u8"TestMono");
    REQUIRE(font->EntryCount() == 2u);
    // The font file came along (a shaper reads its tables), and every entry's font exposes it.
    REQUIRE_FALSE(font->SourceData().IsEmpty());
    CHECK(font->SourceData().Size() > 1000u);
    CHECK(font->EntryAt(0).font->SourceData().Data() == font->SourceData().Data());
    CHECK(font->EntryAt(1).font->SourceData().Size() == font->SourceData().Size());

    const Font::Entry* entry = font->ClosestEntry(12.0f);
    REQUIRE(entry != nullptr);
    CHECK(entry->pixelHeight == doctest::Approx(12.0f));
    REQUIRE(entry->font);
    CHECK(entry->font->HasGlyph('A'));
    CHECK(entry->font->GetGlyphInfo('A').advanceWidth > 0.0f);
    CHECK(entry->font->Metrics().ascent > 0.0f);
    REQUIRE(entry->atlas);
    CHECK(entry->atlas->Mode() == AtlasMode::Coverage);
    CHECK(entry->atlas->Contains(entry->atlas->GlyphFor('A')));
    GlyphQuad quad;
    f32 cursor = 0.0f;
    CHECK(entry->atlas->GetGlyphQuad(entry->atlas->GlyphFor('A'), cursor, 0.0f, quad));
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

    NativeFileSystem outMount(u8"scratch_fontpipe_out_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());
        id = inst->Id();

        FontAsset asset;
        asset.fileName = foundation::vfs::SourcePath(kSourceFont);
        asset.mode = FontBakeMode::DistanceField;
        asset.distanceFieldSize = 32.0f;
        asset.firstCodepoint = 'A';
        asset.lastCodepoint = 'Z'; // a small range keeps msdfgen quick
        asset.atlasWidth = 256;
        asset.atlasHeight = 256;

        FontAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    FontFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);
    REQUIRE(font->EntryCount() == 1u);
    const Font::Entry& entry = font->EntryAt(0);
    REQUIRE(entry.atlas);
    CHECK(entry.atlas->Mode() == AtlasMode::DistanceField);
    CHECK(entry.atlas->DistanceFieldRange() > 0.0f);
    CHECK(entry.atlas->Contains(entry.atlas->GlyphFor('Q')));
    REQUIRE(entry.atlasImage);
    CHECK(entry.atlasImage->ColorSpace() == foundation::image::ImageColorSpace::Linear);

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
    NativeFileSystem outMount(u8"scratch_fontpipe_out_db", DefaultAllocator());
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"uifont", FontResource::StaticType());

    FontAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"does_not_exist_xyz.ttf");
    FontAssetBuilder builder;
    NativeFileSystem srcMount(u8".", DefaultAllocator());
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &srcMount;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());
    RemoveTree();
}


// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// FontAtlasDiskCache: the editor's startup MSDF bake cache. Round-trip a real Roboto DF
// bake through Store/TryLoad and require byte-identical pixels + regions; keys must miss
// on any option change; a corrupt file must fail closed (miss, never a bad atlas).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.distancefield;
import foundation.fonts.distancefield.baker;
import foundation.vfs;
import editor.app;

using namespace foundation::core;
using namespace foundation::fonts;
using namespace foundation::vfs;

namespace
{
    String FontPath()
    {
        const String root = FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return DataPath(root.AsView(), u8"Engine/Fonts/roboto/Roboto-Regular.ttf");
    }

    IFont* LoadRoboto()
    {
        TrueTypeFontParser parser;
        Result<IFont*, FontLoadResult> parsed = parser.ParseFromFile(
            FontPath().AsView(), FontLoadOptions::Default(), DefaultAllocator());
        return parsed.HasValue() ? parsed.Value() : nullptr;
    }

    FontLoadOptions SmallDF()
    {
        FontLoadOptions options = FontLoadOptions::DistanceField();
        options.pixelHeight = 32.0f;
        options.firstCodepoint = 32;
        options.lastCodepoint = 96; // small range keeps the test quick
        options.atlasWidth = 512;
        options.atlasHeight = 512;
        return options;
    }

    void RemoveCacheDir(StringView dir)
    {
        NativeFileSystem fs(dir, DefaultAllocator());
        if (IEnumerableFileSystem* enumerable = fs.AsEnumerable())
        {
            Array<DirEntry> entries;
            if (enumerable->Enumerate(u8"", entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                    if (!entry.isDirectory)
                        (void)fs.AsWritable()->Delete(entry.name.AsView());
            }
        }
        RemoveDirectory(dir);
    }
}

TEST_CASE("font-atlas-cache: DF bakes round-trip byte-identically through the disk cache")
{
    const StringView dir = u8".fontcache_roundtrip";
    RemoveCacheDir(dir);

    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    const FontLoadOptions options = SmallDF();

    DistanceFieldFontAtlasBaker baker;
    Result<IFontAtlas*, FontLoadResult> baked = baker.Bake(*font, options, DefaultAllocator());
    REQUIRE(baked.HasValue());
    IFontAtlas* atlas = baked.Value();

    editor::FontAtlasDiskCache cache(DefaultAllocator(), dir);
    CHECK(cache.TryLoad(*font, options, DefaultAllocator()) == nullptr); // cold: miss
    cache.Store(*font, options, *atlas);

    IFontAtlas* loaded = cache.TryLoad(*font, options, DefaultAllocator());
    REQUIRE(loaded != nullptr);
    CHECK(loaded->Mode() == AtlasMode::DistanceField);
    CHECK(loaded->Width() == atlas->Width());
    CHECK(loaded->Height() == atlas->Height());
    CHECK(loaded->DistanceFieldRange() == atlas->DistanceFieldRange());
    CHECK(loaded->WhitePixelUV().x == atlas->WhitePixelUV().x);
    CHECK(loaded->WhitePixelUV().y == atlas->WhitePixelUV().y);

    for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
    {
        AtlasRegion a, b;
        CHECK(atlas->GlyphFor(cp) == loaded->GlyphFor(cp)); // the codepoint map round-trips too
        const bool ha = atlas->TryGetRegion(atlas->GlyphFor(cp), a);
        const bool hb = loaded->TryGetRegion(loaded->GlyphFor(cp), b);
        CHECK(ha == hb);
        if (ha && hb)
        {
            CHECK(a.x == b.x);
            CHECK(a.y == b.y);
            CHECK(a.width == b.width);
            CHECK(a.height == b.height);
            CHECK(a.advanceX == b.advanceX);
        }
    }
    const Span<const u8> pa = atlas->PixelData();
    const Span<const u8> pb = loaded->PixelData();
    REQUIRE(pa.Size() == pb.Size());
    CHECK(MemCompare(pa.Data(), pb.Data(), pa.Size()) == 0);

    DefaultAllocator().Delete(loaded);
    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
    RemoveCacheDir(dir);
}

TEST_CASE("font-atlas-cache: any option change misses; coverage bakes are declined")
{
    const StringView dir = u8".fontcache_keys";
    RemoveCacheDir(dir);

    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    const FontLoadOptions options = SmallDF();

    DistanceFieldFontAtlasBaker baker;
    Result<IFontAtlas*, FontLoadResult> baked = baker.Bake(*font, options, DefaultAllocator());
    REQUIRE(baked.HasValue());

    editor::FontAtlasDiskCache cache(DefaultAllocator(), dir);
    cache.Store(*font, options, *baked.Value());

    FontLoadOptions differentSize = options;
    differentSize.pixelHeight = 48.0f;
    CHECK(cache.TryLoad(*font, differentSize, DefaultAllocator()) == nullptr);

    FontLoadOptions differentRange = options;
    differentRange.lastCodepoint = 255;
    CHECK(cache.TryLoad(*font, differentRange, DefaultAllocator()) == nullptr);

    FontLoadOptions coverage = options;
    coverage.atlasMode = AtlasMode::Coverage;
    CHECK(cache.TryLoad(*font, coverage, DefaultAllocator()) == nullptr); // declined

    DefaultAllocator().Delete(baked.Value());
    DefaultAllocator().Delete(font);
    RemoveCacheDir(dir);
}

TEST_CASE("font-atlas-cache: a truncated cache file fails closed (miss, no bad atlas)")
{
    const StringView dir = u8".fontcache_corrupt";
    RemoveCacheDir(dir);

    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    const FontLoadOptions options = SmallDF();

    DistanceFieldFontAtlasBaker baker;
    Result<IFontAtlas*, FontLoadResult> baked = baker.Bake(*font, options, DefaultAllocator());
    REQUIRE(baked.HasValue());

    editor::FontAtlasDiskCache cache(DefaultAllocator(), dir);
    cache.Store(*font, options, *baked.Value());

    // Truncate the stored file to half: header parses but the pixel read must fail.
    NativeFileSystem fs(dir, DefaultAllocator());
    if (IEnumerableFileSystem* enumerable = fs.AsEnumerable())
    {
        Array<DirEntry> entries;
        REQUIRE(enumerable->Enumerate(u8"", entries).IsOk());
        REQUIRE(entries.Size() == 1u);
        UniquePtr<IStream> stream = fs.Open(entries[0].name.AsView(), FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        Array<byte> bytes;
        bytes.Resize(static_cast<usize>(stream->Size()) / 2);
        REQUIRE(stream->Read(bytes.Data(), bytes.Size()) == bytes.Size());
        stream = nullptr;
        REQUIRE(fs.AsWritable()
                    ->Save(entries[0].name.AsView(),
                           Span<const byte>{bytes.Data(), bytes.Size()})
                    .IsOk());
    }
    CHECK(cache.TryLoad(*font, options, DefaultAllocator()) == nullptr);

    DefaultAllocator().Delete(baked.Value());
    DefaultAllocator().Delete(font);
    RemoveCacheDir(dir);
}

// Ported from Sedulous.Fonts.Tests/FontImporterTests.bf - drives the editor-
// time baking pipeline (FontImporter::Bake) over the bundled Roboto asset and
// confirms the produced BakedFont + BakedFontAtlas carry usable data.
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.baked;
import draconic.fonts.importer;

using namespace draconic::foundation;
using namespace draconic::fonts;

namespace
{
    String AssetPath(const char* rel)
    {
        String p;
        for (const char* s = DRACONIC_FONTS_ASSET_DIR; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        for (const char* s = rel; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        return p;
    }

    // Read the bundled Roboto font bytes off disk.
    bool ReadFontBytes(Array<u8>& out)
    {
        FileStream fs(AssetPath("/roboto/Roboto-Regular.ttf"), FileMode::Read);
        if (!fs.IsValid())
            return false;
        const i64 length = fs.Size();
        if (length <= 0)
            return false;
        out.Resize(static_cast<usize>(length));
        return fs.Read(out.Data(), static_cast<u64>(length)) == static_cast<u64>(length);
    }
}

TEST_CASE("importer: bake produces glyphs, metrics, atlas regions")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    Result<BakedFontData*, FontLoadResult> result =
        FontImporter::Bake(Span<const u8>(bytes.Data(), bytes.Size()), FontLoadOptions::Default());
    REQUIRE(result.HasValue());
    BakedFontData* baked = result.Value();

    CHECK_FALSE(baked->font->FamilyName().IsEmpty());
    CHECK(baked->font->Metrics().ascent > 0);
    CHECK(baked->font->Metrics().descent < 0);
    CHECK(baked->font->Metrics().lineHeight > 0);
    CHECK(baked->font->PixelHeight() == FontLoadOptions::Default().pixelHeight);

    CHECK(baked->font->HasGlyph(static_cast<i32>('A')));
    CHECK(baked->font->HasGlyph(static_cast<i32>('z')));
    CHECK(baked->font->HasGlyph(static_cast<i32>('0')));
    CHECK(baked->atlas->Contains(static_cast<i32>('A')));
    CHECK(baked->atlas->Contains(static_cast<i32>('z')));
    CHECK(baked->atlas->Contains(static_cast<i32>('0')));

    CHECK(baked->font->GetGlyphInfo(static_cast<i32>('A')).advanceWidth > 0);

    DefaultAllocator().Delete(baked);
}

TEST_CASE("importer: baked atlas matches requested dimensions")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    FontLoadOptions opts = FontLoadOptions::Default();
    opts.atlasWidth = 256;
    opts.atlasHeight = 256;
    opts.firstCodepoint = static_cast<i32>('A');
    opts.lastCodepoint = static_cast<i32>('Z');

    Result<BakedFontData*, FontLoadResult> result =
        FontImporter::Bake(Span<const u8>(bytes.Data(), bytes.Size()), opts);
    REQUIRE(result.HasValue());
    BakedFontData* baked = result.Value();

    CHECK(baked->atlas->Width() == 256);
    CHECK(baked->atlas->Height() == 256);
    CHECK(baked->atlas->PixelData().Size() == 256u * 256u);

    DefaultAllocator().Delete(baked);
}

TEST_CASE("importer: bake fails cleanly on garbage bytes")
{
    Array<u8> junk;
    junk.Resize(256);
    for (usize i = 0; i < junk.Size(); ++i)
        junk[i] = 0xAB;

    Result<BakedFontData*, FontLoadResult> result =
        FontImporter::Bake(Span<const u8>(junk.Data(), junk.Size()), FontLoadOptions::Default());
    CHECK_FALSE(result.HasValue());
}

TEST_CASE("importer: TakeOwnership nulls the BakedFontData fields")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    Result<BakedFontData*, FontLoadResult> result =
        FontImporter::Bake(Span<const u8>(bytes.Data(), bytes.Size()), FontLoadOptions::Default());
    REQUIRE(result.HasValue());
    BakedFontData* bakedData = result.Value();

    BakedFont* takenFont = nullptr;
    BakedFontAtlas* takenAtlas = nullptr;
    bakedData->TakeOwnership(takenFont, takenAtlas);
    CHECK(takenFont != nullptr);
    CHECK(takenAtlas != nullptr);
    CHECK(bakedData->font == nullptr);
    CHECK(bakedData->atlas == nullptr);

    DefaultAllocator().Delete(bakedData); // must not touch the taken objects
    DefaultAllocator().Delete(takenFont);
    DefaultAllocator().Delete(takenAtlas);
}

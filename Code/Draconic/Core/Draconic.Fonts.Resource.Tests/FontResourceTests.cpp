// Model-A font runtime load: author a cooked FontResource (record + "data" atlas stream)
// into a content DB, then load it through the ResourceManager with the device-free
// FontFactory and verify the rasterizer-free product (glyphs, kerning, atlas regions,
// RGBA expansion, MSDF metadata).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.fonts;
import draconic.fonts.resource;
import draconic.image;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::fonts;

namespace
{
    void RemoveTree(StringView dir)
    {
        FileDelete(PathJoin(dir, u8"font.rasset"));
        FileDelete(PathJoin(dir, u8"font.data.bin"));
        RemoveDirectory(dir);
    }

    // A tiny two-entry record: an 'A' glyph with one kerning pair per entry, 2x2 atlases.
    void AuthorFont(FontResource& res, FontResourcePixels mode)
    {
        res.family = String(u8"TestFamily");
        res.pixels = mode;
        const u32 bpp = (mode == FontResourcePixels::DistanceField) ? 4u : 1u;
        for (u32 i = 0; i < 2; ++i)
        {
            FontResourceEntry entry;
            entry.pixelHeight = (i == 0) ? 12.0f : 24.0f;
            entry.ascent = entry.pixelHeight * 0.8f;
            entry.descent = -entry.pixelHeight * 0.2f;
            entry.lineGap = 1.0f;
            entry.scale = 1.0f;

            FontResourceGlyph glyph;
            glyph.codepoint = 'A';
            glyph.info.codepoint = 'A';
            glyph.info.glyphIndex = 1;
            glyph.info.advanceWidth = entry.pixelHeight * 0.5f;
            glyph.info.hasBitmap = true;
            entry.glyphs.PushBack(glyph);

            FontResourceKerning kerning;
            kerning.first = 'A';
            kerning.second = 'V';
            kerning.amount = -1.5f;
            entry.kerning.PushBack(kerning);

            FontResourceRegion region;
            region.codepoint = 'A';
            region.region = AtlasRegion(0, 0, 2, 2, 0.0f, -entry.ascent, glyph.info.advanceWidth);
            entry.regions.PushBack(region);

            entry.atlasWidth = 2;
            entry.atlasHeight = 2;
            entry.whitePixelU = 0.25f;
            entry.whitePixelV = 0.25f;
            entry.dfPixelRange = 3.0f;
            entry.pixelOffset = static_cast<u64>(i) * 4u * bpp;
            entry.pixelBytes = 4u * bpp;
            res.entries.PushBack(Move(entry));
        }
    }
}

TEST_CASE("font.factory: cooked Alpha8 FontResource -> rasterizer-free Font product")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontfac_a8_db";
    RemoveTree(dir);

    NativeFileSystem mount(dir);
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"font", FontResource::StaticType());
        id = inst->Id();

        FontResource res;
        AuthorFont(res, FontResourcePixels::Alpha8);
        REQUIRE(inst->WriteObject(res).IsOk());

        // Two entries x 2x2 alpha texels, concatenated.
        u8 pixels[8];
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(0x10 * (i + 1));
        }
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    ResourceManager manager(db);
    FontFactory factory;
    manager.AddFactory(&factory);

    auto proxy = manager.Bind<Font>(id);
    REQUIRE(proxy);
    Font* font = proxy.Get();
    REQUIRE(font != nullptr);
    CHECK(font->Family() == u8"TestFamily");
    REQUIRE(font->EntryCount() == 2u);

    const Font::Entry* closest = font->ClosestEntry(13.0f);
    REQUIRE(closest != nullptr);
    CHECK(closest->pixelHeight == doctest::Approx(12.0f));
    CHECK(font->ClosestEntry(100.0f)->pixelHeight == doctest::Approx(24.0f));

    // The rasterizer-free tables round-tripped.
    REQUIRE(closest->font);
    CHECK(closest->font->HasGlyph('A'));
    CHECK(closest->font->GetGlyphInfo('A').advanceWidth == doctest::Approx(6.0f));
    CHECK(closest->font->GetKerning('A', 'V') == doctest::Approx(-1.5f));
    CHECK(closest->font->Metrics().ascent == doctest::Approx(12.0f * 0.8f));

    // Atlas: coverage mode expands to RGBA8; regions resolve to quads.
    REQUIRE(closest->atlas);
    CHECK(closest->atlas->Mode() == AtlasMode::Coverage);
    AtlasRegion region;
    CHECK(closest->atlas->TryGetRegion('A', region));
    CHECK(region.width == 2);
    REQUIRE(closest->atlasImage);
    CHECK(closest->atlasImage->Format() == draconic::image::PixelFormat::RGBA8);
    CHECK(closest->atlasImage->PixelData().Size() == 2u * 2u * 4u);
    // Alpha channel carries the coverage texel (entry 0 starts at offset 0: 0x10).
    CHECK(closest->atlasImage->PixelData()[3] == 0x10);

    RemoveTree(dir);
}

TEST_CASE("font.factory: async load matches the sync product")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontfac_async_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir);
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"font", FontResource::StaticType());
        id = inst->Id();
        FontResource res;
        AuthorFont(res, FontResourcePixels::Alpha8);
        REQUIRE(inst->WriteObject(res).IsOk());
        u8 pixels[8];
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(0x10 * (i + 1));
        }
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    FontFactory factory;

    ResourceManager syncManager(db);
    syncManager.AddFactory(&factory);
    auto a = syncManager.Bind<Font>(id);
    REQUIRE(a);

    JobSystem jobs;
    ResourceManager asyncManager(db, &jobs);
    asyncManager.AddFactory(&factory);
    auto b = asyncManager.BindAsync<Font>(id);
    asyncManager.WaitAll();
    REQUIRE(b);
    CHECK(b.Handle()->State() == ResourceState::Ready);

    CHECK(b->Family() == a->Family());
    REQUIRE(b->EntryCount() == a->EntryCount());
    const Font::Entry* ea = a->ClosestEntry(13.0f);
    const Font::Entry* eb = b->ClosestEntry(13.0f);
    REQUIRE(ea);
    REQUIRE(eb);
    CHECK(eb->pixelHeight == doctest::Approx(ea->pixelHeight));
    REQUIRE(eb->atlasImage);
    REQUIRE(ea->atlasImage);
    CHECK(eb->atlasImage->PixelData().Size() == ea->atlasImage->PixelData().Size());

    RemoveTree(dir);
}

TEST_CASE("font.factory: many concurrent async decodes run without a race")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontfac_conc_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir);

    constexpr int kCount = 10;
    Array<Guid> ids;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        for (int i = 0; i < kCount; ++i)
        {
            char8_t name[8] = {u8'f', u8'n', u8't', static_cast<char8_t>(u8'0' + i / 10),
                               static_cast<char8_t>(u8'0' + i % 10), 0};
            auto* inst = db.RootGroup()->CreateInstance(StringView(name), FontResource::StaticType());
            FontResource res;
            AuthorFont(res, FontResourcePixels::Alpha8);
            REQUIRE(inst->WriteObject(res).IsOk());
            u8 pixels[8];
            for (usize j = 0; j < sizeof(pixels); ++j)
            {
                pixels[j] = static_cast<u8>(0x10 * (j + 1));
            }
            REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                               sizeof(pixels)))
                        .IsOk());
            ids.PushBack(inst->Id());
        }
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    FontFactory factory;
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
    manager.AddFactory(&factory);

    Array<Proxy<Font>> fonts;
    for (const Guid& id : ids)
    {
        fonts.PushBack(manager.BindAsync<Font>(id));
    }
    manager.WaitAll();

    for (Proxy<Font>& font : fonts)
    {
        REQUIRE(font);
        CHECK(font.Handle()->State() == ResourceState::Ready);
        CHECK(font->EntryCount() == 2u);
    }

    RemoveTree(dir);
}

TEST_CASE("font.factory: cooked MSDF FontResource keeps range + linear RGBA")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontfac_df_db";
    RemoveTree(dir);

    NativeFileSystem mount(dir);
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"font", FontResource::StaticType());
        id = inst->Id();

        FontResource res;
        AuthorFont(res, FontResourcePixels::DistanceField);
        REQUIRE(inst->WriteObject(res).IsOk());

        u8 pixels[2 * 4 * 4]; // two entries x 2x2 RGBA texels
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(i);
        }
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    ResourceManager manager(db);
    FontFactory factory;
    manager.AddFactory(&factory);

    auto proxy = manager.Bind<Font>(id);
    REQUIRE(proxy);
    REQUIRE(proxy->EntryCount() == 2u);
    const Font::Entry& entry = proxy->EntryAt(1);
    REQUIRE(entry.atlas);
    CHECK(entry.atlas->Mode() == AtlasMode::DistanceField);
    CHECK(entry.atlas->DistanceFieldRange() == doctest::Approx(3.0f));
    REQUIRE(entry.atlasImage);
    CHECK(entry.atlasImage->ColorSpace() == draconic::image::ImageColorSpace::Linear);
    // Entry 1's slice starts at pixelOffset 16.
    CHECK(entry.atlasImage->PixelData()[0] == 16);

    RemoveTree(dir);
}

TEST_CASE("font.service: ResourceFontService resolves (family, size) over bound products")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontsvc_db";
    RemoveTree(dir);

    NativeFileSystem mount(dir);
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"font", FontResource::StaticType());
        id = inst->Id();
        FontResource res;
        AuthorFont(res, FontResourcePixels::Alpha8);
        REQUIRE(inst->WriteObject(res).IsOk());
        u8 pixels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    ResourceManager manager(db);
    FontFactory factory;
    manager.AddFactory(&factory);
    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);

    ResourceFontService service;
    service.AddFont(font.Get());
    CHECK(service.DefaultFontFamily() == u8"TestFamily");

    // Exact + closest + default-family fallback.
    CachedFont* at12 = service.GetFont(u8"TestFamily", 12.0f);
    REQUIRE(at12 != nullptr);
    CHECK(at12->font->PixelHeight() == doctest::Approx(12.0f));
    CHECK(at12->shaper != nullptr); // the IFont-generic TrueTypeTextShaper
    CachedFont* at100 = service.GetFont(100.0f); // default family, closest = 24
    REQUIRE(at100 != nullptr);
    CHECK(at100->font->PixelHeight() == doctest::Approx(24.0f));
    CachedFont* unknownFamily = service.GetFont(u8"NoSuchFamily", 12.0f);
    CHECK(unknownFamily == at12); // falls back to the default family

    // The atlas texture resolves per cached font and per (family, size).
    CHECK(service.GetAtlasTexture(at12) != nullptr);
    CHECK(service.GetAtlasTexture(u8"TestFamily", 24.0f) != nullptr);
    CHECK(service.GetAtlasTexture(at12) != service.GetAtlasTexture(u8"TestFamily", 24.0f));

    // Shaping works end-to-end over the baked tables ('A' has an advance; kerning applies).
    Array<GlyphPosition> positions;
    Result<f32> width = at12->shaper->ShapeText(*at12->font, u8"A", positions);
    REQUIRE(width.HasValue());
    CHECK(width.Value() == doctest::Approx(6.0f)); // 12px * 0.5 advance from AuthorFont

    RemoveTree(dir);
}

TEST_CASE("font.service: DF families synthesize cached per-size scaled views")
{
    RegisterFontResource();
    const StringView dir = u8"draconic_fontsvc_df_db";
    RemoveTree(dir);

    NativeFileSystem mount(dir);
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"font", FontResource::StaticType());
        id = inst->Id();
        FontResource res;
        AuthorFont(res, FontResourcePixels::DistanceField);
        REQUIRE(inst->WriteObject(res).IsOk());
        u8 pixels[32] = {};
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    ResourceManager manager(db);
    FontFactory factory;
    manager.AddFactory(&factory);
    Proxy<Font> font = manager.Bind<Font>(id);
    REQUIRE(font);

    ResourceFontService service;
    service.AddFont(font.Get());

    // Baked sizes resolve to the products themselves (no view wrapping).
    CachedFont* at12 = service.GetFont(u8"TestFamily", 12.0f);
    REQUIRE(at12 != nullptr);
    CHECK(at12->font->PixelHeight() == doctest::Approx(12.0f));

    // A non-baked size gets a scaled view over the closest bake (12px), not the raw tables.
    CachedFont* at16 = service.GetFont(u8"TestFamily", 16.0f);
    REQUIRE(at16 != nullptr);
    CHECK(at16 != at12);
    CHECK(at16->font->PixelHeight() == doctest::Approx(16.0f));
    CHECK(at16->atlas->Mode() == AtlasMode::DistanceField);
    CHECK(at16->atlas->DistanceFieldRange() == doctest::Approx(3.0f));
    // Advance scales with the view: 6.0 at the 12px bake -> 8.0 at 16px.
    CHECK(at16->font->GetGlyphInfo('A').advanceWidth == doctest::Approx(8.0f));

    // The synthesized entry is cached: the same size returns the same CachedFont, and its
    // atlas texture is the BASE bake's image.
    CHECK(service.GetFont(u8"TestFamily", 16.0f) == at16);
    CHECK(service.GetAtlasTexture(at16) != nullptr);
    CHECK(service.GetAtlasTexture(at16) == service.GetAtlasTexture(at12));

    // Shaping runs over the view (IFont-generic shaper).
    Array<GlyphPosition> positions;
    Result<f32> width = at16->shaper->ShapeText(*at16->font, u8"A", positions);
    REQUIRE(width.HasValue());
    CHECK(width.Value() == doctest::Approx(8.0f));

    RemoveTree(dir);
}

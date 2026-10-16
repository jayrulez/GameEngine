// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.Resource - the `foundation.fonts.resource` module (runtime).
//
// The FONT as a runtime resource - the runtime-resource tier of the font triad.
// Same model-A shape as foundation.texture.resource:
//
//   * FontResource (ISerializable): the cooked *record* loaded from the output DB - the
//     family, the atlas pixel mode (Alpha8 coverage or RGBA MSDF), and one ENTRY per baked
//     pixel size (metrics + glyph/kerning/region tables + atlas metadata). The atlas pixel
//     payloads live concatenated in the "data" stream, per-entry offset+size recorded.
//   * Font (Object): the runtime product - owns, per entry, a rasterizer-free BakedFont,
//     its IFontAtlas (BakedFontAtlas for coverage, DistanceFieldFontAtlas for MSDF), and the atlas as
//     an image::OwnedImageData ready for a renderer to upload (the same RGBA expansion the
//     TTF service performs). A packaged game loads THIS and never touches stb_truetype.
//   * FontFactory (IResourceFactory): cooked record + stream -> Font. DEVICE-FREE - the VG
//     layer uploads atlas images itself, so fonts bind headlessly.
//
// The runtime never links the editor/source side; it loads only cooked resources.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.fonts.resource;

import foundation.core;
import foundation.fonts;
import foundation.fonts.coverage;
import foundation.fonts.truetype;
import foundation.fonts.distancefield;
import foundation.image;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::fonts
{
    namespace image = foundation::image;

    // How the cooked atlas payload is encoded (uniform across a resource's entries).
    enum class FontResourcePixels : u32
    {
        Coverage,      // single-channel Alpha8 coverage (expanded to RGBA8 at load, like the TTF path)
        DistanceField, // RGBA8 MSDF channels, linear (no sRGB decode - geometric data)
    };

    // -- serialized table rows ----------------------------------------------------------

    struct FontResourceGlyph
    {
        i32 codepoint = 0;
        GlyphInfo info;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "codepoint", codepoint);
            foundation::core::Serialize(ar, "glyphIndex", info.glyphIndex);
            foundation::core::Serialize(ar, "advanceWidth", info.advanceWidth);
            foundation::core::Serialize(ar, "leftSideBearing", info.leftSideBearing);
            foundation::core::Serialize(ar, "boundsX", info.boundingBox.x);
            foundation::core::Serialize(ar, "boundsY", info.boundingBox.y);
            foundation::core::Serialize(ar, "boundsWidth", info.boundingBox.width);
            foundation::core::Serialize(ar, "boundsHeight", info.boundingBox.height);
            foundation::core::Serialize(ar, "hasBitmap", info.hasBitmap);
            if (ar.Mode() == SerializeMode::Read)
            {
                info.codepoint = codepoint;
            }
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceGlyph& g)
    {
        ar.BeginObject();
        g.Serialize(ar);
        ar.EndObject();
    }

    struct FontResourceKerning
    {
        i32 first = 0;
        i32 second = 0;
        f32 amount = 0.0f;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "first", first);
            foundation::core::Serialize(ar, "second", second);
            foundation::core::Serialize(ar, "amount", amount);
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceKerning& k)
    {
        ar.BeginObject();
        k.Serialize(ar);
        ar.EndObject();
    }

    // A glyph's place in the atlas, by glyph index (the glyph rows map codepoints to glyphs).
    struct FontResourceRegion
    {
        i32 glyph = 0;
        AtlasRegion region;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "glyph", glyph);
            foundation::core::Serialize(ar, "x", region.x);
            foundation::core::Serialize(ar, "y", region.y);
            foundation::core::Serialize(ar, "width", region.width);
            foundation::core::Serialize(ar, "height", region.height);
            foundation::core::Serialize(ar, "offsetX", region.offsetX);
            foundation::core::Serialize(ar, "offsetY", region.offsetY);
            foundation::core::Serialize(ar, "advanceX", region.advanceX);
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceRegion& r)
    {
        ar.BeginObject();
        r.Serialize(ar);
        ar.EndObject();
    }

    // One baked pixel size: metrics + tables + atlas metadata; pixels are the
    // [pixelOffset, pixelOffset + pixelBytes) slice of the resource's "data" stream.
    struct FontResourceEntry
    {
        f32 pixelHeight = 0.0f;

        // FontMetrics, flattened (lineHeight/decorations recompute from these on load).
        f32 ascent = 0.0f;
        f32 descent = 0.0f;
        f32 lineGap = 0.0f;
        f32 scale = 1.0f;

        Array<FontResourceGlyph> glyphs;
        Array<FontResourceKerning> kerning;
        Array<FontResourceRegion> regions;

        u32 atlasWidth = 0;
        u32 atlasHeight = 0;
        f32 whitePixelU = 0.0f;
        f32 whitePixelV = 0.0f;
        f32 distanceFieldPixelRange = 4.0f; // DistanceField mode only

        // Pack-time oversampling (v1+): region spans are RAW atlas pixels at oversample times
        // the logical glyph size; the runtime atlas divides screen quads back down. v0 cooked
        // fonts lack the field and default to 1 - they draw oversized until re-cooked (the
        // builder version bump forces that re-cook).
        f32 oversampleX = 1.0f;
        f32 oversampleY = 1.0f;

        u64 pixelOffset = 0;
        u64 pixelBytes = 0;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "pixelHeight", pixelHeight);
            foundation::core::Serialize(ar, "ascent", ascent);
            foundation::core::Serialize(ar, "descent", descent);
            foundation::core::Serialize(ar, "lineGap", lineGap);
            foundation::core::Serialize(ar, "scale", scale);
            foundation::core::Serialize(ar, "glyphs", glyphs);
            foundation::core::Serialize(ar, "kerning", kerning);
            foundation::core::Serialize(ar, "regions", regions);
            foundation::core::Serialize(ar, "atlasWidth", atlasWidth);
            foundation::core::Serialize(ar, "atlasHeight", atlasHeight);
            foundation::core::Serialize(ar, "whitePixelU", whitePixelU);
            foundation::core::Serialize(ar, "whitePixelV", whitePixelV);
            foundation::core::Serialize(ar, "distanceFieldPixelRange", distanceFieldPixelRange);
            foundation::core::Serialize(ar, "oversampleX", oversampleX);
            foundation::core::Serialize(ar, "oversampleY", oversampleY);
            foundation::core::Serialize(ar, "pixelOffset", pixelOffset);
            foundation::core::Serialize(ar, "pixelBytes", pixelBytes);
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceEntry& e)
    {
        ar.BeginObject();
        e.Serialize(ar);
        ar.EndObject();
    }

    // Cooked font record (output DB). Atlas pixels are the "data" stream; the font file itself is the
    // "font" stream, for a shaper that reads its tables (absent in fonts cooked before it was kept).
    class FontResource final : public ISerializable
    {
        RTTI_OBJECT(FontResource, ISerializable)
    public:
        String family;
        FontResourcePixels pixels = FontResourcePixels::Coverage;
        Array<FontResourceEntry> entries;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "family", family);
            u32 mode = static_cast<u32>(pixels);
            foundation::core::Serialize(ar, "pixels", mode);
            pixels = static_cast<FontResourcePixels>(mode);
            foundation::core::Serialize(ar, "entries", entries);
        }
    };

    // Runtime product: rasterizer-free font + atlas + uploadable atlas image, per entry.
    class Font final : public Object
    {
        RTTI_OBJECT(Font, Object)
    public:
        struct Entry
        {
            f32 pixelHeight = 0.0f;
            UniquePtr<BakedFont> font;
            UniquePtr<IFontAtlas> atlas;        // BakedFontAtlas or DistanceFieldFontAtlas
            UniquePtr<image::OwnedImageData> atlasImage; // RGBA8, renderer-uploadable
        };

        Font() = default;
        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;

        void SetFamily(StringView family) { m_family = String(family); }
        void AddEntry(Entry&& entry) { m_entries.PushBack(Move(entry)); }
        /// The font file's bytes, set before the entries (their fonts borrow them).
        void SetSourceData(Array<u8>&& bytes) { m_sourceData = Move(bytes); }
        [[nodiscard]] Span<const u8> SourceData() const noexcept
        {
            return Span<const u8>(m_sourceData.Data(), m_sourceData.Size());
        }

        [[nodiscard]] StringView Family() const noexcept { return m_family.AsView(); }
        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] const Entry& EntryAt(usize index) const { return m_entries[index]; }

        // The entry whose pixelHeight is closest to `pixelHeight` (null when empty). The
        // font service maps (family, size) requests through this.
        [[nodiscard]] const Entry* ClosestEntry(f32 pixelHeight) const
        {
            const Entry* best = nullptr;
            f32 bestDistance = 0.0f;
            for (const Entry& entry : m_entries)
            {
                const f32 distance = entry.pixelHeight > pixelHeight
                                         ? entry.pixelHeight - pixelHeight
                                         : pixelHeight - entry.pixelHeight;
                if (best == nullptr || distance < bestDistance)
                {
                    best = &entry;
                    bestDistance = distance;
                }
            }
            return best;
        }

    private:
        String m_family;
        Array<u8> m_sourceData; // the font file (empty when the cook did not keep it)
        Array<Entry> m_entries;
    };

    // Cooked FontResource -> runtime Font. Device-free: atlas images stay CPU-side
    // (image::OwnedImageData); the VG layer uploads them like any other font texture.
    class FontFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every font product this factory creates (required -
        // the application that registers the factory decides).
        explicit FontFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Font::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &FontResource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            return BuildFont(instance);
        }

        // Async (task #123): the Font is a pure-CPU product (rasterizer-free glyph/kerning tables +
        // CPU atlas images the renderer uploads itself), so the whole build - including the heavy
        // atlas pixel read and per-entry slice copies - runs on a JobSystem worker via DecodeStage
        // and FinalizeStage is a no-op. Safe: content-DB reads open independent streams and the
        // FontResource + Font types are registered on the main thread at startup.
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override
        {
            return BuildFont(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] RefPtr<Object> BuildFont(foundation::content::Instance& instance) const
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            FontResource* res = Cast<FontResource>(object.Get());
            if (res == nullptr)
            {
                return RefPtr<Object>{};
            }

            // The concatenated atlas payloads.
            Array<u8> pixels;
            if (UniquePtr<IStream> stream = instance.ReadData(u8"data"))
            {
                const i64 size = stream->Size();
                if (size > 0)
                {
                    pixels.Resize(static_cast<usize>(size));
                    if (stream->Read(pixels.Data(), static_cast<u64>(size)) !=
                        static_cast<u64>(size))
                    {
                        pixels.Clear();
                    }
                }
            }

            RefPtr<Font> product = MakeRef<Font>(*m_allocator);
            product->SetFamily(res->family.AsView());
            if (UniquePtr<IStream> stream = instance.ReadData(u8"font"))
            {
                const i64 size = stream->Size();
                Array<u8> bytes;
                if (size > 0)
                {
                    bytes.Resize(static_cast<usize>(size));
                    if (stream->Read(bytes.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
                    {
                        bytes.Clear();
                    }
                }
                product->SetSourceData(Move(bytes));
            }

            for (const FontResourceEntry& e : res->entries)
            {
                Font::Entry entry;
                entry.pixelHeight = e.pixelHeight;

                // The rasterizer-free IFont.
                entry.font = MakeUnique<BakedFont>(*m_allocator);
                entry.font->SetFamilyName(res->family.AsView());
                entry.font->SetSourceData(product->SourceData());
                entry.font->SetPixelHeight(e.pixelHeight);
                entry.font->SetMetrics(
                    FontMetrics(e.ascent, e.descent, e.lineGap, e.pixelHeight, e.scale));
                for (const FontResourceGlyph& g : e.glyphs)
                {
                    entry.font->SetGlyph(g.codepoint, g.info);
                }
                for (const FontResourceKerning& k : e.kerning)
                {
                    entry.font->SetKerning(k.first, k.second, k.amount);
                }

                // The entry's atlas pixel slice.
                Array<u8> slice;
                if (e.pixelBytes > 0 && e.pixelOffset + e.pixelBytes <= pixels.Size())
                {
                    slice.Resize(static_cast<usize>(e.pixelBytes));
                    MemCopy(slice.Data(), pixels.Data() + e.pixelOffset,
                            static_cast<usize>(e.pixelBytes));
                }

                if (res->pixels == FontResourcePixels::DistanceField)
                {
                    auto atlas = MakeUnique<DistanceFieldFontAtlas>(*m_allocator);
                    atlas->SetPixelRange(e.distanceFieldPixelRange);
                    atlas->SetWhitePixelUV(e.whitePixelU, e.whitePixelV);
                    for (const FontResourceRegion& r : e.regions)
                    {
                        atlas->SetRegion(r.glyph, r.region);
                    }
                    for (const FontResourceGlyph& g : e.glyphs)
                    {
                        if (g.info.glyphIndex > 0)
                        {
                            atlas->SetGlyphFor(g.codepoint, g.info.glyphIndex);
                        }
                    }
                    // RGBA8 MSDF channels, LINEAR (geometric data, not color).
                    entry.atlasImage = MakeUnique<image::OwnedImageData>(
                        *m_allocator, e.atlasWidth, e.atlasHeight,
                        image::PixelFormat::RGBA8,
                        Span<const u8>(slice.Data(), slice.Size()),
                        image::ImageColorSpace::Linear);
                    atlas->SetPixels(e.atlasWidth, e.atlasHeight, Move(slice));
                    entry.atlas = Move(atlas); // converting move: DistanceFieldFontAtlas -> IFontAtlas
                }
                else
                {
                    auto atlas = MakeUnique<BakedFontAtlas>(*m_allocator);
                    atlas->SetWhitePixelUV(e.whitePixelU, e.whitePixelV);
                    atlas->SetOversample(e.oversampleX, e.oversampleY);
                    for (const FontResourceRegion& r : e.regions)
                    {
                        atlas->SetRegion(r.glyph, r.region);
                    }
                    for (const FontResourceGlyph& g : e.glyphs)
                    {
                        if (g.info.glyphIndex > 0)
                        {
                            atlas->SetGlyphFor(g.codepoint, g.info.glyphIndex);
                        }
                    }
                    atlas->SetPixels(e.atlasWidth, e.atlasHeight, Move(slice));
                    // Same RGBA8 expansion the TTF service performs for coverage atlases.
                    entry.atlasImage = UniquePtr<image::OwnedImageData>(
                        FontAtlasTexture::ExpandR8ToRGBA8(atlas.Get(), *m_allocator), *m_allocator);
                    entry.atlas = Move(atlas); // converting move: BakedFontAtlas -> IFontAtlas
                }

                if (!entry.atlasImage)
                {
                    continue; // a corrupt entry never produces a half-usable font size
                }
                product->AddEntry(Move(entry));
            }

            if (product->EntryCount() == 0)
            {
                return RefPtr<Object>{};
            }
            return product;
        }

        IAllocator* m_allocator;
    };

    // IFontService over bound Font PRODUCTS - the GUID-addressable runtime font path
    // (fonts are GUID-addressable through the content DB/ResourceManager).
    // The host binds Font products by guid (the ResourceManager keeps them cached) and
    // registers them here; the UI resolves (family, size) against the products' entries.
    //
    // CachedFont OWNERSHIP: CachedFont's destructor deletes its font/atlas/shaper. The
    // font+atlas here belong to the PRODUCT, so Clear() nulls those two before deleting
    // the CachedFont; the per-entry shaper is service-created (TrueTypeTextShaper is
    // IFont-generic - it only reads GetGlyphInfo/Metrics) and stays owned by the
    // CachedFont for normal deletion.
    class ResourceFontService final : public IFontService
    {
    public:
        // The allocator (required - the owner decides) backs cache aggregates,
        // shapers, and scaled views this service creates.
        explicit ResourceFontService(IAllocator& allocator) noexcept : m_allocator(&allocator) {}
        ~ResourceFontService() override { Clear(); }
        ResourceFontService(const ResourceFontService&) = delete;
        ResourceFontService& operator=(const ResourceFontService&) = delete;

        void Clear()
        {
            for (Entry& entry : m_entries)
            {
                if (entry.cached != nullptr)
                {
                    if (!entry.ownsViews)
                    {
                        entry.cached->font = nullptr;  // product-owned
                        entry.cached->atlas = nullptr; // product-owned
                    }
                    m_allocator->Delete(entry.cached); // frees shaper (+ owned views)
                }
            }
            m_entries.Clear();
            m_defaultFamily.Clear();
        }

        // Register a bound product (must be HEAP-allocated/refcounted - a resource product;
        // never a stack Font). The service takes a STRONG ref: a mid-session re-cook
        // replaces the manager's product (the old one is parked briefly, then freed), and a
        // borrowed pointer here kept serving freed glyph tables - the one-shot editor segfault
        // right after "cook finished" (2026-08-12). The ref keeps the OLD product alive (and
        // stale) until a new default font is bound; live REBIND on resource reload is the
        // follow-up that makes it also fresh.
        void AddFont(const Font* font)
        {
            if (font == nullptr)
            {
                return;
            }
            m_keepAlive.PushBack(RefPtr<Object>(const_cast<Font*>(font))); // ownership only
            for (usize i = 0; i < font->EntryCount(); ++i)
            {
                const Font::Entry& productEntry = font->EntryAt(i);
                Entry entry;
                entry.family = String(font->Family());
                entry.pixelHeight = productEntry.pixelHeight;
                entry.cached = m_allocator->New<CachedFont>(
                    *m_allocator, productEntry.font.Get(), productEntry.atlas.Get(),
                    MakeShaper());
                entry.image = productEntry.atlasImage.Get();
                m_entries.PushBack(Move(entry));
            }
            if (m_defaultFamily.IsEmpty() && font->EntryCount() > 0)
            {
                m_defaultFamily = String(font->Family());
            }
        }

        void SetDefaultFamily(StringView family) { m_defaultFamily = String(family); }

        // --- IFontService -------------------------------------------------------------
        [[nodiscard]] CachedFont* GetFont(f32 pixelHeight) override
        {
            return GetFont(m_defaultFamily.AsView(), pixelHeight);
        }

        [[nodiscard]] CachedFont* GetFont(StringView familyName, f32 pixelHeight) override
        {
            if (Entry* exact = FindExact(familyName, pixelHeight))
            {
                return exact->cached;
            }
            Entry* entry = FindClosest(familyName, pixelHeight);
            if (entry == nullptr)
            {
                entry = FindClosest(m_defaultFamily.AsView(), pixelHeight);
            }
            if (entry == nullptr)
            {
                return nullptr;
            }
            // Distance-field families serve every size from one bake: hand out a cached
            // per-size scaled view instead of the bake-size tables.
            if (entry->cached != nullptr && entry->cached->atlas != nullptr &&
                entry->cached->atlas->Mode() == AtlasMode::DistanceField &&
                entry->pixelHeight != pixelHeight)
            {
                return SynthesizeScaled(*entry, pixelHeight);
            }
            return entry->cached;
        }

        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(CachedFont* font) override
        {
            for (Entry& entry : m_entries)
            {
                if (entry.cached == font)
                {
                    return entry.image;
                }
            }
            return nullptr;
        }

        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(StringView familyName,
                                                                  f32 pixelHeight) override
        {
            Entry* entry = FindClosest(familyName, pixelHeight);
            if (entry == nullptr)
            {
                entry = FindClosest(m_defaultFamily.AsView(), pixelHeight);
            }
            return entry != nullptr ? entry->image : nullptr;
        }

        void ReleaseFont(CachedFont*) override {} // products own the payload lifetime

        [[nodiscard]] bool HasFamily(StringView familyName) const override
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.family.AsView() == familyName)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] StringView DefaultFontFamily() const override
        {
            return m_defaultFamily.AsView();
        }

    private:
        // The shaper a font gets: the registered backend's (HarfBuzz, when installed), else the
        // basic one.
        [[nodiscard]] ITextShaper* MakeShaper()
        {
            if (ITextShaper* shaper = TextShaperFactory::Create(*m_allocator))
            {
                return shaper;
            }
            return m_allocator->New<TrueTypeTextShaper>();
        }

        struct Entry
        {
            String family;
            f32 pixelHeight = 0.0f;
            CachedFont* cached = nullptr;              // owned wrapper (see class comment)
            foundation::image::ImageData* image = nullptr; // product-owned
            bool ownsViews = false; // scaled entry: cached->font/atlas are owned view wrappers
        };

        [[nodiscard]] Entry* FindExact(StringView family, f32 pixelHeight)
        {
            for (Entry& entry : m_entries)
            {
                const f32 diff = entry.pixelHeight > pixelHeight
                                     ? entry.pixelHeight - pixelHeight
                                     : pixelHeight - entry.pixelHeight;
                if (entry.family.AsView() == family && diff < 0.001f)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // A scaled per-size entry over a DF base. The views borrow the base's product-owned
        // font/atlas (products outlive this service's entries via the ResourceManager cache);
        // the CachedFont owns the view wrappers + shaper (ownsViews). Cached in m_entries so
        // the next request for this size hits FindExact. NOTE: PushBack may reallocate
        // m_entries, so everything needed from `base` is copied out first.
        [[nodiscard]] CachedFont* SynthesizeScaled(const Entry& base, f32 pixelHeight)
        {
            Entry entry;
            entry.family = base.family;
            entry.pixelHeight = pixelHeight;
            entry.image = base.image; // shared with the base (same atlas texture)
            entry.ownsViews = true;

            auto* fontView =
                m_allocator->New<ScaledFontView>(*base.cached->font, pixelHeight);
            auto* atlasView = m_allocator->New<ScaledFontAtlasView>(*base.cached->atlas,
                                                                          fontView->Scale());
            entry.cached = m_allocator->New<CachedFont>(
                *m_allocator, fontView, atlasView, MakeShaper());

            CachedFont* result = entry.cached;
            m_entries.PushBack(Move(entry));
            return result;
        }

        [[nodiscard]] Entry* FindClosest(StringView family, f32 pixelHeight)
        {
            Entry* best = nullptr;
            f32 bestDistance = 0.0f;
            for (Entry& entry : m_entries)
            {
                if (entry.ownsViews || entry.family.AsView() != family)
                {
                    continue;
                }
                const f32 distance = entry.pixelHeight > pixelHeight
                                         ? entry.pixelHeight - pixelHeight
                                         : pixelHeight - entry.pixelHeight;
                if (best == nullptr || distance < bestDistance)
                {
                    best = &entry;
                    bestDistance = distance;
                }
            }
            return best;
        }

        IAllocator* m_allocator;
        Array<Entry> m_entries;
        Array<RefPtr<Object>> m_keepAlive; // strong refs on registered products (see AddFont)
        String m_defaultFamily;
    };

    inline void RegisterFontResource()
    {
        GlobalTypeRegistry().Register(FontResource::StaticType());
        RegisterSerializable<FontResource>();
    }

    // 2: glyph bounds keys use full names. 3: regions key by glyph index ("glyph"), the codepoint ->
    // glyph map coming from the glyph rows (F3 of vg-and-gui-lab.md; re-cooked by the builder bump).
    RTTI_DEFINE_OBJECT_VERSIONED(FontResource, "rtti::fonts", 3)
    RTTI_DEFINE_OBJECT(Font, "rtti::fonts")
}

export namespace foundation::fonts
{
    /// The fonts resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kFontsResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<Font, FontResource, FontFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kFontsResourceModule{
        u8"fonts", &RegisterFontResource, kFontsResourceFactories,
        sizeof(kFontsResourceFactories) / sizeof(kFontsResourceFactories[0])};
}

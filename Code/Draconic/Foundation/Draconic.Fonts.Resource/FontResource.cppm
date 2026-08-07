// Draconic::FontsResource - the `draconic.fonts.resource` module (runtime).
//
// The FONT as a runtime resource - the triad tier the original port skipped
// ("Ported from Sedulous.Fonts (excluding Fonts.Resources)"; roadmap: "Fonts -> proper
// triad"). Same model-A shape as draconic.texture.resource:
//
//   * FontResource (ISerializable): the cooked *record* loaded from the output DB - the
//     family, the atlas pixel mode (Alpha8 coverage or RGBA MSDF), and one ENTRY per baked
//     pixel size (metrics + glyph/kerning/region tables + atlas metadata). The atlas pixel
//     payloads live concatenated in the "data" stream, per-entry offset+size recorded.
//   * Font (Object): the runtime product - owns, per entry, a rasterizer-free BakedFont,
//     its IFontAtlas (BakedFontAtlas for coverage, DFFontAtlas for MSDF), and the atlas as
//     an image::OwnedImageData ready for a renderer to upload (the same RGBA expansion the
//     TTF service performs). A shipped game loads THIS and never touches stb_truetype.
//   * FontFactory (IResourceFactory): cooked record + stream -> Font. DEVICE-FREE - the VG
//     layer uploads atlas images itself, so fonts bind headlessly.
//
// The runtime never links the editor/source side; it loads only cooked resources.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.fonts.resource;

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.baked;
import draconic.fonts.ttf;
import draconic.fonts.distancefield;
import draconic.image;
import draconic.content;
import draconic.resource;

using namespace draconic::foundation;
using namespace draconic::resource;

export namespace draconic::fonts
{
    namespace image = draconic::image;

    // How the cooked atlas payload is encoded (uniform across a resource's entries).
    enum class FontResourcePixels : u32
    {
        Alpha8,        // single-channel coverage (expanded to RGBA8 at load, like the TTF path)
        DistanceField, // RGBA8 MSDF channels, linear (no sRGB decode - geometric data)
    };

    // -- serialized table rows ----------------------------------------------------------

    struct FontResourceGlyph
    {
        i32 codepoint = 0;
        GlyphInfo info;

        void Serialize(ISerializer& ar)
        {
            draconic::foundation::Serialize(ar, "codepoint", codepoint);
            draconic::foundation::Serialize(ar, "glyphIndex", info.glyphIndex);
            draconic::foundation::Serialize(ar, "advanceWidth", info.advanceWidth);
            draconic::foundation::Serialize(ar, "leftSideBearing", info.leftSideBearing);
            draconic::foundation::Serialize(ar, "bbX", info.boundingBox.x);
            draconic::foundation::Serialize(ar, "bbY", info.boundingBox.y);
            draconic::foundation::Serialize(ar, "bbW", info.boundingBox.width);
            draconic::foundation::Serialize(ar, "bbH", info.boundingBox.height);
            draconic::foundation::Serialize(ar, "hasBitmap", info.hasBitmap);
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
            draconic::foundation::Serialize(ar, "first", first);
            draconic::foundation::Serialize(ar, "second", second);
            draconic::foundation::Serialize(ar, "amount", amount);
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceKerning& k)
    {
        ar.BeginObject();
        k.Serialize(ar);
        ar.EndObject();
    }

    struct FontResourceRegion
    {
        i32 codepoint = 0;
        AtlasRegion region;

        void Serialize(ISerializer& ar)
        {
            draconic::foundation::Serialize(ar, "codepoint", codepoint);
            draconic::foundation::Serialize(ar, "x", region.x);
            draconic::foundation::Serialize(ar, "y", region.y);
            draconic::foundation::Serialize(ar, "width", region.width);
            draconic::foundation::Serialize(ar, "height", region.height);
            draconic::foundation::Serialize(ar, "offsetX", region.offsetX);
            draconic::foundation::Serialize(ar, "offsetY", region.offsetY);
            draconic::foundation::Serialize(ar, "advanceX", region.advanceX);
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
        f32 dfPixelRange = 4.0f; // DistanceField mode only

        u64 pixelOffset = 0;
        u64 pixelBytes = 0;

        void Serialize(ISerializer& ar)
        {
            draconic::foundation::Serialize(ar, "pixelHeight", pixelHeight);
            draconic::foundation::Serialize(ar, "ascent", ascent);
            draconic::foundation::Serialize(ar, "descent", descent);
            draconic::foundation::Serialize(ar, "lineGap", lineGap);
            draconic::foundation::Serialize(ar, "scale", scale);
            draconic::foundation::Serialize(ar, "glyphs", glyphs);
            draconic::foundation::Serialize(ar, "kerning", kerning);
            draconic::foundation::Serialize(ar, "regions", regions);
            draconic::foundation::Serialize(ar, "atlasWidth", atlasWidth);
            draconic::foundation::Serialize(ar, "atlasHeight", atlasHeight);
            draconic::foundation::Serialize(ar, "whitePixelU", whitePixelU);
            draconic::foundation::Serialize(ar, "whitePixelV", whitePixelV);
            draconic::foundation::Serialize(ar, "dfPixelRange", dfPixelRange);
            draconic::foundation::Serialize(ar, "pixelOffset", pixelOffset);
            draconic::foundation::Serialize(ar, "pixelBytes", pixelBytes);
        }
    };
    inline void Serialize(ISerializer& ar, FontResourceEntry& e)
    {
        ar.BeginObject();
        e.Serialize(ar);
        ar.EndObject();
    }

    // Cooked font record (output DB). Atlas pixels are the "data" stream.
    class FontResource final : public ISerializable
    {
        DRACONIC_OBJECT(FontResource, ISerializable)
    public:
        String family;
        FontResourcePixels pixels = FontResourcePixels::Alpha8;
        Array<FontResourceEntry> entries;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "family", family);
            u32 mode = static_cast<u32>(pixels);
            draconic::foundation::Serialize(ar, "pixels", mode);
            pixels = static_cast<FontResourcePixels>(mode);
            draconic::foundation::Serialize(ar, "entries", entries);
        }
    };

    // Runtime product: rasterizer-free font + atlas + uploadable atlas image, per entry.
    class Font final : public Object
    {
        DRACONIC_OBJECT(Font, Object)
    public:
        struct Entry
        {
            f32 pixelHeight = 0.0f;
            UniquePtr<BakedFont> font;
            UniquePtr<IFontAtlas> atlas;        // BakedFontAtlas or DFFontAtlas
            UniquePtr<image::OwnedImageData> atlasImage; // RGBA8, renderer-uploadable
        };

        Font() = default;
        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;

        void SetFamily(StringView family) { m_family = String(family); }
        void AddEntry(Entry&& entry) { m_entries.PushBack(Move(entry)); }

        [[nodiscard]] StringView Family() const noexcept { return m_family.AsView(); }
        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] const Entry& EntryAt(usize index) const { return m_entries[index]; }

        // The entry whose pixelHeight is closest to `pixelHeight` (null when empty). The
        // P3 font service maps (family, size) requests through this.
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
        Array<Entry> m_entries;
    };

    // Cooked FontResource -> runtime Font. Device-free: atlas images stay CPU-side
    // (image::OwnedImageData); the VG layer uploads them like any other font texture.
    class FontFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Font::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            return BuildFont(instance);
        }

        // Async (task #123): the Font is a pure-CPU product (rasterizer-free glyph/kerning tables +
        // CPU atlas images the renderer uploads itself), so the whole build - including the heavy
        // atlas pixel read and per-entry slice copies - runs on a JobSystem worker via DecodeStage
        // and FinalizeStage is a no-op. Safe: content-DB reads open independent streams and the
        // FontResource + Font types are registered on the main thread at startup.
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            return BuildFont(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] static RefPtr<Object> BuildFont(draconic::content::Instance& instance)
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

            RefPtr<Font> product = MakeRef<Font>(DefaultAllocator());
            product->SetFamily(res->family.AsView());

            for (const FontResourceEntry& e : res->entries)
            {
                Font::Entry entry;
                entry.pixelHeight = e.pixelHeight;

                // The rasterizer-free IFont.
                entry.font = MakeUnique<BakedFont>(DefaultAllocator());
                entry.font->SetFamilyName(res->family.AsView());
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
                    auto atlas = MakeUnique<DFFontAtlas>(DefaultAllocator());
                    atlas->SetPixelRange(e.dfPixelRange);
                    atlas->SetWhitePixelUV(e.whitePixelU, e.whitePixelV);
                    for (const FontResourceRegion& r : e.regions)
                    {
                        atlas->SetRegion(r.codepoint, r.region);
                    }
                    // RGBA8 MSDF channels, LINEAR (geometric data, not color).
                    entry.atlasImage = MakeUnique<image::OwnedImageData>(
                        DefaultAllocator(), e.atlasWidth, e.atlasHeight,
                        image::PixelFormat::RGBA8,
                        Span<const u8>(slice.Data(), slice.Size()),
                        image::ImageColorSpace::Linear);
                    atlas->SetPixels(e.atlasWidth, e.atlasHeight, Move(slice));
                    entry.atlas = Move(atlas); // converting move: DFFontAtlas -> IFontAtlas
                }
                else
                {
                    auto atlas = MakeUnique<BakedFontAtlas>(DefaultAllocator());
                    atlas->SetWhitePixelUV(e.whitePixelU, e.whitePixelV);
                    for (const FontResourceRegion& r : e.regions)
                    {
                        atlas->SetRegion(r.codepoint, r.region);
                    }
                    atlas->SetPixels(e.atlasWidth, e.atlasHeight, Move(slice));
                    // Same RGBA8 expansion the TTF service performs for coverage atlases.
                    entry.atlasImage = UniquePtr<image::OwnedImageData>(
                        FontAtlasTexture::ExpandR8ToRGBA8(atlas.Get()), DefaultAllocator());
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
    };

    // IFontService over bound Font PRODUCTS - the GUID-addressable runtime font path
    // (roadmap: "fonts are GUID-addressable through the content DB/ResourceManager").
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
        ResourceFontService() = default;
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
                    DefaultAllocator().Delete(entry.cached); // frees shaper (+ owned views)
                }
            }
            m_entries.Clear();
            m_defaultFamily.Clear();
        }

        // Register a bound product (BORROWED: the caller keeps the product alive - the
        // ResourceManager's cache does, for as long as the manager lives). The first
        // registered font becomes the default family.
        void AddFont(const Font* font)
        {
            if (font == nullptr)
            {
                return;
            }
            for (usize i = 0; i < font->EntryCount(); ++i)
            {
                const Font::Entry& productEntry = font->EntryAt(i);
                Entry entry;
                entry.family = String(font->Family());
                entry.pixelHeight = productEntry.pixelHeight;
                entry.cached = DefaultAllocator().New<CachedFont>(
                    productEntry.font.Get(), productEntry.atlas.Get(),
                    DefaultAllocator().New<TrueTypeTextShaper>());
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

        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(CachedFont* font) override
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

        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(StringView familyName,
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

        [[nodiscard]] StringView DefaultFontFamily() const override
        {
            return m_defaultFamily.AsView();
        }

    private:
        struct Entry
        {
            String family;
            f32 pixelHeight = 0.0f;
            CachedFont* cached = nullptr;              // owned wrapper (see class comment)
            draconic::image::ImageData* image = nullptr; // product-owned
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
                DefaultAllocator().New<ScaledFontView>(*base.cached->font, pixelHeight);
            auto* atlasView = DefaultAllocator().New<ScaledFontAtlasView>(*base.cached->atlas,
                                                                          fontView->Scale());
            entry.cached = DefaultAllocator().New<CachedFont>(
                fontView, atlasView, DefaultAllocator().New<TrueTypeTextShaper>());

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

        Array<Entry> m_entries;
        String m_defaultFamily;
    };

    inline void RegisterFontResource()
    {
        GlobalTypeRegistry().Register(FontResource::StaticType());
        RegisterSerializable<FontResource>();
    }

    DRACONIC_DEFINE_OBJECT(FontResource, "draconic::fonts")
    DRACONIC_DEFINE_OBJECT(Font, "draconic::fonts")
}

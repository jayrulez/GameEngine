// Draconic::FontsTTF - draconic.fonts.ttf:service partition
//
// IFontService that loads TrueType/OpenType fonts through the source-format
// pipeline (parse -> bake -> expand-to-RGBA8). With a VFS file system set, the
// locator is a path opened through it; otherwise it is a disk path (for
// sandboxes/tools/tests). Ported from Sedulous.Fonts.TTF/TrueTypeFontService.bf;
// Beef's "Family@Height" string keys become explicit family + size fields.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.ttf:service;

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.io;
import draconic.image;
import draconic.vfs;
import :text_shaper;
import :init;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class TrueTypeFontService final : public IFontService
    {
    public:
        // `fileSystem` is optional + non-owning. When set, LoadFont treats the
        // locator as a path opened through it; otherwise as a disk path.
        explicit TrueTypeFontService(draconic::vfs::IFileSystem* fileSystem = nullptr)
            : m_fileSystem(fileSystem)
        {
            TrueTypeFonts::Initialize();
        }

        ~TrueTypeFontService() override
        {
            for (FontEntry* entry : m_fonts)
                DeleteEntry(entry);
            m_fonts.Clear();
        }

        TrueTypeFontService(const TrueTypeFontService&) = delete;
        TrueTypeFontService& operator=(const TrueTypeFontService&) = delete;

        // Load a font from `locator` and build its atlas texture. The first
        // font loaded becomes the default. Returns Success or a failure.
        [[nodiscard]] FontLoadResult
        LoadFont(StringView familyName, StringView locator,
                 FontLoadOptions options = FontLoadOptions::ExtendedLatin())
        {
            IFont* font = nullptr;
            if (m_fileSystem != nullptr)
            {
                UniquePtr<IStream> stream = m_fileSystem->Open(locator, FileMode::Read);
                if (!stream || !stream->IsValid())
                    return FontLoadResult::FileNotFound;

                const StringView ext = PathExtension(locator);
                Result<IFont*, FontLoadResult> parsed =
                    FontParserFactory::ParseFromStream(*stream, ext, options);
                if (!parsed.HasValue())
                    return parsed.Error();
                font = parsed.Value();
            }
            else
            {
                Result<IFont*, FontLoadResult> parsed =
                    FontParserFactory::ParseFromFile(locator, options);
                if (!parsed.HasValue())
                    return parsed.Error();
                font = parsed.Value();
            }

            return CacheFont(familyName, font, options);
        }

        // Load a font from in-memory TTF/OTF bytes (an EMBEDDED fallback: a relocated
        // editor must always have a face to fall back on, whatever the disk looks like).
        [[nodiscard]] FontLoadResult
        LoadFontFromMemory(StringView familyName, Span<const u8> bytes,
                           FontLoadOptions options = FontLoadOptions::ExtendedLatin())
        {
            Array<u8> copy;
            copy.Resize(bytes.Size());
            if (bytes.Size() != 0)
                MemCopy(copy.Data(), bytes.Data(), bytes.Size());
            TrueTypeFont* font = DefaultAllocator().New<TrueTypeFont>();
            const FontLoadResult parsed = font->Initialize(Move(copy), options.pixelHeight);
            if (parsed != FontLoadResult::Success)
            {
                DefaultAllocator().Delete(font);
                return parsed;
            }
            return CacheFont(familyName, font, options);
        }

        // Change the default family used by GetFont(pixelHeight).
        void SetDefaultFamily(StringView name) { m_defaultFontFamily = String(name); }

        // --- IFontService --------------------------------------------------
        [[nodiscard]] StringView DefaultFontFamily() const override { return m_defaultFontFamily; }

        [[nodiscard]] CachedFont* GetFont(f32 pixelHeight) override
        {
            return GetFont(m_defaultFontFamily, pixelHeight);
        }

        [[nodiscard]] CachedFont* GetFont(StringView familyName, f32 pixelHeight) override
        {
            if (const FontEntry* exact = FindExact(familyName, pixelHeight))
                return exact->cachedFont;
            if (const FontEntry* closest = FindClosest(familyName, pixelHeight))
            {
                // Distance-field families serve EVERY size from one bake: synthesize (and
                // cache) a scaled view instead of handing out the bake-size tables - the
                // per-size-scaled-views gap that kept the UI off MSDF.
                if (closest->cachedFont != nullptr && closest->cachedFont->atlas != nullptr &&
                    closest->cachedFont->atlas->Mode() == AtlasMode::DistanceField &&
                    closest->pixelHeight != pixelHeight)
                {
                    return SynthesizeScaled(*closest, familyName, pixelHeight);
                }
                return closest->cachedFont;
            }
            return m_defaultFont;
        }

        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(CachedFont* font) override
        {
            for (FontEntry* entry : m_fonts)
                if (entry->cachedFont == font)
                    return entry->texture;
            return nullptr;
        }

        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(StringView familyName,
                                                                  f32 pixelHeight) override
        {
            if (const FontEntry* exact = FindExact(familyName, pixelHeight))
                return exact->texture;
            if (const FontEntry* closest = FindClosest(familyName, pixelHeight))
                return closest->texture;
            for (FontEntry* entry : m_fonts)
                if (entry->cachedFont == m_defaultFont)
                    return entry->texture;
            return nullptr;
        }

        // Fonts are owned by the service - releasing is a no-op.
        void ReleaseFont(CachedFont*) override {}

    private:
        struct FontEntry
        {
            String family;
            f32 pixelHeight = 0;
            CachedFont* cachedFont = nullptr;                   // owns font/atlas/shaper
            draconic::image::OwnedImageData* texture = nullptr; // owned unless sharedTexture
            bool sharedTexture = false; // scaled-view entry: texture belongs to the base entry
        };

        static void DeleteEntry(FontEntry* entry)
        {
            if (entry == nullptr)
                return;
            DefaultAllocator().Delete(entry->cachedFont); // frees font/atlas/shaper
            if (!entry->sharedTexture)
                DefaultAllocator().Delete(entry->texture);
            DefaultAllocator().Delete(entry);
        }

        // ASCII case-insensitive family compare (matches Sedulous's ignore-case).
        static bool FamilyEquals(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char ca = a[i], cb = b[i];
                if (ca >= u8'A' && ca <= u8'Z')
                    ca = static_cast<utf8char>(ca - u8'A' + u8'a');
                if (cb >= u8'A' && cb <= u8'Z')
                    cb = static_cast<utf8char>(cb - u8'A' + u8'a');
                if (ca != cb)
                    return false;
            }
            return true;
        }

        // A scaled per-size entry over a DF base (views borrow the base font/atlas, the
        // CachedFont owns the view wrappers + its own shaper; the entry SHARES the base's
        // atlas texture). Cached in m_fonts so the next request hits FindExact.
        [[nodiscard]] CachedFont* SynthesizeScaled(const FontEntry& base, StringView familyName,
                                                   f32 pixelHeight)
        {
            auto* fontView =
                DefaultAllocator().New<ScaledFontView>(*base.cachedFont->font, pixelHeight);
            auto* atlasView = DefaultAllocator().New<ScaledFontAtlasView>(
                *base.cachedFont->atlas, fontView->Scale());
            ITextShaper* shaper = DefaultAllocator().New<TrueTypeTextShaper>();
            CachedFont* cachedFont =
                DefaultAllocator().New<CachedFont>(fontView, atlasView, shaper);

            FontEntry* entry = DefaultAllocator().New<FontEntry>();
            entry->family = String(familyName);
            entry->pixelHeight = pixelHeight;
            entry->cachedFont = cachedFont;
            entry->texture = base.texture; // SHARED with the base (see DeleteEntry)
            entry->sharedTexture = true;
            m_fonts.PushBack(entry);
            return cachedFont;
        }

        // Bake the atlas, expand to RGBA8, wrap in a CachedFont, and record the
        // entry. Takes ownership of `font`; deletes it on any failure.
        FontLoadResult CacheFont(StringView familyName, IFont* font, FontLoadOptions options)
        {
            Result<IFontAtlas*, FontLoadResult> baked = FontAtlasBakerFactory::Bake(*font, options);
            if (!baked.HasValue())
            {
                DefaultAllocator().Delete(font);
                return baked.Error();
            }
            IFontAtlas* atlas = baked.Value();

            // Distance-field atlases already store RGBA texels (the MSDF channels); wrap them
            // directly in a LINEAR image (no sRGB decode - the field is geometric, not color).
            // Coverage atlases are single-channel R8 and expand to RGBA8 as before.
            draconic::image::OwnedImageData* texture =
                (atlas->Mode() == AtlasMode::DistanceField)
                    ? DefaultAllocator().New<draconic::image::OwnedImageData>(
                          atlas->Width(), atlas->Height(), draconic::image::PixelFormat::RGBA8,
                          atlas->PixelData(), draconic::image::ImageColorSpace::Linear)
                    : FontAtlasTexture::ExpandR8ToRGBA8(atlas);
            if (texture == nullptr)
            {
                DefaultAllocator().Delete(atlas);
                DefaultAllocator().Delete(font);
                return FontLoadResult::OutOfMemory;
            }

            ITextShaper* shaper = DefaultAllocator().New<TrueTypeTextShaper>();
            CachedFont* cachedFont = DefaultAllocator().New<CachedFont>(font, atlas, shaper);

            FontEntry* entry = DefaultAllocator().New<FontEntry>();
            entry->family = String(familyName);
            entry->pixelHeight = options.pixelHeight;
            entry->cachedFont = cachedFont;
            entry->texture = texture;
            m_fonts.PushBack(entry);

            if (m_defaultFont == nullptr)
            {
                m_defaultFont = cachedFont;
                m_defaultFontFamily = String(familyName);
                m_defaultFontSize = options.pixelHeight;
            }
            return FontLoadResult::Success;
        }

        [[nodiscard]] const FontEntry* FindExact(StringView family, f32 pixelHeight) const
        {
            for (const FontEntry* entry : m_fonts)
                if (FamilyEquals(entry->family, family) &&
                    Abs(entry->pixelHeight - pixelHeight) < 0.001f)
                    return entry;
            return nullptr;
        }

        [[nodiscard]] const FontEntry* FindClosest(StringView family, f32 pixelHeight) const
        {
            const FontEntry* best = nullptr;
            f32 bestDiff = 3.4e38f;
            for (const FontEntry* entry : m_fonts)
                if (!entry->sharedTexture && FamilyEquals(entry->family, family))
                {
                    const f32 diff = Abs(entry->pixelHeight - pixelHeight);
                    if (diff < bestDiff)
                    {
                        bestDiff = diff;
                        best = entry;
                    }
                }
            return best;
        }

        draconic::vfs::IFileSystem* m_fileSystem = nullptr; // non-owning
        Array<FontEntry*> m_fonts;
        String m_defaultFontFamily = String(u8"Default");
        CachedFont* m_defaultFont = nullptr;
        f32 m_defaultFontSize = 16;
    };
}

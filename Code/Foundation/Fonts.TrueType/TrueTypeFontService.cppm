// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:service partition
//
// IFontService that loads TrueType/OpenType fonts through the source-format
// pipeline (parse -> bake -> expand-to-RGBA8). With a VFS file system set, the
// locator is a path opened through it; otherwise it is a disk path (for
// sandboxes/tools/tests). A distance-field family serves every size from one bake (scaled views);
// a coverage family is baked again at each size asked for (once, rounded to whole pixels, from its
// kept source), so a label's font-size is the size it draws at. Ported from Sedulous.Fonts.TTF/TrueTypeFontService.bf;
// Beef's "Family@Height" string keys become explicit family + size fields.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:service;

import foundation.core;
import foundation.fonts;
import foundation.image;
import foundation.vfs;
import :text_shaper;
import :init;

using namespace foundation::core;

export namespace foundation::fonts
{
    class TrueTypeFontService final : public IFontService
    {
    public:
        // `fileSystem` is optional + non-owning. When set, LoadFont treats the
        // locator as a path opened through it; otherwise as a disk path.
        // The allocator (required - the owner decides) backs fonts, atlases,
        // shapers, cache entries, and atlas textures this service creates.
        explicit TrueTypeFontService(IAllocator& allocator,
                                     foundation::vfs::IFileSystem* fileSystem = nullptr)
            : m_allocator(&allocator), m_fileSystem(fileSystem)
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
            const FontLoadResult result = LoadFontUnremembered(familyName, locator, options);
            if (result == FontLoadResult::Success)
            {
                Remember(familyName, String(locator), {}, options);
            }
            return result;
        }

        // Load a font from in-memory TTF/OTF bytes (an EMBEDDED fallback: a relocated
        // editor must always have a face to fall back on, whatever the disk looks like).
        [[nodiscard]] FontLoadResult
        LoadFontFromMemory(StringView familyName, Span<const u8> bytes,
                           FontLoadOptions options = FontLoadOptions::ExtendedLatin())
        {
            const FontLoadResult result = BakeFromMemory(familyName, bytes, options);
            if (result == FontLoadResult::Success)
            {
                Array<u8> kept;
                kept.Resize(bytes.Size());
                if (bytes.Size() != 0)
                    MemCopy(kept.Data(), bytes.Data(), bytes.Size());
                Remember(familyName, String(), Move(kept), options);
            }
            return result;
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
                // A coverage atlas holds glyphs at its one size: drawn at another, the text came
                // out at the bake's size whatever was asked (every label in a game without a UI
                // font drew alike). Bake the size asked for, once, from the family's source.
                if (CachedFont* baked = BakeSize(familyName, pixelHeight))
                {
                    return baked;
                }
                return closest->cachedFont;
            }
            return m_defaultFont;
        }

        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(CachedFont* font) override
        {
            for (FontEntry* entry : m_fonts)
                if (entry->cachedFont == font)
                    return entry->texture;
            return nullptr;
        }

        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(StringView familyName,
                                                                  f32 pixelHeight) override
        {
            (void)GetFont(familyName, pixelHeight); // the size's own bake, if it wants one
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
        // Where a family came from, to bake it again at another size (a coverage family).
        struct FamilySource
        {
            String family;
            String locator;   // a LoadFont path (empty for in-memory bytes)
            Array<u8> bytes;  // a LoadFontFromMemory face
            FontLoadOptions options;
        };

        void Remember(StringView family, String locator, Array<u8> bytes, const FontLoadOptions& options)
        {
            for (FamilySource& source : m_sources)
            {
                if (FamilyEquals(source.family, family))
                {
                    return; // the first load names the family's source
                }
            }
            FamilySource source;
            source.family = String(family);
            source.locator = Move(locator);
            source.bytes = Move(bytes);
            source.options = options;
            m_sources.PushBack(Move(source));
        }

        // Parse `locator` and bake it at `options` (LoadFont, and a coverage family's next size).
        [[nodiscard]] FontLoadResult LoadFontUnremembered(StringView familyName, StringView locator,
                                                          const FontLoadOptions& options)
        {
            IFont* font = nullptr;
            if (m_fileSystem != nullptr)
            {
                UniquePtr<IStream> stream = m_fileSystem->Open(locator, FileMode::Read);
                if (!stream || !stream->IsValid())
                    return FontLoadResult::FileNotFound;

                const StringView ext = PathExtension(locator);
                Result<IFont*, FontLoadResult> parsed =
                    FontParserFactory::ParseFromStream(*stream, ext, options, *m_allocator);
                if (!parsed.HasValue())
                    return parsed.Error();
                font = parsed.Value();
            }
            else
            {
                Result<IFont*, FontLoadResult> parsed =
                    FontParserFactory::ParseFromFile(locator, options, *m_allocator);
                if (!parsed.HasValue())
                    return parsed.Error();
                font = parsed.Value();
            }

            return CacheFont(familyName, font, options);
        }

        [[nodiscard]] FontLoadResult BakeFromMemory(StringView familyName, Span<const u8> bytes,
                                                    const FontLoadOptions& options)
        {
            Array<u8> copy;
            copy.Resize(bytes.Size());
            if (bytes.Size() != 0)
                MemCopy(copy.Data(), bytes.Data(), bytes.Size());
            TrueTypeFont* font = m_allocator->New<TrueTypeFont>();
            const FontLoadResult parsed = font->Initialize(Move(copy), options.pixelHeight);
            if (parsed != FontLoadResult::Success)
            {
                m_allocator->Delete(font);
                return parsed;
            }
            return CacheFont(familyName, font, options);
        }

        // A coverage family baked at `pixelHeight` (rounded to whole pixels, 4 to 256), its atlas
        // grown until the glyphs fit (to 4096 a side). Null when the family has no kept source or
        // the bake fails; the caller then draws with the closest bake.
        [[nodiscard]] CachedFont* BakeSize(StringView familyName, f32 pixelHeight)
        {
            const FamilySource* source = nullptr;
            for (const FamilySource& candidate : m_sources)
            {
                if (FamilyEquals(candidate.family, familyName))
                {
                    source = &candidate;
                    break;
                }
            }
            if (source == nullptr || source->options.atlasMode == AtlasMode::DistanceField)
            {
                return nullptr;
            }
            const f32 rounded = Clamp(Round(pixelHeight), 4.0f, 256.0f);
            if (const FontEntry* exact = FindExact(familyName, rounded))
            {
                return exact->cachedFont;
            }
            FontLoadOptions options = source->options;
            options.pixelHeight = rounded;
            // The atlas the family loaded with, grown with the size (glyph area goes with its square).
            const f32 grow = Max(1.0f, rounded / Max(source->options.pixelHeight, 1.0f));
            u32 side = Max(source->options.atlasWidth, source->options.atlasHeight);
            while (static_cast<f32>(side) < static_cast<f32>(source->options.atlasWidth) * grow && side < 4096u)
            {
                side *= 2u;
            }
            for (;; side *= 2u)
            {
                options.atlasWidth = side;
                options.atlasHeight = side;
                FontLoadResult result = FontLoadResult::Unknown;
                if (!source->bytes.IsEmpty())
                {
                    result = BakeFromMemory(familyName, Span<const u8>(source->bytes.Data(), source->bytes.Size()),
                                            options);
                }
                else if (!source->locator.IsEmpty())
                {
                    result = LoadFontUnremembered(familyName, source->locator.AsView(), options);
                }
                if (result == FontLoadResult::Success)
                {
                    const FontEntry* made = FindExact(familyName, rounded);
                    return made != nullptr ? made->cachedFont : nullptr;
                }
                if (result != FontLoadResult::AtlasPackingFailed || side >= 4096u)
                {
                    return nullptr;
                }
            }
        }

        struct FontEntry
        {
            String family;
            f32 pixelHeight = 0;
            CachedFont* cachedFont = nullptr;                   // owns font/atlas/shaper
            foundation::image::OwnedImageData* texture = nullptr; // owned unless sharedTexture
            bool sharedTexture = false; // scaled-view entry: texture belongs to the base entry
        };

        void DeleteEntry(FontEntry* entry)
        {
            if (entry == nullptr)
                return;
            m_allocator->Delete(entry->cachedFont); // frees font/atlas/shaper
            if (!entry->sharedTexture)
                m_allocator->Delete(entry->texture);
            m_allocator->Delete(entry);
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
                m_allocator->New<ScaledFontView>(*base.cachedFont->font, pixelHeight);
            auto* atlasView = m_allocator->New<ScaledFontAtlasView>(
                *base.cachedFont->atlas, fontView->Scale());
            ITextShaper* shaper = m_allocator->New<TrueTypeTextShaper>();
            CachedFont* cachedFont =
                m_allocator->New<CachedFont>(*m_allocator, fontView, atlasView, shaper);

            FontEntry* entry = m_allocator->New<FontEntry>();
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
            Result<IFontAtlas*, FontLoadResult> baked =
                FontAtlasBakerFactory::Bake(*font, options, *m_allocator);
            if (!baked.HasValue())
            {
                m_allocator->Delete(font);
                return baked.Error();
            }
            IFontAtlas* atlas = baked.Value();

            // Distance-field atlases already store RGBA texels (the MSDF channels); wrap them
            // directly in a LINEAR image (no sRGB decode - the field is geometric, not color).
            // Coverage atlases are single-channel R8 and expand to RGBA8 as before.
            foundation::image::OwnedImageData* texture =
                (atlas->Mode() == AtlasMode::DistanceField)
                    ? m_allocator->New<foundation::image::OwnedImageData>(
                          atlas->Width(), atlas->Height(), foundation::image::PixelFormat::RGBA8,
                          atlas->PixelData(), foundation::image::ImageColorSpace::Linear)
                    : FontAtlasTexture::ExpandR8ToRGBA8(atlas, *m_allocator);
            if (texture == nullptr)
            {
                m_allocator->Delete(atlas);
                m_allocator->Delete(font);
                return FontLoadResult::OutOfMemory;
            }

            ITextShaper* shaper = m_allocator->New<TrueTypeTextShaper>();
            CachedFont* cachedFont =
                m_allocator->New<CachedFont>(*m_allocator, font, atlas, shaper);

            FontEntry* entry = m_allocator->New<FontEntry>();
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

        IAllocator* m_allocator;
        foundation::vfs::IFileSystem* m_fileSystem = nullptr; // non-owning
        Array<FontEntry*> m_fonts;
        Array<FamilySource> m_sources; // each family's first load, to bake more sizes from
        String m_defaultFontFamily = String(u8"Default");
        CachedFont* m_defaultFont = nullptr;
        f32 m_defaultFontSize = 16;
    };
}

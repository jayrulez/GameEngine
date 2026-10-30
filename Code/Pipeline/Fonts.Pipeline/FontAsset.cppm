// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Fonts - the `foundation.fonts.editor` module (tooling).
//
// Source-side font authoring + cook (the editor tier of the fonts triad):
//   * FontAsset (pipeline::Asset): references a TTF/OTF/TTC file in Sources/ + the bake
//     intent - atlas mode (raster size ramp or one MSDF bake), pixel sizes, codepoint
//     range, atlas dimensions, and the runtime family name.
//   * FontAssetBuilder (DefaultAssetBuilder): cooks a FontAsset into a runtime
//     FontResource - parse the font bytes once per size, bake coverage atlases through
//     fonts::FontBaker (or the msdfgen MSDF baker for DistanceField mode), flatten the
//     glyph/kerning/region tables into record entries, and concatenate the atlas pixel
//     payloads into the "data" stream.
//   * FontAssetImporter (IFileImporter): claims .ttf/.otf/.ttc - copies the file into
//     Sources/ and creates the FontAsset with the default UI ramp.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module fonts.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.coverage;
import foundation.fonts.coverage.baker;
import foundation.fonts.distancefield;
import foundation.fonts.distancefield.baker;
import foundation.fonts.harfbuzz; // the cook bakes what its substitutions reach
import foundation.fonts.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::fonts;

export namespace pipeline{
    namespace content = foundation::content;

    // How the asset bakes: a ramp of coverage rasterizations (one atlas per size), or a
    // single MSDF atlas (size-independent sampling once the UI's DF path is on).
    enum class FontBakeMode : u32
    {
        RasterRamp,
        DistanceField,
    };

    // Source asset: a font file + how it should become a cooked FontResource.
    class FontAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(FontAsset, pipeline::Asset)
    public:
        String family;      // runtime family name ("" = the file's own family at cook)
        FontBakeMode mode = FontBakeMode::RasterRamp;
        Array<f32> sizes;   // RasterRamp: one cooked entry per size
        f32 distanceFieldSize = 48.0f; // DistanceField: the single bake size
        // DistanceField: the field's spread in texels of the bake; text effects reach half of it
        // past an edge (FontLoadOptions::distanceFieldRange).
        f32 distanceFieldRange = 4.0f;
        i32 firstCodepoint = 32;
        i32 lastCodepoint = 255; // ExtendedLatin default (matches the runtime rasterizer)
        u32 atlasWidth = 1024;
        u32 atlasHeight = 1024;

        FontAsset() { SetupDefaultRamp(); }

        // The game-UI size ramp (mirrors the editor's raster ramp: field to heading sizes).
        void SetupDefaultRamp()
        {
            sizes.Clear();
            const f32 ramp[] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f,
                                16.0f, 18.0f, 20.0f, 24.0f, 32.0f};
            for (f32 s : ramp)
            {
                sizes.PushBack(s);
            }
        }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "family", family);
            u32 bakeMode = static_cast<u32>(mode);
            foundation::core::Serialize(ar, "mode", bakeMode);
            mode = static_cast<FontBakeMode>(bakeMode);
            foundation::core::Serialize(ar, "sizes", sizes);
            foundation::core::Serialize(ar, "distanceFieldSize", distanceFieldSize);
            if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // v1 assets bake the 4-texel default
                foundation::core::Serialize(ar, "distanceFieldRange", distanceFieldRange);
            foundation::core::Serialize(ar, "firstCodepoint", firstCodepoint);
            foundation::core::Serialize(ar, "lastCodepoint", lastCodepoint);
            foundation::core::Serialize(ar, "atlasWidth", atlasWidth);
            foundation::core::Serialize(ar, "atlasHeight", atlasHeight);
        }
    };

    // Cooks a FontAsset into a FontResource (record entries + concatenated "data" pixels).
    class FontAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        // The bakers bake what HarfBuzz's substitutions reach. Installed here, on the thread that
        // registers builders, not in Build: builds run in parallel on job workers, and installing
        // from each raced another worker's shaper lookup (a crash cooking PaperKid's two fonts).
        FontAssetBuilder() { HarfBuzzShaping::Install(); }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &FontAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &FontResource::StaticType();
        }

        // v3 (2026-09-06): FontResourceEntry's "dfPixelRange" key became
        // "distanceFieldPixelRange" (a keyed encoding of the product would miss the old key).
        // v2 (2026-08-12): FontResourceEntry gained the oversample fields (the jumbled-text
        // fix - baked screen quads must divide the pack-time oversampling back out). RULE: a
        // product schema change bumps the builder Version() in the SAME commit - the bump IS
        // the migration; it forces every stale cooked font to re-cook.
        // 4 (2026-09-08): FontResource 2 - the glyph bounds keys use full names.
        // 5 (2026-10-04): a distance-field atlas is sized to its glyphs (the asset's atlas size
        // is a maximum), and a glyph that does not fit fails the bake instead of vanishing.
        // 6 (2026-10-15): FontResource 3 - atlases key by glyph index, so a shaper's glyphs can
        // draw; every codepoint the range maps to keeps its glyph row (a space's too).
        // 7 (2026-10-15): the font file is kept as the product's "font" stream, for a shaper that
        // reads its tables in a shipped game.
        // 8 (2026-10-15): the atlas also holds the glyphs HarfBuzz's substitutions reach from the
        // range (ligatures, joined forms); the cook installs HarfBuzz itself, so every cook bakes
        // the same glyphs whichever host runs it.
        // 9 (2026-10-15): a distance-field cook writes the region of every glyph the atlas holds,
        // not only those the range's codepoints map to (8 baked the ligatures but dropped them).
        // 10 (2026-10-16): a distance-field atlas carries the true signed distance in alpha
        // (MTSDF), what outlines, glows and soft shadows read.
        [[nodiscard]] u32 Version() const override { return 10; }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const FontAsset& fa = static_cast<const FontAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr || fa.fileName.IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            Result<Array<byte>> bytes = ReadSourceBytes(ctx, fa.fileName.View());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            const Span<const u8> fontBytes(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                           bytes.Value().Size());

            FontResource resource;
            resource.family = fa.family; // explicit family wins; bakers fill "" from the file
            resource.pixels = (fa.mode == FontBakeMode::DistanceField)
                                  ? FontResourcePixels::DistanceField
                                  : FontResourcePixels::Coverage;
            Array<u8> pixels; // concatenated atlas payloads ("data" stream)

            if (fa.mode == FontBakeMode::DistanceField)
            {
                const Status baked =
                    BakeDistanceField(*ctx.allocator, fa, fontBytes, resource, pixels);
                if (!baked.IsOk())
                {
                    return baked;
                }
            }
            else
            {
                for (f32 size : fa.sizes)
                {
                    const Status baked =
                        BakeCoverage(*ctx.allocator, fa, fontBytes, size, resource, pixels);
                    if (!baked.IsOk())
                    {
                        return baked;
                    }
                }
            }
            if (resource.entries.IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument}; // no size produced an atlas
            }

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            const Status wroteData = ctx.output->WriteData(
                u8"data",
                Span<const byte>(reinterpret_cast<const byte*>(pixels.Data()), pixels.Size()));
            if (!wroteData.IsOk())
            {
                return wroteData;
            }
            return ctx.output->WriteData(u8"font", Span<const byte>(bytes.Value().Data(), bytes.Value().Size()));
        }

    private:
        [[nodiscard]] static FontLoadOptions OptionsFor(const FontAsset& fa, f32 pixelHeight,
                                                        bool distanceField)
        {
            FontLoadOptions options =
                distanceField ? FontLoadOptions::DistanceField() : FontLoadOptions::Default();
            options.pixelHeight = pixelHeight;
            options.firstCodepoint = fa.firstCodepoint;
            options.lastCodepoint = fa.lastCodepoint;
            options.atlasWidth = fa.atlasWidth;
            options.atlasHeight = fa.atlasHeight;
            if (distanceField)
                options.distanceFieldRange = fa.distanceFieldRange;
            return options;
        }

        // Shared entry skeleton from a parsed font (metrics come from the IFont).
        static void FillEntryHeader(const IFont& font, f32 pixelHeight, FontResourceEntry& entry)
        {
            const FontMetrics metrics = font.Metrics();
            entry.pixelHeight = pixelHeight;
            entry.ascent = metrics.ascent;
            entry.descent = metrics.descent;
            entry.lineGap = metrics.lineGap;
            entry.scale = metrics.scale;
        }

        // One coverage (A8) size via the existing baked-font importer.
        [[nodiscard]] Status BakeCoverage(IAllocator& allocator, const FontAsset& fa,
                                          Span<const u8> fontBytes, f32 size,
                                          FontResource& resource, Array<u8>& pixels)
        {
            Result<BakedFontData*, FontLoadResult> baked =
                FontBaker::Bake(fontBytes, OptionsFor(fa, size, /*distanceField*/ false),
                                   allocator);
            if (!baked.HasValue())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<BakedFontData> data(baked.Value(), allocator);

            FontResourceEntry entry;
            FillEntryHeader(*data->font, size, entry);
            if (resource.family.IsEmpty())
            {
                resource.family = String(data->font->FamilyName());
            }

            for (const auto& pair : data->font->Glyphs())
            {
                FontResourceGlyph glyph;
                glyph.codepoint = pair.key;
                glyph.info = pair.value;
                entry.glyphs.PushBack(glyph);
            }
            for (const auto& pair : data->font->Kerning())
            {
                FontResourceKerning kerning;
                kerning.first = static_cast<i32>(pair.key >> 32);
                kerning.second = static_cast<i32>(static_cast<u32>(pair.key));
                kerning.amount = pair.value;
                entry.kerning.PushBack(kerning);
            }
            for (const auto& pair : data->atlas->Regions())
            {
                FontResourceRegion region;
                region.glyph = pair.key;
                region.region = pair.value;
                entry.regions.PushBack(region);
            }

            entry.atlasWidth = data->atlas->Width();
            entry.atlasHeight = data->atlas->Height();
            const Float2 white = data->atlas->WhitePixelUV();
            entry.whitePixelU = white.x;
            entry.whitePixelV = white.y;
            entry.oversampleX = data->atlas->OversampleX();
            entry.oversampleY = data->atlas->OversampleY();

            AppendPixels(data->atlas->PixelData(), entry, pixels);
            resource.entries.PushBack(Move(entry));
            return Status{};
        }

        // The single MSDF bake: parse, run the registered msdfgen baker, flatten tables.
        [[nodiscard]] Status BakeDistanceField(IAllocator& allocator, const FontAsset& fa,
                                               Span<const u8> fontBytes,
                                               FontResource& resource, Array<u8>& pixels)
        {
            DistanceFieldFonts::Initialize(); // idempotent baker registration (msdfgen)

            Array<u8> bytesCopy;
            bytesCopy.Resize(fontBytes.Size());
            if (fontBytes.Size() != 0)
            {
                MemCopy(bytesCopy.Data(), fontBytes.Data(), fontBytes.Size());
            }
            const FontLoadOptions options = OptionsFor(fa, fa.distanceFieldSize, /*distanceField*/ true);
            TrueTypeFont font;
            if (font.Initialize(Move(bytesCopy), options.pixelHeight) != FontLoadResult::Success)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            Result<IFontAtlas*, FontLoadResult> baked =
                FontAtlasBakerFactory::Bake(font, options, allocator);
            if (!baked.HasValue())
            {
                return Status{ErrorCode::NotSupported}; // no DF baker (msdfgen missing?)
            }
            UniquePtr<IFontAtlas> atlas(baked.Value(), allocator);

            FontResourceEntry entry;
            FillEntryHeader(font, fa.distanceFieldSize, entry);
            if (resource.family.IsEmpty())
            {
                resource.family = String(font.FamilyName());
            }

            // Every codepoint the atlas maps keeps its glyph row, and every glyph the atlas holds its
            // region: those the codepoints map to and those a shaper's substitutions reach (a
            // ligature, which no codepoint maps to, is drawn from its region alone).
            for (i32 cp = fa.firstCodepoint; cp <= fa.lastCodepoint; ++cp)
            {
                if (atlas->GlyphFor(cp) < 0)
                {
                    continue;
                }
                FontResourceGlyph glyph;
                glyph.codepoint = cp;
                glyph.info = font.GetGlyphInfo(cp);
                entry.glyphs.PushBack(glyph);
            }
            const auto& field = static_cast<const DistanceFieldFontAtlas&>(*atlas); // the DF baker's
            for (const auto& pair : field.Regions())
            {
                FontResourceRegion rr;
                rr.glyph = pair.key;
                rr.region = pair.value;
                entry.regions.PushBack(rr);
            }
            for (i32 a = fa.firstCodepoint; a <= fa.lastCodepoint; ++a)
            {
                if (!atlas->Contains(atlas->GlyphFor(a)))
                {
                    continue;
                }
                for (i32 b = fa.firstCodepoint; b <= fa.lastCodepoint; ++b)
                {
                    if (!atlas->Contains(atlas->GlyphFor(b)))
                    {
                        continue;
                    }
                    const f32 adjustment = font.GetKerning(a, b);
                    if (adjustment != 0)
                    {
                        FontResourceKerning kerning;
                        kerning.first = a;
                        kerning.second = b;
                        kerning.amount = adjustment;
                        entry.kerning.PushBack(kerning);
                    }
                }
            }

            entry.atlasWidth = atlas->Width();
            entry.atlasHeight = atlas->Height();
            const Float2 white = atlas->WhitePixelUV();
            entry.whitePixelU = white.x;
            entry.whitePixelV = white.y;
            entry.distanceFieldPixelRange = atlas->DistanceFieldRange();

            AppendPixels(atlas->PixelData(), entry, pixels);
            resource.entries.PushBack(Move(entry));
            return Status{};
        }

        static void AppendPixels(Span<const u8> src, FontResourceEntry& entry, Array<u8>& pixels)
        {
            entry.pixelOffset = pixels.Size();
            entry.pixelBytes = src.Size();
            const usize base = pixels.Size();
            pixels.Resize(base + src.Size());
            if (src.Size() != 0)
            {
                MemCopy(pixels.Data() + base, src.Data(), src.Size());
            }
        }
    };

    // Claims dropped .ttf/.otf/.ttc files: copy into Sources/, create the FontAsset.
    class FontAssetImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Font"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"ttf" || extension == u8"otf" || extension == u8"ttc";
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"FontAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"FontAsset");
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, FontAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(sourcePath));
            FontAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.family = String(stem); // sensible default; cook falls back to the file's name
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers the FontBakeMode enum reflection (idempotent). FontAsset's own reflection body is
    // its StaticType(), defined in FontAssetImpl.cpp.
    void RegisterFontAssetReflection();

    // Registers FontAsset for content-DB construction + deserialization.
    inline void RegisterFontAsset()
    {
        RegisterFontAssetReflection(); // FontBakeMode names for the property grid
        GlobalTypeRegistry().Register(FontAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<FontAsset>();
        RegisterFontResource(); // the product type, for the cook's output DB
    }
}

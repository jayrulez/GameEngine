// Draconic::FontsEditor - the `draconic.fonts.editor` module (tooling).
//
// Source-side font authoring + cook (the editor tier of the fonts triad):
//   * FontAsset (editor::Asset): references a TTF/OTF/TTC file in Sources/ + the bake
//     intent - atlas mode (raster size ramp or one MSDF bake), pixel sizes, codepoint
//     range, atlas dimensions, and the runtime family name.
//   * FontAssetBuilder (DefaultAssetBuilder): cooks a FontAsset into a runtime
//     FontResource - parse the font bytes once per size, bake coverage atlases through
//     fonts::FontImporter (or the msdfgen MSDF baker for DistanceField mode), flatten the
//     glyph/kerning/region tables into record entries, and concatenate the atlas pixel
//     payloads into the "data" stream.
//   * FontAssetImporter (IFileImporter): claims .ttf/.otf/.ttc - copies the file into
//     Sources/ and creates the FontAsset with the default UI ramp.
//
// Never linked by the runtime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.fonts.editor;

import draconic.foundation;
import draconic.editor;
import draconic.editor.core;
import draconic.fonts;
import draconic.fonts.ttf;
import draconic.fonts.io;
import draconic.fonts.baked;
import draconic.fonts.importer;
import draconic.fonts.distancefield;
import draconic.fonts.distancefield.baker;
import draconic.fonts.resource;
import draconic.content;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    namespace content = draconic::content;

    // How the asset bakes: a ramp of coverage rasterizations (one atlas per size), or a
    // single MSDF atlas (size-independent sampling once the UI's DF path is on).
    enum class FontBakeMode : u32
    {
        RasterRamp,
        DistanceField,
    };

    // Source asset: a font file + how it should become a cooked FontResource.
    class FontAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(FontAsset, draconic::editor::Asset)
    public:
        String family;      // runtime family name ("" = the file's own family at cook)
        FontBakeMode mode = FontBakeMode::RasterRamp;
        Array<f32> sizes;   // RasterRamp: one cooked entry per size
        f32 dfSize = 48.0f; // DistanceField: the single bake size
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
            draconic::editor::Asset::Serialize(ar); // fileName
            draconic::foundation::Serialize(ar, "family", family);
            u32 bakeMode = static_cast<u32>(mode);
            draconic::foundation::Serialize(ar, "mode", bakeMode);
            mode = static_cast<FontBakeMode>(bakeMode);
            draconic::foundation::Serialize(ar, "sizes", sizes);
            draconic::foundation::Serialize(ar, "dfSize", dfSize);
            draconic::foundation::Serialize(ar, "firstCodepoint", firstCodepoint);
            draconic::foundation::Serialize(ar, "lastCodepoint", lastCodepoint);
            draconic::foundation::Serialize(ar, "atlasWidth", atlasWidth);
            draconic::foundation::Serialize(ar, "atlasHeight", atlasHeight);
        }
    };

    // Cooks a FontAsset into a FontResource (record entries + concatenated "data" pixels).
    class FontAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &FontAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &FontResource::StaticType();
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
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
                                  : FontResourcePixels::Alpha8;
            Array<u8> pixels; // concatenated atlas payloads ("data" stream)

            if (fa.mode == FontBakeMode::DistanceField)
            {
                const Status baked =
                    BakeDistanceField(fa, fontBytes, resource, pixels);
                if (!baked.IsOk())
                {
                    return baked;
                }
            }
            else
            {
                for (f32 size : fa.sizes)
                {
                    const Status baked = BakeCoverage(fa, fontBytes, size, resource, pixels);
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
            return ctx.output->WriteData(
                u8"data",
                Span<const byte>(reinterpret_cast<const byte*>(pixels.Data()), pixels.Size()));
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
        [[nodiscard]] Status BakeCoverage(const FontAsset& fa, Span<const u8> fontBytes, f32 size,
                                          FontResource& resource, Array<u8>& pixels)
        {
            Result<BakedFontData*, FontLoadResult> baked =
                FontImporter::Bake(fontBytes, OptionsFor(fa, size, /*distanceField*/ false));
            if (!baked.HasValue())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<BakedFontData> data(baked.Value(), DefaultAllocator());

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
                region.codepoint = pair.key;
                region.region = pair.value;
                entry.regions.PushBack(region);
            }

            entry.atlasWidth = data->atlas->Width();
            entry.atlasHeight = data->atlas->Height();
            const Float2 white = data->atlas->WhitePixelUV();
            entry.whitePixelU = white.x;
            entry.whitePixelV = white.y;

            AppendPixels(data->atlas->PixelData(), entry, pixels);
            resource.entries.PushBack(Move(entry));
            return Status{};
        }

        // The single MSDF bake: parse, run the registered msdfgen baker, flatten tables.
        [[nodiscard]] Status BakeDistanceField(const FontAsset& fa, Span<const u8> fontBytes,
                                               FontResource& resource, Array<u8>& pixels)
        {
            DFFonts::Initialize(); // idempotent baker registration (msdfgen)

            Array<u8> bytesCopy;
            bytesCopy.Resize(fontBytes.Size());
            if (fontBytes.Size() != 0)
            {
                MemCopy(bytesCopy.Data(), fontBytes.Data(), fontBytes.Size());
            }
            const FontLoadOptions options = OptionsFor(fa, fa.dfSize, /*distanceField*/ true);
            TrueTypeFont font;
            if (font.Initialize(Move(bytesCopy), options.pixelHeight) != FontLoadResult::Success)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            Result<IFontAtlas*, FontLoadResult> baked = FontAtlasBakerFactory::Bake(font, options);
            if (!baked.HasValue())
            {
                return Status{ErrorCode::NotSupported}; // no DF baker (msdfgen missing?)
            }
            UniquePtr<IFontAtlas> atlas(baked.Value(), DefaultAllocator());

            FontResourceEntry entry;
            FillEntryHeader(font, fa.dfSize, entry);
            if (resource.family.IsEmpty())
            {
                resource.family = String(font.FamilyName());
            }

            for (i32 cp = fa.firstCodepoint; cp <= fa.lastCodepoint; ++cp)
            {
                AtlasRegion region;
                if (!atlas->TryGetRegion(cp, region))
                {
                    continue;
                }
                FontResourceGlyph glyph;
                glyph.codepoint = cp;
                glyph.info = font.GetGlyphInfo(cp);
                entry.glyphs.PushBack(glyph);
                FontResourceRegion rr;
                rr.codepoint = cp;
                rr.region = region;
                entry.regions.PushBack(rr);
            }
            for (i32 a = fa.firstCodepoint; a <= fa.lastCodepoint; ++a)
            {
                if (!atlas->Contains(a))
                {
                    continue;
                }
                for (i32 b = fa.firstCodepoint; b <= fa.lastCodepoint; ++b)
                {
                    if (!atlas->Contains(b))
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
            entry.dfPixelRange = atlas->DistanceFieldRange();

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
    class FontAssetImporter final : public draconic::editor::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Font"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"ttf" || extension == u8"otf" || extension == u8"ttc";
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, draconic::editor::EditorProject& project,
               content::Group& group, const draconic::editor::ImportOptions*, Object*,
               Array<draconic::editor::DeferredImportWrite>*) override
        {
            Result<String> fileName = draconic::editor::CopyIntoSources(project, sourcePath);
            if (!fileName.HasValue())
            {
                return Err(fileName.Error());
            }
            const StringView stem = draconic::editor::FileStemOf(fileName.Value().AsView());
            content::Instance* instance = group.CreateInstance(stem, FontAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            FontAsset asset;
            asset.fileName = draconic::vfs::SourcePath(fileName.Value().AsView());
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
    // its StaticType(), defined in FontAssetImpl.cpp. Reflection track P1.
    void RegisterFontAssetReflection();

    // Registers FontAsset for content-DB construction + deserialization.
    inline void RegisterFontAsset()
    {
        RegisterFontAssetReflection(); // FontBakeMode names for the property grid
        GlobalTypeRegistry().Register(FontAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<FontAsset>();
        RegisterFontResource(); // the product type, for the cook's output DB
    }
}

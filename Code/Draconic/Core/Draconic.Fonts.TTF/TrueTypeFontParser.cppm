// Draconic::FontsTTF - draconic.fonts.ttf:parser partition
//
// IFontParser for TrueType/OpenType (.ttf/.ttc/.otf): copies source bytes into
// an owned buffer and builds a TrueTypeFont. Atlas baking is a separate step.
// Ported from Sedulous.Fonts.TTF/TrueTypeFontParser.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.ttf:parser;

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.io;
import :common;
import :font;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class TrueTypeFontParser final : public IFontParser
    {
    public:
        [[nodiscard]] Span<const StringView> SupportedExtensions() const override
        {
            return TrueTypeExtensions();
        }

        [[nodiscard]] bool SupportsExtension(StringView fileExtension) const override
        {
            for (const StringView ext : TrueTypeExtensions())
                if (ExtEquals(fileExtension, ext))
                    return true;
            return false;
        }

        [[nodiscard]] Result<IFont*, FontLoadResult>
        ParseFromStream(IStream& stream, FontLoadOptions options) override
        {
            // stb_truetype needs the whole buffer addressable, so copy the
            // remaining stream contents into a fresh owned array.
            const i64 length = stream.Size() - stream.Tell();
            if (length <= 0)
                return Err(FontLoadResult::CorruptedData);

            Array<u8> fontData;
            fontData.Resize(static_cast<usize>(length));
            if (stream.Read(fontData.Data(), static_cast<u64>(length)) != static_cast<u64>(length))
                return Err(FontLoadResult::CorruptedData);

            return Build(Move(fontData), options);
        }

        [[nodiscard]] Result<IFont*, FontLoadResult>
        ParseFromMemory(Span<const u8> data, FontLoadOptions options) override
        {
            Array<u8> fontData;
            fontData.Resize(data.Size());
            if (data.Size() != 0)
                MemCopy(fontData.Data(), data.Data(), data.Size());
            return Build(Move(fontData), options);
        }

        [[nodiscard]] Result<IFont*, FontLoadResult> ParseFromFile(StringView filePath,
                                                                   FontLoadOptions options) override
        {
            FileStream file(filePath, FileMode::Read);
            if (!file.IsValid())
                return Err(FontLoadResult::FileNotFound);
            return ParseFromStream(file, options);
        }

    private:
        static Result<IFont*, FontLoadResult> Build(Array<u8>&& fontData, FontLoadOptions options)
        {
            TrueTypeFont* font = DefaultAllocator().New<TrueTypeFont>();
            const FontLoadResult result = font->Initialize(Move(fontData), options.pixelHeight);
            if (result != FontLoadResult::Success)
            {
                DefaultAllocator().Delete(font);
                return Err(result);
            }
            return static_cast<IFont*>(font);
        }
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:parser partition
//
// IFontParser for TrueType/OpenType (.ttf/.ttc/.otf): copies source bytes into
// an owned buffer and builds a TrueTypeFont. Atlas baking is a separate step.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:parser;

import foundation.core;
import foundation.fonts;
import :common;
import :font;

using namespace foundation::core;

export namespace foundation::fonts
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
        ParseFromStream(IStream& stream, FontLoadOptions options, IAllocator& allocator) override
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

            return Build(Move(fontData), options, allocator);
        }

        [[nodiscard]] Result<IFont*, FontLoadResult>
        ParseFromMemory(Span<const u8> data, FontLoadOptions options,
                        IAllocator& allocator) override
        {
            Array<u8> fontData;
            fontData.Resize(data.Size());
            if (data.Size() != 0)
                MemCopy(fontData.Data(), data.Data(), data.Size());
            return Build(Move(fontData), options, allocator);
        }

        [[nodiscard]] Result<IFont*, FontLoadResult>
        ParseFromFile(StringView filePath, FontLoadOptions options,
                      IAllocator& allocator) override
        {
            FileStream file(filePath, FileMode::Read);
            if (!file.IsValid())
                return Err(FontLoadResult::FileNotFound);
            return ParseFromStream(file, options, allocator);
        }

    private:
        static Result<IFont*, FontLoadResult> Build(Array<u8>&& fontData, FontLoadOptions options,
                                                    IAllocator& allocator)
        {
            TrueTypeFont* font = allocator.New<TrueTypeFont>();
            const FontLoadResult result = font->Initialize(Move(fontData), options.pixelHeight);
            if (result != FontLoadResult::Success)
            {
                allocator.Delete(font);
                return Err(result);
            }
            return static_cast<IFont*>(font);
        }
    };
}

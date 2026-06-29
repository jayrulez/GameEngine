// Raptor::FontsTTF — raptor.fonts.ttf:atlas_baker partition
//
// IFontAtlasBaker for TrueType/OpenType: bakes a TrueTypeFont into a
// TrueTypeFontAtlas via stb's pack-font-range. Requires a concrete
// TrueTypeFont (checked via the no-RTTI backend tag). Ported from
// Sedulous.Fonts.TTF/TrueTypeFontAtlasBaker.bf.

module;
#include "Core/Prelude.h"

export module raptor.fonts.ttf:atlas_baker;

import raptor.core;
import raptor.fonts;
import raptor.fonts.io;
import :common;
import :font;
import :atlas;

using namespace raptor::core;

export namespace raptor::fonts
{
    class TrueTypeFontAtlasBaker final : public IFontAtlasBaker
    {
    public:
        [[nodiscard]] Span<const StringView> SupportedExtensions() const override { return TrueTypeExtensions(); }

        [[nodiscard]] bool SupportsExtension(StringView fileExtension) const override
        {
            for (const StringView ext : TrueTypeExtensions())
                if (ExtEquals(fileExtension, ext))
                    return true;
            return false;
        }

        [[nodiscard]] bool CanBake(const IFont& font) const override
        {
            return font.BackendTypeId() == kTrueTypeFontTypeId;
        }

        [[nodiscard]] bool CanBake(const IFont& font, const FontLoadOptions& opts) const override
        {
            return font.BackendTypeId() == kTrueTypeFontTypeId
                && opts.atlasMode == AtlasMode::Coverage;
        }

        [[nodiscard]] Result<IFontAtlas*, FontLoadResult> Bake(IFont& font, FontLoadOptions options) override
        {
            if (font.BackendTypeId() != kTrueTypeFontTypeId)
                return Err(FontLoadResult::UnsupportedFormat);
            const TrueTypeFont& ttf = static_cast<const TrueTypeFont&>(font);

            TrueTypeFontAtlas* atlas = DefaultAllocator().New<TrueTypeFontAtlas>();
            const FontLoadResult result = atlas->Create(ttf, options);
            if (result != FontLoadResult::Success)
            {
                DefaultAllocator().Delete(atlas);
                return Err(result);
            }
            return static_cast<IFontAtlas*>(atlas);
        }
    };
}

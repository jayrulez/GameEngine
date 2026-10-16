// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:atlas_baker partition
//
// IFontAtlasBaker for TrueType/OpenType: bakes a TrueTypeFont into a
// TrueTypeFontAtlas via stb's pack-font-range. Requires a concrete
// TrueTypeFont (checked via the no-RTTI backend tag). Ported from
// Sedulous.Fonts.TTF/TrueTypeFontAtlasBaker.bf.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:atlas_baker;

import foundation.core;
import foundation.fonts;
import :common;
import :font;
import :atlas;

using namespace foundation::core;

export namespace foundation::fonts
{
    class TrueTypeFontAtlasBaker final : public IFontAtlasBaker
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

        [[nodiscard]] bool CanBake(const IFont& font) const override
        {
            return font.BackendTypeId() == kTrueTypeFontTypeId;
        }

        // This baker produces COVERAGE atlases only; distance-field requests go to the MSDF baker.
        [[nodiscard]] bool CanBake(const IFont& font, const FontLoadOptions& options) const override
        {
            return font.BackendTypeId() == kTrueTypeFontTypeId &&
                   options.atlasMode == AtlasMode::Coverage;
        }

        [[nodiscard]] Result<IFontAtlas*, FontLoadResult>
        Bake(IFont& font, FontLoadOptions options, IAllocator& allocator) override
        {
            if (font.BackendTypeId() != kTrueTypeFontTypeId)
                return Err(FontLoadResult::UnsupportedFormat);
            const TrueTypeFont& ttf = static_cast<const TrueTypeFont&>(font);

            // The range's glyphs, and what the registered shaper's substitutions reach from them.
            const Array<i32> reachable = ReachableGlyphs(font, options, allocator);
            TrueTypeFontAtlas* atlas = allocator.New<TrueTypeFontAtlas>();
            const FontLoadResult result =
                atlas->Create(ttf, options, Span<const i32>(reachable.Data(), reachable.Size()));
            if (result != FontLoadResult::Success)
            {
                allocator.Delete(atlas);
                return Err(result);
            }
            return static_cast<IFontAtlas*>(atlas);
        }
    };
}

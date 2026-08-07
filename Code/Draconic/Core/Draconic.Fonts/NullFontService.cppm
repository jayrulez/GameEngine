// Draconic::Fonts - :null_service partition
//
// A no-op IFontService (returns null for everything) for tests/headless use.
// Ported from Sedulous.Fonts (NullFontService.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:null_service;

import draconic.foundation;
import draconic.image;
import :interfaces;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class NullFontService final : public IFontService
    {
    public:
        [[nodiscard]] CachedFont* GetFont(f32) override { return nullptr; }
        [[nodiscard]] CachedFont* GetFont(StringView, f32) override { return nullptr; }
        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(CachedFont*) override
        {
            return nullptr;
        }
        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(StringView, f32) override
        {
            return nullptr;
        }
        void ReleaseFont(CachedFont*) override {}
        [[nodiscard]] StringView DefaultFontFamily() const override { return u8""; }
    };
}

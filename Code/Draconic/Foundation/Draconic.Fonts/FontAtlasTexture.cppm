// Draconic::Fonts - :atlas_texture partition
//
// Expands a font atlas's single-channel R8 coverage buffer into a renderer-
// friendly RGBA8 image (white RGB, alpha = coverage). Ported from
// Sedulous.Fonts (FontAtlasTexture.bf). Caller owns the returned image.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:atlas_texture;

import draconic.foundation;
import draconic.image;
import :interfaces;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class FontAtlasTexture
    {
    public:
        // Returns a heap OwnedImageData (caller deletes), or null if the atlas has
        // no usable pixel data.
        [[nodiscard]] static draconic::image::OwnedImageData*
        ExpandR8ToRGBA8(const IFontAtlas* atlas)
        {
            if (atlas == nullptr)
            {
                return nullptr;
            }
            const u32 w = atlas->Width();
            const u32 h = atlas->Height();
            if (w == 0 || h == 0)
            {
                return nullptr;
            }

            const Span<const u8> r8 = atlas->PixelData();
            const usize pixelCount = static_cast<usize>(w) * h;
            if (r8.Size() < pixelCount)
            {
                return nullptr;
            }

            Array<u8> rgba;
            rgba.Resize(pixelCount * 4);
            for (usize i = 0; i < pixelCount; ++i)
            {
                rgba[i * 4 + 0] = 255;   // R
                rgba[i * 4 + 1] = 255;   // G
                rgba[i * 4 + 2] = 255;   // B
                rgba[i * 4 + 3] = r8[i]; // A = coverage
            }
            return DefaultAllocator().New<draconic::image::OwnedImageData>(
                w, h, draconic::image::PixelFormat::RGBA8,
                Span<const u8>(rgba.Data(), rgba.Size()));
        }
    };
}

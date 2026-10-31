// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - :atlas_texture partition
//
// Expands a font atlas's single-channel R8 coverage buffer into a renderer-
// friendly RGBA8 image (white RGB, alpha = coverage). Caller owns the returned image.

module;
#include "Core/Prelude.h"

export module foundation.fonts:atlas_texture;

import foundation.core;
import foundation.image;
import :interfaces;

using namespace foundation::core;

export namespace foundation::fonts
{
    class FontAtlasTexture
    {
    public:
        // Returns a heap OwnedImageData allocated from `allocator` (caller deletes
        // through the same allocator), or null if the atlas has no usable pixel data.
        [[nodiscard]] static foundation::image::OwnedImageData*
        ExpandR8ToRGBA8(const IFontAtlas* atlas, IAllocator& allocator)
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
            return allocator.New<foundation::image::OwnedImageData>(
                w, h, foundation::image::PixelFormat::RGBA8,
                Span<const u8>(rgba.Data(), rgba.Size()));
        }
    };
}

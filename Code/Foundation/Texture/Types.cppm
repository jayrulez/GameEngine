// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture - :types partition
//
// Logical texture descriptors: shape, filter, wrap. Used by TextureResource to
// say how pixel data should be interpreted and sampled.

module;
#include "Core/Prelude.h"

export module foundation.texture:types;

import foundation.core;

using namespace foundation::core;

export namespace foundation::texture
{
    // The logical shape of a texture asset.
    enum class TextureShape : u8
    {
        Texture2D,      // standard 2D image
        Texture2DArray, // multiple same-size layers
        Texture3D,      // volume
        Cubemap,        // 6 square faces (+X,-X,+Y,-Y,+Z,-Z)
        CubemapArray,   // array of cubemaps
    };

    enum class TextureFilter : u8
    {
        Nearest,
        Linear,
        MipmapNearest,
        MipmapLinear,
    };

    enum class TextureWrap : u8
    {
        Repeat,
        ClampToEdge,
        ClampToBorder,
        MirroredRepeat,
    };

    // Reflects TextureShape, TextureFilter and TextureWrap for tooling (enum-by-name dropdowns in
    // the generic asset page) and scripting. Idempotent; called by consumers that need the enum
    // names (the editor, cook and export tools, through RegisterTextureAsset). The body lives in
    // the implementation unit to keep REFLECT_* bodies out of the module interface.
    void RegisterTextureReflection();
}

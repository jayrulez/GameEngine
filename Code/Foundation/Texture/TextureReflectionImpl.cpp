// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture - reflection implementation unit: enum reflection bodies.
//
// Kept out of the :types interface partition because REFLECT_* bodies make GCC emit a gcm
// cluster. Types.cppm declares RegisterTextureReflection(); this unit defines it and the
// RttiRegisterEnum_* bodies.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.texture;

import foundation.core;

using namespace foundation::core;

namespace foundation::texture
{
    REFLECT_ENUM(TextureShape, "rtti::texture")
    {
        builder.Value("Texture2D", TextureShape::Texture2D);
        builder.Value("Texture2DArray", TextureShape::Texture2DArray);
        builder.Value("Texture3D", TextureShape::Texture3D);
        builder.Value("Cubemap", TextureShape::Cubemap);
        builder.Value("CubemapArray", TextureShape::CubemapArray);
    }

    REFLECT_ENUM(TextureFilter, "rtti::texture")
    {
        builder.Value("Nearest", TextureFilter::Nearest);
        builder.Value("Linear", TextureFilter::Linear);
        builder.Value("MipmapNearest", TextureFilter::MipmapNearest);
        builder.Value("MipmapLinear", TextureFilter::MipmapLinear);
    }

    REFLECT_ENUM(TextureWrap, "rtti::texture")
    {
        builder.Value("Repeat", TextureWrap::Repeat);
        builder.Value("ClampToEdge", TextureWrap::ClampToEdge);
        builder.Value("ClampToBorder", TextureWrap::ClampToBorder);
        builder.Value("MirroredRepeat", TextureWrap::MirroredRepeat);
    }

    void RegisterTextureReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_TextureShape();
            RttiRegisterEnum_TextureFilter();
            RttiRegisterEnum_TextureWrap();
            return true;
        }();
        (void)once;
    }
}

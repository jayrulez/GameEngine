// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// texture.compression - reflection implementation unit: the TextureUsage and CompressionChoice enum
// bodies. Kept out of the interface (REFLECT_* bodies make GCC emit a gcm cluster) and separate
// from the encoder impl (no reason to pull the reflection header in beside the encoders).
// TextureCompression.cppm declares RegisterCompressionReflection(); this defines it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module texture.compression;

import foundation.core;

using namespace foundation::core;

namespace texcomp
{
    REFLECT_ENUM(TextureUsage, "rtti::texcomp")
    {
        builder.Value("Color", TextureUsage::Color);
        builder.Value("Normal", TextureUsage::Normal);
        builder.Value("Mask", TextureUsage::Mask);
        builder.Value("HDR", TextureUsage::HDR);
    }

    REFLECT_ENUM(CompressionChoice, "rtti::texcomp")
    {
        builder.Value("Default", CompressionChoice::Default);
        builder.Value("None", CompressionChoice::None);
        builder.Value("Quality", CompressionChoice::Quality);
    }

    void RegisterCompressionReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_TextureUsage();
            RttiRegisterEnum_CompressionChoice();
            return true;
        }();
        (void)once;
    }
}

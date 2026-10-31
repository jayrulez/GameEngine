// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Image - reflection implementation unit: enum reflection bodies.
//
// Kept out of the :image_data interface partition (REFLECT_* bodies make GCC emit a gcm
// cluster). ImageData.cppm declares RegisterImageReflection(); this unit defines it and the
// RttiRegisterEnum_ImageColorSpace body.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.image;

import foundation.core;

using namespace foundation::core;

namespace foundation::image
{
    REFLECT_ENUM(ImageColorSpace, "rtti::image")
    {
        builder.Value("Srgb", ImageColorSpace::Srgb);
        builder.Value("Linear", ImageColorSpace::Linear);
    }

    void RegisterImageReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_ImageColorSpace();
            return true;
        }();
        (void)once;
    }
}

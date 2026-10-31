// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Image - reflection implementation unit: ImageAsset's reflected surface.
//
// Kept out of the ImageAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit a
// gcm cluster. ImageAsset::StaticType() gains its colorSpace property here;
// the ImageColorSpace enum reflection lives in foundation.image (RegisterImageReflection).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module image.pipeline;

import foundation.core;
import pipeline.core;
import foundation.image;

using namespace foundation::core;
using namespace foundation::image;

namespace pipeline{
    REFLECT_MEMBERS(ImageAsset, "rtti::pipeline::image")
    {
        builder.Attribute("displayName", String(u8"Image"))
            .Attribute("category", String(u8"Textures"))
            .Property<&ImageAsset::colorSpace>("colorSpace")
            .PropAttribute("displayName", String(u8"Color Space"));
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Heightfield - reflection implementation unit: HeightfieldAsset's reflected surface.
//
// Kept out of the HeightfieldAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC
// emit a gcm cluster. HeightfieldAsset::StaticType() gains its size, worldSize, minY and maxY
// properties here.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module heightfield.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(HeightfieldAsset, "rtti::pipeline::heightfield")
    {
        builder.Attribute("displayName", String(u8"Heightfield"))
            .Attribute("category", String(u8"Terrain"))
            .Property<&HeightfieldAsset::size>("size")
            .PropAttribute("displayName", String(u8"Grid Size (64k+1)"))
            .Property<&HeightfieldAsset::worldSize>("worldSize")
            .PropAttribute("displayName", String(u8"World Size (XZ)"))
            .Property<&HeightfieldAsset::minY>("minY")
            .PropAttribute("displayName", String(u8"Min Height"))
            .Property<&HeightfieldAsset::maxY>("maxY")
            .PropAttribute("displayName", String(u8"Max Height"));
    }
}

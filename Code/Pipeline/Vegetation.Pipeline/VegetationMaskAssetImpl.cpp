// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// vegetation.pipeline implementation: the asset's reflection body and RTTI definition, kept out
// of the module interface.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module vegetation.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(VegetationMaskAsset, "rtti::pipeline::vegetation")
    {
        builder.Attribute("displayName", String(u8"Vegetation Mask"))
            .Attribute("category", String(u8"Terrain"))
            .Property<&VegetationMaskAsset::width>("width")
            .PropAttribute("displayName", String(u8"Width"))
            .Property<&VegetationMaskAsset::height>("height")
            .PropAttribute("displayName", String(u8"Height"))
            .Property<&VegetationMaskAsset::planeCount>("planeCount")
            .PropAttribute("displayName", String(u8"Planes"))
            .PropAttribute("description",
                           String(u8"Density planes: one per vegetation layer that uses Mask "
                                  u8"placement."));
    }
}

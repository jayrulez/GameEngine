// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Fonts - reflection implementation unit: FontAsset's reflected surface.
//
// Kept out of the FontAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit a
// gcm cluster. The class declares identity with RTTI_OBJECT in the interface; this unit defines
// FontAsset::StaticType() with properties and tooling attributes, and the FontBakeMode enum
// reflection.
// TODO: the `sizes` ramp (Array<f32>) is unreflected; array container properties need editor
// support beyond the flat-field pass.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module fonts.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;
using namespace foundation::fonts;

namespace pipeline{
    REFLECT_ENUM(FontBakeMode, "rtti::pipeline::fonts")
    {
        builder.Value("RasterRamp", FontBakeMode::RasterRamp);
        builder.Value("DistanceField", FontBakeMode::DistanceField);
    }

    REFLECT_MEMBERS(FontAsset, "rtti::pipeline::fonts")
    {
        // v1: the distance-field size key is "distanceFieldSize". v2 adds "distanceFieldRange";
        // v1 payloads still read (the legacy reader) and take the 4-texel default.
        builder.DataVersion(2)
            .ReadsDataVersionsFrom(1)
            .Attribute("displayName", String(u8"Font"))
            .Attribute("category", String(u8"Fonts"))
            .Property<&FontAsset::family>("family")
            .PropAttribute("displayName", String(u8"Family"))
            .PropAttribute("description",
                           String(u8"Runtime family name (empty = the file's own family)"))
            .Property<&FontAsset::mode>("mode")
            .PropAttribute("displayName", String(u8"Bake Mode"))
            .Property<&FontAsset::distanceFieldSize>("distanceFieldSize")
            .PropAttribute("displayName", String(u8"Distance-Field Size"))
            .PropAttribute("range", Float4{8.0f, 128.0f, 1.0f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"mode=1")) // DistanceField only
            .Property<&FontAsset::distanceFieldRange>("distanceFieldRange")
            .PropAttribute("displayName", String(u8"Distance-Field Range"))
            .PropAttribute("description",
                           String(u8"The field's spread in texels of the bake; outlines and glows reach half of it"))
            .PropAttribute("range", Float4{1.0f, 32.0f, 0.5f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"mode=1"))
            .Property<&FontAsset::firstCodepoint>("firstCodepoint")
            .PropAttribute("displayName", String(u8"First Codepoint"))
            .Property<&FontAsset::lastCodepoint>("lastCodepoint")
            .PropAttribute("displayName", String(u8"Last Codepoint"))
            .Property<&FontAsset::atlasWidth>("atlasWidth")
            .PropAttribute("displayName", String(u8"Atlas Width"))
            .Property<&FontAsset::atlasHeight>("atlasHeight")
            .PropAttribute("displayName", String(u8"Atlas Height"));
    }

    void RegisterFontAssetReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_FontBakeMode();
            return true;
        }();
        (void)once;
    }
}

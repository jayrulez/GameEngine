// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Terrain - reflection implementation unit: TerrainAsset's reflected surface.
//
// Kept out of the TerrainAsset.cppm interface (REFLECT_MEMBERS bodies make GCC emit a gcm cluster).
// The heightfield and texture pickers are on the Editor.Terrain page; this reflects the scalar fields (the
// cast-shadows flag is the one live toggle).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module terrain.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(TerrainAsset, "rtti::pipeline::terrain")
    {
        // DataVersion 5: per-layer coverage/opacity mask ids (nil is opaque). v4: per-layer
        // height ids and heightBlendContrast (nil and 0.25 give the off path). v3: per-layer normal
        // and ORM ids (nil is flat/default). v2: the top-K splat model (base, palette, weightsId);
        // v<2 payloads carry the fixed-4-layer fields and upgrade in Serialize.
        builder.DataVersion(5)
            .Attribute("displayName", String(u8"Terrain"))
            .Attribute("category", String(u8"Terrain"))
            .Property<&TerrainAsset::castShadows>("castShadows")
            .PropAttribute("displayName", String(u8"Cast Shadows"));
    }

    REFLECT_MEMBERS(SplatmapAsset, "rtti::pipeline::splatmap")
    {
        builder.Attribute("displayName", String(u8"Splatmap"))
            .Attribute("category", String(u8"Terrain"))
            .Property<&SplatmapAsset::width>("width")
            .PropAttribute("displayName", String(u8"Width"))
            .Property<&SplatmapAsset::height>("height")
            .PropAttribute("displayName", String(u8"Height"));
    }
}

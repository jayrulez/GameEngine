// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain.Pipeline - the terrain domain's New Asset creators: what File > New and asset_create
// make, with no editor (the source database and the sources folder are all a creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module terrain.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterTerrainCreators(AssetCreatorRegistry& registry)
    {
        // A terrain with no references yet (the terrain page's slots fill them), and a blank
        // 1024 weight raster the builder seeds to layer 0.
        {
            AssetCreator creator;
            creator.label = String(u8"Terrain");
            creator.category = String(u8"Terrain");
            creator.type = &TerrainAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                TerrainAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"Terrain"),
                                             TerrainAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
        {
            AssetCreator creator;
            creator.label = String(u8"Splatmap");
            creator.category = String(u8"Terrain");
            creator.type = &SplatmapAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                SplatmapAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"Splatmap"),
                                             SplatmapAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}

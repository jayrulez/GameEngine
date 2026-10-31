// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Heightfield.Pipeline - the heightfield domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module heightfield.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterHeightfieldCreators(AssetCreatorRegistry& registry)
    {
        // A blank flat grid: the asset's defaults (square 257, 256 m footprint, 0..64 m Y).
        {
            AssetCreator creator;
            creator.label = String(u8"Heightfield");
            creator.category = String(u8"Terrain");
            creator.type = &HeightfieldAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                HeightfieldAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"Heightfield"),
                                             HeightfieldAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}

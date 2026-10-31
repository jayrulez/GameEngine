// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Input.Pipeline - the input domain's New Asset creators: what File > New and asset_create
// make, with no editor (the source database and the sources folder are all a creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module input.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterInputCreators(AssetCreatorRegistry& registry)
    {
        {
            // An input map seeded with the conventional Gameplay starter set.
            AssetCreator creator;
            creator.label = String(u8"Input Map");
            creator.type = &InputMapAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                InputMapAsset asset;
                asset.SeedDefaultContent();
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"InputMap"),
                                             InputMapAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}

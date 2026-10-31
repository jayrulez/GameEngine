// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Texture.Pipeline - the texture domain's New Asset creators: what File > New and asset_create
// make, with no editor. A texture from a file is an import; a render texture has no file, so it
// is made here.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module texture.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterTextureCreators(AssetCreatorRegistry& registry)
    {
        {
            // A 256x256 LDR render texture: a minimap or a monitor's size.
            AssetCreator creator;
            creator.label = String(u8"Render Texture");
            creator.type = &RenderTextureAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                RenderTextureAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"RenderTexture"),
                                             RenderTextureAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Physics.Pipeline - the physics domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module physics.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterPhysicsCreators(AssetCreatorRegistry& registry)
    {
        {
            AssetCreator creator;
            creator.label = String(u8"Physical Material");
            creator.category = String(u8"Physics");
            creator.type = &PhysicalMaterialAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                PhysicalMaterialAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"PhysicalMaterial"),
                                             PhysicalMaterialAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
        {
            AssetCreator creator;
            creator.label = String(u8"Collision Shape");
            creator.category = String(u8"Physics");
            creator.type = &CollisionShapeAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                CollisionShapeAsset asset;
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"CollisionShape"),
                                             CollisionShapeAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }
}

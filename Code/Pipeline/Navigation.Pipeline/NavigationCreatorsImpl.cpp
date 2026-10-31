// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Navigation.Pipeline - the navigation domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module navigation.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void RegisterNavigationCreators(AssetCreatorRegistry& registry)
    {
        {
            // An empty zone: the NavMeshZoneComponent's Bake fills its navmesh sidecar.
            AssetCreator creator;
            creator.label = String(u8"Navigation Zone");
            creator.type = &NavigationZoneAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                foundation::content::Group* target = context.Target();
                if (target == nullptr)
                {
                    return nullptr;
                }
                foundation::content::Instance* instance = target->CreateInstance(
                    target->UniqueInstanceName(context.NameOr(u8"NavZone")).AsView(),
                    NavigationZoneAsset::StaticType());
                NavigationZoneAsset asset;
                if (instance == nullptr || !WriteNavigationZoneAsset(*instance, asset).IsOk())
                {
                    return nullptr;
                }
                return instance;
            };
            registry.Register(Move(creator));
        }
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(NavigationZoneAsset, "rtti::pipeline::navigation")
    {
        builder.DataVersion(2) // 2: no frame stamp
            .Attribute("displayName", String(u8"Navigation Zone"));
    }
}

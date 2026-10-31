// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PropertyAnimation.Pipeline - the property animation domain's New Asset creators: what File > New
// and asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module propertyanimation.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance* CreatePropertyAnimationClip(foundation::content::Group* target,
                                                               StringView name)
    {
        if (target == nullptr)
        {
            return nullptr;
        }
        foundation::content::Instance* instance = target->CreateInstance(
            target->UniqueInstanceName(name.IsEmpty() ? StringView(u8"Clip") : name).AsView(),
            PropertyAnimationClipAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        PropertyAnimationClipAsset asset;
        return instance->WriteObject(asset).IsOk() ? instance : nullptr;
    }

    void RegisterPropertyAnimationCreators(AssetCreatorRegistry& registry)
    {
        {
            // An empty clip in the picked group (clips are authored in the editor's panel).
            AssetCreator creator;
            creator.label = String(u8"Property Animation Clip");
            creator.category = String(u8"Animation");
            creator.type = &PropertyAnimationClipAsset::StaticType();
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            { return CreatePropertyAnimationClip(context.Target(), context.name); };
            registry.Register(Move(creator));
        }
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(PropertyAnimationClipAsset, "rtti::pipeline::propertyanimation")
    {
        builder.Attribute("displayName", String(u8"Property Animation Clip"));
    }
}

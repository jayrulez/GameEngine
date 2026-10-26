// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Animation.Pipeline - the animation domain's New Asset creators
// (agent-playtesting-and-asset-creation.md P1): what File > New and asset_create make, with no
// editor (the source database and the sources folder are all a creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module animation.pipeline;

import foundation.core;
import foundation.content;
import foundation.animation;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    void SeedDefaultAnimationGraph(AnimationGraphAsset& asset)
    {
        namespace animation = foundation::animation;
        animation::AnimationGraphSource& source = asset.source;
        source.paramNames.PushBack(String(u8"Speed"));
        source.paramTypes.PushBack(0); // Float
        source.paramFloats.PushBack(0.0f);
        source.paramInts.PushBack(0);
        source.paramBools.PushBack(0);

        animation::GraphLayerData layer;
        layer.name = String(u8"Base");
        animation::GraphStateData idle;
        idle.name = String(u8"Idle");
        idle.node.kind = 0; // clip (unassigned - pick in the inspector)
        layer.states.PushBack(Move(idle));
        layer.defaultState = 0;
        source.layers.PushBack(Move(layer));

        Array<Float2> positions;
        positions.PushBack(Float2{280.0f, 120.0f});
        asset.layerStatePositions.PushBack(Move(positions));
        asset.layerAnyStatePositions.PushBack(Float2{60.0f, 40.0f});
    }

    void RegisterAnimationCreators(AssetCreatorRegistry& registry)
    {
        {
            // A graph with a Speed parameter and one Base layer holding an Idle state.
            AssetCreator creator;
            creator.label = String(u8"Animation Graph");
            creator.category = String(u8"Animation");
            creator.type = &AnimationGraphAsset::StaticType();
            creator.defaultGroup = String(u8"Animation");
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                AnimationGraphAsset asset;
                SeedDefaultAnimationGraph(asset);
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"AnimationGraph"),
                                             AnimationGraphAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(SkeletonAsset, "rtti::pipeline::animation")
    {
        builder.Attribute("displayName", String(u8"Skeleton"));
    }

    REFLECT_MEMBERS(AnimationClipAsset, "rtti::pipeline::animation")
    {
        builder.Attribute("displayName", String(u8"Animation Clip"));
    }

    REFLECT_MEMBERS(AnimationGraphAsset, "rtti::pipeline::animation")
    {
        builder.Attribute("displayName", String(u8"Animation Graph"));
    }
}

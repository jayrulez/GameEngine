// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::PropertyAnimation - `propertyanimation.pipeline` (tooling).
//
// The source asset for a property-animation clip + its XML->binary builder. Clips are authored
// in-editor (no OS-file importer): the asset EMBEDS the cooked wire struct (PropertyAnimationClipSource)
// as its authored data (the AnimationClipAsset pattern), so the builder writes it verbatim to the
// cooked product and the runtime factory (foundation.propertyanimation.resource) loads it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module propertyanimation.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content; // CreatePropertyAnimationClip
import foundation.propertyanimation;
import foundation.propertyanimation.resource;

using namespace foundation::core;

export namespace pipeline
{
    namespace propanim = foundation::propertyanimation;

    // Source asset: the authored clip. `source` IS the cooked wire (edited in place by the clip page);
    // the builder cooks it unchanged.
    class PropertyAnimationClipAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(PropertyAnimationClipAsset, pipeline::Asset)
    public:
        propanim::PropertyAnimationClipSource source;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            source.Serialize(ar);
        }
    };

    // Cooks the authored asset -> the PropertyAnimationClipSource product (written verbatim; the
    // runtime factory rebuilds the evaluate-ready clip).
    class PropertyAnimationClipAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &PropertyAnimationClipAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &propanim::PropertyAnimationClipSource::StaticType();
        }
        // v2: dropped the dead `loop` source field (loop is the animator component's loopMode).
        // v3: each key's tangent mode (keyTangentMode).
        [[nodiscard]] u32 Version() const override { return 3; }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const PropertyAnimationClipAsset& a = static_cast<const PropertyAnimationClipAsset&>(asset);
            return ctx.output->WriteObject(
                const_cast<propanim::PropertyAnimationClipSource&>(a.source));
        }
    };

    // Register the asset type (Pipeline domain) + the cooked source/resource types (Runtime domain).
    inline void RegisterPropertyAnimationAssets()
    {
        propanim::RegisterPropertyAnimationResource(); // the cooked source + runtime resource types
        GlobalTypeRegistry().Register(PropertyAnimationClipAsset::StaticType(),
                                      TypeDomain(u8"Pipeline"));
        RegisterSerializable<PropertyAnimationClipAsset>();
    }


    /// An empty clip in `target`, named `name` made unique (empty: "Clip"); the creator and
    /// the animation panel's Create Clip both make clips through this.
    [[nodiscard]] foundation::content::Instance* CreatePropertyAnimationClip(
        foundation::content::Group* target, StringView name);
    /// File > New's property animation creator (pipeline.registration composes every domain's).
    void RegisterPropertyAnimationCreators(AssetCreatorRegistry& registry);
}

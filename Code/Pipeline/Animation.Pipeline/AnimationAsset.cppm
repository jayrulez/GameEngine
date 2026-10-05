// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::Animation - the `foundation.animation.editor` module.
///
/// Authoring/cook side: a SkeletonAsset / AnimationClipAsset wraps the cooked source + the source
/// file reference; the builders cook them into the content DB (Source -> product at load). Mirrors
/// foundation.geometry.editor. (Sources are populated round-trip from the runtime types via the
/// resource layer; the model importer - foundation.model IR -> these sources - is a later transform.)

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module animation.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content;
import foundation.animation;
import foundation.animation.resource;

using namespace foundation::core;
using namespace foundation::animation;

export namespace pipeline{

    class SkeletonAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(SkeletonAsset, pipeline::Asset)
    public:
        SkeletonSource source;
        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (source model note)
            source.Serialize(ar);
        }
    };

    class AnimationClipAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(AnimationClipAsset, pipeline::Asset)
    public:
        AnimationClipSource source;
        // Appended (root-motion.md P0), for the root motion cook: the skeleton the clip's bone
        // indices address (a named root resolves against it), and the armature's rest transform
        // (its own channels, the clip's model tracks, are turned into model space by its inverse).
        // The importer sets both.
        Guid skeleton;
        Transform modelRest;
        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            source.Serialize(ar);
            SerializeAppended(ar, "skeleton", skeleton);
            SerializeAppended(ar, "restPosition", modelRest.position);
            SerializeAppended(ar, "restRotation", modelRest.rotation);
            SerializeAppended(ar, "restScale", modelRest.scale);
        }
    };

    // The animation graph is authored directly into its source (state machine, blend trees, clip refs).
    // Editor-only canvas layout rides on the ASSET (the builder cooks `source` alone, so none of it
    // reaches the runtime wire): per layer, per state, the node position on the graph canvas -
    // parallel to source.layers[i].states (the page keeps them in sync on add/remove).
    class AnimationGraphAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(AnimationGraphAsset, pipeline::Asset)
    public:
        AnimationGraphSource source;
        Array<Array<Float2>> layerStatePositions;
        Array<Float2> layerAnyStatePositions; // the per-layer "Any State" pseudo-node
        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            source.Serialize(ar);
            foundation::core::Serialize(ar, "layerStatePositions", layerStatePositions);
            foundation::core::Serialize(ar, "layerAnyStatePositions", layerAnyStatePositions);
        }
    };

    class SkeletonAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &SkeletonAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SkeletonSource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const SkeletonAsset& a = static_cast<const SkeletonAsset&>(asset);
            return ctx.output->WriteObject(const_cast<SkeletonSource&>(a.source));
        }
    };

    class AnimationClipAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &AnimationClipAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AnimationClipSource::StaticType();
        }
        // v2 (root-motion.md P0): the root motion bake and strip; every clip re-cooks.
        [[nodiscard]] u32 Version() const override { return 2; }

        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const AnimationClipAsset& a = static_cast<const AnimationClipAsset&>(asset);
            if (a.source.rootMotion.Any() && !a.skeleton.IsNil())
            {
                out.reads.PushBack(a.skeleton); // a renamed or rebuilt skeleton re-cooks the clip
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const AnimationClipAsset& a = static_cast<const AnimationClipAsset&>(asset);
            if (!a.source.rootMotion.Any())
            {
                return ctx.output->WriteObject(const_cast<AnimationClipSource&>(a.source));
            }
            AnimationClipSource cooked;
            cooked.CopyFrom(a.source);
            const i32 root = RootMotionRoot(a, ctx);
            if (root < -1)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (!BakeRootMotion(cooked, root, a.modelRest))
            {
                LOG_ERROR(u8"Animation", u8"clip '{}': root motion has no track to take from its root",
                          a.source.name.AsView());
                return Status{ErrorCode::InvalidArgument};
            }
            return ctx.output->WriteObject(cooked);
        }

    private:
        /// The root the clip's travel is taken from: the named bone, else the armature's own
        /// channels (model tracks) when the clip has them, else the skeleton's first root. -2 (and
        /// a logged reason) when it cannot be found.
        [[nodiscard]] static i32 RootMotionRoot(const AnimationClipAsset& a, pipeline::AssetBuildContext& ctx)
        {
            const StringView name = a.source.rootMotion.rootBone.AsView();
            if (name.IsEmpty())
            {
                for (const i32 bone : a.source.trackBone)
                {
                    if (bone < 0)
                    {
                        return -1;
                    }
                }
            }
            foundation::content::Instance* instance =
                (ctx.sourceDb != nullptr && !a.skeleton.IsNil()) ? ctx.sourceDb->GetInstance(a.skeleton) : nullptr;
            RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
            const auto* skeleton = Cast<SkeletonAsset>(object.Get());
            if (skeleton == nullptr)
            {
                LOG_ERROR(u8"Animation", u8"clip '{}': root motion needs its skeleton, which it does not name",
                          a.source.name.AsView());
                return -2;
            }
            const SkeletonSource& bones = skeleton->source;
            for (usize i = 0; i < bones.boneNames.Size(); ++i)
            {
                const bool match = name.IsEmpty() ? (i < bones.parentIndices.Size() && bones.parentIndices[i] < 0)
                                                  : bones.boneNames[i].AsView() == name;
                if (match)
                {
                    return static_cast<i32>(i);
                }
            }
            LOG_ERROR(u8"Animation", u8"clip '{}': root motion's root bone '{}' is not in its skeleton",
                      a.source.name.AsView(), name);
            return -2;
        }
    };

    // Registers the animation asset types for content-DB construction + deserialization.
    inline void RegisterAnimationAssets()
    {
        GlobalTypeRegistry().Register(SkeletonAsset::StaticType(), TypeDomain(u8"Pipeline"));
        GlobalTypeRegistry().Register(AnimationClipAsset::StaticType(), TypeDomain(u8"Pipeline"));
        GlobalTypeRegistry().Register(AnimationGraphAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<SkeletonAsset>();
        RegisterSerializable<AnimationClipAsset>();
        RegisterSerializable<AnimationGraphAsset>();
        // The graph's cooked PRODUCT: ReadObject constructs it by type name in cook hosts, so
        // it must be registered here too (the clip/skeleton products ride
        // RegisterModelResourceTypes; the graph source has no other registration site).
        GlobalTypeRegistry().Register(foundation::animation::AnimationGraphSource::StaticType());
        RegisterSerializable<foundation::animation::AnimationGraphSource>();
    }

    class AnimationGraphAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &AnimationGraphAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &AnimationGraphSource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const AnimationGraphAsset& a = static_cast<const AnimationGraphAsset&>(asset);
            return ctx.output->WriteObject(const_cast<AnimationGraphSource&>(a.source));
        }
    };

    RTTI_DEFINE_OBJECT(SkeletonAsset, "rtti::pipeline::animation")
    RTTI_DEFINE_OBJECT(AnimationClipAsset, "rtti::pipeline::animation")
    RTTI_DEFINE_OBJECT(AnimationGraphAsset, "rtti::pipeline::animation")


    /// A new graph's content: a Speed parameter and a Base layer holding an Idle state.
    void SeedDefaultAnimationGraph(AnimationGraphAsset& asset);
    /// File > New's animation creators (pipeline.registration composes every domain's).
    void RegisterAnimationCreators(AssetCreatorRegistry& registry);
} // namespace foundation::animation

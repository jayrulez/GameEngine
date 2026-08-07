/// Draconic::AnimationEditor - the `draconic.animation.editor` module.
///
/// Authoring/cook side: a SkeletonAsset / AnimationClipAsset wraps the cooked source + the source
/// file reference; the builders cook them into the content DB (Source -> product at load). Mirrors
/// draconic.geometry.editor. (The model importer - draconic.model IR -> these sources - lands later;
/// for now sources are populated round-trip from the runtime types via the resource layer.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.animation.editor;

import draconic.foundation;
import draconic.editor;
import draconic.content;
import draconic.animation;
import draconic.animation.resource;

using namespace draconic::foundation;

export namespace draconic::animation
{

    class SkeletonAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(SkeletonAsset, draconic::editor::Asset)
    public:
        SkeletonSource source;
        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (source model note)
            source.Serialize(ar);
        }
    };

    class AnimationClipAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(AnimationClipAsset, draconic::editor::Asset)
    public:
        AnimationClipSource source;
        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            source.Serialize(ar);
        }
    };

    // The animation graph is authored directly into its source (state machine, blend trees, clip refs).
    // Editor-only canvas layout rides on the ASSET (the builder cooks `source` alone, so none of it
    // reaches the runtime wire): per layer, per state, the node position on the graph canvas -
    // parallel to source.layers[i].states (the page keeps them in sync on add/remove).
    class AnimationGraphAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(AnimationGraphAsset, draconic::editor::Asset)
    public:
        AnimationGraphSource source;
        Array<Array<Float2>> layerStatePositions;
        Array<Float2> layerAnyStatePositions; // the per-layer "Any State" pseudo-node
        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            source.Serialize(ar);
            draconic::foundation::Serialize(ar, "layerStatePositions", layerStatePositions);
            draconic::foundation::Serialize(ar, "layerAnyStatePositions", layerAnyStatePositions);
        }
    };

    class SkeletonAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const SkeletonAsset& a = static_cast<const SkeletonAsset&>(asset);
            return ctx.output->WriteObject(const_cast<SkeletonSource&>(a.source));
        }
    };

    class AnimationClipAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const AnimationClipAsset& a = static_cast<const AnimationClipAsset&>(asset);
            return ctx.output->WriteObject(const_cast<AnimationClipSource&>(a.source));
        }
    };

    // Registers the animation asset types for content-DB construction + deserialization.
    inline void RegisterAnimationAssets()
    {
        GlobalTypeRegistry().Register(SkeletonAsset::StaticType(), TypeDomain(u8"Editor"));
        GlobalTypeRegistry().Register(AnimationClipAsset::StaticType(), TypeDomain(u8"Editor"));
        GlobalTypeRegistry().Register(AnimationGraphAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<SkeletonAsset>();
        RegisterSerializable<AnimationClipAsset>();
        RegisterSerializable<AnimationGraphAsset>();
    }

    class AnimationGraphAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const AnimationGraphAsset& a = static_cast<const AnimationGraphAsset&>(asset);
            return ctx.output->WriteObject(const_cast<AnimationGraphSource&>(a.source));
        }
    };

    DRACONIC_DEFINE_OBJECT(SkeletonAsset, "draconic::animation")
    DRACONIC_DEFINE_OBJECT(AnimationClipAsset, "draconic::animation")
    DRACONIC_DEFINE_OBJECT(AnimationGraphAsset, "draconic::animation")

} // namespace draconic::animation

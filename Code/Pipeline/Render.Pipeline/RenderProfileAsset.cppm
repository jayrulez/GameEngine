// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Render - the `render.pipeline` module (tooling): the render profile assets. An
// Environment Profile or a Post Process Profile carries the value fields of a scene's environment
// or post settings block (the block's own serializer writes them), cooks to its record by a copy,
// and is what a block whose source is Profile uses (engine.render :profiles).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module render.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;
import engine.render;

using namespace foundation::core;

export namespace pipeline
{
    // A profile asset: a settings block's values, reached without naming the block (the editor
    // edits any profile through this, the reflected layout ValuesType() over Values()).
    class SettingsProfileAsset : public pipeline::Asset
    {
        RTTI_OBJECT(SettingsProfileAsset, pipeline::Asset)
    public:
        [[nodiscard]] virtual const TypeInfo* ValuesType() const noexcept = 0;
        [[nodiscard]] virtual void* Values() noexcept = 0;
        /// Take a settings block's values (ValuesType()'s layout); the block's own source and
        /// profile reference are not a profile's.
        virtual void SetValues(const void* values) = 0;
        /// Give a settings block these values (a preview scene's); its source and profile kept.
        virtual void CopyValuesInto(void* block) const = 0;
    };

    class EnvironmentProfileAsset final : public SettingsProfileAsset
    {
        RTTI_OBJECT(EnvironmentProfileAsset, SettingsProfileAsset)
    public:
        engine::render::EnvironmentSettings values;
        [[nodiscard]] const TypeInfo* ValuesType() const noexcept override
        {
            return &TypeOf<engine::render::EnvironmentSettings>();
        }
        [[nodiscard]] void* Values() noexcept override { return &values; }
        void SetValues(const void* from) override
        {
            values = *static_cast<const engine::render::EnvironmentSettings*>(from);
            values.source = engine::render::SettingsSource::Scene;
            values.profile = {};
        }
        void CopyValuesInto(void* block) const override
        {
            auto& to = *static_cast<engine::render::EnvironmentSettings*>(block);
            const engine::render::SettingsSource source = to.source;
            const auto profile = to.profile;
            to = values;
            to.source = source;
            to.profile = profile;
        }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            engine::render::SerializeEnvironmentValues(ar, values, true);
        }
    };

    class PostProcessProfileAsset final : public SettingsProfileAsset
    {
        RTTI_OBJECT(PostProcessProfileAsset, SettingsProfileAsset)
    public:
        engine::render::PostProcessSettings values;
        [[nodiscard]] const TypeInfo* ValuesType() const noexcept override
        {
            return &TypeOf<engine::render::PostProcessSettings>();
        }
        [[nodiscard]] void* Values() noexcept override { return &values; }
        void SetValues(const void* from) override
        {
            values = *static_cast<const engine::render::PostProcessSettings*>(from);
            values.source = engine::render::SettingsSource::Scene;
            values.profile = {};
        }
        void CopyValuesInto(void* block) const override
        {
            auto& to = *static_cast<engine::render::PostProcessSettings*>(block);
            const engine::render::SettingsSource source = to.source;
            const auto profile = to.profile;
            to = values;
            to.source = source;
            to.profile = profile;
        }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            engine::render::SerializePostValues(ar, values);
        }
    };

    class EnvironmentProfileAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &EnvironmentProfileAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &engine::render::EnvironmentProfileSource::StaticType();
        }
        // The sky texture is a runtime reference: its product must exist, but its edits never
        // re-cook the profile (the factory re-binds it at load).
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const auto& profile = static_cast<const EnvironmentProfileAsset&>(asset);
            if (!profile.values.skyTexture.id.IsNil())
            {
                out.references.PushBack(profile.values.skyTexture.id);
            }
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            engine::render::EnvironmentProfileSource cooked;
            cooked.values = static_cast<const EnvironmentProfileAsset&>(asset).values;
            return ctx.output->WriteObject(cooked);
        }
    };

    class PostProcessProfileAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &PostProcessProfileAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &engine::render::PostProcessProfileSource::StaticType();
        }
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const auto& profile = static_cast<const PostProcessProfileAsset&>(asset);
            if (!profile.values.gradingLut.id.IsNil())
            {
                out.references.PushBack(profile.values.gradingLut.id);
            }
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            engine::render::PostProcessProfileSource cooked;
            cooked.values = static_cast<const PostProcessProfileAsset&>(asset).values;
            return ctx.output->WriteObject(cooked);
        }
    };

    // Registers the asset types for content-DB construction + deserialization.
    inline void RegisterRenderProfileAssets()
    {
        GlobalTypeRegistry().Register(SettingsProfileAsset::StaticType(), TypeDomain(u8"Pipeline"));
        GlobalTypeRegistry().Register(EnvironmentProfileAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<EnvironmentProfileAsset>();
        GlobalTypeRegistry().Register(PostProcessProfileAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<PostProcessProfileAsset>();
    }

    /// File > New's render creators (pipeline.registration composes every domain's).
    void RegisterRenderCreators(AssetCreatorRegistry& registry);

    RTTI_DEFINE_OBJECT(SettingsProfileAsset, "rtti::pipeline::render")
    // The concrete profiles' reflection (their display names) is in RenderProfileAssetImpl.cpp.
}

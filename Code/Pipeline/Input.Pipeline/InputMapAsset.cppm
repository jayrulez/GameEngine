// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Input - the `foundation.input.editor` module.
//
// The authored input-map asset (a source with an XML envelope, like every authored asset) and
// its builder. The cook validates and writes through: the model is pure data, so the bake's
// only job is refusing kind-mismatched or nameless entries before they reach the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module input.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;
import foundation.input;
import foundation.input.resource;

using namespace foundation::core;
using namespace foundation::input;

export namespace pipeline{
    class InputMapAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(InputMapAsset, pipeline::Asset)
    public:
        [[nodiscard]] InputMap& Map() noexcept { return m_map; }
        [[nodiscard]] const InputMap& Map() const noexcept { return m_map; }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused; authored in the editor)
            SerializeInputMap(ar, m_map);
        }

        /// A fresh asset seeds the conventional starter set so the editor page never opens
        /// empty: Gameplay with Move, Look, Jump and Fire actions (bindings left empty).
        void SeedDefaultContent()
        {
            ActionSet gameplay;
            gameplay.name = String(u8"Gameplay");
            const StringView names[] = {u8"Move", u8"Look", u8"Jump", u8"Fire"};
            const ActionKind kinds[] = {ActionKind::Axis2D, ActionKind::Axis2D, ActionKind::Button,
                                        ActionKind::Button};
            for (usize i = 0; i < 4; ++i)
            {
                Action action;
                action.name = String(names[i]);
                action.kind = kinds[i];
                gameplay.actions.PushBack(static_cast<Action&&>(action));
            }
            m_map.sets.PushBack(static_cast<ActionSet&&>(gameplay));
        }

        // Public so the reflected Nested `map` property can take its address (the reflect body is
        // a free function); the Map() accessors above remain the preferred call site.
        InputMap m_map;
    };

    class InputMapAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &InputMapAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &InputMapResource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const InputMapAsset& source = static_cast<const InputMapAsset&>(asset);
            String error;
            if (!ValidateInputMap(source.Map(), &error))
            {
                LOG_ERROR(u8"Cook", u8"input map invalid: {}", error);
                return Status{ErrorCode::InvalidArgument};
            }
            InputMapResource cooked;
            cooked.Map() = source.Map();
            return ctx.output->WriteObject(cooked);
        }
    };

    // Registers the asset and cooked types (tooling side).
    inline void RegisterInputMapAsset()
    {
        RegisterInputMapResource();
        RegisterInputTypeReflection(); // the InputMap tree the asset's Nested `map` recurses into
        GlobalTypeRegistry().Register(InputMapAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<InputMapAsset>();
    }

    // InputMapAsset::StaticType() is defined with its reflected surface (a Nested `map` property)
    // in InputMapAssetImpl.cpp, keeping REFLECT_MEMBERS out of the interface for GCC.

    /// File > New's input creators (pipeline.registration composes every domain's).
    void RegisterInputCreators(AssetCreatorRegistry& registry);
}

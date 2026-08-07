// Draconic::InputEditor - the `draconic.input.editor` module.
//
// The authored input-map asset (source, XML envelope like every authored asset) + its
// builder. Cook = VALIDATE + write-through: the model is pure data, so the bake's whole
// job is refusing kind-mismatched or nameless entries before they reach the runtime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.input.editor;

import draconic.foundation;
import draconic.content;
import draconic.editor;
import draconic.input;
import draconic.input.resource;

using namespace draconic::foundation;

export namespace draconic::input
{
    class InputMapAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(InputMapAsset, draconic::editor::Asset)
    public:
        [[nodiscard]] InputMap& Map() noexcept { return m_map; }
        [[nodiscard]] const InputMap& Map() const noexcept { return m_map; }

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (unused - authored in-editor)
            SerializeInputMap(ar, m_map);
        }

        /// A fresh asset seeds the conventional starter set so the editor page never opens
        /// on a void: Gameplay with Move/Look/Jump/Fire skeletons (bindings left empty).
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

    class InputMapAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const InputMapAsset& source = static_cast<const InputMapAsset&>(asset);
            String error;
            if (!ValidateInputMap(source.Map(), &error))
            {
                DRACONIC_LOG_ERROR(u8"Cook", u8"input map invalid: {}", error);
                return Status{ErrorCode::InvalidArgument};
            }
            InputMapResource cooked;
            cooked.Map() = source.Map();
            return ctx.output->WriteObject(cooked);
        }
    };

    // Register asset + cooked types (tooling-side).
    inline void RegisterInputMapAsset()
    {
        RegisterInputMapResource();
        RegisterInputTypeReflection(); // the InputMap tree the asset's Nested `map` recurses into
        GlobalTypeRegistry().Register(InputMapAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<InputMapAsset>();
    }

    // InputMapAsset::StaticType() is defined WITH its reflected surface (a Nested `map` property)
    // in InputMapAssetImpl.cpp - GCC module hygiene: DRACONIC_REFLECT out of interfaces.
}

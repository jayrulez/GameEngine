// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Input.Resource - the `foundation.input.resource` module.
//
// The cooked form of an input map: pure data (no processing beyond validation at cook),
// deserialized straight into the runtime's InputMap. The subsystem/app binds the project's
// default map through the ResourceManager and hands it to an ActionRuntime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.input.resource;

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.input;

using namespace foundation::core;
namespace content = foundation::content;
namespace resource = foundation::resource;

export namespace foundation::input
{
    // Cooked record and runtime product (data only, no GPU transform, as particles are).
    class InputMapResource final : public ISerializable
    {
        RTTI_OBJECT(InputMapResource, ISerializable)
    public:
        [[nodiscard]] InputMap& Map() noexcept { return m_map; }
        [[nodiscard]] const InputMap& Map() const noexcept { return m_map; }
        void Serialize(ISerializer& ar) override { SerializeInputMap(ar, m_map); }

    private:
        InputMap m_map;
    };

    class InputMapFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &InputMapResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &InputMapResource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            content::Instance& instance) override
        {
            return instance.ReadObject();
        }
    };

    // Call once at startup (tooling and runtime alike).
    inline void RegisterInputMapResource()
    {
        GlobalTypeRegistry().Register(InputMapResource::StaticType());
        RegisterSerializable<InputMapResource>();
    }

    RTTI_DEFINE_OBJECT(InputMapResource, "rtti::input")
}

export namespace foundation::input
{
    /// The input resource module: the module the engine composition composes this
    /// library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kInputResourceFactories[] = {
        foundation::resource::FactoryByDefault<InputMapResource, InputMapResource, InputMapFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kInputResourceModule{
        u8"input", &RegisterInputMapResource, kInputResourceFactories,
        sizeof(kInputResourceFactories) / sizeof(kInputResourceFactories[0])};
}

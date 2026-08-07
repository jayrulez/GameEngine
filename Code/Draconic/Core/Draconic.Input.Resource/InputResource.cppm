// Draconic::InputResource - the `draconic.input.resource` module.
//
// The cooked form of an input map: pure data (no processing beyond validation at cook),
// deserialized straight into the runtime's InputMap. The subsystem/app binds the project's
// default map through the ResourceManager and hands it to an ActionRuntime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.input.resource;

import draconic.foundation;
import draconic.content;
import draconic.resource;
import draconic.input;

using namespace draconic::foundation;

export namespace draconic::input
{
    // Cooked record AND runtime product (data-only, no GPU transform - the particles model).
    class InputMapResource final : public ISerializable
    {
        DRACONIC_OBJECT(InputMapResource, ISerializable)
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

    DRACONIC_DEFINE_OBJECT(InputMapResource, "draconic::input")
}

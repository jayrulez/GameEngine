// Draconic Foundation - :serializable_registry partition
//
// SerializableRegistry: maps a reflected TypeId to a factory that default-builds
// the concrete ISerializable. The polymorphic load path (content database) reads
// a stored type name, resolves the TypeInfo via the type registry, then asks
// this registry to create the object before running Serialize() on it.
//
// Registration is explicit (RegisterSerializable<T>()), matching the reflection
// registration convention - no static-init-order reliance.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:serializable_registry;

import :base;
import :type_info;
import :ref_counted;
import :allocator;
import :hash_map;
import :iserializable;
import :object; // Cast<Base> for the polymorphic-container create adapter

export namespace draconic::foundation
{
    using SerializableFactory = RefPtr<ISerializable> (*)();

    class SerializableRegistry
    {
    public:
        void Register(TypeId id, SerializableFactory factory)
        {
            m_factories.InsertOrAssign(id, factory);
        }

        // Default-builds the ISerializable for `id`, or null if unregistered.
        [[nodiscard]] RefPtr<ISerializable> Create(TypeId id) const
        {
            const SerializableFactory* factory = m_factories.Find(id);
            return (factory != nullptr) ? (*factory)() : RefPtr<ISerializable>{};
        }

        [[nodiscard]] bool Contains(TypeId id) const { return m_factories.Find(id) != nullptr; }

    private:
        HashMap<TypeId, SerializableFactory> m_factories;
    };

    [[nodiscard]] SerializableRegistry& GlobalSerializableRegistry() noexcept
    {
        static SerializableRegistry registry;
        return registry;
    }

    // Registers `T`'s default factory. Call once at startup (e.g. from a module's
    // RegisterTypes()). T must derive ISerializable and have a default ctor.
    template <typename T>
    void RegisterSerializable(SerializableRegistry& registry = GlobalSerializableRegistry())
    {
        registry.Register(T::StaticType().id,
                          []() -> RefPtr<ISerializable> { return MakeRef<T>(DefaultAllocator()); });
    }

    // The standard create-by-type adapter for a polymorphic reflected container of `Base` elements:
    // wraps this registry so reflection's RegisterPolymorphicArrayType<Base> can create + eligibility-
    // check by type WITHOUT importing serialization (capability flows into reflection as function
    // pointers - CONVENTIONS.md). Registrants pass &CreateSerializableElement<Base> +
    // &CanCreateSerializableElement<Base>. Create returns null when `concrete` is unregistered or not
    // a `Base` (the reflection side then reports a clean failure).
    template <typename Base>
    [[nodiscard]] RefPtr<Base> CreateSerializableElement(const TypeInfo& concrete)
    {
        return RefPtr<Base>(Cast<Base>(GlobalSerializableRegistry().Create(concrete.id).Get()));
    }
    template <typename Base>
    [[nodiscard]] bool CanCreateSerializableElement(const TypeInfo& concrete)
    {
        return GlobalSerializableRegistry().Contains(concrete.id);
    }
}

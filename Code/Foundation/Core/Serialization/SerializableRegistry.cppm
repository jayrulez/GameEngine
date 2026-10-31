// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :serializable_registry partition
//
// SerializableRegistry: maps a reflected TypeId to a factory that default-builds
// the concrete ISerializable. The polymorphic load path (content database) reads
// a stored type name, resolves the TypeInfo via the type registry, then asks
// this registry to create the object before running Serialize() on it.
//
// Registration is explicit (RegisterSerializable<T>()), matching the reflection
// registration convention; nothing relies on static-init order.

module;
#include "Core/Prelude.h"

export module foundation.core:serializable_registry;

import :base;
import :type_info;
import :ref_counted;
import :allocator;
import :hash_map;
import :iserializable;
import :object; // Cast<Base> for the polymorphic-container create adapter
import :mutex;
import :scoped_lock;

export namespace foundation::core
{
    using SerializableFactory = RefPtr<ISerializable> (*)();

    class SerializableRegistry
    {
    public:
        void Register(TypeId id, SerializableFactory factory)
        {
            ScopedLock lock(m_lock); // registrars run from cook workers too (TypeRegistry rule)
            const bool inserted = m_factories.Find(id) == nullptr;
            m_factories.InsertOrAssign(id, factory);
            if (inserted && m_observer != nullptr)
            {
                m_observer(m_observerContext, id); // fires only on a real insert
            }
        }

        // Remove a factory (hot reload: a plugin's recorded registrations reverse before
        // its library closes, since a factory pointer into an unloaded module is a dangling
        // call). No-op for unknown ids; the rebuilt module re-registers into the slot.
        void Unregister(TypeId id)
        {
            ScopedLock lock(m_lock);
            m_factories.Remove(id);
        }

        // Registration observer (one at a time): the PluginHost recording hook; see
        // TypeRegistry::SetRegistrationObserver for the contract.
        void SetRegistrationObserver(void (*observer)(void*, TypeId), void* context) noexcept
        {
            m_observer = observer;
            m_observerContext = context;
        }

        // Default-builds the ISerializable for `id`, or null if unregistered.
        [[nodiscard]] RefPtr<ISerializable> Create(TypeId id) const
        {
            const SerializableFactory* factory = m_factories.Find(id);
            return (factory != nullptr) ? (*factory)() : RefPtr<ISerializable>{};
        }

        [[nodiscard]] bool Contains(TypeId id) const { return m_factories.Find(id) != nullptr; }

    private:
        Mutex m_lock;
        HashMap<TypeId, SerializableFactory> m_factories;
        void (*m_observer)(void*, TypeId) = nullptr;
        void* m_observerContext = nullptr;
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
    // wraps this registry so reflection's RegisterPolymorphicArrayType<Base> can create and
    // eligibility-check by type without importing serialization (the capability reaches reflection
    // as function pointers). Registrants pass &CreateSerializableElement<Base> and
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

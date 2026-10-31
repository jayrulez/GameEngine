// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :type_registry partition
//
// Explicit type registration; lookup by id or qualified name.

module;
#include "Core/Prelude.h"

export module foundation.core:type_registry;

import :base;
import :type_info;
import :array;
import :hash_map;
import :string;
import :string_hash;
import :mutex;
import :scoped_lock;

export namespace foundation::core
{
    // The availability domain a type was registered under: an open set identified by
    // name hash, not an enum. Core owns only the default domain "Runtime" (what the player
    // runtime registers); other layers tag registrations with their own names (the editor
    // passes TypeDomain(u8"Editor")). A type's domain says which processes have the type.
    // Tooling reads it (e.g. the script API browser marks non-runtime bindings); runtime
    // behavior never depends on it.
    class TypeDomain
    {
    public:
        constexpr TypeDomain() noexcept = default; // empty ("no domain"); registry-internal
        constexpr explicit TypeDomain(StringView name) noexcept : m_name(name) {}

        [[nodiscard]] constexpr StringHash Name() const noexcept { return m_name; }
        [[nodiscard]] constexpr bool operator==(const TypeDomain&) const noexcept = default;

    private:
        StringHash m_name;
    };

    inline constexpr TypeDomain kRuntimeTypeDomain{StringView(u8"Runtime")};

    // =======================================================================
    // Type registry - explicit registration; lookup by id or qualified name.
    // =======================================================================
    class TypeRegistry
    {
    public:
        // Mutations lock: registrars run from cook workers as well as the main thread, and two
        // first-time registrations at once would corrupt the maps. Reads stay lock-free by the
        // registration contract: a reader has called its own registrar (which returned after
        // the write it depends on completed), and the pipeline root registers everything up front.
        void Register(const TypeInfo& info, TypeDomain domain = kRuntimeTypeDomain)
        {
            ScopedLock lock(m_lock);
            if (m_byId.Contains(info.id))
            {
                // Already registered: the domain may only widen to Runtime. Registration
                // order is arbitrary, and if any path the player takes registers the type,
                // the player has it. It never narrows.
                if (domain == kRuntimeTypeDomain)
                {
                    m_domains.Remove(info.id); // absent = Runtime
                }
                return;
            }
            m_byId.InsertOrAssign(info.id, &info);
            m_all.PushBack(&info);
            if (!(domain == kRuntimeTypeDomain))
            {
                m_domains.InsertOrAssign(info.id, domain);
            }
            if (m_observer != nullptr)
            {
                m_observer(m_observerContext, info.id); // fires only on a real insert
            }
        }

        // Remove a type (hot reload: a plugin's RegistrationScope reverses what it
        // recorded before its library closes, since a registry entry pointing into an
        // unloaded module is a dangling read). No-op for unknown ids. A later
        // Register with the same id (the rebuilt module) takes the freed slot.
        void Unregister(TypeId id)
        {
            ScopedLock lock(m_lock);
            if (!m_byId.Contains(id))
            {
                return;
            }
            m_byId.Remove(id);
            m_domains.Remove(id);
            for (usize i = 0; i < m_all.Size(); ++i)
            {
                if (m_all[i]->id == id)
                {
                    m_all.RemoveAt(i);
                    break;
                }
            }
        }

        // Registration observer (one at a time): PluginHost records what a plugin's
        // OnLoad registers so unload can reverse it exactly, without the plugin keeping its
        // own list of registrations. Fires only on real inserts (a duplicate
        // Register is a no-op and stays owned by the first registrant).
        void SetRegistrationObserver(void (*observer)(void*, TypeId),
                                     void* context) noexcept
        {
            m_observer = observer;
            m_observerContext = context;
        }

        // The domain `id` was registered under; unknown ids (and unregistered types)
        // read as Runtime: the default is the absence of a tag.
        [[nodiscard]] TypeDomain DomainOf(TypeId id) const noexcept
        {
            const TypeDomain* found = m_domains.Find(id);
            return (found != nullptr) ? *found : kRuntimeTypeDomain;
        }

        [[nodiscard]] const TypeInfo* FindById(TypeId id) const noexcept
        {
            const TypeInfo* const* found = m_byId.Find(id);
            return (found != nullptr) ? *found : nullptr;
        }

        // Resolves possibly-foreign type metadata (another shared library's copy of the
        // same type; Register() keeps exactly one canonical TypeInfo per id) to the
        // registered instance. Unregistered types resolve to the argument itself, so the
        // result is always usable. Consumers reading patched metadata (properties,
        // container, enumerators, filled in by REFLECT_* registrars) must go through
        // this rather than trusting a local TypeOf<T>() copy.
        [[nodiscard]] const TypeInfo& Canonical(const TypeInfo& info) const noexcept
        {
            const TypeInfo* found = FindById(info.id);
            return (found != nullptr) ? *found : info;
        }

        [[nodiscard]] const TypeInfo* FindByName(const char* namespaceName,
                                                 const char* name) const noexcept
        {
            return FindById(ComputeTypeId(namespaceName, name)); // current spellings only
        }

        [[nodiscard]] usize Count() const noexcept { return m_all.Size(); }
        [[nodiscard]] const Array<const TypeInfo*>& All() const noexcept { return m_all; }

    private:
        Mutex m_lock;
        HashMap<TypeId, const TypeInfo*> m_byId;
        void (*m_observer)(void*, TypeId) = nullptr;
        void* m_observerContext = nullptr;
        Array<const TypeInfo*> m_all;
        HashMap<TypeId, TypeDomain> m_domains; // sparse: only non-Runtime entries
    };

    [[nodiscard]] TypeRegistry& GlobalTypeRegistry() noexcept
    {
        static TypeRegistry instance;
        return instance;
    }
}

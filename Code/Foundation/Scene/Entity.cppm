// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Scene - the `:entity` partition.
///
/// EntityHandle: a lightweight, copyable reference to an entity in a Scene - a pool
/// index plus a generation counter. The generation makes a stale handle (one whose
/// slot was destroyed and reused) detectable in O(1) without any lookup table. Never
/// store a raw pointer to entity/component data - always hold a handle and resolve it
/// through the Scene (the data-oriented discipline: pools move, handles don't).

module;
#include "Core/Prelude.h"

export module foundation.scene:entity;

import foundation.core;

using namespace foundation::core;

export namespace foundation::scene
{

    struct EntityHandle
    {
        static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

        u32 index = kInvalidIndex;
        u32 generation = 0;

        // The unassigned handle.
        [[nodiscard]] static constexpr EntityHandle Invalid() noexcept
        {
            return EntityHandle{kInvalidIndex, 0};
        }

        // Whether this handle was ever assigned (not necessarily still valid in a Scene -
        // ask Scene::IsValid for that).
        [[nodiscard]] constexpr bool IsAssigned() const noexcept { return index != kInvalidIndex; }

        [[nodiscard]] constexpr bool operator==(const EntityHandle& o) const noexcept
        {
            return index == o.index && generation == o.generation;
        }
        [[nodiscard]] constexpr bool operator!=(const EntityHandle& o) const noexcept
        {
            return !(*this == o);
        }
    };

    // A PERSISTENT reference to another entity by its stable Guid - the serialized, prefab-remappable
    // counterpart to EntityHandle (a transient live index+generation). A dedicated type, not a bare
    // Guid, so tooling can dispatch on it: the inspector renders an entity picker for EntityRef
    // fields (a bare Guid is ambiguous and renders nothing), and prefab instancing can find and remap
    // EntityRef fields by type. Resolve to a live handle at the point of use via
    // Scene::FindEntity(ref.id); EntityRef itself stays a dumb guid holder (no cached handle),
    // matching the "entity access re-resolves, never borrows" rule.
    struct EntityRef
    {
        Guid id; // stable id of the referenced entity (nil = unset)

        EntityRef() = default;
        // Implicit from Guid (like resource::Ref<T>): `ref = scene.GetEntityId(handle)`.
        EntityRef(const Guid& guid) noexcept : id(guid) {}

        [[nodiscard]] bool IsNil() const noexcept { return id.IsNil(); }
        [[nodiscard]] const Guid& Id() const noexcept { return id; }

        [[nodiscard]] bool operator==(const EntityRef& o) const noexcept { return id == o.id; }
        [[nodiscard]] bool operator!=(const EntityRef& o) const noexcept { return !(*this == o); }
    };

    // Identity-only serialization (found by ADL from component Serialize bodies), byte-identical to a
    // bare Guid - so migrating a field from Guid to EntityRef leaves the wire/disk format unchanged.
    inline void Serialize(ISerializer& ar, EntityRef& ref)
    {
        foundation::core::Serialize(ar, ref.id);
    }

    /// EntityRef's reflection: its `id`, so tooling finds what it names by property (an inspector
    /// takes it as a reference by that id). Idempotent; RegisterAllSceneComponentReflection calls it.
    void RegisterEntityRefReflection();

} // namespace foundation::scene

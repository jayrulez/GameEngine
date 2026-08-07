/// Draconic::Scene - the `:entity` partition.
///
/// EntityHandle: a lightweight, copyable reference to an entity in a Scene - a pool
/// index plus a generation counter. The generation makes a stale handle (one whose
/// slot was destroyed and reused) detectable in O(1) without any lookup table. Never
/// store a raw pointer to entity/component data - always hold a handle and resolve it
/// through the Scene (the data-oriented discipline: pools move, handles don't).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene:entity;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::scene
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

} // namespace draconic::scene

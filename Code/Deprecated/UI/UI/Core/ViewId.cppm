// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :view_id partition
//
// Unique identifier for a view. Used by managers (Input, Focus, DragDrop) to track
// views safely without raw pointers: if a view is deleted, lookups by its ViewId
// return null. Ported from Sedulous.UI/src/Core/ViewId.bf.

module;
#include "Core/Prelude.h"

export module foundation.ui:view_id;

import foundation.core; // Atomic

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    // ENGINE_EXPORT_DATA for ViewId::Invalid: a static data member crossing a
    // shared-library boundary needs the annotation (a .def entry is not enough -
    // only a dllimport declaration changes the use site). shared-libraries.md P5/W1.
    struct ENGINE_EXPORT_DATA ViewId
    {
        /// Creates a new unique ViewId. NON-inline (UITypeRegistryImpl.cpp): a
        /// per-library counter would mint duplicate ids across shared-library
        /// boundaries (shared-libraries.md rendezvous rule).
        [[nodiscard]] static ViewId Create() noexcept;

        [[nodiscard]] constexpr bool IsValid() const noexcept { return m_value != 0u; }
        /// Raw value for use as a hash-map key.
        [[nodiscard]] constexpr u32 RawValue() const noexcept { return m_value; }
        [[nodiscard]] constexpr u64 GetHashCode() const noexcept
        {
            return static_cast<u64>(m_value);
        }

        [[nodiscard]] constexpr bool Equals(ViewId other) const noexcept
        {
            return m_value == other.m_value;
        }
        [[nodiscard]] constexpr bool operator==(ViewId other) const noexcept
        {
            return m_value == other.m_value;
        }

        /// Append a debug string "ViewId(<value>)" (Sedulous ViewId.ToString).
        void ToString(core::String& out) const { core::AppendFormat(out, u8"ViewId({})", m_value); }

        static const ViewId Invalid;

    private:
        u32 m_value = 0u;
    };

    inline const ViewId ViewId::Invalid{};

}

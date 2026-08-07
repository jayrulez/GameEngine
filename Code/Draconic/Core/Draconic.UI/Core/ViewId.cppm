// Draconic UI - :view_id partition
//
// Unique identifier for a view. Used by managers (Input, Focus, DragDrop) to track
// views safely without raw pointers: if a view is deleted, lookups by its ViewId
// return null. Ported from Sedulous.UI/src/Core/ViewId.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:view_id;

import draconic.foundation; // Atomic

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    struct ViewId
    {
        /// Creates a new unique ViewId.
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
        void ToString(foundation::String& out) const { foundation::AppendFormat(out, u8"ViewId({})", m_value); }

        static const ViewId Invalid;

    private:
        u32 m_value = 0u;
    };

    inline const ViewId ViewId::Invalid{};

    inline ViewId ViewId::Create() noexcept
    {
        static foundation::Atomic<u32> s_next{1u};
        ViewId id;
        id.m_value = s_next.fetch_add(1u);
        return id;
    }
}

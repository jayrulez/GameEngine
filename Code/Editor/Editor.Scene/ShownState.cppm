// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :shown_state partition.
//
// What an inspector source showed at its last look, as the bytes its data serializes to. A source
// the inspector looks at every frame (its data may change under it: an undo, a simulation) tells
// the grid only when Take finds the bytes moved, so a frame where nothing moved costs the grid no
// layout.

module;
#include "Core/Prelude.h"

export module editor.scene:shown_state;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    class ShownState
    {
    public:
        explicit ShownState(IAllocator& allocator) : m_seen(allocator), m_allocator(&allocator) {}

        /// Whether what `write` serializes differs from the last look, remembering it. The first look
        /// returns true.
        [[nodiscard]] bool Take(const Function<void(ISerializer&)>& write)
        {
            MemoryStream buffer(*m_allocator);
            {
                BinarySerializer ar(buffer, SerializeMode::Write);
                write(ar);
            }
            const Span<const byte> now = buffer.Bytes();
            bool same = m_looked && now.Size() == m_seen.Size();
            for (usize i = 0; same && i < now.Size(); ++i)
            {
                same = static_cast<u8>(now[i]) == m_seen[i];
            }
            if (same)
            {
                return false;
            }
            m_seen.Resize(now.Size());
            if (!now.IsEmpty())
            {
                MemCopy(m_seen.Data(), now.Data(), now.Size());
            }
            m_looked = true;
            return true;
        }

    private:
        Array<u8> m_seen;
        IAllocator* m_allocator;
        bool m_looked = false;
    };
}

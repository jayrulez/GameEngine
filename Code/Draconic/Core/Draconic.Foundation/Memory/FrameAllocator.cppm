// Draconic Foundation - :frame_allocator partition
//
// FrameAllocator: double-buffered linear allocator; NextFrame() swaps halves
// so a frame's allocations stay valid through the following frame.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:frame_allocator;

import :base;
import :allocator;
import :linear_allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // FrameAllocator - double-buffered linear allocator. Splits a buffer in
    // two; allocations come from the current half. NextFrame() swaps halves and
    // resets the new current, so a frame's allocations stay valid through the
    // following frame (transient cross-frame data). Free is a no-op.
    // =======================================================================
    class FrameAllocator final : public IAllocator
    {
    public:
        FrameAllocator() noexcept = default;
        FrameAllocator(void* buffer, usize size) noexcept { Init(buffer, size); }

        void Init(void* buffer, usize size) noexcept
        {
            const usize half = size / 2;
            byte* bytes = static_cast<byte*>(buffer);
            m_buffers[0].Init(bytes, half);
            m_buffers[1].Init(bytes + half, size - half);
            m_current = 0;
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            return m_buffers[m_current].Allocate(size, alignment);
        }

        void Free(void* /*pointer*/) override {}

        // Advances to the next frame: the other half becomes current and is reset.
        void NextFrame() noexcept
        {
            m_current ^= 1u;
            m_buffers[m_current].Reset();
        }

        [[nodiscard]] usize Used() const noexcept { return m_buffers[m_current].Used(); }
        [[nodiscard]] usize Capacity() const noexcept { return m_buffers[m_current].Capacity(); }

    private:
        LinearAllocator m_buffers[2];
        u32 m_current = 0;
    };
}

// Draconic Foundation - :stack_allocator partition
//
// StackAllocator: LIFO bump allocator with markers; reclaim to a marker.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:stack_allocator;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // StackAllocator - LIFO bump allocator with markers. Free is a no-op;
    // reclaim back to a saved marker (or Reset to reclaim everything).
    // =======================================================================
    class StackAllocator final : public IAllocator
    {
    public:
        using Marker = usize;

        StackAllocator() noexcept = default;
        StackAllocator(void* buffer, usize size) noexcept { Init(buffer, size); }

        void Init(void* buffer, usize size) noexcept
        {
            m_begin = static_cast<byte*>(buffer);
            m_current = m_begin;
            m_end = m_begin + size;
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            DRACONIC_ASSERT(IsPowerOfTwo(alignment));
            const usize aligned = AlignUp(reinterpret_cast<usize>(m_current), alignment);
            byte* result = reinterpret_cast<byte*>(aligned);
            if (result + size > m_end)
            {
                return nullptr;
            }
            m_current = result + size;
            return result;
        }

        void Free(void* /*pointer*/) override {}

        [[nodiscard]] Marker GetMarker() const noexcept
        {
            return static_cast<Marker>(m_current - m_begin);
        }
        void FreeToMarker(Marker marker) noexcept { m_current = m_begin + marker; }
        void Reset() noexcept { m_current = m_begin; }

        [[nodiscard]] usize Used() const noexcept
        {
            return static_cast<usize>(m_current - m_begin);
        }
        [[nodiscard]] usize Capacity() const noexcept
        {
            return static_cast<usize>(m_end - m_begin);
        }

    private:
        byte* m_begin = nullptr;
        byte* m_current = nullptr;
        byte* m_end = nullptr;
    };
}

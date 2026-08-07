// Draconic Foundation - :linear_allocator partition
//
// LinearAllocator: bump-pointer arena over a caller buffer; bulk Reset().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:linear_allocator;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // LinearAllocator - bump-pointer arena over a caller-provided buffer.
    //   Individual frees are no-ops; reclaim all at once with Reset().
    //   Allocate returns nullptr when the arena is exhausted.
    // =======================================================================
    class LinearAllocator final : public IAllocator
    {
    public:
        LinearAllocator() noexcept = default;

        LinearAllocator(void* buffer, usize size) noexcept { Init(buffer, size); }

        void Init(void* buffer, usize size) noexcept
        {
            m_begin = static_cast<byte*>(buffer);
            m_current = m_begin;
            m_end = m_begin + size;
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            DRACONIC_ASSERT(IsPowerOfTwo(alignment));

            const usize current = reinterpret_cast<usize>(m_current);
            const usize aligned = AlignUp(current, alignment);
            byte* result = reinterpret_cast<byte*>(aligned);

            if (result + size > m_end)
            {
                return nullptr; // exhausted
            }

            m_current = result + size;
            return result;
        }

        // No-op: linear allocators reclaim in bulk via Reset().
        void Free(void* /*pointer*/) override {}

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

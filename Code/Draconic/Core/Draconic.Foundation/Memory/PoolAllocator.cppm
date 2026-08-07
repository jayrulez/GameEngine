// Draconic Foundation - :pool_allocator partition
//
// PoolAllocator: fixed-size block allocator over a caller buffer; O(1)
// allocate/free via an intrusive free list.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:pool_allocator;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // PoolAllocator - fixed-size block allocator over a caller buffer.
    //   O(1) allocate/free via an intrusive free list. Allocations must fit in
    //   the block size; returns nullptr when exhausted.
    // =======================================================================
    class PoolAllocator final : public IAllocator
    {
    public:
        PoolAllocator() noexcept = default;

        PoolAllocator(void* buffer, usize bufferSize, usize blockSize,
                      usize blockAlign = kDefaultAlignment) noexcept
        {
            Init(buffer, bufferSize, blockSize, blockAlign);
        }

        void Init(void* buffer, usize bufferSize, usize blockSize,
                  usize blockAlign = kDefaultAlignment) noexcept
        {
            DRACONIC_ASSERT(IsPowerOfTwo(blockAlign));

            // Each free block stores a next-pointer, so blocks are at least pointer-sized.
            usize actualBlock = (blockSize < sizeof(void*)) ? sizeof(void*) : blockSize;
            actualBlock = AlignUp(actualBlock, blockAlign);
            m_blockSize = actualBlock;

            const usize alignedStart = AlignUp(reinterpret_cast<usize>(buffer), blockAlign);
            byte* cursor = reinterpret_cast<byte*>(alignedStart);
            byte* end = static_cast<byte*>(buffer) + bufferSize;

            m_freeList = nullptr;
            m_blockCount = 0;
            m_freeCount = 0;
            while (cursor + actualBlock <= end)
            {
                *reinterpret_cast<void**>(cursor) = m_freeList;
                m_freeList = cursor;
                ++m_blockCount;
                ++m_freeCount;
                cursor += actualBlock;
            }
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            DRACONIC_ASSERT_MSG(size <= m_blockSize, "PoolAllocator allocation exceeds block size");
            (void)alignment;
            if (m_freeList == nullptr)
            {
                return nullptr;
            }
            void* block = m_freeList;
            m_freeList = *reinterpret_cast<void**>(m_freeList);
            --m_freeCount;
            return block;
        }

        void Free(void* pointer) override
        {
            if (pointer == nullptr)
            {
                return;
            }
            *reinterpret_cast<void**>(pointer) = m_freeList;
            m_freeList = pointer;
            ++m_freeCount;
        }

        [[nodiscard]] usize BlockSize() const noexcept { return m_blockSize; }
        [[nodiscard]] usize Capacity() const noexcept { return m_blockCount; }
        [[nodiscard]] usize FreeCount() const noexcept { return m_freeCount; }

    private:
        void* m_freeList = nullptr;
        usize m_blockSize = 0;
        usize m_blockCount = 0;
        usize m_freeCount = 0;
    };
}

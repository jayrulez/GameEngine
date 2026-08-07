// Draconic Foundation - :tracking_allocator partition
//
// TrackingAllocator: wraps an allocator, tracks live/total bytes+counts and
// peak (per-allocation header). Thread-safe (atomic counters).

module;
#include "Draconic.Foundation/Prelude.h"
#include <atomic>

export module draconic.foundation:tracking_allocator;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // TrackingAllocator - wraps another allocator and tracks live/total
    // allocations and bytes (via a per-allocation header) for leak detection
    // and budgeting. Thread-safe (atomic counters).
    // =======================================================================
    class TrackingAllocator final : public IAllocator
    {
    public:
        explicit TrackingAllocator(IAllocator& backing) noexcept : m_backing(&backing) {}

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            void* user = detail::AllocWithHeader(*m_backing, size, alignment);
            if (user != nullptr)
            {
                m_liveCount.fetch_add(1, std::memory_order_relaxed);
                m_totalAllocations.fetch_add(1, std::memory_order_relaxed);
                const u64 live = m_liveBytes.fetch_add(size, std::memory_order_relaxed) + size;
                m_totalBytes.fetch_add(size, std::memory_order_relaxed);
                UpdatePeak(live);
            }
            return user;
        }

        void Free(void* pointer) override
        {
            if (pointer != nullptr)
            {
                const usize size = detail::FreeWithHeader(*m_backing, pointer);
                m_liveCount.fetch_sub(1, std::memory_order_relaxed);
                m_totalFrees.fetch_add(1, std::memory_order_relaxed);
                m_liveBytes.fetch_sub(size, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] u64 LiveAllocations() const noexcept
        {
            return m_liveCount.load(std::memory_order_relaxed);
        }
        [[nodiscard]] u64 TotalAllocations() const noexcept
        {
            return m_totalAllocations.load(std::memory_order_relaxed);
        }
        [[nodiscard]] u64 TotalFrees() const noexcept
        {
            return m_totalFrees.load(std::memory_order_relaxed);
        }
        [[nodiscard]] u64 LiveBytes() const noexcept
        {
            return m_liveBytes.load(std::memory_order_relaxed);
        }
        [[nodiscard]] u64 TotalBytesAllocated() const noexcept
        {
            return m_totalBytes.load(std::memory_order_relaxed);
        }
        [[nodiscard]] u64 PeakBytes() const noexcept
        {
            return m_peakBytes.load(std::memory_order_relaxed);
        }
        [[nodiscard]] bool HasLeaks() const noexcept { return LiveAllocations() != 0; }

    private:
        void UpdatePeak(u64 live) noexcept
        {
            u64 peak = m_peakBytes.load(std::memory_order_relaxed);
            while (live > peak &&
                   !m_peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed))
            {
            }
        }

        IAllocator* m_backing;
        std::atomic<u64> m_liveCount{0};
        std::atomic<u64> m_totalAllocations{0};
        std::atomic<u64> m_totalFrees{0};
        std::atomic<u64> m_liveBytes{0};
        std::atomic<u64> m_totalBytes{0};
        std::atomic<u64> m_peakBytes{0};
    };
}

/// Bump-allocating staging region within a GPU-visible descriptor heap.
/// Copies bind group descriptors from CPU heap into GPU heap at bind time.
/// Ported from Sedulous.RHI.DX12/DX12DescriptorStaging.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

#include <algorithm>

export module draconic.rhi.dx12:descriptor_staging;

import draconic.foundation;
import draconic.rhi; // LogErrorf
import :gpu_descriptor_heap;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxDescriptorStaging
    {
    public:
        DxDescriptorStaging() = default;

        void init(DxGpuDescriptorHeap* cpuHeap, DxGpuDescriptorHeap* gpuHeap, ID3D12Device* device,
                  D3D12_DESCRIPTOR_HEAP_TYPE heapType, u32 initialCapacity)
        {
            m_cpuHeap = cpuHeap;
            m_gpuHeap = gpuHeap;
            m_device = device;
            m_heapType = heapType;
            m_capacity = initialCapacity;
        }

        /// Copies `count` descriptors from `srcOffset` in CPU heap into GPU staging.
        /// Returns the staging offset in the GPU heap, or -1 on failure.
        /// Identical (srcOffset, count) runs staged earlier this cycle are deduplicated:
        /// bind groups are immutable after creation, so re-binding the same group per draw
        /// reuses its first staged copy instead of burning heap space every draw.
        i32 copyFrom(u32 srcOffset, u32 count)
        {
            if (count == 0)
                return -1;

            // Dedup: already staged this cycle?
            for (const auto& e : m_staged)
            {
                if (e.srcOffset == srcOffset && e.count == count)
                    return e.stagedOffset;
            }

            // Lazy allocation.
            if (m_blockOffset < 0)
            {
                m_blockOffset = m_gpuHeap->allocate(m_capacity);
                if (m_blockOffset < 0)
                {
                    logExhausted();
                    return -1;
                }
                m_current = 0;
            }

            // Grow if needed: retire current block, allocate bigger.
            if (m_current + count > m_capacity)
            {
                u32 newCap = std::max(m_capacity * 2, m_current + count);
                i32 newBlock = m_gpuHeap->allocate(newCap);
                if (newBlock < 0)
                {
                    logExhausted();
                    return -1;
                }
                m_retiredBlocks.PushBack({m_blockOffset, m_capacity});
                m_blockOffset = newBlock;
                m_capacity = newCap;
                m_current = 0;
            }

            u32 dstOffset = static_cast<u32>(m_blockOffset) + m_current;
            m_device->CopyDescriptorsSimple(count, m_gpuHeap->getCpuHandle(dstOffset),
                                            m_cpuHeap->getCpuHandle(srcOffset), m_heapType);
            m_current += count;
            if (m_staged.Size() < kMaxDedupEntries)
                m_staged.PushBack({srcOffset, count, static_cast<i32>(dstOffset)});
            return static_cast<i32>(dstOffset);
        }

        /// Resets bump pointer. Called when command pool resets after fence wait.
        void Reset()
        {
            m_current = 0;
            m_staged.Clear(); // prior cycle's staged copies are gone once the pool recycles
            m_failLogged = false;
            for (auto& b : m_retiredBlocks)
                m_gpuHeap->free(static_cast<u32>(b.offset), b.capacity);
            m_retiredBlocks.Clear();
        }

        /// Frees all blocks.
        void Destroy()
        {
            if (m_blockOffset >= 0)
            {
                m_gpuHeap->free(static_cast<u32>(m_blockOffset), m_capacity);
                m_blockOffset = -1;
            }
            for (auto& b : m_retiredBlocks)
                m_gpuHeap->free(static_cast<u32>(b.offset), b.capacity);
            m_retiredBlocks.Clear();
        }

    private:
        struct RetiredBlock
        {
            i32 offset;
            u32 capacity;
        };

        struct StagedRun
        {
            u32 srcOffset;
            u32 count;
            i32 stagedOffset;
        };
        // Bound on the dedup cache's linear scan; runs beyond it stage without caching.
        static constexpr usize kMaxDedupEntries = 256;

        // A silent staging failure leaves the previous root descriptor table bound -
        // draws then sample stale (or later, recycled) descriptors: flickering at best,
        // a device hang at worst. Scream once per cycle so exhaustion is never silent.
        void logExhausted()
        {
            if (m_failLogged)
                return;
            m_failLogged = true;
            LogErrorf("DxDescriptorStaging: shader-visible descriptor heap exhausted "
                      "(type %d) - bind groups will go stale this frame",
                      static_cast<int>(m_heapType));
        }

        DxGpuDescriptorHeap* m_cpuHeap = nullptr;
        DxGpuDescriptorHeap* m_gpuHeap = nullptr;
        ID3D12Device* m_device = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE m_heapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        i32 m_blockOffset = -1;
        u32 m_capacity = 0;
        u32 m_current = 0;
        bool m_failLogged = false;
        Array<RetiredBlock> m_retiredBlocks;
        Array<StagedRun> m_staged; // dedup cache, cleared each Reset
    };

} // namespace draconic::rhi::dx12

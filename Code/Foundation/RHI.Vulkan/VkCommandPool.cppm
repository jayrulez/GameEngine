// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of CommandPool.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:command_pool;

import foundation.core;
import foundation.rhi;
import :adapter;
import :command_buffer;
import :query_set; // kPipelineStatisticsFlags (a bundle inherits them)

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkDeviceImpl;         // forward
    class VkCommandEncoderImpl; // forward

    class VkCommandPoolImpl : public CommandPool
    {
    public:
        Status init(VkDevice device, VkAdapterImpl* adapter, QueueType queueType,
                    IAllocator& allocator)
        {
            m_device = device;
            m_allocator = &allocator;

            i32 familyIndex = adapter->findQueueFamily(queueType);
            if (familyIndex < 0)
                return ErrorCode::Unknown;
            m_familyIndex = static_cast<u32>(familyIndex);
            m_queueType = queueType;
            // A statistics query may be active around a bundle (the profiler's, per pass); Vulkan
            // then requires the bundle to inherit the query's flags, which the device can only
            // when it enabled pipelineStatisticsQuery (it enables every feature the adapter has).
            m_inheritedStatistics =
                adapter->features10().pipelineStatisticsQuery ? kPipelineStatisticsFlags : 0;

            VkCommandPoolCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            ci.queueFamilyIndex = m_familyIndex;

            if (vkCreateCommandPool(device, &ci, nullptr, &m_pool) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        // ---- CommandPool interface ----
        Status CreateEncoder(CommandEncoder*& out) override;
        void DestroyEncoder(CommandEncoder*& encoder) override;
        void Reset() override;
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc& desc) override;

        void cleanup()
        {
            for (auto* cb : m_trackedBuffers)
                m_allocator->Delete(cb);
            m_trackedBuffers.Clear();
            m_freeHandles.Clear();
            for (auto* e : m_trackedBundleEncoders)
                m_allocator->Delete(e); // each frees its produced bundle
            m_trackedBundleEncoders.Clear();
            m_liveSecondaries.Clear();
            m_freeSecondaries.Clear();

            if (m_pool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_device, m_pool, nullptr);
                m_pool = VK_NULL_HANDLE;
            }
        }

        // Called by encoder's finish() to register the command buffer.
        void trackCommandBuffer(VkCommandBufferImpl* cb) { m_trackedBuffers.PushBack(cb); }

        // ---- render bundles (secondary command buffers) ----

        // Allocate (or recycle) a secondary command buffer for a render bundle encoder.
        [[nodiscard]] VkCommandBuffer acquireSecondary()
        {
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (!m_freeSecondaries.IsEmpty())
            {
                cb = m_freeSecondaries.Back();
                m_freeSecondaries.PopBack();
            }
            else
            {
                VkCommandBufferAllocateInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                ai.commandPool = m_pool;
                ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                ai.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(m_device, &ai, &cb) != VK_SUCCESS)
                    return VK_NULL_HANDLE;
            }
            m_liveSecondaries.PushBack(cb); // recycled on Reset
            return cb;
        }

        // Track a bundle-encoder wrapper so it (and the bundle it owns) is freed on Reset.
        void trackBundleEncoder(RenderBundleEncoder* e) { m_trackedBundleEncoders.PushBack(e); }

        [[nodiscard]] VkCommandPool handle() const { return m_pool; }
        [[nodiscard]] VkDevice vkDevice() const { return m_device; }
        [[nodiscard]] IAllocator& allocator() const { return *m_allocator; }

        // Stored so the encoder can access it.
        VkDeviceImpl* ownerDevice = nullptr;
        // The device's counts its encoders bump (Device::Counters).
        DeviceCounterBlock* counters = nullptr;

        /// The queue family this pool records for; barriers must not name stages the family
        /// cannot execute (VkBarrierHelper::maskStagesForQueue).
        [[nodiscard]] QueueType queueType() const noexcept { return m_queueType; }

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkCommandPool m_pool = VK_NULL_HANDLE;
        IAllocator* m_allocator = nullptr;
        u32 m_familyIndex = 0;
        QueueType m_queueType = QueueType::Graphics;
        VkQueryPipelineStatisticFlags m_inheritedStatistics = 0; // what a bundle inherits (init)
        Array<VkCommandBuffer> m_freeHandles;
        Array<VkCommandBufferImpl*> m_trackedBuffers;
        Array<VkCommandBuffer> m_freeSecondaries;            // recyclable secondary handles
        Array<VkCommandBuffer> m_liveSecondaries;            // handed out this cycle
        Array<RenderBundleEncoder*> m_trackedBundleEncoders; // wrappers freed on reset
    };

} // namespace foundation::rhi::vk

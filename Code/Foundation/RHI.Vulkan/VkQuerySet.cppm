// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of QuerySet.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:query_set;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{
    /// The RHI's six pipeline statistics, the flags of every statistics query pool; Vulkan writes
    /// the enabled ones in bit order, which VkDeviceImpl::DecodePipelineStatistics reads back. A
    /// render bundle inherits the same flags, since a pool's query may be active around it.
    inline constexpr VkQueryPipelineStatisticFlags kPipelineStatisticsFlags =
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;


    class VkQuerySetImpl : public QuerySet
    {
    public:
        /// The counters a statistics query counts (the RHI's six), each a u64.
        static constexpr u32 kPipelineStatisticsCounters = 6;

        Status init(VkDevice device, const QuerySetDesc& d)
        {
            type = d.type;
            count = d.count;

            VkQueryPoolCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            ci.queryCount = d.count;

            switch (d.type)
            {
            case QueryType::Timestamp:
                ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
                break;
            case QueryType::Occlusion:
                ci.queryType = VK_QUERY_TYPE_OCCLUSION;
                break;
            case QueryType::PipelineStatistics:
                ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                ci.pipelineStatistics = kPipelineStatisticsFlags;
                break;
            }

            if (vkCreateQueryPool(device, &ci, nullptr, &m_pool) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        void cleanup(VkDevice device)
        {
            if (m_pool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(device, m_pool, nullptr);
                m_pool = VK_NULL_HANDLE;
            }
        }

        [[nodiscard]] VkQueryPool handle() const { return m_pool; }

    private:
        VkQueryPool m_pool = VK_NULL_HANDLE;
    };

} // namespace foundation::rhi::vk

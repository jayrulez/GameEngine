// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:query_set - QuerySet over WGPUQuerySet.
///
/// Timestamp sets require the TimestampQuery feature (requested at device creation when
/// the adapter has it); occlusion sets are core. PipelineStatistics has no WebGPU shape
/// and fails with NotSupported.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:query_set;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuQuerySet final : public QuerySet
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device, const QuerySetDesc& setDesc)
        {
            m_api = &api;
            type = setDesc.type;
            count = setDesc.count;

            WGPUQuerySetDescriptor wgpuDesc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(setDesc.label);
            wgpuDesc.count = setDesc.count;
            switch (setDesc.type)
            {
            case QueryType::Timestamp:
                wgpuDesc.type = WGPUQueryType_Timestamp;
                break;
            case QueryType::Occlusion:
                wgpuDesc.type = WGPUQueryType_Occlusion;
                break;
            default:
                return ErrorCode::NotSupported; // pipeline statistics
            }
            m_querySet = api.wgpuDeviceCreateQuerySet(device, &wgpuDesc);
            return m_querySet != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_querySet != nullptr)
            {
                m_api->wgpuQuerySetRelease(m_querySet);
                m_querySet = nullptr;
            }
        }

        [[nodiscard]] WGPUQuerySet Handle() const { return m_querySet; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUQuerySet m_querySet = nullptr;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of QuerySet. Wraps ID3D12QueryHeap.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:query_set;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxQuerySetImpl : public QuerySet
    {
    public:
        Status init(ID3D12Device* device, const QuerySetDesc& d)
        {
            type = d.type;
            count = d.count;

            D3D12_QUERY_HEAP_DESC hd{};
            hd.Type = toQueryHeapType(d.type);
            hd.Count = d.count;
            HRESULT hr = device->CreateQueryHeap(&hd, IID_PPV_ARGS(&m_heap));
            if (FAILED(hr))
            {
                LogErrorf("DxQuerySet: CreateQueryHeap failed (0x%08X)", static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        void cleanup() { m_heap.Reset(); }

        [[nodiscard]] ID3D12QueryHeap* handle() const { return m_heap.Get(); }

        static D3D12_QUERY_HEAP_TYPE toQueryHeapType(QueryType t)
        {
            switch (t)
            {
            case QueryType::Timestamp:
                return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            case QueryType::Occlusion:
                return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
            case QueryType::PipelineStatistics:
                return D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
            }
            return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        }

        static D3D12_QUERY_TYPE toDxQueryType(QueryType t)
        {
            switch (t)
            {
            case QueryType::Timestamp:
                return D3D12_QUERY_TYPE_TIMESTAMP;
            case QueryType::Occlusion:
                return D3D12_QUERY_TYPE_OCCLUSION;
            case QueryType::PipelineStatistics:
                return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
            }
            return D3D12_QUERY_TYPE_TIMESTAMP;
        }

    private:
        ComPtr<ID3D12QueryHeap> m_heap;
    };

} // namespace foundation::rhi::dx12

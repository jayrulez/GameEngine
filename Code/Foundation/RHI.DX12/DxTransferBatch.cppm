// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of TransferBatch.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

#include <cstring>
#include <vector>

export module foundation.rhi.dx12:transfer_batch;

import foundation.core;
import foundation.rhi;
import :conversions;
import :buffer;
import :texture;
import :fence;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxQueueImpl; // forward

    class DxTransferBatchImpl : public TransferBatch
    {
    public:
        DeviceCounterBlock* counters = nullptr; // the device's counts (transfer bytes)

        Status init(ID3D12Device* device, ID3D12CommandQueue* queue, QueueType queueType)
        {
            m_device = device;
            m_queue = queue;

            HRESULT hr = device->CreateCommandAllocator(toCommandListType(queueType),
                                                        IID_PPV_ARGS(&m_allocator));
            if (FAILED(hr))
            {
                LogErrorf("DxTransferBatch: CreateCommandAllocator failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            hr = device->CreateCommandList(0, toCommandListType(queueType), m_allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&m_cmdList));
            if (FAILED(hr))
            {
                LogErrorf("DxTransferBatch: CreateCommandList failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            // The command list starts open; close it until it is needed.
            m_cmdList->Close();

            // Create the fence for synchronous submit.
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
            if (FAILED(hr))
            {
                LogErrorf("DxTransferBatch: CreateFence failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            m_fenceValue = 0;

            return ErrorCode::Ok;
        }

        // ---- TransferBatch interface ----

        void WriteBuffer(Buffer* dst, u64 dstOffset, Span<const u8> data) override
        {
            auto* dxDst = static_cast<DxBufferImpl*>(dst);
            if (!dxDst || data.Size() == 0)
                return;
            if (counters)
                counters->Transfer(data.Size());

            ensureRecording();

            // Create upload-heap staging buffer.
            u64 stagingSize = static_cast<u64>(data.Size());
            ComPtr<ID3D12Resource> staging;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = stagingSize;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc = {1, 0};
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr, IID_PPV_ARGS(&staging));
            if (FAILED(hr))
                return;

            // Map and copy data.
            void* mapped = nullptr;
            staging->Map(0, nullptr, &mapped);
            std::memcpy(mapped, data.Data(), data.Size());
            staging->Unmap(0, nullptr);

            m_stagingBuffers.push_back(std::move(staging));

            // Record copy command.
            m_cmdList->CopyBufferRegion(dxDst->handle(), dstOffset, m_stagingBuffers.back().Get(),
                                        0, stagingSize);
        }

        void WriteTexture(Texture* dst, Span<const u8> data, const TextureDataLayout& layout,
                          Extent3D extent, u32 mipLevel, u32 arrayLayer) override
        {
            auto* dxTex = static_cast<DxTextureImpl*>(dst);
            if (!dxTex || data.Size() == 0)
                return;
            if (counters)
                counters->Transfer(data.Size());

            ensureRecording();

            // Calculate aligned row pitch (D3D12 requires 256-byte row alignment).
            u32 alignedRowPitch = (layout.bytesPerRow + 255) & ~u32(255);
            u32 rowsPerImage = (layout.rowsPerImage > 0) ? layout.rowsPerImage : extent.height;
            u64 stagingSize = static_cast<u64>(alignedRowPitch) * rowsPerImage * extent.depth;

            ComPtr<ID3D12Resource> staging;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = stagingSize;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc = {1, 0};
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr, IID_PPV_ARGS(&staging));
            if (FAILED(hr))
                return;

            // Map and copy data row by row (handles pitch alignment).
            void* mapped = nullptr;
            staging->Map(0, nullptr, &mapped);

            const u8* srcPtr = data.Data() + layout.offset;
            u8* dstPtr = static_cast<u8*>(mapped);
            for (u32 z = 0; z < extent.depth; ++z)
            {
                for (u32 row = 0; row < rowsPerImage; ++row)
                {
                    std::memcpy(dstPtr + (z * rowsPerImage + row) * alignedRowPitch,
                                srcPtr + (z * rowsPerImage + row) * layout.bytesPerRow,
                                layout.bytesPerRow);
                }
            }

            staging->Unmap(0, nullptr);
            m_stagingBuffers.push_back(std::move(staging));

            // Transition the texture to copy dest through the shared helper rather than an
            // ALL_SUBRESOURCES barrier built from currentState(): that is only valid while the
            // texture is uniform, and re-uploading a mip level of a texture that has been through
            // mip generation (which leaves per-subresource tracking behind) would otherwise carry
            // a stale before-state and be rejected.
            TransitionWholeTexture(m_cmdList.Get(), dxTex, D3D12_RESOURCE_STATE_COPY_DEST);

            u32 subresource = mipLevel + arrayLayer * dxTex->desc.mipLevelCount;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = m_stagingBuffers.back().Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Offset = 0;
            srcLoc.PlacedFootprint.Footprint.Format = toDxgiFormat(dxTex->desc.format);
            srcLoc.PlacedFootprint.Footprint.Width = extent.width;
            srcLoc.PlacedFootprint.Footprint.Height = extent.height;
            srcLoc.PlacedFootprint.Footprint.Depth = extent.depth;
            srcLoc.PlacedFootprint.Footprint.RowPitch = alignedRowPitch;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dxTex->handle();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = subresource;

            m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

            // Transition back to common (the resting state the rest of the backend assumes).
            TransitionWholeTexture(m_cmdList.Get(), dxTex, D3D12_RESOURCE_STATE_COMMON);
        }

        Status Submit() override
        {
            if (!m_isRecording)
                return ErrorCode::Ok;

            m_cmdList->Close();
            m_isRecording = false;

            ID3D12CommandList* lists[] = {m_cmdList.Get()};
            m_queue->ExecuteCommandLists(1, lists);

            // Wait for completion.
            ++m_fenceValue;
            m_queue->Signal(m_fence.Get(), m_fenceValue);
            if (m_fence->GetCompletedValue() < m_fenceValue)
            {
                m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }

            releaseStagingBuffers();
            return ErrorCode::Ok;
        }

        Status SubmitAsync(Fence* fence, u64 signalValue) override
        {
            if (!m_isRecording)
                return ErrorCode::Ok;

            m_cmdList->Close();
            m_isRecording = false;

            ID3D12CommandList* lists[] = {m_cmdList.Get()};
            m_queue->ExecuteCommandLists(1, lists);

            if (auto* dxFence = static_cast<DxFenceImpl*>(fence))
                m_queue->Signal(dxFence->handle(), signalValue);

            // Staging buffers cannot be released until the GPU is done.
            // The caller must wait on the fence before calling Reset().
            return ErrorCode::Ok;
        }

        void Reset() override { releaseStagingBuffers(); }

        void Destroy() override
        {
            releaseStagingBuffers();

            if (m_fenceEvent)
            {
                CloseHandle(m_fenceEvent);
                m_fenceEvent = nullptr;
            }
            m_fence.Reset();
            m_cmdList.Reset();
            m_allocator.Reset();
        }

    private:
        void ensureRecording()
        {
            if (!m_isRecording)
            {
                m_allocator->Reset();
                m_cmdList->Reset(m_allocator.Get(), nullptr);
                m_isRecording = true;
            }
        }

        void releaseStagingBuffers() { m_stagingBuffers.clear(); }

        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_queue = nullptr;
        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList> m_cmdList;
        bool m_isRecording = false;

        // ComPtr's operator overloads are incompatible with Array's placement-new.
        std::vector<ComPtr<ID3D12Resource>> m_stagingBuffers;

        ComPtr<ID3D12Fence> m_fence;
        u64 m_fenceValue = 0;
        HANDLE m_fenceEvent = nullptr;
    };

} // namespace foundation::rhi::dx12

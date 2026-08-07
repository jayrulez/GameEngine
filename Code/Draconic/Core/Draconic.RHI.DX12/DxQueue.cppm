/// DX12 implementation of Queue.
/// Ported from Sedulous.RHI.DX12/DX12Queue.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:queue;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :command_buffer;
import :fence;
import :transfer_batch;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxDeviceImpl; // forward

    class DxQueueImpl : public Queue
    {
    public:
        explicit DxQueueImpl(IAllocator& allocator) noexcept : m_allocator(allocator) {}

        Status init(ID3D12Device* device, QueueType type, DxDeviceImpl* owner)
        {
            queueType = type;
            m_device = owner;
            m_d3dDevice = device;

            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = toCommandListType(type);
            HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue));
            if (FAILED(hr))
                return ErrorCode::Unknown;

            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_internalFence));
            if (FAILED(hr))
                return ErrorCode::Unknown;
            m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            // Query timestamp frequency.
            UINT64 freq = 0;
            m_queue->GetTimestampFrequency(&freq);
            m_tsPeriod = (freq > 0) ? (1e9f / static_cast<f32>(freq)) : 1.0f;

            return ErrorCode::Ok;
        }

        // ---- Queue interface ----

        void Submit(Span<CommandBuffer* const> cmdBufs) override
        {
            if (cmdBufs.Size() == 0)
                return;
            Array<ID3D12CommandList*> lists(cmdBufs.Size());
            for (usize i = 0; i < cmdBufs.Size(); ++i)
            {
                if (auto* dxCb = static_cast<DxCommandBufferImpl*>(cmdBufs[i]))
                    lists[i] = dxCb->handle();
            }
            m_queue->ExecuteCommandLists(static_cast<UINT>(lists.Size()), lists.Data());
        }

        void Submit(Span<CommandBuffer* const> cmdBufs, Fence* signalFence,
                    u64 signalValue) override
        {
            Submit(cmdBufs);
            if (auto* f = static_cast<DxFenceImpl*>(signalFence))
                m_queue->Signal(f->handle(), signalValue);
        }

        void Submit(Span<CommandBuffer* const> cmdBufs, Span<Fence* const> waitFences,
                    Span<const u64> waitValues, Fence* signalFence, u64 signalValue) override
        {
            for (usize i = 0; i < waitFences.Size(); ++i)
                if (auto* f = static_cast<DxFenceImpl*>(waitFences[i]))
                    m_queue->Wait(f->handle(), waitValues[i]);
            Submit(cmdBufs);
            if (auto* f = static_cast<DxFenceImpl*>(signalFence))
                m_queue->Signal(f->handle(), signalValue);
        }

        void WaitIdle() override
        {
            ++m_fenceValue;
            m_queue->Signal(m_internalFence.Get(), m_fenceValue);
            if (m_internalFence->GetCompletedValue() < m_fenceValue)
            {
                m_internalFence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }
        }

        Status CreateTransferBatch(TransferBatch*& out) override
        {
            auto* batch = m_allocator.New<DxTransferBatchImpl>();
            if (batch->init(m_d3dDevice, m_queue.Get(), queueType) != ErrorCode::Ok)
            {
                m_allocator.Delete(batch);
                return ErrorCode::Unknown;
            }
            out = batch;
            return ErrorCode::Ok;
        }

        void DestroyTransferBatch(TransferBatch*& batch) override
        {
            if (auto* dx = static_cast<DxTransferBatchImpl*>(batch))
            {
                dx->Destroy();
                m_allocator.Delete(dx);
            }
            batch = nullptr;
        }

        f32 TimestampPeriod() const override { return m_tsPeriod; }

        void cleanup()
        {
            if (m_fenceEvent)
            {
                CloseHandle(m_fenceEvent);
                m_fenceEvent = nullptr;
            }
            m_internalFence.Reset();
            m_queue.Reset();
        }

        // ---- Internal ----
        [[nodiscard]] ID3D12CommandQueue* handle() const { return m_queue.Get(); }
        [[nodiscard]] DxDeviceImpl* owner() const { return m_device; }

    private:
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12Fence> m_internalFence;
        HANDLE m_fenceEvent = nullptr;
        u64 m_fenceValue = 0;
        f32 m_tsPeriod = 1.0f;
        DxDeviceImpl* m_device = nullptr;
        ID3D12Device* m_d3dDevice = nullptr;
        IAllocator& m_allocator;
    };

} // namespace draconic::rhi::dx12

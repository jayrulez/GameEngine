/// DX12 implementation of Fence using ID3D12Fence.
/// Ported from Sedulous.RHI.DX12/DX12Fence.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:fence;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxFenceImpl : public Fence
    {
    public:
        Status init(ID3D12Device* device, u64 initialValue)
        {
            HRESULT hr =
                device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
            if (FAILED(hr))
            {
                LogErrorf("DxFence: CreateFence failed (0x%08X)", static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return ErrorCode::Ok;
        }

        u64 CompletedValue() override { return m_fence->GetCompletedValue(); }

        bool Wait(u64 value, u64 timeoutNs) override
        {
            if (m_fence->GetCompletedValue() >= value)
                return true;
            m_fence->SetEventOnCompletion(value, m_event);
            DWORD timeoutMs =
                (timeoutNs == ~0ull) ? INFINITE : static_cast<DWORD>(timeoutNs / 1000000);
            return WaitForSingleObject(m_event, timeoutMs) == WAIT_OBJECT_0;
        }

        void cleanup()
        {
            if (m_event)
            {
                CloseHandle(m_event);
                m_event = nullptr;
            }
            m_fence.Reset();
        }

        // ---- Internal ----
        [[nodiscard]] ID3D12Fence* handle() const { return m_fence.Get(); }

        void signal(ID3D12CommandQueue* queue, u64 value) { queue->Signal(m_fence.Get(), value); }

    private:
        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_event = nullptr;
    };

} // namespace draconic::rhi::dx12

/// DX12 implementation of CommandPool.
/// Wraps an ID3D12CommandAllocator.
/// Ported from Sedulous.RHI.DX12/DX12CommandPool.bf.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:command_pool;

import draconic.core;
import draconic.rhi;
import :conversions;
import :command_buffer;
import :descriptor_staging;

using namespace draconic::core;

export namespace draconic::rhi::dx12
{

    class DxDeviceImpl;         // forward
    class DxCommandEncoderImpl; // forward

    class DxCommandPoolImpl : public CommandPool
    {
    public:
        Status init(DxDeviceImpl* device, ID3D12Device* d3dDevice, QueueType queueType,
                    DxGpuDescriptorHeap* cpuSrvHeap, DxGpuDescriptorHeap* gpuSrvHeap,
                    DxGpuDescriptorHeap* cpuSamplerHeap, DxGpuDescriptorHeap* gpuSamplerHeap,
                    IAllocator& allocator)
        {
            m_device = device;
            m_d3dDevice = d3dDevice;
            m_allocPtr = &allocator;
            m_type = toCommandListType(queueType);

            HRESULT hr = d3dDevice->CreateCommandAllocator(m_type, IID_PPV_ARGS(&m_allocator));
            if (FAILED(hr))
                return ErrorCode::Unknown;

            // Create a reusable command list. DX12's CreateCommandList starts it in
            // recording state; close it immediately so the lifecycle is always:
            //   Reset (pool) → cmdList->Reset (open) → record → Close → submit.
            // This avoids the Vulkan/DX12 mismatch where Vulkan's vkResetCommandPool
            // implicitly handles open command buffers but DX12's allocator Reset does not.
            {
                ID3D12GraphicsCommandList* rawList = nullptr;
                hr = d3dDevice->CreateCommandList(0, m_type, m_allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&rawList));
                if (FAILED(hr) || !rawList)
                    return ErrorCode::Unknown;
                rawList->Close();
                m_cmdList.Attach(rawList); // take ownership without AddRef
            }

            // Create descriptor staging (shared by all encoders from this pool).
            m_srvStaging.init(cpuSrvHeap, gpuSrvHeap, d3dDevice,
                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024);
            m_samplerStaging.init(cpuSamplerHeap, gpuSamplerHeap, d3dDevice,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64);

            return ErrorCode::Ok;
        }

        // ---- CommandPool interface ----
        Status CreateEncoder(CommandEncoder*& out) override;
        void DestroyEncoder(CommandEncoder*& encoder) override;

        void Reset() override;

        void cleanup()
        {
            releaseCommandBuffers();
            if (m_cmdListOpen)
            {
                m_cmdList->Close();
                m_cmdListOpen = false;
            }
            m_cmdList.Reset();
            m_srvStaging.Destroy();
            m_samplerStaging.Destroy();
            m_allocator.Reset();
        }

        // ---- Internal ----
        [[nodiscard]] ID3D12CommandAllocator* handle() const { return m_allocator.Get(); }
        [[nodiscard]] DxDeviceImpl* ownerDevice() const { return m_device; }
        [[nodiscard]] DxDescriptorStaging* srvStaging() { return &m_srvStaging; }
        [[nodiscard]] DxDescriptorStaging* samplerStaging() { return &m_samplerStaging; }
        [[nodiscard]] IAllocator& allocator() const { return *m_allocPtr; }

        /// Called by DxCommandEncoderImpl::finish() to register a command buffer with this pool.
        void trackCommandBuffer(DxCommandBufferImpl* cb) { m_trackedBuffers.PushBack(cb); }

        /// Called by DxCommandEncoderImpl::Finish() after Close().
        void markCmdListClosed() { m_cmdListOpen = false; }

        /// Track the live encoder so Reset can release its bundles.
        void trackEncoder(DxCommandEncoderImpl* enc) { m_liveEncoder = enc; }

    private:
        void releaseCommandBuffers()
        {
            for (auto* cb : m_trackedBuffers)
            {
                cb->release();
                m_allocPtr->Delete(cb);
            }
            m_trackedBuffers.Clear();
        }

        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList> m_cmdList; // created once in init, reused across frames
        DxCommandEncoderImpl* m_liveEncoder = nullptr; // persistent encoder (if any)
        bool m_cmdListOpen = false; // true while the command list is in recording state
        ID3D12Device* m_d3dDevice = nullptr;
        DxDeviceImpl* m_device = nullptr;
        IAllocator* m_allocPtr = nullptr;
        D3D12_COMMAND_LIST_TYPE m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Array<DxCommandBufferImpl*> m_trackedBuffers;
        DxDescriptorStaging m_srvStaging;
        DxDescriptorStaging m_samplerStaging;
    };

} // namespace draconic::rhi::dx12

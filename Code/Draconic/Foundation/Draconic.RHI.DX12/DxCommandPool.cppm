/// DX12 implementation of CommandPool.
/// Wraps an ID3D12CommandAllocator.
/// Ported from Sedulous.RHI.DX12/DX12CommandPool.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:command_pool;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :command_buffer;
import :descriptor_staging;

using namespace draconic::foundation;

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

            // Create SRV descriptor staging (shared by all encoders from this pool).
            // Samplers are NOT staged: the shader-visible sampler heap is hard-capped at 2048
            // by D3D12, which cannot fit per-pool staging blocks - sampler tables are baked
            // into that heap once at bind-group creation instead (see DxBindGroup).
            (void)cpuSamplerHeap;
            (void)gpuSamplerHeap;
            m_srvStaging.init(cpuSrvHeap, gpuSrvHeap, d3dDevice,
                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024);

            return ErrorCode::Ok;
        }

        // ---- CommandPool interface ----
        Status CreateEncoder(CommandEncoder*& out) override;
        void DestroyEncoder(CommandEncoder*& encoder) override;
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc& desc) override;

        void Reset() override
        {
            // Contract: every encoder has called Finish (closing its list) before Reset.
            // DX12 cannot reset an allocator while one of its lists is recording -
            // surface the violation here instead of via a cryptic driver error.
            if (m_unfinishedEncoders != 0)
                LogErrorf("DxCommandPool::Reset: %d encoder(s) still recording (Finish not "
                          "called); ID3D12CommandAllocator::Reset will fail",
                          static_cast<int>(m_unfinishedEncoders));
            releaseCommandBuffers();
            // Render bundles minted this cycle die at the frame boundary (fence-guarded).
            releaseBundleEncoders();
            // Reset descriptor staging -- GPU is done (fence waited), so staging
            // bump pointers can safely return to start.
            m_srvStaging.Reset();
            m_allocator->Reset();
        }

        void cleanup()
        {
            releaseCommandBuffers();
            releaseBundleEncoders();
            m_srvStaging.Destroy();
            m_allocator.Reset();
        }

        // ---- Internal ----
        [[nodiscard]] ID3D12CommandAllocator* handle() const { return m_allocator.Get(); }
        [[nodiscard]] DxDeviceImpl* ownerDevice() const { return m_device; }
        [[nodiscard]] DxDescriptorStaging* srvStaging() { return &m_srvStaging; }
        [[nodiscard]] IAllocator& allocator() const { return *m_allocPtr; }

        /// Called by DxCommandEncoderImpl::finish() to register a command buffer with this pool.
        void trackCommandBuffer(DxCommandBufferImpl* cb) { m_trackedBuffers.PushBack(cb); }

        /// Called by DxCommandEncoderImpl::Finish() -- its command list is now closed.
        void markEncoderFinished()
        {
            if (m_unfinishedEncoders > 0)
                --m_unfinishedEncoders;
        }

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

        // Defined out-of-line (needs DxRenderBundleEncoderImpl's complete type).
        void releaseBundleEncoders();

        ComPtr<ID3D12CommandAllocator> m_allocator;
        ID3D12Device* m_d3dDevice = nullptr;
        DxDeviceImpl* m_device = nullptr;
        IAllocator* m_allocPtr = nullptr;
        D3D12_COMMAND_LIST_TYPE m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Array<DxCommandBufferImpl*> m_trackedBuffers;
        Array<RenderBundleEncoder*> m_trackedBundleEncoders; // bundle wrappers, freed on Reset
        i32 m_unfinishedEncoders = 0; // encoders created whose Finish has not run yet
        DxDescriptorStaging m_srvStaging;
    };

} // namespace draconic::rhi::dx12

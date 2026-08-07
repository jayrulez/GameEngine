/// DX12 implementation of ComputePipeline.
/// Ported from Sedulous.RHI.DX12/DX12ComputePipeline.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:compute_pipeline;

import draconic.foundation;
import draconic.rhi;
import :pipeline_layout;
import :shader_module;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxComputePipelineImpl : public ComputePipeline
    {
    public:
        Status init(ID3D12Device* device, const ComputePipelineDesc& d)
        {
            m_layout = static_cast<DxPipelineLayoutImpl*>(d.layout);
            if (!m_layout)
                return ErrorCode::Unknown;
            auto* csMod = static_cast<DxShaderModuleImpl*>(d.compute.module);
            if (!csMod)
                return ErrorCode::Unknown;

            auto cs = csMod->bytecode();
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = m_layout->handle();
            pso.CS = {cs.Data(), cs.Size()};

            HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pipelineState));
            if (FAILED(hr))
            {
                LogErrorf("DxComputePipeline: CreateComputePipelineState failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        void cleanup() { m_pipelineState.Reset(); }

        [[nodiscard]] ID3D12PipelineState* handle() const { return m_pipelineState.Get(); }
        [[nodiscard]] DxPipelineLayoutImpl* pipelineLayout() const { return m_layout; }

    private:
        ComPtr<ID3D12PipelineState> m_pipelineState;
        DxPipelineLayoutImpl* m_layout = nullptr;
    };

} // namespace draconic::rhi::dx12

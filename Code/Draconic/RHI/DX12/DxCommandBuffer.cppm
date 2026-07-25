/// DX12 implementation of CommandBuffer.
/// Simple wrapper for a closed ID3D12GraphicsCommandList.
/// Ported from Sedulous.RHI.DX12/DX12CommandBuffer.bf.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:command_buffer;

import draconic.core;
import draconic.rhi;

using namespace draconic::core;

export namespace draconic::rhi::dx12
{

    class DxCommandBufferImpl : public CommandBuffer
    {
    public:
        explicit DxCommandBufferImpl(ID3D12GraphicsCommandList* cmdList) : m_cmdList(cmdList) {}

        [[nodiscard]] ID3D12GraphicsCommandList* handle() const { return m_cmdList; }

        void release()
        {
            // The command list is now owned by the pool's ComPtr (m_cachedCmdList)
            // and reused across frames. Just null out our reference.
            m_cmdList = nullptr;
        }

    private:
        ID3D12GraphicsCommandList* m_cmdList = nullptr; // raw, released via release()
    };

} // namespace draconic::rhi::dx12

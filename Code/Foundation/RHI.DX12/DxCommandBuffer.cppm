// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of CommandBuffer.
/// Simple wrapper for a closed ID3D12GraphicsCommandList.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:command_buffer;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxCommandBufferImpl : public CommandBuffer
    {
    public:
        explicit DxCommandBufferImpl(ID3D12GraphicsCommandList* cmdList) : m_cmdList(cmdList) {}

        [[nodiscard]] ID3D12GraphicsCommandList* handle() const { return m_cmdList; }

        void release()
        {
            if (m_cmdList)
            {
                m_cmdList->Release();
                m_cmdList = nullptr;
            }
        }

    private:
        ID3D12GraphicsCommandList* m_cmdList = nullptr; // raw, released via release()
    };

} // namespace foundation::rhi::dx12

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of ShaderModule. Stores DXIL bytecode.

module;
#include "Core/Prelude.h"

#include <cstring>

export module foundation.rhi.dx12:shader_module;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxShaderModuleImpl : public ShaderModule
    {
    public:
        Status init(const ShaderModuleDesc& d)
        {
            m_bytecode.Resize(d.code.Size());
            std::memcpy(m_bytecode.Data(), d.code.Data(), d.code.Size());
            return ErrorCode::Ok;
        }

        void cleanup() { m_bytecode.Clear(); }

        [[nodiscard]] Span<const u8> bytecode() const
        {
            return {m_bytecode.Data(), m_bytecode.Size()};
        }

    private:
        Array<u8> m_bytecode;
    };

} // namespace foundation::rhi::dx12

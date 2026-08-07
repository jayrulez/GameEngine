/// DX12 implementation of ShaderModule. Stores DXIL bytecode.
/// Ported from Sedulous.RHI.DX12/DX12ShaderModule.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include <cstring>

export module draconic.rhi.dx12:shader_module;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
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

} // namespace draconic::rhi::dx12

/// DX12 implementation of Surface. Simply stores the HWND.
/// Ported from Sedulous.RHI.DX12/DX12Surface.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:surface;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxSurfaceImpl : public Surface
    {
    public:
        explicit DxSurfaceImpl(HWND hwnd) : m_hwnd(hwnd) {}

        [[nodiscard]] HWND handle() const { return m_hwnd; }

    private:
        HWND m_hwnd = nullptr;
    };

} // namespace draconic::rhi::dx12

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of Surface. Simply stores the HWND.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:surface;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxSurfaceImpl : public Surface
    {
    public:
        explicit DxSurfaceImpl(HWND hwnd) : m_hwnd(hwnd) {}

        [[nodiscard]] HWND handle() const { return m_hwnd; }

    private:
        HWND m_hwnd = nullptr;
    };

} // namespace foundation::rhi::dx12

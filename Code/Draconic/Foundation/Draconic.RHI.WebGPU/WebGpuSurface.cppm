/// draconic.rhi.webgpu:surface - Surface over WGPUSurface.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:surface;

import draconic.foundation;
import draconic.rhi;
import :api;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuSurface final : public Surface
    {
    public:
        void Adopt(const WebGpuApi& api, WGPUSurface surface)
        {
            m_api = &api;
            m_surface = surface;
        }

        void Release()
        {
            if (m_surface != nullptr)
            {
                m_api->wgpuSurfaceRelease(m_surface);
                m_surface = nullptr;
            }
        }

        [[nodiscard]] WGPUSurface Handle() const { return m_surface; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUSurface m_surface = nullptr;
    };
}

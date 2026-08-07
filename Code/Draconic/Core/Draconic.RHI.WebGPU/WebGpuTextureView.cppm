/// draconic.rhi.webgpu:texture_view - TextureView over WGPUTextureView.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:texture_view;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;
import :texture;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuTextureView final : public TextureView
    {
    public:
        Status Initialize(const WebGpuApi& api, Texture* owner, const TextureViewDesc& viewDesc)
        {
            m_api = &api;
            desc = viewDesc;
            texture = owner;

            const WGPUTextureViewDimension dimension =
                ToWgpuTextureViewDimension(viewDesc.dimension);
            if (dimension == WGPUTextureViewDimension_Undefined)
            {
                return ErrorCode::NotSupported; // 1D arrays do not exist in WebGPU
            }

            WGPUTextureViewDescriptor wgpuDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(viewDesc.label);
            wgpuDesc.format = ToWgpuTextureFormat(viewDesc.format);
            wgpuDesc.dimension = dimension;
            wgpuDesc.baseMipLevel = viewDesc.baseMipLevel;
            wgpuDesc.mipLevelCount = viewDesc.mipLevelCount;
            wgpuDesc.baseArrayLayer = viewDesc.baseArrayLayer;
            wgpuDesc.arrayLayerCount = viewDesc.arrayLayerCount;
            wgpuDesc.aspect = ToWgpuTextureAspect(viewDesc.aspect);

            m_view = api.wgpuTextureCreateView(
                static_cast<WebGpuTexture*>(owner)->Handle(), &wgpuDesc);
            return m_view != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_view != nullptr)
            {
                m_api->wgpuTextureViewRelease(m_view);
                m_view = nullptr;
            }
        }

        [[nodiscard]] WGPUTextureView Handle() const { return m_view; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUTextureView m_view = nullptr;
    };
}

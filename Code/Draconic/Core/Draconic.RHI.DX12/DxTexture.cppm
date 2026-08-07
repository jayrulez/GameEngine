/// DX12 implementation of Texture.
/// Ported from Sedulous.RHI.DX12/DX12Texture.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

#include <algorithm>
#include <cstring>

export module draconic.rhi.dx12:texture;

import draconic.foundation;
import draconic.rhi;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxTextureImpl : public Texture
    {
    public:
        Status init(ID3D12Device* device, const TextureDesc& d)
        {
            desc = d;

            DXGI_FORMAT format =
                IsDepthFormat(d.format) ? toTypelessDepthFormat(d.format) : toDxgiFormat(d.format);

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = toResourceDimension(d.dimension);
            rd.Width = static_cast<UINT64>(d.width);
            rd.Height = d.height;
            rd.DepthOrArraySize = static_cast<UINT16>(
                (d.dimension == TextureDimension::Texture3D) ? d.depth : d.arrayLayerCount);
            rd.MipLevels = static_cast<UINT16>(d.mipLevelCount);
            rd.Format = format;
            rd.SampleDesc = {d.sampleCount, 0};
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = toTextureResourceFlags(d.usage);

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            m_state = D3D12_RESOURCE_STATE_COMMON;

            D3D12_CLEAR_VALUE clearVal{};
            D3D12_CLEAR_VALUE* pClearVal = nullptr;

            if (static_cast<u32>(d.usage & TextureUsage::DepthStencil))
            {
                clearVal.Format = toDxgiFormat(d.format);
                clearVal.DepthStencil = {1.0f, 0};
                pClearVal = &clearVal;
                m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            else if (static_cast<u32>(d.usage & TextureUsage::RenderTarget))
            {
                clearVal.Format = format;
                clearVal.Color[0] = 0;
                clearVal.Color[1] = 0;
                clearVal.Color[2] = 0;
                clearVal.Color[3] = 1;
                pClearVal = &clearVal;
                m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            HRESULT hr =
                device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &rd, m_state,
                                                pClearVal, IID_PPV_ARGS(&m_resource));
            if (FAILED(hr))
            {
                LogErrorf("DxTexture: CreateCommittedResource failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }
            m_ownsResource = true;
            return ErrorCode::Ok;
        }

        /// Initialize from an existing resource (e.g. swap chain buffer). Does not own.
        void initFromExisting(ID3D12Resource* resource, const TextureDesc& d)
        {
            m_resource.Attach(resource);
            m_resource->AddRef(); // ComPtr will Release - balance it
            desc = d;
            m_ownsResource = false;
            m_state = D3D12_RESOURCE_STATE_PRESENT;
        }

        void cleanup()
        {
            m_subresourceStates.Clear();
            m_resource.Reset();
        }

        // ---- Internal ----
        [[nodiscard]] ID3D12Resource* handle() const { return m_resource.Get(); }

        [[nodiscard]] D3D12_RESOURCE_STATES currentState() const { return m_state; }
        void setState(D3D12_RESOURCE_STATES s)
        {
            m_state = s;
            m_subresourceStates.Clear();
        }

        [[nodiscard]] D3D12_RESOURCE_STATES getSubresourceState(u32 mip, u32 layer) const
        {
            if (m_subresourceStates.IsEmpty())
                return m_state;
            u32 idx = mip + layer * desc.mipLevelCount;
            return (idx < m_subresourceStates.Size()) ? m_subresourceStates[idx] : m_state;
        }

        void setSubresourceState(u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount,
                                 D3D12_RESOURCE_STATES s)
        {
            u32 totalMips = desc.mipLevelCount;
            u32 totalLayers = std::max(
                (desc.dimension == TextureDimension::Texture3D) ? desc.depth : desc.arrayLayerCount,
                1u);
            u32 mipEnd = (mipCount == ~0u) ? totalMips : std::min(baseMip + mipCount, totalMips);
            u32 layerEnd =
                (layerCount == ~0u) ? totalLayers : std::min(baseLayer + layerCount, totalLayers);

            // All subresources? Collapse to uniform.
            if (baseMip == 0 && mipEnd >= totalMips && baseLayer == 0 && layerEnd >= totalLayers)
            {
                m_state = s;
                m_subresourceStates.Clear();
                return;
            }
            // Promote to per-subresource.
            if (m_subresourceStates.IsEmpty())
            {
                if (s == m_state)
                    return;
                m_subresourceStates.Resize(totalMips * totalLayers, m_state);
            }
            for (u32 l = baseLayer; l < layerEnd; ++l)
                for (u32 m = baseMip; m < mipEnd; ++m)
                    m_subresourceStates[m + l * totalMips] = s;

            // Try to collapse.
            auto first = m_subresourceStates[0];
            for (usize i = 1; i < m_subresourceStates.Size(); ++i)
                if (m_subresourceStates[i] != first)
                    return;
            m_state = first;
            m_subresourceStates.Clear();
        }

    private:
        ComPtr<ID3D12Resource> m_resource;
        D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;
        Array<D3D12_RESOURCE_STATES> m_subresourceStates;
        bool m_ownsResource = true;
    };

} // namespace draconic::rhi::dx12

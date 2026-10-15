// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of Texture.
/// Ported from Sedulous.RHI.DX12/DX12Texture.bf.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

#include <algorithm>
#include <cstring>

export module foundation.rhi.dx12:texture;

import foundation.core;
import foundation.rhi;
import :conversions;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    class DxTextureImpl : public Texture
    {
    public:
        Status init(ID3D12Device* device, const TextureDesc& d)
        {
            desc = d;

            // A texture whose views may take its sRGB or UNORM twin is created typeless; its views
            // (and its clear value) name the typed format.
            DXGI_FORMAT format = IsDepthFormat(d.format)                            ? toTypelessDepthFormat(d.format)
                                 : d.alternateViewFormat != TextureFormat::Undefined ? toTypelessColorFormat(d.format)
                                                                                     : toDxgiFormat(d.format);

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
                clearVal.DepthStencil = {depth::ClearValue(), 0}; // the far plane (reverse-Z: 0)
                pClearVal = &clearVal;
                m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            else if (static_cast<u32>(d.usage & TextureUsage::RenderTarget))
            {
                clearVal.Format = toDxgiFormat(d.format); // typed: a typeless resource clears as its format
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

        /// Whether every subresource shares one state.
        ///
        /// READ THIS BEFORE currentState(). In per-subresource mode m_state is a leftover, NOT
        /// the resource's state, so currentState() lies and setState() would additionally erase
        /// the per-subresource truth. A caller that wants to move the WHOLE resource must either
        /// check this first or go through TransitionWholeTexture below, which handles both modes.
        [[nodiscard]] bool hasUniformState() const noexcept { return m_subresourceStates.IsEmpty(); }

        /// Subresource grid, matching the indexing setSubresourceState/getSubresourceState use.
        [[nodiscard]] u32 stateLayerCount() const noexcept
        {
            return std::max(
                (desc.dimension == TextureDimension::Texture3D) ? desc.depth : desc.arrayLayerCount,
                1u);
        }

        /// Only meaningful when hasUniformState().
        [[nodiscard]] D3D12_RESOURCE_STATES currentState() const { return m_state; }
        /// Declares the WHOLE resource to be in `s`; drops any per-subresource tracking, so only
        /// call it when every subresource really was transitioned.
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

    /// Move an ENTIRE texture to `after`, from whatever its subresources are actually in, and
    /// leave the tracker uniform.
    ///
    /// The obvious spelling - one ALL_SUBRESOURCES barrier from currentState() - is only correct
    /// while the texture is uniform. In per-subresource mode it reads a stale m_state and the
    /// debug layer rejects the barrier ("Before state ... does not match with the state ...
    /// specified in the previous call to ResourceBarrier"). Every caller that wants whole-resource
    /// movement should come through here rather than re-deriving it.
    inline void TransitionWholeTexture(ID3D12GraphicsCommandList* cmdList, DxTextureImpl* tex,
                                       D3D12_RESOURCE_STATES after)
    {
        if (cmdList == nullptr || tex == nullptr)
            return;

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->handle();
        b.Transition.StateAfter = after;

        if (tex->hasUniformState())
        {
            const D3D12_RESOURCE_STATES before = tex->currentState();
            if (before == after)
                return;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            cmdList->ResourceBarrier(1, &b);
            tex->setState(after);
            return;
        }

        // Mixed: one barrier per subresource that is not already there, then collapse.
        const u32 mips = tex->desc.mipLevelCount;
        const u32 layers = tex->stateLayerCount();
        for (u32 layer = 0; layer < layers; ++layer)
        {
            for (u32 mip = 0; mip < mips; ++mip)
            {
                const D3D12_RESOURCE_STATES before = tex->getSubresourceState(mip, layer);
                if (before == after)
                    continue;
                b.Transition.Subresource = mip + layer * mips;
                b.Transition.StateBefore = before;
                cmdList->ResourceBarrier(1, &b);
            }
        }
        tex->setState(after); // every subresource is now `after`, so uniform is the truth
    }

} // namespace foundation::rhi::dx12

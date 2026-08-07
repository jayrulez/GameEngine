/// DX12 implementation of SwapChain.
/// Wraps IDXGISwapChain3 for presentation.
/// Ported from Sedulous.RHI.DX12/DX12SwapChain.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:swap_chain;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :surface;
import :texture;
import :texture_view;
import :descriptor_heap;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxDeviceImpl; // forward
    class DxQueueImpl;  // forward

    class DxSwapChainImpl : public SwapChain
    {
    public:
        Status init(ID3D12Device* device, IDXGIFactory4* factory, ID3D12CommandQueue* gfxQueue,
                    DxSurfaceImpl* surface, const SwapChainDesc& d,
                    DxDescriptorHeapAllocator* srvHeap, DxDescriptorHeapAllocator* rtvHeap,
                    DxDescriptorHeapAllocator* dsvHeap, IAllocator& allocator)
        {
            m_d3dDevice = device;
            m_format = d.format;
            m_width = d.width;
            m_height = d.height;
            m_bufferCount = d.bufferCount;
            m_presentMode = d.presentMode;
            m_srvHeap = srvHeap;
            m_rtvHeap = rtvHeap;
            m_dsvHeap = dsvHeap;
            m_allocator = &allocator;

            DXGI_FORMAT swapFmt = stripSrgb(toDxgiFormat(d.format));

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = d.width;
            sd.Height = d.height;
            sd.Format = swapFmt;
            sd.SampleDesc = {1, 0};
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = d.bufferCount;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            if (d.presentMode == PresentMode::Immediate)
                sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

            ComPtr<IDXGISwapChain1> sc1;
            HRESULT hr = factory->CreateSwapChainForHwnd(gfxQueue, surface->handle(), &sd, nullptr,
                                                         nullptr, &sc1);
            if (FAILED(hr))
            {
                LogErrorf("DxSwapChain: CreateSwapChainForHwnd failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            factory->MakeWindowAssociation(surface->handle(), DXGI_MWA_NO_ALT_ENTER);

            hr = sc1.As(&m_swapChain);
            if (FAILED(hr))
                return ErrorCode::Unknown;

            if (acquireBackBuffers() != ErrorCode::Ok)
                return ErrorCode::Unknown;
            m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
            return ErrorCode::Ok;
        }

        // ---- SwapChain interface ----
        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_bufferCount; }
        u32 CurrentImageIndex() const override { return m_currentIndex; }
        Texture* CurrentTexture() override
        {
            return (m_currentIndex < m_textures.Size()) ? m_textures[m_currentIndex] : nullptr;
        }
        TextureView* CurrentTextureView() override
        {
            return (m_currentIndex < m_views.Size()) ? m_views[m_currentIndex] : nullptr;
        }

        Status AcquireNextImage() override
        {
            m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
            return ErrorCode::Ok;
        }

        Status Present(Queue* /*queue*/) override
        {
            UINT syncInterval = 1, flags = 0;
            switch (m_presentMode)
            {
            case PresentMode::Immediate:
                syncInterval = 0;
                flags = DXGI_PRESENT_ALLOW_TEARING;
                break;
            case PresentMode::Mailbox:
                syncInterval = 0;
                break;
            case PresentMode::Fifo:
                syncInterval = 1;
                break;
            case PresentMode::FifoRelaxed:
                syncInterval = 1;
                break;
            }
            return SUCCEEDED(m_swapChain->Present(syncInterval, flags)) ? ErrorCode::Ok
                                                                        : ErrorCode::Unknown;
        }

        Status Resize(u32 w, u32 h) override
        {
            if (w == 0 || h == 0)
                return ErrorCode::Ok;
            m_width = w;
            m_height = h;
            releaseBackBuffers();
            HRESULT hr = m_swapChain->ResizeBuffers(m_bufferCount, w, h,
                                                    stripSrgb(toDxgiFormat(m_format)), 0);
            if (FAILED(hr))
                return ErrorCode::Unknown;
            if (acquireBackBuffers() != ErrorCode::Ok)
                return ErrorCode::Unknown;
            m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
            return ErrorCode::Ok;
        }

        void cleanup()
        {
            releaseBackBuffers();
            m_swapChain.Reset();
        }

    private:
        Status acquireBackBuffers()
        {
            for (u32 i = 0; i < m_bufferCount; ++i)
            {
                ID3D12Resource* resource = nullptr;
                if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&resource))))
                    return ErrorCode::Unknown;

                auto* tex = m_allocator->New<DxTextureImpl>();
                TextureDesc td{};
                td.dimension = TextureDimension::Texture2D;
                td.format = m_format;
                td.width = m_width;
                td.height = m_height;
                td.arrayLayerCount = 1;
                td.mipLevelCount = 1;
                td.sampleCount = 1;
                td.usage = TextureUsage::RenderTarget;
                tex->initFromExisting(resource, td);
                resource->Release(); // initFromExisting AddRef'd
                m_textures.PushBack(tex);

                auto* view = m_allocator->New<DxTextureViewImpl>();
                TextureViewDesc vd{};
                vd.format = m_format;
                vd.dimension = TextureViewDimension::Texture2D;
                vd.mipLevelCount = 1;
                vd.arrayLayerCount = 1;
                view->init(m_d3dDevice, tex, vd, m_srvHeap, m_rtvHeap, m_dsvHeap);
                m_views.PushBack(view);
            }
            return ErrorCode::Ok;
        }

        void releaseBackBuffers()
        {
            for (auto* v : m_views)
            {
                v->cleanup();
                m_allocator->Delete(v);
            }
            m_views.Clear();
            for (auto* t : m_textures)
            {
                t->cleanup();
                m_allocator->Delete(t);
            }
            m_textures.Clear();
        }

        ComPtr<IDXGISwapChain3> m_swapChain;
        ID3D12Device* m_d3dDevice = nullptr;
        IAllocator* m_allocator = nullptr;
        TextureFormat m_format = TextureFormat::RGBA8UnormSrgb;
        u32 m_width = 0, m_height = 0, m_bufferCount = 2;
        u32 m_currentIndex = 0;
        PresentMode m_presentMode = PresentMode::Fifo;
        Array<DxTextureImpl*> m_textures;
        Array<DxTextureViewImpl*> m_views;
        DxDescriptorHeapAllocator* m_srvHeap = nullptr;
        DxDescriptorHeapAllocator* m_rtvHeap = nullptr;
        DxDescriptorHeapAllocator* m_dsvHeap = nullptr;
    };

} // namespace draconic::rhi::dx12

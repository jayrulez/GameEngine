/// DX12 implementation of Backend.
/// Creates DXGI factory, enumerates adapters, creates surfaces.
/// Ported from Sedulous.RHI.DX12/DX12Backend.bf.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

#include <cstdio>

export module draconic.rhi.dx12:backend;

import draconic.core;
import draconic.rhi;
import :surface;
import :adapter;

using namespace draconic::core;

export namespace draconic::rhi::dx12
{

    /// Configuration for DX12 backend creation.
    struct DxBackendDesc
    {
        bool enableValidation = false;
    };

    /// DX12 implementation of Backend.
    class DxBackendImpl : public Backend
    {
    public:
        explicit DxBackendImpl(IAllocator& allocator) noexcept : m_allocator(allocator) {}
        ~DxBackendImpl() override { destroyImpl(); }

        friend Status CreateDxBackend(const DxBackendDesc&, Backend*&, IAllocator&);

        // ---- Backend interface ----

        Span<Adapter* const> EnumerateAdapters() override
        {
            return Span<Adapter* const>(m_adapterPtrs.Data(), m_adapterPtrs.Size());
        }

        Status CreateSurface(void* windowHandle, void* /*displayHandle*/, Surface*& out,
                             SurfacePlatform /*platform*/ = SurfacePlatform::Unknown) override
        {
            out = nullptr;
            if (!windowHandle)
            {
                LogError("DxBackend: window handle is null");
                return ErrorCode::InvalidArgument;
            }
            out = m_allocator.New<DxSurfaceImpl>(reinterpret_cast<HWND>(windowHandle));
            return ErrorCode::Ok;
        }

        void Destroy() override
        {
            destroyImpl();
            IAllocator& alloc = m_allocator;
            this->~DxBackendImpl();
            alloc.Free(this);
        }

        // ---- Internal ----
        [[nodiscard]] IDXGIFactory4* factory() const { return m_factory.Get(); }
        [[nodiscard]] bool validationEnabled() const { return m_validationEnabled; }
        [[nodiscard]] IAllocator& allocator() const noexcept { return m_allocator; }

    private:
        Status init(bool enableValidation)
        {
            m_validationEnabled = enableValidation;

            // Enable debug layer + DRED (Device Removed Extended Data) before device creation.
            if (m_validationEnabled)
            {
                ComPtr<ID3D12Debug> debugController;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
                {
                    debugController->EnableDebugLayer();
                }
                // DRED (Device Removed Extended Data): enable only when needed — auto-breadcrumbs
                // add a GPU write per draw call, which can itself trigger TDR on heavy scenes.
                // Uncomment the block below to diagnose a GPU hang.
                // ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
                // if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
                // {
                //     dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                //     dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                // }
            }

            // Create DXGI factory.
            UINT factoryFlags = m_validationEnabled ? DXGI_CREATE_FACTORY_DEBUG : 0;
            HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
            if (FAILED(hr))
            {
                LogErrorf("DxBackend: CreateDXGIFactory2 failed (0x%08X)",
                          static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            enumerateAdaptersInternal();
            isInitialized = true;
            return ErrorCode::Ok;
        }

        void enumerateAdaptersInternal()
        {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);

                // Skip software adapters.
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    adapter.Reset();
                    continue;
                }

                // Check D3D12 feature level 12.0 support.
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                                __uuidof(ID3D12Device), nullptr)))
                {
                    auto* a = m_allocator.New<DxAdapterImpl>(adapter.Detach(), m_factory.Get(),
                                                             m_allocator);
                    m_adapters.PushBack(a);
                    m_adapterPtrs.PushBack(a);
                }

                adapter.Reset();
            }

            // Expose adapters best-GPU-first; callers take [0]. See Backend::enumerateAdapters.
            SortAdaptersByPreference(m_adapterPtrs);
        }

        void destroyImpl()
        {
            for (auto* a : m_adapters)
                m_allocator.Delete(a);
            m_adapters.Clear();
            m_adapterPtrs.Clear();
            m_factory.Reset();
        }

        IAllocator& m_allocator;
        ComPtr<IDXGIFactory4> m_factory;
        bool m_validationEnabled = false;
        Array<DxAdapterImpl*> m_adapters;
        Array<Adapter*> m_adapterPtrs;
    };

    /// Creates a DX12 backend. Caller owns the returned pointer - dispose via Destroy().
    [[nodiscard]] Status CreateDxBackend(const DxBackendDesc& desc, Backend*& out,
                                         IAllocator& allocator = DefaultAllocator())
    {
        out = nullptr;
        auto* b = allocator.New<DxBackendImpl>(allocator);
        if (b == nullptr)
        {
            return ErrorCode::OutOfMemory;
        }
        Status r = b->init(desc.enableValidation);
        if (r != ErrorCode::Ok)
        {
            allocator.Delete(b);
            return r;
        }
        out = b;
        return ErrorCode::Ok;
    }

} // namespace draconic::rhi::dx12

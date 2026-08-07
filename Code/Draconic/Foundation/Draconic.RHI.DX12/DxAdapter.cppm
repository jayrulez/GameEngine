/// DX12 implementation of Adapter.
/// Wraps IDXGIAdapter1, queries device features, creates DxDevice.
/// Ported from Sedulous.RHI.DX12/DX12Adapter.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

#include <cstring>

export module draconic.rhi.dx12:adapter;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxDeviceImpl; // forward

    class DxAdapterImpl : public Adapter
    {
    public:
        DxAdapterImpl(IDXGIAdapter1* adapter, IDXGIFactory4* factory, IAllocator& allocator)
            : m_allocator(allocator), m_adapter(adapter), m_factory(factory)
        {
            m_adapter->GetDesc1(&m_desc);
        }

        ~DxAdapterImpl() override
        {
            if (m_adapter)
            {
                m_adapter->Release();
                m_adapter = nullptr;
            }
        }

        // ---- Adapter interface ----

        void GetInfo(AdapterInfo& out) override
        {
            // DXGI Description is a WCHAR[] (UTF-16) - transcode to the UTF-8 String.
            out.name =
                ToUTF8(WideStringView(reinterpret_cast<const widechar*>(m_desc.Description)));
            out.vendorId = m_desc.VendorId;
            out.deviceId = m_desc.DeviceId;
            out.type = (m_desc.DedicatedVideoMemory > 0) ? AdapterType::DiscreteGpu
                                                         : AdapterType::IntegratedGpu;
            out.supportedFeatures = buildFeatures();
        }

        DeviceFeatures buildFeatures()
        {
            // Create a temporary device to query features.
            ComPtr<ID3D12Device> tempDevice;
            HRESULT hr =
                D3D12CreateDevice(m_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&tempDevice));
            if (FAILED(hr) || !tempDevice)
                return {};

            DeviceFeatures f{};

            // Check feature support.
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            if (SUCCEEDED(tempDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                          sizeof(options))))
            {
                f.bindlessDescriptors = true; // DX12 always supports descriptor indexing
                f.timestampQueries = true;
                f.occlusionQueries = true; // BeginQuery works anywhere in a pass
                f.borderSampling = true;
                f.multiDrawIndirect = true;
                f.depthClamp = true;
                f.fillModeWireframe = true;
                f.textureCompressionBC = true;
                f.textureCompressionASTC = false;
                f.independentBlend = true;
                f.multiViewport = true;
                f.pipelineStatisticsQueries = true;
            }

            // Check mesh shader support.
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
            if (SUCCEEDED(tempDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                                          sizeof(options7))))
            {
                f.meshShaders = (options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
            }

            // Check ray tracing support.
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
            if (SUCCEEDED(tempDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                                          sizeof(options5))))
            {
                f.rayTracing = (options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
            }

            // Conservative limits for D3D12 feature level 12.0.
            f.maxBindGroups = 32;
            f.maxBindingsPerGroup = 1000000;
            f.maxPushConstantSize = 128;
            f.maxTextureDimension2D = 16384;
            f.maxTextureArrayLayers = 2048;
            f.maxComputeWorkgroupSizeX = 1024;
            f.maxComputeWorkgroupSizeY = 1024;
            f.maxComputeWorkgroupSizeZ = 64;
            f.maxComputeWorkgroupsPerDimension = 65535;
            f.maxBufferSize = static_cast<u64>(m_desc.DedicatedVideoMemory);
            f.minUniformBufferOffsetAlignment = 256;
            f.minStorageBufferOffsetAlignment = 16;
            f.timestampPeriodNs = 1; // DX12 timestamps in ticks, period queried at runtime

            return f;
        }

        Status CreateDevice(const DeviceDesc& desc, Device*& out) override;

        // ---- Internal ----
        [[nodiscard]] IDXGIAdapter1* handle() const { return m_adapter; }
        [[nodiscard]] IDXGIFactory4* factory() const { return m_factory; }
        [[nodiscard]] DXGI_ADAPTER_DESC1 adapterDesc() const { return m_desc; }
        [[nodiscard]] IAllocator& allocator() const noexcept { return m_allocator; }

    private:
        IAllocator& m_allocator;
        IDXGIAdapter1* m_adapter = nullptr; // owned, released in destructor
        IDXGIFactory4* m_factory = nullptr; // not owned (Backend owns it)
        DXGI_ADAPTER_DESC1 m_desc{};
    };

} // namespace draconic::rhi::dx12

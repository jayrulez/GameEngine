/// draconic.rhi.webgpu:bind_group_layout - BindGroupLayout over WGPUBindGroupLayout.
///
/// Bindings are declared SHIFTED (ShiftedBinding - the compact WebGPU profile,
/// matching what the compile side bakes into SPIR-V for WebGPU devices). Bindless
/// and acceleration-structure entry types have no WebGPU shape - honest
/// NotSupported, as are binding arrays (count > 1). Texture sample types come from
/// the entry's EXPLICIT textureSampleType (WebGPU validates it against the shader).

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:bind_group_layout;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuBindGroupLayout final : public BindGroupLayout
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device,
                          const BindGroupLayoutDesc& layoutDesc)
        {
            m_api = &api;

            // Own a copy: Entries() serves it, and bind-group creation replays it to
            // shift bindings by the SAME rule the layout used.
            for (const BindGroupLayoutEntry& entry : layoutDesc.entries)
            {
                m_entries.PushBack(entry);
            }

            Array<WGPUBindGroupLayoutEntry> wgpuEntries;
            for (const BindGroupLayoutEntry& entry : m_entries)
            {
                if (entry.count > 1)
                {
                    return ErrorCode::NotSupported; // no binding arrays in core WebGPU
                }
                WGPUBindGroupLayoutEntry wgpuEntry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                wgpuEntry.binding = ShiftedBinding(entry.type, entry.binding);
                wgpuEntry.visibility = ToWgpuShaderStage(entry.visibility);
                switch (entry.type)
                {
                case BindingType::UniformBuffer:
                    wgpuEntry.buffer.type = WGPUBufferBindingType_Uniform;
                    wgpuEntry.buffer.hasDynamicOffset = entry.hasDynamicOffset;
                    break;
                case BindingType::StorageBufferReadOnly:
                    wgpuEntry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                    wgpuEntry.buffer.hasDynamicOffset = entry.hasDynamicOffset;
                    break;
                case BindingType::StorageBufferReadWrite:
                    wgpuEntry.buffer.type = WGPUBufferBindingType_Storage;
                    wgpuEntry.buffer.hasDynamicOffset = entry.hasDynamicOffset;
                    break;
                case BindingType::SampledTexture:
                    wgpuEntry.texture.sampleType =
                        ToWgpuTextureSampleType(entry.textureSampleType);
                    wgpuEntry.texture.viewDimension =
                        ToWgpuTextureViewDimension(entry.textureDimension);
                    wgpuEntry.texture.multisampled = entry.textureMultisampled;
                    break;
                case BindingType::StorageTextureReadOnly:
                    wgpuEntry.storageTexture.access = WGPUStorageTextureAccess_ReadOnly;
                    wgpuEntry.storageTexture.format =
                        ToWgpuTextureFormat(entry.storageTextureFormat);
                    wgpuEntry.storageTexture.viewDimension =
                        ToWgpuTextureViewDimension(entry.textureDimension);
                    break;
                case BindingType::StorageTextureReadWrite:
                    wgpuEntry.storageTexture.access = WGPUStorageTextureAccess_ReadWrite;
                    wgpuEntry.storageTexture.format =
                        ToWgpuTextureFormat(entry.storageTextureFormat);
                    wgpuEntry.storageTexture.viewDimension =
                        ToWgpuTextureViewDimension(entry.textureDimension);
                    break;
                case BindingType::Sampler:
                    wgpuEntry.sampler.type = entry.samplerNonFiltering
                                                 ? WGPUSamplerBindingType_NonFiltering
                                                 : WGPUSamplerBindingType_Filtering;
                    break;
                case BindingType::ComparisonSampler:
                    wgpuEntry.sampler.type = WGPUSamplerBindingType_Comparison;
                    break;
                default:
                    return ErrorCode::NotSupported; // bindless / accel structs
                }
                wgpuEntries.PushBack(wgpuEntry);
            }

            WGPUBindGroupLayoutDescriptor wgpuDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(layoutDesc.label);
            wgpuDesc.entryCount = wgpuEntries.Size();
            wgpuDesc.entries = wgpuEntries.Data();
            m_layout = api.wgpuDeviceCreateBindGroupLayout(device, &wgpuDesc);
            return m_layout != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        Span<const BindGroupLayoutEntry> Entries() const override
        {
            return Span<const BindGroupLayoutEntry>(m_entries.Data(), m_entries.Size());
        }

        void Release()
        {
            if (m_layout != nullptr)
            {
                m_api->wgpuBindGroupLayoutRelease(m_layout);
                m_layout = nullptr;
            }
        }

        [[nodiscard]] WGPUBindGroupLayout Handle() const { return m_layout; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUBindGroupLayout m_layout = nullptr;
        Array<BindGroupLayoutEntry> m_entries;
    };
}

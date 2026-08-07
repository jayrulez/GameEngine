/// draconic.rhi.webgpu:pipeline_layout - PipelineLayout over WGPUPipelineLayout.
///
/// Push constants take one of two shapes:
///  - IMMEDIATES (wgpu-native, WGPUNativeFeature_Immediates): the layout declares
///    immediateSize and the encoders call SetImmediates.
///  - UNIFORM-BUFFER FALLBACK (browsers - Dawn has no immediates - or the fallback forced
///    for testing): the block is bound as an ordinary uniform buffer at @group(space)
///    @binding(0), matching the web WGSL cook (Data/Shaders/push_constant.hlsli). The layout
///    synthesizes that group's bind-group layout here and reports it; the pass encoders keep
///    a CPU shadow of the block and, before each draw, upload it and bind the group.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:pipeline_layout;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;
import :bind_group_layout;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    /// A pipeline's push-constant emulation binding, snapshotted onto pipelines so the pass
    /// encoders can resolve it on SetPipeline. `group` is -1 when the pipeline issues native
    /// immediates (no emulation); otherwise the block binds as a uniform at (group, binding 0).
    struct PushConstantEmulation
    {
        i32 group = -1;
        WGPUBindGroupLayout layout = nullptr;
        u32 blockSize = 0;
    };

    class WebGpuPipelineLayout final : public PipelineLayout
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device,
                          const PipelineLayoutDesc& layoutDesc, bool emulatePushConstants)
        {
            m_api = &api;

            Array<WGPUBindGroupLayout> layouts;
            for (BindGroupLayout* layout : layoutDesc.bindGroupLayouts)
            {
                if (layout == nullptr)
                {
                    return ErrorCode::InvalidArgument;
                }
                layouts.PushBack(static_cast<WebGpuBindGroupLayout*>(layout)->Handle());
            }

            // Aggregate the push-constant block: its byte size (max range end), the union of
            // stages that touch it, and the target group (all ranges share the one b0 block).
            u32 blockSize = 0;
            ShaderStage stages = ShaderStage::None;
            u32 group = 0;
            bool havePushConstants = false;
            for (const PushConstantRange& range : layoutDesc.pushConstantRanges)
            {
                const u32 end = range.offset + range.size;
                blockSize = end > blockSize ? end : blockSize;
                stages = stages | range.stages;
                group = range.bindGroupIndex;
                havePushConstants = true;
            }

            u32 immediateSize = blockSize;
            if (havePushConstants && emulatePushConstants)
            {
                // Fallback: synthesize the uniform-buffer bind-group layout the WGSL expects at
                // @group(group) @binding(0), and place it at that index (padding earlier unused
                // groups with empty layouts). No immediates are declared in this mode.
                WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                entry.binding = 0; // CBV shift is 0, so the block stays at binding 0
                entry.visibility = ToWgpuShaderStage(stages);
                entry.buffer.type = WGPUBufferBindingType_Uniform;
                entry.buffer.hasDynamicOffset = false;
                WGPUBindGroupLayoutDescriptor bglDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
                bglDesc.entryCount = 1;
                bglDesc.entries = &entry;
                m_emulatedLayout = api.wgpuDeviceCreateBindGroupLayout(device, &bglDesc);
                if (m_emulatedLayout == nullptr)
                {
                    return ErrorCode::Unknown;
                }

                while (layouts.Size() < group)
                {
                    WGPUBindGroupLayoutDescriptor emptyDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
                    WGPUBindGroupLayout empty =
                        api.wgpuDeviceCreateBindGroupLayout(device, &emptyDesc);
                    if (empty == nullptr)
                    {
                        return ErrorCode::Unknown;
                    }
                    m_emptyFillers.PushBack(empty);
                    layouts.PushBack(empty);
                }
                if (group < layouts.Size())
                {
                    return ErrorCode::InvalidArgument; // slot already holds a real bind group
                }
                layouts.PushBack(m_emulatedLayout); // now at index `group`
                m_emulatedGroup = static_cast<i32>(group);
                m_emulatedBlockSize = blockSize;
                immediateSize = 0;
            }

            WGPUPipelineLayoutDescriptor wgpuDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(layoutDesc.label);
            wgpuDesc.bindGroupLayoutCount = layouts.Size();
            wgpuDesc.bindGroupLayouts = layouts.Data();
            wgpuDesc.immediateSize = immediateSize;
            m_layout = api.wgpuDeviceCreatePipelineLayout(device, &wgpuDesc);
            return m_layout != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_layout != nullptr)
            {
                m_api->wgpuPipelineLayoutRelease(m_layout);
                m_layout = nullptr;
            }
            if (m_emulatedLayout != nullptr)
            {
                m_api->wgpuBindGroupLayoutRelease(m_emulatedLayout);
                m_emulatedLayout = nullptr;
            }
            for (WGPUBindGroupLayout filler : m_emptyFillers)
            {
                m_api->wgpuBindGroupLayoutRelease(filler);
            }
            m_emptyFillers.Clear();
        }

        [[nodiscard]] WGPUPipelineLayout Handle() const { return m_layout; }

        /// Push-constant emulation (uniform-buffer fallback): the @group the block binds to,
        /// or -1 when this layout issues native immediates instead. When >= 0, the pass
        /// encoders shadow `EmulatedPushConstantBlockSize()` bytes and bind this layout.
        [[nodiscard]] i32 EmulatedPushConstantGroup() const { return m_emulatedGroup; }
        [[nodiscard]] WGPUBindGroupLayout EmulatedPushConstantLayout() const
        {
            return m_emulatedLayout;
        }
        [[nodiscard]] u32 EmulatedPushConstantBlockSize() const { return m_emulatedBlockSize; }
        [[nodiscard]] PushConstantEmulation EmulationInfo() const
        {
            return PushConstantEmulation{m_emulatedGroup, m_emulatedLayout, m_emulatedBlockSize};
        }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUPipelineLayout m_layout = nullptr;
        WGPUBindGroupLayout m_emulatedLayout = nullptr;
        Array<WGPUBindGroupLayout> m_emptyFillers;
        i32 m_emulatedGroup = -1;
        u32 m_emulatedBlockSize = 0;
    };
}

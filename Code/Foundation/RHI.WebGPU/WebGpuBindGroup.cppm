// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:bind_group - BindGroup over WGPUBindGroup.
///
/// Desc entries are positional against the layout's entries (the RHI contract all
/// backends share); each resolves to the layout entry's shifted binding number.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:bind_group;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;
import :bind_group_layout;
import :buffer;
import :texture_view;
import :sampler;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuBindGroup final : public BindGroup
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device, const BindGroupDesc& groupDesc)
        {
            m_api = &api;
            m_layout = static_cast<WebGpuBindGroupLayout*>(groupDesc.layout);
            if (m_layout == nullptr)
            {
                return ErrorCode::InvalidArgument;
            }
            const Span<const BindGroupLayoutEntry> layoutEntries = m_layout->Entries();
            if (groupDesc.entries.Size() != layoutEntries.Size())
            {
                return ErrorCode::InvalidArgument; // positional contract violated
            }

            Array<WGPUBindGroupEntry> wgpuEntries;
            for (usize i = 0; i < groupDesc.entries.Size(); ++i)
            {
                const BindGroupEntry& entry = groupDesc.entries[i];
                const BindGroupLayoutEntry& layoutEntry = layoutEntries[i];

                WGPUBindGroupEntry wgpuEntry = WGPU_BIND_GROUP_ENTRY_INIT;
                wgpuEntry.binding = ShiftedBinding(layoutEntry.type, layoutEntry.binding);
                if (entry.buffer != nullptr)
                {
                    wgpuEntry.buffer = static_cast<WebGpuBuffer*>(entry.buffer)->Handle();
                    wgpuEntry.offset = entry.bufferOffset;
                    wgpuEntry.size =
                        entry.bufferSize != 0 ? entry.bufferSize : WGPU_WHOLE_SIZE;
                }
                else if (entry.textureView != nullptr)
                {
                    wgpuEntry.textureView =
                        static_cast<WebGpuTextureView*>(entry.textureView)->Handle();
                }
                else if (entry.sampler != nullptr)
                {
                    wgpuEntry.sampler = static_cast<WebGpuSampler*>(entry.sampler)->Handle();
                }
                else
                {
                    return ErrorCode::InvalidArgument; // empty entry (accel structs never here)
                }
                wgpuEntries.PushBack(wgpuEntry);
            }

            WGPUBindGroupDescriptor wgpuDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(groupDesc.label);
            wgpuDesc.layout = m_layout->Handle();
            wgpuDesc.entryCount = wgpuEntries.Size();
            wgpuDesc.entries = wgpuEntries.Data();
            m_group = api.wgpuDeviceCreateBindGroup(device, &wgpuDesc);
            return m_group != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        BindGroupLayout* Layout() override { return m_layout; }

        void UpdateBindless(Span<const BindlessUpdateEntry>) override
        {
            // Bindless never reaches WebGPU (its layouts fail creation), so there is nothing to do.
        }

        void Release()
        {
            if (m_group != nullptr)
            {
                m_api->wgpuBindGroupRelease(m_group);
                m_group = nullptr;
            }
        }

        [[nodiscard]] WGPUBindGroup Handle() const { return m_group; }

    private:
        const WebGpuApi* m_api = nullptr;
        WebGpuBindGroupLayout* m_layout = nullptr;
        WGPUBindGroup m_group = nullptr;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of PipelineLayout.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:pipeline_layout;

import foundation.core;
import foundation.rhi;
import :conversions;
import :bind_group_layout;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkPipelineLayoutImpl : public PipelineLayout
    {
    public:
        Status init(VkDevice device, const PipelineLayoutDesc& desc)
        {
            Array<VkDescriptorSetLayout> setLayouts(desc.bindGroupLayouts.Size());
            for (usize i = 0; i < desc.bindGroupLayouts.Size(); ++i)
            {
                auto* vkl = static_cast<VkBindGroupLayoutImpl*>(desc.bindGroupLayouts[i]);
                if (!vkl)
                    return ErrorCode::Unknown;
                setLayouts[i] = vkl->handle();
            }

            Array<VkPushConstantRange> pushRanges(desc.pushConstantRanges.Size());
            for (usize i = 0; i < desc.pushConstantRanges.Size(); ++i)
            {
                pushRanges[i] = {};
                pushRanges[i].stageFlags = toVkShaderStageFlags(desc.pushConstantRanges[i].stages);
                pushRanges[i].offset = desc.pushConstantRanges[i].offset;
                pushRanges[i].size = desc.pushConstantRanges[i].size;
            }

            VkPipelineLayoutCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            ci.setLayoutCount = static_cast<u32>(setLayouts.Size());
            ci.pSetLayouts = setLayouts.Data();
            ci.pushConstantRangeCount = static_cast<u32>(pushRanges.Size());
            ci.pPushConstantRanges = pushRanges.Data();

            if (vkCreatePipelineLayout(device, &ci, nullptr, &m_layout) != VK_SUCCESS)
                return ErrorCode::Unknown;
            return ErrorCode::Ok;
        }

        void cleanup(VkDevice device)
        {
            if (m_layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(device, m_layout, nullptr);
                m_layout = VK_NULL_HANDLE;
            }
        }

        [[nodiscard]] VkPipelineLayout handle() const { return m_layout; }

    private:
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
    };

} // namespace foundation::rhi::vk

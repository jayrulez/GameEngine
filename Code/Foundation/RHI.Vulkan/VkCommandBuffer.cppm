// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of CommandBuffer.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:command_buffer;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkCommandBufferImpl : public CommandBuffer
    {
    public:
        explicit VkCommandBufferImpl(VkCommandBuffer cmdBuf) : m_cmdBuf(cmdBuf) {}

        [[nodiscard]] VkCommandBuffer handle() const { return m_cmdBuf; }

    private:
        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
    };

} // namespace foundation::rhi::vk

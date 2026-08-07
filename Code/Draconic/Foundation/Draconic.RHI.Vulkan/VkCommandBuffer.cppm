/// Vulkan implementation of CommandBuffer.
/// Ported from Sedulous.RHI.Vulkan/VulkanCommandBuffer.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:command_buffer;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkCommandBufferImpl : public CommandBuffer
    {
    public:
        explicit VkCommandBufferImpl(VkCommandBuffer cmdBuf) : m_cmdBuf(cmdBuf) {}

        [[nodiscard]] VkCommandBuffer handle() const { return m_cmdBuf; }

    private:
        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
    };

} // namespace draconic::rhi::vk

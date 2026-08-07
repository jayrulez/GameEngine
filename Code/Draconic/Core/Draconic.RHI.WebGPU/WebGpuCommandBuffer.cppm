/// draconic.rhi.webgpu:command_buffer - CommandBuffer over WGPUCommandBuffer.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:command_buffer;

import draconic.foundation;
import draconic.rhi;
import :api;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuCommandBuffer final : public CommandBuffer
    {
    public:
        /// Takes ownership of a finished WGPUCommandBuffer; consumed (released) by
        /// Queue::Submit, or on the next Adopt if never submitted.
        void Adopt(const WebGpuApi& api, WGPUCommandBuffer commandBuffer)
        {
            m_api = &api;
            ReleaseHandle();
            m_commandBuffer = commandBuffer;
        }

        /// The queue takes the handle for submission; the wrapper forgets it.
        [[nodiscard]] WGPUCommandBuffer Take()
        {
            WGPUCommandBuffer out = m_commandBuffer;
            m_commandBuffer = nullptr;
            return out;
        }

        void ReleaseHandle()
        {
            if (m_commandBuffer != nullptr)
            {
                m_api->wgpuCommandBufferRelease(m_commandBuffer);
                m_commandBuffer = nullptr;
            }
        }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUCommandBuffer m_commandBuffer = nullptr;
    };
}

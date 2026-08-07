/// draconic.rhi.webgpu:push_constant_emulator - the uniform-buffer push-constant fallback.
///
/// Where a device has no immediates path (browsers - Dawn/emdawnwebgpu - or the fallback
/// forced for testing on wgpu-native), push constants are emulated: the block is bound as an
/// ordinary uniform buffer at the pipeline's @group(space) @binding(0) (the shape the web WGSL
/// cook emits, see Data/Shaders/push_constant.hlsli). This helper is shared by the pass
/// encoders. It keeps a CPU shadow of the block, and before each draw uploads a FRESH uniform
/// buffer (so consecutive draws never alias the same memory) and binds it. Every buffer/bind
/// group created this way lives until the pass ends, then Release() frees them - the recorded
/// commands retain what the GPU needs, so releasing the wrappers after End is safe.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:push_constant_emulator;

import draconic.foundation;
import :api;
import :pipeline_layout;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class PushConstantEmulator
    {
    public:
        static constexpr u32 kMaxBlockSize = 256; // RHI push-constant contract is 128; pad

        /// Called when a pass opens: (re)binds the device and clears any residual state.
        void Begin(const WebGpuApi& api, WGPUDevice device)
        {
            m_api = &api;
            m_device = device;
            m_group = -1;
            m_layout = nullptr;
            m_blockSize = 0;
            m_dirty = false;
            m_buffers.Clear();
            m_bindGroups.Clear();
            // Clear the shadow too: push data is "undefined until set" per pass, and a
            // reused pass-encoder object must not carry the PREVIOUS pass's bytes into a
            // draw that never called SetPushConstants (Vulkan immediates would not).
            for (u8& b : m_shadow)
            {
                b = 0;
            }
        }

        /// On SetPipeline: adopt the pipeline's emulated binding. group < 0 means the pipeline
        /// issues native immediates, so this helper stays inert for it.
        void SetPipeline(const PushConstantEmulation& pushConstants)
        {
            m_group = pushConstants.group;
            m_layout = pushConstants.layout;
            if (pushConstants.blockSize > m_blockSize)
            {
                m_blockSize = pushConstants.blockSize;
            }
            if (m_group >= 0)
            {
                m_dirty = true; // a freshly bound pipeline needs its group set before drawing
            }
        }

        /// On SetPushConstants: fold the sub-range into the shadow. Returns false when the
        /// current pipeline is NOT emulating, so the caller takes the native SetImmediates path.
        bool Write(u32 offset, u32 size, const void* data)
        {
            if (m_group < 0)
            {
                return false;
            }
            if (data != nullptr && static_cast<u64>(offset) + size <= kMaxBlockSize)
            {
                const u8* source = static_cast<const u8*>(data);
                for (u32 i = 0; i < size; ++i)
                {
                    m_shadow[offset + i] = source[i];
                }
                if (offset + size > m_blockSize)
                {
                    m_blockSize = offset + size;
                }
            }
            m_dirty = true;
            return true;
        }

        /// Before a draw/dispatch: if a fresh block is pending, upload it to a new uniform buffer
        /// and hand back the bind group + @group to set. Returns false when nothing needs
        /// (re)binding (not emulating, or the current block is already bound).
        bool FlushBeforeDraw(i32& group, WGPUBindGroup& bindGroup)
        {
            if (m_group < 0 || !m_dirty || m_layout == nullptr || m_device == nullptr)
            {
                return false;
            }
            u32 size = m_blockSize > 0 ? m_blockSize : 16u;
            size = (size + 15u) & ~15u; // uniform buffers round to 16
            if (size > kMaxBlockSize)
            {
                size = kMaxBlockSize;
            }

            WGPUBufferDescriptor bufferDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            bufferDesc.size = size;
            bufferDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            WGPUBuffer buffer = m_api->wgpuDeviceCreateBuffer(m_device, &bufferDesc);
            if (buffer == nullptr)
            {
                return false;
            }
            WGPUQueue queue = m_api->wgpuDeviceGetQueue(m_device);
            m_api->wgpuQueueWriteBuffer(queue, buffer, 0, m_shadow, size);
            m_api->wgpuQueueRelease(queue);

            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = 0;
            entry.buffer = buffer;
            entry.offset = 0;
            entry.size = size;
            WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            groupDesc.layout = m_layout;
            groupDesc.entryCount = 1;
            groupDesc.entries = &entry;
            WGPUBindGroup created = m_api->wgpuDeviceCreateBindGroup(m_device, &groupDesc);
            if (created == nullptr)
            {
                m_api->wgpuBufferRelease(buffer);
                return false;
            }

            m_buffers.PushBack(buffer);
            m_bindGroups.PushBack(created);
            m_dirty = false;
            group = m_group;
            bindGroup = created;
            return true;
        }

        /// On pass End (AFTER the pass encoder's End): free everything created this pass.
        void Release()
        {
            if (m_api == nullptr)
            {
                return;
            }
            for (WGPUBindGroup group : m_bindGroups)
            {
                m_api->wgpuBindGroupRelease(group);
            }
            for (WGPUBuffer buffer : m_buffers)
            {
                m_api->wgpuBufferRelease(buffer);
            }
            m_bindGroups.Clear();
            m_buffers.Clear();
            m_group = -1;
            m_layout = nullptr;
            m_dirty = false;
        }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUDevice m_device = nullptr;
        i32 m_group = -1;
        WGPUBindGroupLayout m_layout = nullptr;
        u32 m_blockSize = 0;
        bool m_dirty = false;
        u8 m_shadow[kMaxBlockSize] = {};
        Array<WGPUBuffer> m_buffers;
        Array<WGPUBindGroup> m_bindGroups;
    };
}

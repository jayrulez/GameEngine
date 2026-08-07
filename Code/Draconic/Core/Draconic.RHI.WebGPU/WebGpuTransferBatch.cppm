/// draconic.rhi.webgpu:transfer_batch - TransferBatch over queue writes.
///
/// WebGPU's wgpuQueueWriteBuffer/WriteTexture ARE staged uploads (the runtime owns
/// the staging ring), so the batch records payload COPIES and replays them as queue
/// writes at Submit - preserving the RHI ordering contract (writes land at Submit
/// time, not Write time). Submit blocks like the Vulkan batch (queue drain);
/// SubmitAsync signals the fence from OnSubmittedWorkDone instead.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:transfer_batch;

import draconic.foundation;
import draconic.rhi;
import :api;
import :buffer;
import :texture;
import :fence;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuTransferBatch final : public TransferBatch
    {
    public:
        void Initialize(const WebGpuApi& api, WGPUInstance instance, WGPUDevice device,
                        WGPUQueue queue, IAllocator& allocator)
        {
            m_api = &api;
            m_instance = instance;
            m_device = device;
            m_queue = queue;
            m_allocator = &allocator;
        }

        void WriteBuffer(Buffer* dst, u64 dstOffset, Span<const u8> data) override
        {
            BufferWrite write;
            write.destination = static_cast<WebGpuBuffer*>(dst)->Handle();
            write.offset = dstOffset;
            write.dataOffset = m_payload.Size();
            write.size = data.Size();
            AppendPayload(data);
            m_bufferWrites.PushBack(write);
        }

        void WriteTexture(Texture* dst, Span<const u8> data, const TextureDataLayout& layout,
                          Extent3D extent, u32 mipLevel, u32 arrayLayer) override
        {
            TextureWrite write;
            write.destination = static_cast<WebGpuTexture*>(dst)->Handle();
            write.layout = layout;
            write.extent = extent;
            write.mipLevel = mipLevel;
            write.arrayLayer = arrayLayer;
            write.dataOffset = m_payload.Size();
            write.size = data.Size();
            AppendPayload(data);
            m_textureWrites.PushBack(write);
        }

        Status Submit() override
        {
            Replay();
#if !DRACONIC_PLATFORM_WEB
            // The Vulkan batch drains the queue before returning; match it on desktop.
            //
            // On WEB the drain is SKIPPED - and it must be: wgpuQueueWriteBuffer/WriteTexture
            // copy the payload at CALL time and the single WebGPU queue preserves ordering, so
            // every later submit already sees the data (nothing here needs completion). Pumping
            // would yield to the browser MID-FRAME - lazy first-use uploads (GpuMesh, textures,
            // debug-draw fonts) run inside a frame, the yield returns the rAF, the browser
            // expires the canvas texture, and that frame's submit is dropped ("Destroyed
            // texture used in a submit" - the startup killer of one-shot bakes/uploads).
            bool done = false;
            WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
            { *static_cast<bool*>(userdata1) = true; };
            info.userdata1 = &done;
            (void)m_api->wgpuQueueOnSubmittedWorkDone(m_queue, info);
            m_api->PumpUntilWithDevice(m_instance, m_device, done);
#endif
            Reset();
            return ErrorCode::Ok;
        }

        Status SubmitAsync(Fence* fence, u64 signalValue) override
        {
            Replay();
            if (fence != nullptr)
            {
                struct Pending
                {
                    WebGpuFence* fence;
                    u64 value;
                    IAllocator* allocator;
                };
                auto* pending = m_allocator->New<Pending>();
                pending->fence = static_cast<WebGpuFence*>(fence);
                pending->value = signalValue;
                pending->allocator = m_allocator;
                WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
                info.mode = WGPUCallbackMode_AllowProcessEvents;
                info.callback =
                    [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
                {
                    auto* p = static_cast<Pending*>(userdata1);
                    p->fence->SignalFromCallback(p->value);
                    p->allocator->Delete(p);
                };
                info.userdata1 = pending;
                (void)m_api->wgpuQueueOnSubmittedWorkDone(m_queue, info);
            }
            Reset();
            return ErrorCode::Ok;
        }

        void Reset() override
        {
            m_bufferWrites.Clear();
            m_textureWrites.Clear();
            m_payload.Clear();
        }

        void Destroy() override
        {
            IAllocator& allocator = *m_allocator;
            this->~WebGpuTransferBatch();
            allocator.Free(this);
        }

    private:
        struct BufferWrite
        {
            WGPUBuffer destination = nullptr;
            u64 offset = 0;
            usize dataOffset = 0;
            usize size = 0;
        };
        struct TextureWrite
        {
            WGPUTexture destination = nullptr;
            TextureDataLayout layout;
            Extent3D extent;
            u32 mipLevel = 0;
            u32 arrayLayer = 0;
            usize dataOffset = 0;
            usize size = 0;
        };

        void AppendPayload(Span<const u8> data)
        {
            const usize base = m_payload.Size();
            m_payload.Resize(base + data.Size());
            MemCopy(m_payload.Data() + base, data.Data(), data.Size());
        }

        void Replay()
        {
            for (const BufferWrite& write : m_bufferWrites)
            {
                // WriteBuffer sizes must be 4-byte multiples; the payload slice is
                // already contiguous, so rounding within it is safe only when the
                // tail exists - pad via the aligned staging copy when it does not.
                const usize aligned = (write.size + 3u) & ~usize{3};
                if (aligned == write.size)
                {
                    m_api->wgpuQueueWriteBuffer(m_queue, write.destination, write.offset,
                                                m_payload.Data() + write.dataOffset,
                                                write.size);
                }
                else
                {
                    Array<u8> padded;
                    padded.Resize(aligned);
                    MemCopy(padded.Data(), m_payload.Data() + write.dataOffset, write.size);
                    m_api->wgpuQueueWriteBuffer(m_queue, write.destination, write.offset,
                                                padded.Data(), aligned);
                }
            }
            for (const TextureWrite& write : m_textureWrites)
            {
                WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                destination.texture = write.destination;
                destination.mipLevel = write.mipLevel;
                destination.origin.z = write.arrayLayer;
                WGPUTexelCopyBufferLayout layout;
                layout.offset = write.layout.offset;
                layout.bytesPerRow = write.layout.bytesPerRow;
                layout.rowsPerImage = write.layout.rowsPerImage;
                const WGPUExtent3D extent{write.extent.width, write.extent.height,
                                          write.extent.depth};
                m_api->wgpuQueueWriteTexture(m_queue, &destination,
                                             m_payload.Data() + write.dataOffset, write.size,
                                             &layout, &extent);
            }
        }

        const WebGpuApi* m_api = nullptr;
        WGPUInstance m_instance = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUQueue m_queue = nullptr;
        IAllocator* m_allocator = nullptr;
        Array<BufferWrite> m_bufferWrites;
        Array<TextureWrite> m_textureWrites;
        Array<u8> m_payload;
    };
}

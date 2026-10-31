// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:buffer - Buffer over WGPUBuffer, with the Map emulation.
///
/// The RHI's Map contract is Vulkan-shaped: a persistent coherent pointer. Callers may
/// Map once, hold the pointer, write every frame, and never Unmap (Vulkan's Unmap is a
/// no-op). WebGPU forbids mapping buffers that carry normal usages, so:
///   - CpuToGpu: Map returns a CPU shadow. Unmap flushes it (wgpuQueueWriteBuffer,
///     queue-ordered) and closes the mapping. A mapping left open emulates coherence: the
///     queue re-flushes every outstanding shadow before each submit (see
///     WebGpuBufferRegistry), so pointer writes become visible as they do on Vulkan. Each
///     flush skips the upload when the shadow is byte-identical to the last one sent: on
///     web every wgpuQueueWriteBuffer marshals a copy across the wasm-to-JS boundary, so
///     re-sending unchanged persistent buffers every submit would dominate the frame (a
///     native MemCompare is nearly free by comparison). The skip is safe because the GPU
///     already holds those exact bytes.
///   - GpuToCpu: a genuine WebGPU mapping, MapAsync(Read) and a ProcessEvents pump in
///     Map, wgpuBufferUnmap in Unmap. Usage is forced to MapRead|CopyDst (all WebGPU
///     allows).
///   - GpuOnly: Map returns nullptr, as on every backend.
/// WriteBuffer requires 4-byte-multiple sizes, so shadow and upload sizes round up.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:buffer;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuBuffer final : public Buffer
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUInstance instance, WGPUDevice device,
                          WGPUQueue queue, const BufferDesc& bufferDesc)
        {
            m_api = &api;
            m_instance = instance;
            m_device = device;
            m_queue = queue;
            desc = bufferDesc;

            // wgpu does not return null for an invalid descriptor: it returns an
            // "invalid" object and raises an uncaptured error. Reject the cases the
            // validator would: a buffer needs a size and at least one usage (GpuToCpu
            // is exempt; its usage is forced to MapRead|CopyDst below).
            if (bufferDesc.size == 0 ||
                (bufferDesc.usage == BufferUsage::None &&
                 bufferDesc.memory != MemoryLocation::GpuToCpu))
            {
                return ErrorCode::InvalidArgument;
            }

            WGPUBufferDescriptor wgpuDesc = WGPU_BUFFER_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(bufferDesc.label);
            wgpuDesc.usage = ToWgpuBufferUsage(bufferDesc.usage, bufferDesc.memory);
            wgpuDesc.size = AlignedSize();
            m_buffer = api.wgpuDeviceCreateBuffer(device, &wgpuDesc);
            if (m_buffer == nullptr)
            {
                return ErrorCode::Unknown;
            }

            if (bufferDesc.memory == MemoryLocation::CpuToGpu ||
                bufferDesc.memory == MemoryLocation::Auto)
            {
                m_shadow.Resize(static_cast<usize>(AlignedSize()));
            }
            return ErrorCode::Ok;
        }

        void* Map() override
        {
            if (!m_shadow.IsEmpty())
            {
                m_shadowOutstanding = true; // flushed on Unmap and before every submit
                m_rangeFlushed = false;     // a fresh mapping: Unmap flushes whole unless ranged
                return m_shadow.Data();
            }
            if (desc.memory != MemoryLocation::GpuToCpu)
            {
                return nullptr; // GpuOnly has no host view
            }

            // Genuine readback mapping: async map and pump (see :api for why there is
            // no WaitAny). A heap record, orphaned on timeout: a timed-out pump leaves
            // the callback registered, and a stack record would be a dead frame when a
            // later ProcessEvents finally delivers it.
            struct Result
            {
                IAllocator* allocator = nullptr;
                bool done = false;
                bool mapped = false;
                bool orphaned = false; // waiter gave up; the callback owns deletion
            };
            // Async map-callback box (captureless C callback; it may outlive the buffer
            // through orphaning), so it is a process-scope allocation.
            auto* result = DefaultAllocator().New<Result>();
            result->allocator = &DefaultAllocator();
            WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowProcessEvents;
            callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1,
                                   void*)
            {
                auto* r = static_cast<Result*>(userdata1);
                if (r->orphaned)
                {
                    r->allocator->Delete(r);
                    return;
                }
                r->mapped = status == WGPUMapAsyncStatus_Success;
                r->done = true;
            };
            callback.userdata1 = result;
            (void)m_api->wgpuBufferMapAsync(m_buffer, WGPUMapMode_Read, 0,
                                            static_cast<usize>(AlignedSize()), callback);
            m_api->PumpUntilWithDevice(m_instance, m_device, result->done);
            if (!result->done)
            {
                // The request is still pending: a pending map keeps the buffer in the
                // "mapped" state and later submissions touching it will fail. Logged as
                // an error because the caller only sees nullptr.
                LogError("[webgpu] Buffer::Map timed out with the map request PENDING");
                result->orphaned = true; // the callback owns the record now
                return nullptr;
            }
            const bool mapped = result->mapped;
            DefaultAllocator().Delete(result);
            if (!mapped)
            {
                LogError("[webgpu] Buffer::Map failed (MapAsync error)");
                return nullptr;
            }
            m_readMapped = true;
            // Readback is a read-only view; the RHI contract hands out void*. Callers
            // reading through it are fine; writes would be lost.
            return const_cast<void*>(m_api->wgpuBufferGetConstMappedRange(
                m_buffer, 0, static_cast<usize>(AlignedSize())));
        }

        void Unmap() override
        {
            if (!m_shadow.IsEmpty())
            {
                // A mapping that flushed its writes by range has nothing left to send: the
                // whole-shadow compare would only re-read every byte to learn that.
                if (!m_rangeFlushed)
                {
                    UploadShadowIfChanged();
                }
                m_shadowOutstanding = false; // paired callers pay exactly one upload
                m_rangeFlushed = false;
                return;
            }
            if (m_readMapped)
            {
                m_api->wgpuBufferUnmap(m_buffer);
                m_readMapped = false;
            }
        }

        /// The ranged half of the emulation: upload just [offset, offset+size) of the shadow
        /// (4-byte aligned outward, as WriteBuffer requires), skipping it when the GPU already
        /// holds those bytes. Marks the mapping as range-flushed, so neither Unmap nor the
        /// submit hook re-walks the whole shadow: the caller owns every write from here.
        void FlushRange(u64 offset, u64 size) override
        {
            if (m_shadow.IsEmpty() || size == 0)
            {
                return;
            }
            const u64 total = static_cast<u64>(m_shadow.Size());
            u64 begin = offset & ~3ull;
            u64 end = (offset + size + 3ull) & ~3ull;
            if (begin >= total)
            {
                return;
            }
            end = end > total ? total : end;
            const usize first = static_cast<usize>(begin);
            const usize count = static_cast<usize>(end - begin);
            if (m_lastUploaded.Size() != m_shadow.Size())
            {
                // First upload through this buffer: the GPU side is zero-initialized, and so is
                // an untouched shadow, so a zeroed ledger is an accurate picture of it.
                m_lastUploaded.Resize(m_shadow.Size());
            }
            m_rangeFlushed = true;
            m_shadowOutstanding = false;
            if (MemCompare(m_lastUploaded.Data() + first, m_shadow.Data() + first, count) == 0)
            {
                return;
            }
            m_api->wgpuQueueWriteBuffer(m_queue, m_buffer, begin, m_shadow.Data() + first, count);
            MemCopy(m_lastUploaded.Data() + first, m_shadow.Data() + first, count);
            ++m_uploadCount;
        }

        /// Queue-submit hook: re-upload the shadow while a mapping is left open
        /// (the persistent-coherent emulation).
        void FlushShadowIfOutstanding()
        {
            if (m_shadowOutstanding)
            {
                UploadShadowIfChanged();
            }
        }

        /// Actual wgpuQueueWriteBuffer uploads this buffer has issued. Redundant,
        /// byte-identical flushes are skipped and not counted. The RHI tests read it to
        /// prove the skip fires.
        [[nodiscard]] u64 UploadCount() const { return m_uploadCount; }

        void Release()
        {
            if (m_buffer != nullptr)
            {
                m_api->wgpuBufferRelease(m_buffer);
                m_buffer = nullptr;
            }
        }

        [[nodiscard]] WGPUBuffer Handle() const { return m_buffer; }

    private:
        [[nodiscard]] u64 AlignedSize() const { return (desc.size + 3ull) & ~3ull; }

        /// Send the shadow to the GPU unless the GPU already holds these exact bytes.
        /// The native compare is cheap; the skipped wgpuQueueWriteBuffer is not (it
        /// crosses the wasm->JS boundary on web). See the file header for why.
        void UploadShadowIfChanged()
        {
            if (m_lastUploaded.Size() == m_shadow.Size() &&
                MemCompare(m_lastUploaded.Data(), m_shadow.Data(), m_shadow.Size()) == 0)
            {
                return;
            }
            m_api->wgpuQueueWriteBuffer(m_queue, m_buffer, 0, m_shadow.Data(),
                                        m_shadow.Size());
            m_lastUploaded.Resize(m_shadow.Size());
            MemCopy(m_lastUploaded.Data(), m_shadow.Data(), m_shadow.Size());
            ++m_uploadCount;
        }

        const WebGpuApi* m_api = nullptr;
        WGPUInstance m_instance = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUQueue m_queue = nullptr;
        WGPUBuffer m_buffer = nullptr;
        Array<u8> m_shadow;
        Array<u8> m_lastUploaded; // the bytes last uploaded; the flush skips when unchanged
        bool m_shadowOutstanding = false;
        bool m_rangeFlushed = false; // this mapping's writes went through FlushRange
        bool m_readMapped = false;
        u64 m_uploadCount = 0;
    };

    /// The device's ledger of live shadow-backed buffers, walked by the queue before
    /// every submit to flush open (persistently mapped) shadows. Single-threaded by
    /// the same contract as the rest of the backend.
    class WebGpuBufferRegistry final
    {
    public:
        void Add(WebGpuBuffer* buffer) { m_buffers.PushBack(buffer); }

        void Remove(WebGpuBuffer* buffer)
        {
            for (usize i = 0; i < m_buffers.Size(); ++i)
            {
                if (m_buffers[i] == buffer)
                {
                    m_buffers.RemoveAtSwap(i);
                    return;
                }
            }
        }

        void FlushOutstanding()
        {
            for (WebGpuBuffer* buffer : m_buffers)
            {
                buffer->FlushShadowIfOutstanding();
            }
        }

    private:
        Array<WebGpuBuffer*> m_buffers;
    };
}

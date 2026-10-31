// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:resources` partition.
///
/// GPU resource primitives shared by the renderers. `DynamicUniformRing` is a
/// frames-in-flight ring of fixed-stride slots over one buffer (a fixed-stride ring, not a
/// grow-the-buffer UBO). The buffer is partitioned into `framesInFlight` equal
/// regions; each frame writes only its own region (selected by the device ring index), so the
/// CPU never overwrites data the GPU is still reading for a frame in flight.
///
/// It serves three roles by varying usage and stride: per-view and per-object dynamic-offset
/// uniform data (bound with the returned byte offset), per-instance storage data (a
/// StructuredBuffer bound whole and indexed by the returned absolute slot index), and the
/// per-instance vertex offsets stream (bound with the returned byte offset). The ring grows
/// by reallocating (rarely: only when scene complexity exceeds the current per-frame capacity),
/// draining the GPU first so no in-flight frame references the old buffer; each (re)allocation
/// bumps a generation so a consumer can rebuild the bind group it created over `Buffer()`.

module;
#include "Core/Prelude.h"

export module foundation.render:resources;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    /// Probe the device for a single-sampled stencil-capable depth-stencil format, in
    /// the same candidate order as the VG renderer's PickStencilCapableFormat (keep the
    /// two in sync): the overlay passes attach the format this returns, and the UI
    /// subsystem records stencil fills only when its own probe agrees. Undefined means no
    /// stencil support (overlay UI falls back to tessellated fills).
    [[nodiscard]] inline rhi::TextureFormat PickStencilFormat(rhi::Device& device)
    {
        const rhi::TextureFormat candidates[3] = {rhi::TextureFormat::Depth24PlusStencil8,
                                                  rhi::TextureFormat::Depth32FloatStencil8,
                                                  rhi::TextureFormat::Stencil8};
        for (rhi::TextureFormat format : candidates)
        {
            rhi::TextureDesc desc{};
            desc.dimension = rhi::TextureDimension::Texture2D;
            desc.format = format;
            desc.width = 4;
            desc.height = 4;
            desc.depth = 1;
            desc.usage = rhi::TextureUsage::DepthStencil;
            desc.sampleCount = 1;
            rhi::Texture* probe = nullptr;
            if (device.CreateTexture(desc, probe).IsOk() && probe != nullptr)
            {
                device.DestroyTexture(probe);
                return format;
            }
        }
        return rhi::TextureFormat::Undefined;
    }

    /// Frames-in-flight deferred GPU destruction: the web-safe alternative to a grow-path
    /// `WaitIdle` (on web the wait pumps the browser event loop mid-frame, which expires
    /// the canvas texture and drops the whole frame's submit). A replaced resource is
    /// retired instead: it stays alive until every frame that could reference it has aged
    /// out, then frees on Tick().
    /// One queue per owner (the render subsystem shares one across its systems);
    /// consumers fall back to WaitIdle when no queue is wired, so standalone and test use
    /// keeps working.
    class GpuRetireQueue
    {
    public:
        void Initialize(rhi::Device* device, i32 framesInFlight)
        {
            m_device = device;
            // +1: an entry retired during frame N is safe once N's whole ring has cycled.
            m_age = Max(framesInFlight, 1) + 1;
        }

        void Retire(rhi::Buffer* buffer)
        {
            if (buffer != nullptr)
            {
                m_entries.PushBack(Entry{nullptr, nullptr, nullptr, buffer, m_age});
            }
        }
        void Retire(rhi::Texture* texture)
        {
            if (texture != nullptr)
            {
                m_entries.PushBack(Entry{texture, nullptr, nullptr, nullptr, m_age});
            }
        }
        void Retire(rhi::TextureView* view)
        {
            if (view != nullptr)
            {
                m_entries.PushBack(Entry{nullptr, view, nullptr, nullptr, m_age});
            }
        }
        void Retire(rhi::BindGroup* bindGroup)
        {
            if (bindGroup != nullptr)
            {
                m_entries.PushBack(Entry{nullptr, nullptr, bindGroup, nullptr, m_age});
            }
        }

        /// Age everything one frame; free what has outlived every in-flight frame.
        /// Call once per frame, before or after the frame's submits; the +1 margin
        /// covers either placement.
        void Tick()
        {
            if (m_device == nullptr)
            {
                return;
            }
            for (usize i = m_entries.Size(); i-- > 0;)
            {
                Entry& entry = m_entries[i];
                if (--entry.framesLeft > 0)
                {
                    continue;
                }
                Free(entry);
                m_entries.RemoveAt(i);
            }
        }

        /// Destroy everything now (shutdown; the caller has idled the GPU).
        void Flush()
        {
            if (m_device == nullptr)
            {
                m_entries.Clear();
                return;
            }
            for (Entry& entry : m_entries)
            {
                Free(entry);
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize PendingCount() const { return m_entries.Size(); }

    private:
        struct Entry
        {
            rhi::Texture* texture = nullptr;
            rhi::TextureView* view = nullptr;
            rhi::BindGroup* bindGroup = nullptr;
            rhi::Buffer* buffer = nullptr;
            i32 framesLeft = 0;
        };

        void Free(Entry& entry)
        {
            // Views before their textures; bind groups before the buffers they reference.
            if (entry.view != nullptr)
            {
                m_device->DestroyTextureView(entry.view);
            }
            if (entry.texture != nullptr)
            {
                m_device->DestroyTexture(entry.texture);
            }
            if (entry.bindGroup != nullptr)
            {
                m_device->DestroyBindGroup(entry.bindGroup);
            }
            if (entry.buffer != nullptr)
            {
                m_device->DestroyBuffer(entry.buffer);
            }
        }

        rhi::Device* m_device = nullptr;
        i32 m_age = 3;
        Array<Entry> m_entries;
    };


    // A chunked GPU buffer sub-allocator: hands out (buffer, offset) ranges from large shared
    // chunks instead of one buffer per allocation (no per-mesh buffers). Allocations are
    // bump-forward and persist until Clear(); the pool grows by adding a new chunk (existing
    // allocations keep their buffer, so growth never invalidates a live range). Used for mesh
    // vertex and index streams (uploaded once, kept for the resource's lifetime).
    class GpuBufferPool
    {
    public:
        GpuBufferPool(rhi::Device& device, rhi::BufferUsage usage, u64 chunkSize,
                      const char8_t* label) noexcept
            : m_device(&device), m_usage(usage), m_chunkSize(chunkSize), m_label(label)
        {
        }

        ~GpuBufferPool() { Clear(); }

        GpuBufferPool(const GpuBufferPool&) = delete;
        GpuBufferPool& operator=(const GpuBufferPool&) = delete;

        struct Alloc
        {
            rhi::Buffer* buffer = nullptr;
            u64 offset = 0;
            bool ok = false;
        };

        // Sub-allocate `size` bytes aligned to `alignment` from the current chunk, growing (a new
        // chunk) if it doesn't fit. Returns the chunk buffer and the byte offset within it.
        [[nodiscard]] Alloc Allocate(u64 size, u64 alignment)
        {
            if (size == 0)
            {
                return Alloc{};
            }
            if (!m_chunks.IsEmpty())
            {
                Chunk& c = m_chunks[m_current];
                const u64 aligned = AlignUp(c.used, alignment);
                if (aligned + size <= c.size)
                {
                    c.used = aligned + size;
                    return Alloc{c.buffer, aligned, true};
                }
            }
            if (!AddChunk(size > m_chunkSize ? size : m_chunkSize))
            {
                return Alloc{};
            }
            Chunk& c = m_chunks[m_current];
            c.used = size;
            return Alloc{c.buffer, 0, true};
        }

        void Clear()
        {
            for (Chunk& c : m_chunks)
            {
                if (c.buffer)
                {
                    m_device->DestroyBuffer(c.buffer);
                }
            }
            m_chunks.Clear();
            m_current = 0;
        }

        [[nodiscard]] usize ChunkCount() const noexcept { return m_chunks.Size(); }

    private:
        struct Chunk
        {
            rhi::Buffer* buffer = nullptr;
            u64 used = 0;
            u64 size = 0;
        };

        bool AddChunk(u64 size)
        {
            rhi::BufferDesc bd{};
            bd.size = size;
            bd.usage = m_usage;
            bd.memory = rhi::MemoryLocation::GpuOnly;
            bd.label = m_label;
            rhi::Buffer* buffer = nullptr;
            if (!m_device->CreateBuffer(bd, buffer).IsOk())
            {
                return false;
            }
            m_chunks.PushBack(Chunk{buffer, 0, size});
            m_current = m_chunks.Size() - 1;
            return true;
        }

        rhi::Device* m_device;
        rhi::BufferUsage m_usage;
        u64 m_chunkSize;
        const char8_t* m_label;
        Array<Chunk> m_chunks;
        usize m_current = 0;
    };

    class DynamicUniformRing
    {
    public:
        // `slotSize` is the per-allocation stride (256-aligned for dynamic-offset uniforms; the
        // natural struct size for a storage ring). `usage` selects the buffer role. `framesInFlight`
        // is the device ring depth (>= 1).
        DynamicUniformRing(rhi::Device& device, u32 framesInFlight, u64 slotSize,
                           rhi::BufferUsage usage = rhi::BufferUsage::Uniform |
                                                    rhi::BufferUsage::CopyDst,
                           const char8_t* label = u8"ring") noexcept
            : m_device(&device), m_usage(usage), m_label(label),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight), m_slotSize(slotSize)
        {
        }

        ~DynamicUniformRing() { Release(); }

        DynamicUniformRing(const DynamicUniformRing&) = delete;
        DynamicUniformRing& operator=(const DynamicUniformRing&) = delete;

        // Ensure each frame region holds at least `slotsPerFrame` slots. Grows by reallocating
        // (draining the GPU first); never shrinks. Returns false if the buffer can't be created.
        bool Reserve(u32 slotsPerFrame)
        {
            if (slotsPerFrame <= m_slotsPerFrame && m_buffer != nullptr)
            {
                return true;
            }
            if (slotsPerFrame == 0)
            {
                slotsPerFrame = 1;
            }

            // An in-flight frame may still reference the old buffer: retire it when a
            // queue is wired (web-safe), otherwise drain (standalone and test use).
            if (m_retire != nullptr)
            {
                m_retire->Retire(m_buffer);
                m_buffer = nullptr;
            }
            else
            {
                m_device->WaitIdle();
            }
            Release();

            rhi::BufferDesc bd{};
            bd.size =
                static_cast<u64>(m_framesInFlight) * static_cast<u64>(slotsPerFrame) * m_slotSize;
            bd.usage = m_usage;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = m_label;
            if (!m_device->CreateBuffer(bd, m_buffer).IsOk())
            {
                m_buffer = nullptr;
                return false;
            }

            m_slotsPerFrame = slotsPerFrame;
            ++m_generation;
            return true;
        }

        // Begin a frame: select the device ring slot's region. The buffer is mapped lazily by
        // the first allocation, so a ring nothing writes this frame never maps, flushes or
        // compares anything. On a backend that emulates mapping with a CPU shadow (WebGPU)
        // an unconditional map and unmap costs a full-buffer compare per ring per frame, even
        // in a scene that uses none of the skinning, terrain, sprite or particle rings.
        void BeginFrame(u32 frameIndex)
        {
            m_frameBase = static_cast<u32>(frameIndex % m_framesInFlight) * m_slotsPerFrame;
            m_cursor = 0;
            m_mapped = nullptr;
            m_inFrame = true;
        }

        // A run of `count` contiguous slots in this frame's region. `slotIndex` is the absolute
        // index of the first slot (for StructuredBuffer indexing); `byteOffset` is its byte offset
        // (for dynamic-offset uniform binding or the SetVertexBuffer offset); `ptr` is writable for the
        // whole run. ok=false if the region is exhausted (it never silently grows mid-frame).
        struct Range
        {
            u32 slotIndex = 0;
            u32 byteOffset = 0;
            void* ptr = nullptr;
            bool ok = false;
        };

        [[nodiscard]] Range AllocateRange(u32 count)
        {
            if (!m_inFrame || m_buffer == nullptr || count == 0 ||
                m_cursor + count > m_slotsPerFrame)
            {
                return Range{};
            }
            if (m_mapped == nullptr)
            {
                m_mapped = static_cast<u8*>(m_buffer->Map()); // first write this frame
                if (m_mapped == nullptr)
                {
                    return Range{};
                }
            }
            const u32 slot = m_frameBase + m_cursor;
            m_cursor += count;
            const u64 byteOffset = static_cast<u64>(slot) * m_slotSize;
            return Range{slot, static_cast<u32>(byteOffset), m_mapped + byteOffset, true};
        }

        [[nodiscard]] Range Allocate() { return AllocateRange(1); }

        // End a frame: flush exactly the slots this frame wrote (a no-op on a coherent
        // mapping; the emulated one uploads that window and nothing else), then unmap.
        void EndFrame()
        {
            if (m_mapped != nullptr && m_buffer != nullptr)
            {
                if (m_cursor > 0)
                {
                    m_buffer->FlushRange(static_cast<u64>(m_frameBase) * m_slotSize,
                                         static_cast<u64>(m_cursor) * m_slotSize);
                }
                m_buffer->Unmap();
            }
            m_mapped = nullptr;
            m_inFrame = false;
        }

        /// Slots allocated so far this frame (0 outside a frame or when untouched).
        [[nodiscard]] u32 FrameAllocatedSlots() const noexcept { return m_cursor; }
        /// Whether this frame's first allocation has mapped the buffer yet.
        [[nodiscard]] bool IsMappedThisFrame() const noexcept { return m_mapped != nullptr; }

        [[nodiscard]] rhi::Buffer* Buffer() const noexcept { return m_buffer; }
        [[nodiscard]] u64 SlotSize() const noexcept { return m_slotSize; }
        [[nodiscard]] u32 SlotsPerFrame() const noexcept { return m_slotsPerFrame; }
        [[nodiscard]] u64 ByteCapacity() const noexcept
        {
            return static_cast<u64>(m_framesInFlight) * static_cast<u64>(m_slotsPerFrame) *
                   m_slotSize;
        }
        [[nodiscard]] u32 Generation() const noexcept { return m_generation; } // bumps on realloc

        /// Wire the frames-in-flight retire queue (web-safe grows). Null means drain on grow.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept { m_retire = retire; }

    private:
        void Release()
        {
            if (m_buffer != nullptr)
            {
                m_device->DestroyBuffer(m_buffer);
                m_buffer = nullptr;
            }
            m_mapped = nullptr;
        }

        rhi::Device* m_device;
        rhi::BufferUsage m_usage;
        const char8_t* m_label;
        GpuRetireQueue* m_retire = nullptr; // borrowed; null means WaitIdle on grow
        rhi::Buffer* m_buffer = nullptr;
        u8* m_mapped = nullptr;
        u32 m_framesInFlight;
        u64 m_slotSize;
        u32 m_slotsPerFrame = 0;
        u32 m_frameBase = 0;
        u32 m_cursor = 0;
        u32 m_generation = 0;
        bool m_inFrame = false;
    };

} // namespace foundation::render

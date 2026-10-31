// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - :profiler partition
//
// The render graph's half of the profiler. Each executed pass is timed by
// a pair of GPU timestamps and, where the device counts them, a pipeline-statistics query, into
// query sets of the frame's slot; when the slot comes round (the GPU finished that frame), its
// results are put on the CPU's clock and handed to the profiler as that frame's GPU passes.
// Nothing is measured, nor any query written, while the profiler is not armed.

module;
#include "Core/Prelude.h"

export module foundation.rendergraph:profiler;

import foundation.core;
import foundation.rhi;
import foundation.profiler;

using namespace foundation::core;

export namespace foundation::rendergraph
{
    namespace rhi = foundation::rhi;
    namespace profiler = foundation::profiler;

    /// Pass names as the profiler's ids, interned once each: a graph names the same passes every
    /// frame. Frame thread only (it reads the profiler's name table).
    class ProfileNames
    {
    public:
        explicit ProfileNames(IAllocator& allocator) noexcept : m_ids(allocator) {}

        [[nodiscard]] profiler::NameId NameFor(StringView name)
        {
            profiler::Profiler& p = profiler::Profiler::Get();
            const u64 key = HashBytes(name.Data(), name.Size());
            if (const profiler::NameId* known = m_ids.Find(key); known != nullptr && p.NameOf(*known) == name)
            {
                return *known;
            }
            const profiler::NameId id = p.InternName(name);
            m_ids.InsertOrAssign(key, id);
            return id;
        }

    private:
        HashMap<u64, profiler::NameId> m_ids;
    };

    class GraphProfiler
    {
    public:
        static constexpr u32 kMaxPasses = 256; // passes timed in one frame of one graph

        explicit GraphProfiler(IAllocator& allocator) noexcept : m_allocator(&allocator), m_slots(allocator) {}
        ~GraphProfiler() { Destroy(); }
        GraphProfiler(const GraphProfiler&) = delete;
        GraphProfiler& operator=(const GraphProfiler&) = delete;

        /// Make the query sets and readbacks for `slots` frames in flight. Needs a device with
        /// timestamps (Features::timestampQueries); statistics where it has them.
        [[nodiscard]] Status Init(rhi::Device& device, u32 slots)
        {
            if (!device.features.timestampQueries || slots == 0)
            {
                return Status{ErrorCode::NotSupported};
            }
            m_device = &device;
            m_queue = device.GetQueue(rhi::QueueType::Graphics);
            if (m_queue == nullptr)
            {
                return Status{ErrorCode::NotSupported};
            }
            m_statisticsSize = device.features.pipelineStatisticsQueries
                                   ? device.QueryResultSize(rhi::QueryType::PipelineStatistics)
                                   : 0u;
            for (u32 i = 0; i < slots; ++i)
            {
                m_slots.PushBack(Slot(*m_allocator));
                Slot& slot = m_slots[m_slots.Size() - 1];
                rhi::QuerySetDesc qd{};
                qd.type = rhi::QueryType::Timestamp;
                qd.count = kMaxPasses * 2;
                qd.label = u8"RG_Profiler_Timestamps";
                if (!device.CreateQuerySet(qd, slot.timestamps).IsOk() ||
                    !MakeReadback(static_cast<u64>(kMaxPasses) * 2u * sizeof(u64), slot.timestampReadback))
                {
                    Destroy();
                    return Status{ErrorCode::Unknown};
                }
                if (m_statisticsSize > 0)
                {
                    qd.type = rhi::QueryType::PipelineStatistics;
                    qd.count = kMaxPasses;
                    qd.label = u8"RG_Profiler_Statistics";
                    if (!device.CreateQuerySet(qd, slot.statistics).IsOk() ||
                        !MakeReadback(static_cast<u64>(kMaxPasses) * m_statisticsSize, slot.statisticsReadback))
                    {
                        Destroy();
                        return Status{ErrorCode::Unknown};
                    }
                }
            }
            m_period = static_cast<f64>(m_queue->TimestampPeriod());
            return Status{};
        }

        /// Start a frame in `slot`: first hand the profiler what the slot's last frame measured
        /// (its GPU work is done by now), then, when the profiler is armed, start measuring this
        /// one. False when nothing is measured this frame.
        bool BeginFrame(rhi::CommandEncoder& encoder, u32 slot)
        {
            if (m_slots.IsEmpty())
            {
                return false;
            }
            m_current = &m_slots[slot % m_slots.Size()];
            Collect(*m_current);
            profiler::Profiler& p = profiler::Profiler::Get();
            if (!p.Armed() || !Calibrate())
            {
                m_current = nullptr;
                return false;
            }
            m_current->frameNumber = p.CurrentFrameNumber();
            m_current->names.Clear();
            encoder.ResetQuerySet(m_current->timestamps, 0, kMaxPasses * 2);
            if (m_current->statistics != nullptr)
            {
                encoder.ResetQuerySet(m_current->statistics, 0, kMaxPasses);
            }
            return true;
        }

        /// Around a pass's work (barriers included), outside any render pass.
        void BeginPass(rhi::CommandEncoder& encoder, profiler::NameId name)
        {
            if (m_current == nullptr || m_current->names.Size() >= kMaxPasses)
            {
                return;
            }
            const u32 index = static_cast<u32>(m_current->names.Size());
            m_current->names.PushBack(name);
            encoder.WriteTimestamp(m_current->timestamps, index * 2);
            if (m_current->statistics != nullptr)
            {
                encoder.BeginPipelineStatisticsQuery(m_current->statistics, index);
            }
            m_inPass = true;
        }
        void EndPass(rhi::CommandEncoder& encoder)
        {
            if (m_current == nullptr || !m_inPass)
            {
                return;
            }
            const u32 index = static_cast<u32>(m_current->names.Size()) - 1;
            if (m_current->statistics != nullptr)
            {
                encoder.EndPipelineStatisticsQuery(m_current->statistics, index);
            }
            encoder.WriteTimestamp(m_current->timestamps, index * 2 + 1);
            m_inPass = false;
        }

        /// After the frame's last pass: copy the results out, to be read when the slot comes round.
        void EndFrame(rhi::CommandEncoder& encoder)
        {
            if (m_current == nullptr)
            {
                return;
            }
            const u32 count = static_cast<u32>(m_current->names.Size());
            if (count > 0)
            {
                encoder.ResolveQuerySet(m_current->timestamps, 0, count * 2, m_current->timestampReadback, 0);
                if (m_current->statistics != nullptr)
                {
                    encoder.ResolveQuerySet(m_current->statistics, 0, count, m_current->statisticsReadback, 0);
                }
                m_current->pending = true;
            }
            m_current = nullptr;
        }

        void Destroy()
        {
            if (m_device != nullptr)
            {
                for (Slot& slot : m_slots)
                {
                    if (slot.timestamps != nullptr)
                    {
                        m_device->DestroyQuerySet(slot.timestamps);
                    }
                    if (slot.statistics != nullptr)
                    {
                        m_device->DestroyQuerySet(slot.statistics);
                    }
                    if (slot.timestampReadback != nullptr)
                    {
                        m_device->DestroyBuffer(slot.timestampReadback);
                    }
                    if (slot.statisticsReadback != nullptr)
                    {
                        m_device->DestroyBuffer(slot.statisticsReadback);
                    }
                }
            }
            m_slots.Clear();
            m_current = nullptr;
            m_device = nullptr;
        }

    private:
        struct Slot
        {
            rhi::QuerySet* timestamps = nullptr;
            rhi::QuerySet* statistics = nullptr;
            rhi::Buffer* timestampReadback = nullptr;
            rhi::Buffer* statisticsReadback = nullptr;
            Array<profiler::NameId> names; // the passes measured, in order
            u64 frameNumber = 0;
            bool pending = false;

            explicit Slot(IAllocator& allocator) noexcept : names(allocator) {}
        };

        [[nodiscard]] bool MakeReadback(u64 size, rhi::Buffer*& out)
        {
            rhi::BufferDesc bd{};
            bd.size = size;
            bd.usage = rhi::BufferUsage::CopyDst;
            bd.memory = rhi::MemoryLocation::GpuToCpu;
            bd.label = u8"RG_Profiler_Readback";
            return m_device->CreateBuffer(bd, out).IsOk();
        }

        // Keep the GPU-to-CPU clock mapping fresh: a calibrated pair once a second where the
        // device reads both clocks together (drift corrected as it goes), else one sync point.
        [[nodiscard]] bool Calibrate()
        {
            const u64 now = GetTicks();
            if (m_device->features.timestampCalibration)
            {
                if (m_calibrated && now - m_lastCalibration < GetTickFrequency())
                {
                    return true;
                }
                u64 gpu = 0, cpu = 0;
                if (m_queue->CalibrateTimestamps(gpu, cpu))
                {
                    m_gpuOrigin = gpu;
                    m_cpuOrigin = cpu;
                    m_lastCalibration = now;
                    m_calibrated = true;
                }
                return m_calibrated;
            }
            if (!m_calibrated)
            {
                m_calibrated = SyncPoint();
            }
            return m_calibrated;
        }

        // Without calibrated timestamps: let the queue go idle, then write one timestamp and wait
        // for it; it ran between the submit and the wait's end, so its CPU time is the middle of
        // the two (off by half that round trip at most). Once, when profiling first starts.
        [[nodiscard]] bool SyncPoint()
        {
            rhi::QuerySet* set = nullptr;
            rhi::Buffer* readback = nullptr;
            rhi::CommandPool* pool = nullptr;
            rhi::Fence* fence = nullptr;
            rhi::QuerySetDesc qd{};
            qd.type = rhi::QueryType::Timestamp;
            qd.count = 1;
            qd.label = u8"RG_Profiler_Sync";
            bool ok = m_device->CreateQuerySet(qd, set).IsOk() && MakeReadback(sizeof(u64), readback) &&
                      m_device->CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk() &&
                      m_device->CreateFence(0, fence).IsOk();
            rhi::CommandEncoder* encoder = nullptr;
            ok = ok && pool->CreateEncoder(encoder).IsOk();
            if (ok)
            {
                encoder->ResetQuerySet(set, 0, 1);
                encoder->WriteTimestamp(set, 0);
                encoder->ResolveQuerySet(set, 0, 1, readback, 0);
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                ok = commandBuffer != nullptr;
                if (ok)
                {
                    m_queue->WaitIdle();
                    const u64 before = GetTicks();
                    rhi::CommandBuffer* buffers[] = {commandBuffer};
                    m_queue->Submit(Span<rhi::CommandBuffer* const>(buffers, 1), fence, 1);
                    ok = fence->Wait(1, ~0ull);
                    const u64 after = GetTicks();
                    const u64* mapped = ok ? static_cast<const u64*>(readback->Map()) : nullptr;
                    ok = mapped != nullptr && mapped[0] != 0;
                    if (ok)
                    {
                        m_gpuOrigin = mapped[0];
                        m_cpuOrigin = before + (after - before) / 2;
                    }
                    if (mapped != nullptr)
                    {
                        readback->Unmap();
                    }
                }
                pool->DestroyEncoder(encoder);
            }
            if (fence != nullptr)
            {
                m_device->DestroyFence(fence);
            }
            if (pool != nullptr)
            {
                m_device->DestroyCommandPool(pool);
            }
            if (readback != nullptr)
            {
                m_device->DestroyBuffer(readback);
            }
            if (set != nullptr)
            {
                m_device->DestroyQuerySet(set);
            }
            return ok;
        }

        [[nodiscard]] u64 ToCpuTicks(u64 gpu) const noexcept
        {
            const f64 ns = static_cast<f64>(static_cast<i64>(gpu - m_gpuOrigin)) * m_period;
            const f64 ticks = ns * static_cast<f64>(GetTickFrequency()) / 1.0e9;
            return static_cast<u64>(static_cast<i64>(m_cpuOrigin) + static_cast<i64>(ticks));
        }

        // A slot's last frame, done on the GPU: its passes on the CPU clock, to the profiler.
        void Collect(Slot& slot)
        {
            if (!slot.pending)
            {
                return;
            }
            slot.pending = false;
            const u64* stamps = static_cast<const u64*>(slot.timestampReadback->Map());
            if (stamps == nullptr)
            {
                return;
            }
            const u8* statistics = slot.statistics != nullptr
                                       ? static_cast<const u8*>(slot.statisticsReadback->Map())
                                       : nullptr;
            Array<profiler::GpuPassEvent> passes(*m_allocator);
            for (u32 i = 0; i < slot.names.Size(); ++i)
            {
                const u64 begin = stamps[i * 2];
                const u64 end = stamps[i * 2 + 1];
                if (begin == 0 || end < begin)
                {
                    continue; // not written (or the counter wrapped)
                }
                profiler::GpuPassEvent pass;
                pass.name = slot.names[i];
                pass.beginTick = ToCpuTicks(begin);
                pass.endTick = ToCpuTicks(end);
                if (statistics != nullptr)
                {
                    const rhi::PipelineStatistics s =
                        m_device->DecodePipelineStatistics(statistics + static_cast<usize>(i) * m_statisticsSize);
                    pass.hasCounts = true;
                    pass.counts.inputVertices = s.inputVertices;
                    pass.counts.inputPrimitives = s.inputPrimitives;
                    pass.counts.vertexInvocations = s.vertexInvocations;
                    pass.counts.clippingPrimitives = s.clippingPrimitives;
                    pass.counts.fragmentInvocations = s.fragmentInvocations;
                    pass.counts.computeInvocations = s.computeInvocations;
                }
                passes.PushBack(pass);
            }
            if (statistics != nullptr)
            {
                slot.statisticsReadback->Unmap();
            }
            slot.timestampReadback->Unmap();
            profiler::Profiler::Get().SubmitGpuPasses(
                slot.frameNumber, Span<const profiler::GpuPassEvent>(passes.Data(), passes.Size()));
        }

        IAllocator* m_allocator;
        rhi::Device* m_device = nullptr;
        rhi::Queue* m_queue = nullptr;
        Array<Slot> m_slots;
        Slot* m_current = nullptr; // the slot being measured this frame
        bool m_inPass = false;
        u32 m_statisticsSize = 0;
        f64 m_period = 1.0; // ns per GPU tick
        bool m_calibrated = false;
        u64 m_gpuOrigin = 0;
        u64 m_cpuOrigin = 0;
        u64 m_lastCalibration = 0;
    };
}

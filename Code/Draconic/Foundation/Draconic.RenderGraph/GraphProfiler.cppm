// Draconic::RenderGraph - :profiler partition
//
// Optional GPU profiler: per-pass timing via timestamp queries. BeginPass/EndPass
// write timestamps around each pass; Resolve copies the query results to a
// readback buffer; ReadResults (after a fence wait) maps it and builds a report.
// Ported from Sedulous.RenderGraph (GraphProfiler.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:profiler;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class GraphProfiler
    {
    public:
        bool enabled = true;

        ~GraphProfiler() { Destroy(); }

        GraphProfiler() = default;
        GraphProfiler(const GraphProfiler&) = delete;
        GraphProfiler& operator=(const GraphProfiler&) = delete;

        // Two timestamp queries per pass (begin + end) + a GpuToCpu readback buffer.
        [[nodiscard]] Status Init(rhi::Device& device, i32 maxPasses = 64)
        {
            m_device = &device;
            m_maxPasses = maxPasses;

            rhi::QuerySetDesc queryDesc{};
            queryDesc.type = rhi::QueryType::Timestamp;
            queryDesc.count = static_cast<u32>(maxPasses * 2);
            queryDesc.label = u8"RG_Profiler_Queries";
            if (!device.CreateQuerySet(queryDesc, m_querySet).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::BufferDesc bufDesc{};
            bufDesc.size = static_cast<u64>(maxPasses) * 2u * sizeof(u64);
            bufDesc.usage = rhi::BufferUsage::CopyDst;
            bufDesc.memory = rhi::MemoryLocation::GpuToCpu;
            bufDesc.label = u8"RG_Profiler_Readback";
            if (!device.CreateBuffer(bufDesc, m_readbackBuffer).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            m_passTimesMs.Resize(static_cast<usize>(maxPasses));
            m_initialized = true;
            return Status{};
        }

        // Reset the query set at the START of the frame's encoder - timestamps can only be written
        // into a freshly-reset pool, and the reset must be outside any render pass.
        void BeginFrame(rhi::CommandEncoder& encoder)
        {
            if (!m_initialized || !enabled)
            {
                return;
            }
            encoder.ResetQuerySet(m_querySet, 0, static_cast<u32>(m_maxPasses * 2));
            m_passNames.Clear();
        }

        void BeginPass(rhi::CommandEncoder& encoder, i32 passIndex, StringView passName)
        {
            if (!m_initialized || !enabled || passIndex >= m_maxPasses)
            {
                return;
            }
            while (static_cast<i32>(m_passNames.Size()) <= passIndex)
            {
                m_passNames.PushBack(String{});
            }
            m_passNames[static_cast<usize>(passIndex)] = String(passName);
            encoder.WriteTimestamp(m_querySet, static_cast<u32>(passIndex * 2));
        }

        void EndPass(rhi::CommandEncoder& encoder, i32 passIndex)
        {
            if (!m_initialized || !enabled || passIndex >= m_maxPasses)
            {
                return;
            }
            encoder.WriteTimestamp(m_querySet, static_cast<u32>(passIndex * 2 + 1));
        }

        // Resolve queries into the readback buffer (call after recording all passes). The pool was
        // already reset by BeginFrame, so this only copies the written timestamps out.
        void Resolve(rhi::CommandEncoder& encoder, i32 passCount)
        {
            if (!m_initialized || !enabled || passCount == 0)
            {
                return;
            }
            const u32 queryCount = static_cast<u32>(Min(passCount * 2, m_maxPasses * 2));
            encoder.ResolveQuerySet(m_querySet, 0, queryCount, m_readbackBuffer, 0);
        }

        // Read results and append a timing report (call after the GPU has finished).
        void ReadResults(i32 passCount, String& outReport)
        {
            if (!m_initialized || !enabled || passCount == 0)
            {
                return;
            }

            const u64* mapped = static_cast<const u64*>(m_readbackBuffer->Map());
            if (mapped == nullptr)
            {
                return;
            }

            const i32 count = Min(passCount, m_maxPasses);
            f32 totalMs = 0.0f;

            outReport.Append(u8"=== GPU Pass Timing ===\n");
            for (i32 i = 0; i < count; ++i)
            {
                const u64 begin = mapped[i * 2];
                const u64 end = mapped[i * 2 + 1];
                const u64 ticks = end > begin ? end - begin : 0;
                const f32 ms = static_cast<f32>(ticks) * m_gpuTimestampPeriod / 1000000.0f;
                m_passTimesMs[static_cast<usize>(i)] = ms;
                totalMs += ms;

                const StringView name = i < static_cast<i32>(m_passNames.Size())
                                            ? m_passNames[static_cast<usize>(i)].AsView()
                                            : StringView(u8"???");
                AppendFormat(outReport, u8"  {} ms  {}\n", ms, name);
            }
            AppendFormat(outReport, u8"  --------\n  {} ms  TOTAL\n", totalMs);

            // Aggregate by pass NAME so many same-named passes (e.g. 24x probes.prefilter) read as one
            // line - a "which pass category is expensive" summary, sorted most-expensive first.
            struct Agg
            {
                StringView name;
                f32 sum = 0.0f;
                i32 n = 0;
            };
            Array<Agg> agg;
            for (i32 i = 0; i < count; ++i)
            {
                const StringView nm = i < static_cast<i32>(m_passNames.Size())
                                          ? m_passNames[static_cast<usize>(i)].AsView()
                                          : StringView(u8"???");
                bool found = false;
                for (Agg& a : agg)
                {
                    if (a.name == nm)
                    {
                        a.sum += m_passTimesMs[static_cast<usize>(i)];
                        ++a.n;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    Agg a;
                    a.name = nm;
                    a.sum = m_passTimesMs[static_cast<usize>(i)];
                    a.n = 1;
                    agg.PushBack(a);
                }
            }
            for (usize i = 0; i < agg.Size(); ++i) // selection sort by total (few entries)
                for (usize j = i + 1; j < agg.Size(); ++j)
                    if (agg[j].sum > agg[i].sum)
                    {
                        const Agg t = agg[i];
                        agg[i] = agg[j];
                        agg[j] = t;
                    }
            outReport.Append(u8"=== GPU by pass name (expensive first) ===\n");
            for (const Agg& a : agg)
            {
                AppendFormat(outReport, u8"  {} ms  (x{})  {}\n", a.sum, a.n, a.name);
            }

            m_readbackBuffer->Unmap();
        }

        [[nodiscard]] f32 GetPassTimeMs(i32 passIndex) const
        {
            if (passIndex < 0 || passIndex >= static_cast<i32>(m_passTimesMs.Size()))
            {
                return 0.0f;
            }
            return m_passTimesMs[static_cast<usize>(passIndex)];
        }

        // GPU timestamp period (nanoseconds per tick); backend-specific.
        void SetTimestampPeriod(f32 nanosecondsPerTick) noexcept
        {
            m_gpuTimestampPeriod = nanosecondsPerTick;
        }

        void Destroy()
        {
            if (m_device != nullptr)
            {
                if (m_readbackBuffer != nullptr)
                {
                    m_device->DestroyBuffer(m_readbackBuffer);
                }
                if (m_querySet != nullptr)
                {
                    m_device->DestroyQuerySet(m_querySet);
                }
            }
            m_initialized = false;
        }

    private:
        rhi::Device* m_device = nullptr;
        rhi::QuerySet* m_querySet = nullptr;
        rhi::Buffer* m_readbackBuffer = nullptr;
        i32 m_maxPasses = 0;
        bool m_initialized = false;
        Array<String> m_passNames;
        Array<f32> m_passTimesMs;
        f32 m_gpuTimestampPeriod = 0.0f;
    };
}

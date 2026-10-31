// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Runtime.Client - :device_counters partition
//
// Hands a device's counts to the profiler once a frame: what the frame
// recorded (the differences of the device's running totals: draws, dispatches, passes, binds,
// bytes through transfer batches), the GPU memory its resources hold now, and the driver's own
// budget and use where it gives them, read once a second. The host calls it at each frame's end.

module;
#include "Core/Prelude.h"

export module foundation.runtime.client:device_counters;

import foundation.core;
import foundation.rhi;
import foundation.profiler;

using namespace foundation::core;

export namespace foundation::runtime
{
    namespace rhi = foundation::rhi;
    namespace profiler = foundation::profiler;

    class DeviceCounterPublisher
    {
    public:
        /// Set this frame's counters from `device` (the frame thread, before the frame ends). Keeps
        /// its totals current whether or not the profiler is armed, so an armed frame's numbers
        /// are that frame's.
        void Publish(rhi::Device& device, profiler::Profiler& p)
        {
            if (!m_registered)
            {
                Register(p);
            }
            const rhi::DeviceCounters now = device.Counters();
            if (m_haveLast)
            {
                p.SetCounter(m_draws, Delta(now.draws, m_last.draws));
                p.SetCounter(m_dispatches, Delta(now.dispatches, m_last.dispatches));
                p.SetCounter(m_renderPasses, Delta(now.renderPasses, m_last.renderPasses));
                p.SetCounter(m_computePasses, Delta(now.computePasses, m_last.computePasses));
                p.SetCounter(m_pipelineBinds, Delta(now.pipelineBinds, m_last.pipelineBinds));
                p.SetCounter(m_bindGroupBinds, Delta(now.bindGroupBinds, m_last.bindGroupBinds));
                p.SetCounter(m_transferBytes, Delta(now.transferBytes, m_last.transferBytes));
            }
            p.SetCounter(m_bufferBytes, static_cast<f64>(now.bufferBytes));
            p.SetCounter(m_textureBytes, static_cast<f64>(now.textureBytes));
            p.SetCounter(m_renderTargetBytes, static_cast<f64>(now.renderTargetBytes));
            m_last = now;
            m_haveLast = true;

            const u64 tick = GetTicks();
            if (device.features.memoryBudget && (!m_haveBudget || tick - m_budgetTick >= GetTickFrequency()))
            {
                m_haveBudget = device.MemoryBudget(m_budget, m_used);
                m_budgetTick = tick;
            }
            if (m_haveBudget)
            {
                p.SetCounter(m_budgetId, static_cast<f64>(m_budget));
                p.SetCounter(m_usedId, static_cast<f64>(m_used));
            }
        }

    private:
        void Register(profiler::Profiler& p)
        {
            using U = profiler::CounterUnit;
            m_draws = p.RegisterCounter(StringView(u8"Draws"), U::Count);
            m_dispatches = p.RegisterCounter(StringView(u8"Dispatches"), U::Count);
            m_renderPasses = p.RegisterCounter(StringView(u8"Render passes"), U::Count);
            m_computePasses = p.RegisterCounter(StringView(u8"Compute passes"), U::Count);
            m_pipelineBinds = p.RegisterCounter(StringView(u8"Pipeline binds"), U::Count);
            m_bindGroupBinds = p.RegisterCounter(StringView(u8"Bind group binds"), U::Count);
            m_transferBytes = p.RegisterCounter(StringView(u8"Transfer"), U::Bytes);
            m_bufferBytes = p.RegisterCounter(StringView(u8"Buffer memory"), U::Bytes);
            m_textureBytes = p.RegisterCounter(StringView(u8"Texture memory"), U::Bytes);
            m_renderTargetBytes = p.RegisterCounter(StringView(u8"Render target memory"), U::Bytes);
            m_budgetId = p.RegisterCounter(StringView(u8"GPU memory budget"), U::Bytes);
            m_usedId = p.RegisterCounter(StringView(u8"GPU memory used"), U::Bytes);
            m_registered = true;
        }

        [[nodiscard]] static f64 Delta(u64 now, u64 last) noexcept
        {
            return static_cast<f64>(now >= last ? now - last : 0);
        }

        bool m_registered = false;
        u32 m_draws = 0, m_dispatches = 0, m_renderPasses = 0, m_computePasses = 0;
        u32 m_pipelineBinds = 0, m_bindGroupBinds = 0, m_transferBytes = 0;
        u32 m_bufferBytes = 0, m_textureBytes = 0, m_renderTargetBytes = 0;
        u32 m_budgetId = 0, m_usedId = 0;
        rhi::DeviceCounters m_last{};
        bool m_haveLast = false;
        bool m_haveBudget = false;
        u64 m_budgetTick = 0;
        u64 m_budget = 0;
        u64 m_used = 0;
    };
}

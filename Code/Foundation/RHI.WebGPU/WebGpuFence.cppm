// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:fence - CPU-side timeline fence.
///
/// WebGPU has no fence object. The RHI's timeline contract is emulated CPU-side:
/// Queue::Submit(..., fence, value) registers a wgpuQueueOnSubmittedWorkDone callback
/// that stores `value` here when the GPU passes that submission, and Wait() pumps
/// ProcessEvents until the value arrives. This is safe single-threaded, which is the
/// web threading model.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:fence;

import foundation.core;
import foundation.rhi;
import :api;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuFence;

    /// One queued wgpuQueueOnSubmittedWorkDone signal, owned by the callback that frees it.
    ///
    /// It can be delivered long after the fence already resolved by submission index (Wait's
    /// fast path), including during device teardown, after the fence itself was destroyed. The
    /// record therefore outlives the fence: the fence detaches itself on destruction and the
    /// callback only touches `fence` while it is still attached.
    struct WebGpuPendingSignal
    {
        WebGpuFence* fence = nullptr;
        u64 value = 0;
        IAllocator* allocator = nullptr;
    };

    /// CompletedValue advances only when callbacks get delivered. Wait() pumps;
    /// passive observers see new values after any pump on the same instance.
    class WebGpuFence final : public Fence
    {
    public:
        WebGpuFence(const WebGpuApi& api, WGPUInstance instance, WGPUDevice device,
                    u64 initialValue)
            : m_api(&api), m_instance(instance), m_device(device), m_completed(initialValue)
        {
        }

        ~WebGpuFence() override
        {
            // Undelivered work-done callbacks still point here; sever them so the one that
            // fires during device teardown does not write through a freed fence.
            for (WebGpuPendingSignal* pending : m_pending)
            {
                pending->fence = nullptr;
            }
        }

        /// Registers a queued signal record so this fence can detach it on destruction.
        void AttachPending(WebGpuPendingSignal* pending) { m_pending.PushBack(pending); }

        /// Drops a record the callback is about to free.
        void DetachPending(const WebGpuPendingSignal* pending)
        {
            for (usize i = 0; i < m_pending.Size(); ++i)
            {
                if (m_pending[i] == pending)
                {
                    m_pending.RemoveAt(i);
                    return;
                }
            }
        }

        u64 CompletedValue() override { return m_completed; }

        /// The queue records each fenced submission's wgpu submission index here.
        /// Wait() prefers waiting on that index: DevicePoll(wait, &index) returns when
        /// that submission retires, immune to unrelated queue state (a presented frame
        /// in flight can starve blanket polls on Wayland/FIFO).
        void NoteSubmission(u64 value, WGPUSubmissionIndex submissionIndex)
        {
            // Monotonic values: the latest note supersedes for lower values too.
            m_notedValue = value;
            m_notedIndex = submissionIndex;
            m_hasNote = true;
        }

        bool Wait(u64 value, u64 /*timeoutNs*/) override
        {
            if (m_completed >= value)
            {
                return true;
            }
            if (m_hasNote && m_notedValue >= value && m_api->wgpuDevicePoll != nullptr &&
                m_device != nullptr)
            {
                (void)m_api->wgpuDevicePoll(m_device, 1u, &m_notedIndex);
                SignalFromCallback(m_notedValue);
                m_hasNote = false;
                return true;
            }
            // Work-done callbacks fire on device polls. Polls are non-blocking (wait=1
            // can block forever on an empty queue; see :api), and the iteration guard
            // stands in for a timeout (wgpu-native's timed waits are unimplemented).
            for (u32 i = 0; i < 1000000 && m_completed < value; ++i)
            {
                m_api->wgpuInstanceProcessEvents(m_instance);
                if (m_completed >= value)
                {
                    break;
                }
                if (m_api->wgpuDevicePoll != nullptr && m_device != nullptr)
                {
                    (void)m_api->wgpuDevicePoll(m_device, 0u, nullptr);
                }
                // On web the work-done callback resolves from a browser microtask that
                // cannot run while this loop spins: without yielding, m_completed never
                // advances and the loop burns the full iteration guard walking Dawn's
                // event maps every frame. Yielding returns to the event loop so the
                // callback fires; it is a no-op on desktop, where polls deliver
                // synchronously.
                m_api->YieldToEventLoop();
            }
            return m_completed >= value;
        }

        void SignalFromCallback(u64 value)
        {
            if (value > m_completed)
            {
                m_completed = value;
            }
        }

    private:
        const WebGpuApi* m_api;
        WGPUInstance m_instance;
        WGPUDevice m_device;
        u64 m_completed;
        u64 m_notedValue = 0;
        WGPUSubmissionIndex m_notedIndex{};
        bool m_hasNote = false;
        Array<WebGpuPendingSignal*> m_pending; // queued callbacks still pointing at this fence
    };
}

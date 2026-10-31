// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Validation wrapper for Queue.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:validated_queue;

import foundation.core;
import foundation.rhi;
import :validated_fence;
import :validated_transfer_batch;

using namespace foundation::core;

export namespace foundation::rhi::validation
{

    class ValidatedQueue : public Queue
    {
    public:
        explicit ValidatedQueue(Queue* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
            queueType = inner->queueType;
        }

        void Submit(Span<CommandBuffer* const> cmdBufs) override
        {
            for (usize i = 0; i < cmdBufs.Size(); ++i)
                if (!cmdBufs[i])
                    LogErrorf("[Validation] Queue::submit: commandBuffer[%zu] is null", i);
            m_inner->Submit(cmdBufs);
        }

        void Submit(Span<CommandBuffer* const> cmdBufs, Fence* signalFence,
                    u64 signalValue) override
        {
            if (!signalFence)
            {
                LogError("[Validation] Queue::submit: signalFence is null");
                return;
            }
            auto* vf = static_cast<ValidatedFence*>(signalFence);
            Fence* innerFence = vf ? vf->inner() : signalFence;
            if (vf)
                vf->trackSignal(signalValue);
            m_inner->Submit(cmdBufs, innerFence, signalValue);
        }

        void Submit(Span<CommandBuffer* const> cmdBufs, Span<Fence* const> waitFences,
                    Span<const u64> waitValues, Fence* signalFence, u64 signalValue) override
        {
            if (waitFences.Size() != waitValues.Size())
                LogError("[Validation] Queue::submit: waitFences and waitValues count mismatch");
            // Same contract as the fenced overload above: the signal fence is required (the
            // plain overload is the unsignalled path). The Vulkan backend rejects the submit,
            // so both fenced overloads report it.
            if (!signalFence)
                LogError("[Validation] Queue::submit: signalFence is null");
            // Unwrap validated fences for both wait and signal.
            Array<Fence*> innerWait(waitFences.Size());
            for (usize i = 0; i < waitFences.Size(); ++i)
            {
                auto* vw = static_cast<ValidatedFence*>(waitFences[i]);
                innerWait[i] = vw ? vw->inner() : waitFences[i];
            }
            auto* vf = static_cast<ValidatedFence*>(signalFence);
            Fence* innerSignal = vf ? vf->inner() : signalFence;
            if (vf)
                vf->trackSignal(signalValue);
            m_inner->Submit(cmdBufs, Span<Fence* const>(innerWait.Data(), innerWait.Size()),
                            waitValues, innerSignal, signalValue);
        }

        void WaitIdle() override { m_inner->WaitIdle(); }

        Status CreateTransferBatch(TransferBatch*& out) override
        {
            TransferBatch* innerBatch = nullptr;
            Status r = m_inner->CreateTransferBatch(innerBatch);
            if (r != ErrorCode::Ok || !innerBatch)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedTransferBatch>(innerBatch);
            return ErrorCode::Ok;
        }

        void DestroyTransferBatch(TransferBatch*& batch) override
        {
            if (!batch)
                return;
            auto* vt = static_cast<ValidatedTransferBatch*>(batch);
            if (vt)
            {
                TransferBatch* innerBatch = vt->inner();
                m_inner->DestroyTransferBatch(innerBatch);
                m_allocator.Delete(vt);
            }
            else
            {
                m_inner->DestroyTransferBatch(batch);
            }
            batch = nullptr;
        }

        f32 TimestampPeriod() const override { return m_inner->TimestampPeriod(); }
        bool CalibrateTimestamps(u64& gpuTimestamp, u64& cpuTicks) override
        {
            return m_inner->CalibrateTimestamps(gpuTimestamp, cpuTicks);
        }

        Queue* inner() const { return m_inner; }

    private:
        Queue* m_inner;
        IAllocator& m_allocator;
    };

} // namespace foundation::rhi::validation

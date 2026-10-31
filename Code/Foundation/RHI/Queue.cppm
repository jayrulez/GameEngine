// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Abstract GPU command queue. Handles command submission, fence
/// synchronization, and transfer batch creation.

export module foundation.rhi:queue;

import foundation.core;
import :enums;
import :resources;

using namespace foundation::core;

export namespace foundation::rhi
{

    class TransferBatch;

    class Queue
    {
    public:
        virtual ~Queue() = default;

        /// The type of work this queue supports.
        QueueType queueType = QueueType::Graphics;

        /// Submit command buffers for execution.
        virtual void Submit(Span<CommandBuffer* const> commandBuffers) = 0;

        /// Submit with fence signaling. `signalFence` is required: a fenced overload with a null
        /// fence is rejected (nothing is submitted; the validation layer reports it). Use the
        /// plain overload to submit without signalling.
        virtual void Submit(Span<CommandBuffer* const> commandBuffers, Fence* signalFence,
                            u64 signalValue) = 0;

        /// Submit with full synchronization: wait on fences, then signal. `signalFence` is
        /// required here as well, by the same rule as the fenced overload above.
        virtual void Submit(Span<CommandBuffer* const> commandBuffers,
                            Span<Fence* const> waitFences, Span<const u64> waitValues,
                            Fence* signalFence, u64 signalValue) = 0;

        /// Block until all submitted work on this queue completes.
        virtual void WaitIdle() = 0;

        /// Create a transfer batch for batching CPU to GPU uploads.
        virtual Status CreateTransferBatch(TransferBatch*& out) = 0;
        /// Destroy a transfer batch.
        virtual void DestroyTransferBatch(TransferBatch*& batch) = 0;

        /// Timestamp period in nanoseconds per tick.
        [[nodiscard]] virtual f32 TimestampPeriod() const = 0;

        /// A GPU timestamp (in this queue's timestamp ticks) and core::GetTicks() read at the same
        /// moment, to place the queue's timestamps on the CPU's clock. False, with both left
        /// alone, when the device cannot (Features::timestampCalibration).
        [[nodiscard]] virtual bool CalibrateTimestamps(u64& gpuTimestamp, u64& cpuTicks) = 0;
    };

} // namespace foundation::rhi

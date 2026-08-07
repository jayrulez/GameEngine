/// Abstract GPU command queue. Handles command submission, fence
/// synchronization, and transfer batch creation.

export module draconic.rhi:queue;

import draconic.foundation;
import :enums;
import :resources;

using namespace draconic::foundation;

export namespace draconic::rhi
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

        /// Submit with fence signaling.
        virtual void Submit(Span<CommandBuffer* const> commandBuffers, Fence* signalFence,
                            u64 signalValue) = 0;

        /// Submit with full synchronization: wait on fences, then signal.
        virtual void Submit(Span<CommandBuffer* const> commandBuffers,
                            Span<Fence* const> waitFences, Span<const u64> waitValues,
                            Fence* signalFence, u64 signalValue) = 0;

        /// Block until all submitted work on this queue completes.
        virtual void WaitIdle() = 0;

        /// Create a transfer batch for batching CPU→GPU uploads.
        virtual Status CreateTransferBatch(TransferBatch*& out) = 0;
        /// Destroy a transfer batch.
        virtual void DestroyTransferBatch(TransferBatch*& batch) = 0;

        /// Timestamp period in nanoseconds per tick.
        [[nodiscard]] virtual f32 TimestampPeriod() const = 0;
    };

} // namespace draconic::rhi

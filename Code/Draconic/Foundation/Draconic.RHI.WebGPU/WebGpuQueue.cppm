/// draconic.rhi.webgpu:queue - Queue over the device's single WGPUQueue.
///
/// WebGPU exposes exactly ONE queue per device. The RHI models Graphics/Compute/
/// Transfer queues, so the device hands out three thin wrappers that all funnel into
/// the same WGPUQueue - correct by construction (one timeline) and shape-compatible
/// with callers that ask for a specific queue type. Fence signaling rides
/// wgpuQueueOnSubmittedWorkDone (see :fence for the timeline emulation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:queue;

import draconic.foundation;
import draconic.rhi;
import :api;
import :buffer;
import :command_buffer;
import :transfer_batch;
import :fence;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuQueue final : public Queue
    {
    public:
        WebGpuQueue() = default;

        void Initialize(const WebGpuApi& api, WGPUInstance instance, WGPUDevice device,
                        WGPUQueue queue, IAllocator& allocator, QueueType type,
                        WebGpuBufferRegistry& bufferRegistry)
        {
            m_api = &api;
            m_instance = instance;
            m_device = device;
            m_queue = queue;
            m_allocator = &allocator;
            queueType = type;
            m_bufferRegistry = &bufferRegistry;
        }

        [[nodiscard]] WGPUQueue Handle() const { return m_queue; }

        void Submit(Span<CommandBuffer* const> commandBuffers) override
        {
            SubmitInternal(commandBuffers);
        }

        void Submit(Span<CommandBuffer* const> commandBuffers, Fence* signalFence,
                    u64 signalValue) override
        {
            const WGPUSubmissionIndex index = SubmitInternal(commandBuffers);
            NoteAndSignal(signalFence, signalValue, index);
        }

        void Submit(Span<CommandBuffer* const> commandBuffers, Span<Fence* const> waitFences,
                    Span<const u64> waitValues, Fence* signalFence, u64 signalValue) override
        {
            // One queue, one timeline: anything previously submitted here is already
            // ordered before this submission, so same-queue waits are satisfied by
            // construction. Cross-queue waits cannot exist (there is only one queue) -
            // waiting CPU-side would deadlock nothing but adds latency; the values are
            // still honored for fences signaled by earlier submissions.
            for (usize i = 0; i < waitFences.Size() && i < waitValues.Size(); ++i)
            {
                if (waitFences[i] != nullptr)
                {
                    static_cast<WebGpuFence*>(waitFences[i])->Wait(waitValues[i], 0);
                }
            }
            const WGPUSubmissionIndex index = SubmitInternal(commandBuffers);
            NoteAndSignal(signalFence, signalValue, index);
        }

        void WaitIdle() override
        {
            // Heap record + orphan-on-timeout (the fence-fix pattern): if the pump gives up,
            // the still-registered callback must not write through a dead stack frame.
            struct Result
            {
                IAllocator* allocator = nullptr;
                bool done = false;
                bool orphaned = false; // waiter gave up; the callback owns deletion
            };
            auto* result = m_allocator->New<Result>();
            result->allocator = m_allocator;
            WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
            {
                auto* r = static_cast<Result*>(userdata1);
                if (r->orphaned)
                {
                    r->allocator->Delete(r);
                    return;
                }
                r->done = true;
            };
            info.userdata1 = result;
            (void)m_api->wgpuQueueOnSubmittedWorkDone(m_queue, info);
            m_api->PumpUntilWithDevice(m_instance, m_device, result->done);
            if (!result->done)
            {
                result->orphaned = true; // the callback owns the record now
                return;
            }
            m_allocator->Delete(result);
        }

        // Release `texture` only once every submission queued SO FAR has been consumed -
        // fire-and-forget (the heap record owns itself, the fence-fix lifetime pattern). The web
        // swapchain releases its borrowed surface texture through this: on web wgpuQueueSubmit
        // validates asynchronously, so releasing the borrow on any fixed frame boundary races the
        // drain - under startup load or a resize the not-yet-consumed submit loses its texture
        // ("Destroyed texture used in a submit") and Dawn drops the whole command buffer.
        void ReleaseTextureWhenConsumed(WGPUTexture texture)
        {
            struct Pending
            {
                const WebGpuApi* api = nullptr;
                IAllocator* allocator = nullptr;
                WGPUTexture texture = nullptr;
            };
            auto* pending = m_allocator->New<Pending>();
            pending->api = m_api;
            pending->allocator = m_allocator;
            pending->texture = texture;
            WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
            {
                auto* p = static_cast<Pending*>(userdata1);
                p->api->wgpuTextureRelease(p->texture);
                IAllocator* allocator = p->allocator;
                allocator->Delete(p);
            };
            info.userdata1 = pending;
            (void)m_api->wgpuQueueOnSubmittedWorkDone(m_queue, info);
        }

        Status CreateTransferBatch(TransferBatch*& out) override
        {
            auto* batch = m_allocator->New<WebGpuTransferBatch>();
            batch->Initialize(*m_api, m_instance, m_device, m_queue, *m_allocator);
            out = batch;
            return ErrorCode::Ok;
        }

        void DestroyTransferBatch(TransferBatch*& batch) override
        {
            if (batch != nullptr)
            {
                batch->Destroy(); // self-deleting (owns its allocator)
                batch = nullptr;
            }
        }

        f32 TimestampPeriod() const override
        {
            return m_api->wgpuQueueGetTimestampPeriod != nullptr
                       ? m_api->wgpuQueueGetTimestampPeriod(m_queue)
                       : 1.0f;
        }

    private:
        WGPUSubmissionIndex SubmitInternal(Span<CommandBuffer* const> commandBuffers)
        {
            // Persistent-map coherence: flush every OPEN shadow first (queue-ordered,
            // so the writes land before this submission reads them).
            m_bufferRegistry->FlushOutstanding();

            WGPUCommandBuffer handles[16];
            usize count = 0;
            WGPUSubmissionIndex last{};
            for (CommandBuffer* commandBuffer : commandBuffers)
            {
                // Take() transfers the handle; submission consumes (releases) it.
                const WGPUCommandBuffer handle =
                    static_cast<WebGpuCommandBuffer*>(commandBuffer)->Take();
                if (handle == nullptr)
                {
                    continue; // already submitted or never finished
                }
                if (count == 16)
                {
                    last = SubmitAndRelease(handles, count);
                    count = 0;
                }
                handles[count++] = handle;
            }
            if (count > 0 || commandBuffers.IsEmpty())
            {
                last = SubmitAndRelease(handles, count);
            }
            return last;
        }

        WGPUSubmissionIndex SubmitAndRelease(WGPUCommandBuffer* handles, usize count)
        {
            // SubmitForIndex (wgpu extension) hands back the submission's index so
            // fences can wait on EXACTLY this submission; plain Submit on web.
            WGPUSubmissionIndex index{};
            if (m_api->wgpuQueueSubmitForIndex != nullptr)
            {
                index = m_api->wgpuQueueSubmitForIndex(m_queue, count, handles);
            }
            else
            {
                m_api->wgpuQueueSubmit(m_queue, count, handles);
            }
            for (usize i = 0; i < count; ++i)
            {
                m_api->wgpuCommandBufferRelease(handles[i]);
            }
            return index;
        }

        void NoteAndSignal(Fence* fence, u64 value, WGPUSubmissionIndex index)
        {
            if (fence == nullptr)
            {
                return;
            }
            if (m_api->wgpuQueueSubmitForIndex != nullptr)
            {
                static_cast<WebGpuFence*>(fence)->NoteSubmission(value, index);
            }
            // The record outlives the fence on purpose: Wait's submission-index fast path
            // resolves without this callback, so it can still be queued when the fence is
            // destroyed (and then fire during device teardown). Attaching lets the fence
            // null out `fence` on destruction; the callback frees the record either way.
            auto* pending = m_allocator->New<WebGpuPendingSignal>();
            pending->fence = static_cast<WebGpuFence*>(fence);
            pending->value = value;
            pending->allocator = m_allocator;
            static_cast<WebGpuFence*>(fence)->AttachPending(pending);

            WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            info.mode = WGPUCallbackMode_AllowProcessEvents;
            info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*)
            {
                auto* p = static_cast<WebGpuPendingSignal*>(userdata1);
                if (p->fence != nullptr) // still alive - it detaches us on destruction
                {
                    p->fence->DetachPending(p);
                    p->fence->SignalFromCallback(p->value);
                }
                p->allocator->Delete(p);
            };
            info.userdata1 = pending;
            (void)m_api->wgpuQueueOnSubmittedWorkDone(m_queue, info);
        }

        const WebGpuApi* m_api = nullptr;
        WGPUInstance m_instance = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUQueue m_queue = nullptr;
        IAllocator* m_allocator = nullptr;
        WebGpuBufferRegistry* m_bufferRegistry = nullptr;
    };
}

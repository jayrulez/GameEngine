/// draconic.rhi.webgpu:command_pool - CommandPool over per-encoder allocation.
///
/// WebGPU has no pool object - encoders come straight from the device and are
/// one-shot. The pool is ownership bookkeeping: it allocates encoder wrappers and
/// deletes them at destruction; Reset is a no-op because each wrapper already opens
/// a fresh WGPUCommandEncoder after Finish (see :command_encoder).

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:command_pool;

import draconic.foundation;
import draconic.rhi;
import :api;
import :blit_helper;
import :command_encoder;
import :render_bundle_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuCommandPool final : public CommandPool
    {
    public:
        void Initialize(const WebGpuApi& api, WGPUDevice device, IAllocator& allocator,
                        WebGpuBlitHelper& blitHelper)
        {
            m_api = &api;
            m_device = device;
            m_allocator = &allocator;
            m_blitHelper = &blitHelper;
        }

        Status CreateEncoder(CommandEncoder*& out) override
        {
            auto* encoder = m_allocator->New<WebGpuCommandEncoder>();
            encoder->Initialize(*m_api, m_device, *m_allocator, *m_blitHelper);
            m_encoders.PushBack(encoder);
            out = encoder;
            return ErrorCode::Ok;
        }

        void DestroyEncoder(CommandEncoder*& encoder) override
        {
            if (encoder == nullptr)
            {
                return;
            }
            auto* wrapped = static_cast<WebGpuCommandEncoder*>(encoder);
            for (usize i = 0; i < m_encoders.Size(); ++i)
            {
                if (m_encoders[i] == wrapped)
                {
                    m_encoders.RemoveAtSwap(i);
                    break;
                }
            }
            m_allocator->Delete(wrapped);
            encoder = nullptr;
        }

        void Reset() override
        {
            // One-shot encoders re-open lazily; nothing to recycle.
        }

        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc& bundleDesc) override
        {
            auto* encoder = m_allocator->New<WebGpuRenderBundleEncoder>();
            if (!encoder->Initialize(*m_api, m_device, *m_allocator, bundleDesc).IsOk())
            {
                m_allocator->Delete(encoder);
                return nullptr;
            }
            m_bundleEncoders.PushBack(encoder);
            return encoder;
        }

        ~WebGpuCommandPool() override
        {
            for (WebGpuCommandEncoder* encoder : m_encoders)
            {
                m_allocator->Delete(encoder);
            }
            for (WebGpuRenderBundleEncoder* encoder : m_bundleEncoders)
            {
                m_allocator->Delete(encoder);
            }
        }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUDevice m_device = nullptr;
        IAllocator* m_allocator = nullptr;
        WebGpuBlitHelper* m_blitHelper = nullptr;
        Array<WebGpuCommandEncoder*> m_encoders;
        Array<WebGpuRenderBundleEncoder*> m_bundleEncoders;
    };
}

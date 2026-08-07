/// Validation wrapper for SwapChain.
/// Ported from Sedulous.RHI.Validation/ValidatedSwapChain.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_swap_chain;

import draconic.foundation;
import draconic.rhi;
import :validated_queue;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedSwapChain : public SwapChain
    {
    public:
        explicit ValidatedSwapChain(SwapChain* inner) : m_inner(inner) {}

        TextureFormat Format() const override { return m_inner->Format(); }
        u32 Width() const override { return m_inner->Width(); }
        u32 Height() const override { return m_inner->Height(); }
        u32 BufferCount() const override { return m_inner->BufferCount(); }
        u32 CurrentImageIndex() const override { return m_inner->CurrentImageIndex(); }
        Texture* CurrentTexture() override { return m_inner->CurrentTexture(); }
        TextureView* CurrentTextureView() override { return m_inner->CurrentTextureView(); }

        Status AcquireNextImage() override
        {
            if (m_imageAcquired)
                LogWarning("[Validation] SwapChain::acquireNextImage: image already acquired");
            Status r = m_inner->AcquireNextImage();
            if (r == ErrorCode::Ok)
                m_imageAcquired = true;
            return r;
        }

        Status Present(Queue* queue) override
        {
            if (!m_imageAcquired)
                LogWarning("[Validation] SwapChain::present: no image acquired");
            m_imageAcquired = false;
            // Unwrap validated queue so the inner swap chain gets the raw VK queue.
            auto* vq = static_cast<ValidatedQueue*>(queue);
            return m_inner->Present(vq ? vq->inner() : queue);
        }

        Status Resize(u32 w, u32 h) override
        {
            if (m_imageAcquired)
                LogError("[Validation] SwapChain::resize: cannot resize while image is acquired");
            if (w == 0 || h == 0)
                LogError("[Validation] SwapChain::resize: dimensions are zero");
            return m_inner->Resize(w, h);
        }

        SwapChain* inner() const { return m_inner; }

    private:
        SwapChain* m_inner;
        bool m_imageAcquired = false;
    };

} // namespace draconic::rhi::validation

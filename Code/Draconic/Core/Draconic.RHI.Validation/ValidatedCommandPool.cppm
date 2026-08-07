/// Validation wrapper for CommandPool.
/// Ported from Sedulous.RHI.Validation/ValidatedCommandPool.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_command_pool;

import draconic.foundation;
import draconic.rhi;
import :validated_command_encoder;
import :validated_render_bundle_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedCommandPool : public CommandPool
    {
    public:
        explicit ValidatedCommandPool(CommandPool* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
        }

        ~ValidatedCommandPool() override { releaseBundleEncoders(); }

        Status CreateEncoder(CommandEncoder*& out) override
        {
            CommandEncoder* innerEnc = nullptr;
            Status r = m_inner->CreateEncoder(innerEnc);
            if (r != ErrorCode::Ok || !innerEnc)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedCommandEncoder>(innerEnc, m_allocator);
            return ErrorCode::Ok;
        }

        void DestroyEncoder(CommandEncoder*& encoder) override
        {
            if (!encoder)
                return;
            auto* ve = static_cast<ValidatedCommandEncoder*>(encoder);
            if (ve)
            {
                CommandEncoder* innerEnc = ve->inner();
                m_inner->DestroyEncoder(innerEnc);
                m_allocator.Delete(ve);
            }
            else
            {
                m_inner->DestroyEncoder(encoder);
            }
            encoder = nullptr;
        }

        void Reset() override
        {
            // Mirror the pool-ownership contract: bundles minted this cycle (and their
            // validation wrappers) die at Reset, after the inner pool's fence wait.
            releaseBundleEncoders();
            m_inner->Reset();
        }

        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc& desc) override
        {
            auto* inner = m_inner->CreateRenderBundleEncoder(desc);
            if (!inner)
                return nullptr; // backend does not support bundles
            auto* wrapped = m_allocator.New<ValidatedRenderBundleEncoder>(inner, m_allocator);
            m_bundleEncoders.PushBack(wrapped); // pool-owned: freed on Reset
            return wrapped;
        }

        CommandPool* inner() const { return m_inner; }

    private:
        void releaseBundleEncoders()
        {
            for (auto* e : m_bundleEncoders)
                m_allocator.Delete(e);
            m_bundleEncoders.Clear();
        }

        CommandPool* m_inner;
        IAllocator& m_allocator;
        Array<ValidatedRenderBundleEncoder*> m_bundleEncoders; // wrappers freed on Reset
    };

} // namespace draconic::rhi::validation

/// Validation wrappers for RenderBundleEncoder + RenderBundle.
/// Mirrors ValidatedRenderPassEncoder's draw-recording checks for the bundle subset.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_render_bundle_encoder;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    // Wraps an inner bundle so the validation layer can unwrap it at ExecuteBundles time.
    class ValidatedRenderBundle : public RenderBundle
    {
    public:
        explicit ValidatedRenderBundle(RenderBundle* inner) : m_inner(inner) {}
        [[nodiscard]] RenderBundle* inner() const noexcept { return m_inner; }

    private:
        RenderBundle* m_inner;
    };

    // Validates the draw-recording subset (pipeline bound before draws, non-null resources) and
    // forwards to the inner bundle encoder. Owns the ValidatedRenderBundle it produces at Finish.
    class ValidatedRenderBundleEncoder : public RenderBundleEncoder
    {
    public:
        explicit ValidatedRenderBundleEncoder(RenderBundleEncoder* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
        }
        ~ValidatedRenderBundleEncoder() override { m_allocator.Delete(m_bundle); }

        void SetPipeline(RenderPipeline* pipeline) override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle setPipeline: bundle already finished");
                return;
            }
            if (!pipeline)
            {
                LogError("[Validation] bundle setPipeline: pipeline is null");
                return;
            }
            m_pipelineBound = true;
            m_inner->SetPipeline(pipeline);
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle setBindGroup: bundle already finished");
                return;
            }
            if (!group)
            {
                LogError("[Validation] bundle setBindGroup: group is null");
                return;
            }
            m_inner->SetBindGroup(index, group, dynOffsets);
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle setPushConstants: bundle already finished");
                return;
            }
            if (!data && size > 0)
            {
                LogError("[Validation] bundle setPushConstants: data is null but size > 0");
                return;
            }
            if (offset % 4 != 0)
                LogError("[Validation] bundle setPushConstants: offset must be 4-byte aligned");
            if (size % 4 != 0)
                LogError("[Validation] bundle setPushConstants: size must be 4-byte aligned");
            m_inner->SetPushConstants(stages, offset, size, data);
        }

        void SetVertexBuffer(u32 slot, Buffer* buffer, u64 offset) override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle setVertexBuffer: bundle already finished");
                return;
            }
            if (!buffer)
            {
                LogError("[Validation] bundle setVertexBuffer: buffer is null");
                return;
            }
            m_inner->SetVertexBuffer(slot, buffer, offset);
        }

        void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset) override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle setIndexBuffer: bundle already finished");
                return;
            }
            if (!buffer)
            {
                LogError("[Validation] bundle setIndexBuffer: buffer is null");
                return;
            }
            m_inner->SetIndexBuffer(buffer, format, offset);
        }

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override
        {
            if (!checkDrawReady("bundle draw"))
                return;
            if (vertexCount == 0)
                LogWarning("[Validation] bundle draw: vertexCount is 0");
            m_inner->Draw(vertexCount, instanceCount, firstVertex, firstInstance);
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 baseVertex,
                         u32 firstInstance) override
        {
            if (!checkDrawReady("bundle drawIndexed"))
                return;
            if (indexCount == 0)
                LogWarning("[Validation] bundle drawIndexed: indexCount is 0");
            m_inner->DrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        }

        void DrawIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            if (!checkDrawReady("bundle drawIndirect"))
                return;
            if (!buffer)
            {
                LogError("[Validation] bundle drawIndirect: buffer is null");
                return;
            }
            m_inner->DrawIndirect(buffer, offset, drawCount, stride);
        }

        void DrawIndexedIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            if (!checkDrawReady("bundle drawIndexedIndirect"))
                return;
            if (!buffer)
            {
                LogError("[Validation] bundle drawIndexedIndirect: buffer is null");
                return;
            }
            m_inner->DrawIndexedIndirect(buffer, offset, drawCount, stride);
        }

        RenderBundle* Finish() override
        {
            if (m_finished)
            {
                LogError("[Validation] bundle finish: already finished");
                return m_bundle;
            }
            m_finished = true;
            RenderBundle* innerBundle = m_inner->Finish();
            if (innerBundle == nullptr)
            {
                LogError("[Validation] bundle finish: inner returned null");
                return nullptr;
            }
            m_bundle = m_allocator.New<ValidatedRenderBundle>(
                innerBundle); // freed by this encoder's destructor
            return m_bundle;
        }

    private:
        bool checkDrawReady(const char* method)
        {
            if (m_finished)
            {
                LogErrorf("[Validation] %s: bundle already finished", method);
                return false;
            }
            if (!m_pipelineBound)
            {
                LogErrorf("[Validation] %s: no pipeline bound", method);
                return false;
            }
            return true;
        }

        RenderBundleEncoder* m_inner;
        IAllocator& m_allocator;
        ValidatedRenderBundle* m_bundle = nullptr;
        bool m_pipelineBound = false;
        bool m_finished = false;
    };

} // namespace draconic::rhi::validation

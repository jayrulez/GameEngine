/// Validation wrapper for RenderPassEncoder + MeshShaderPassExt.
/// Ported from Sedulous.RHI.Validation/ValidatedRenderPassEncoder.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_render_pass_encoder;

import draconic.foundation;
import draconic.rhi;
import :validated_render_bundle_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedCommandEncoder; // forward

    class ValidatedRenderPassEncoder : public RenderPassEncoder, public MeshShaderPassExt
    {
    public:
        MeshShaderPassExt* AsMeshShaderExt() noexcept override { return this; }
        void begin(RenderPassEncoder* inner, ValidatedCommandEncoder* parent)
        {
            m_inner = inner;
            m_parent = parent;
            m_pipelineBound = false;
            m_viewportSet = false;
            m_scissorSet = false;
            m_ended = false;
            m_meshPipelineBound = false;
        }

        // ---- RenderPassEncoder ----

        void SetPipeline(RenderPipeline* pipeline) override
        {
            if (m_ended)
            {
                LogError("[Validation] setPipeline: render pass ended");
                return;
            }
            if (!pipeline)
            {
                LogError("[Validation] setPipeline: pipeline is null");
                return;
            }
            m_pipelineBound = true;
            m_meshPipelineBound = false;
            m_inner->SetPipeline(pipeline);
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            if (m_ended)
            {
                LogError("[Validation] setBindGroup: render pass ended");
                return;
            }
            if (!group)
            {
                LogError("[Validation] setBindGroup: group is null");
                return;
            }
            m_inner->SetBindGroup(index, group, dynOffsets);
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            if (m_ended)
            {
                LogError("[Validation] setPushConstants: render pass ended");
                return;
            }
            if (!m_pipelineBound && !m_meshPipelineBound)
                LogWarning("[Validation] setPushConstants: no pipeline bound");
            if (!data && size > 0)
            {
                LogError("[Validation] setPushConstants: data is null but size > 0");
                return;
            }
            if (size == 0)
            {
                LogWarning("[Validation] setPushConstants: size is 0");
                return;
            }
            if (offset % 4 != 0)
                LogError("[Validation] setPushConstants: offset must be 4-byte aligned");
            if (size % 4 != 0)
                LogError("[Validation] setPushConstants: size must be 4-byte aligned");
            m_inner->SetPushConstants(stages, offset, size, data);
        }

        void SetVertexBuffer(u32 slot, Buffer* buffer, u64 offset) override
        {
            if (m_ended)
            {
                LogError("[Validation] setVertexBuffer: render pass ended");
                return;
            }
            if (!buffer)
            {
                LogError("[Validation] setVertexBuffer: buffer is null");
                return;
            }
            m_inner->SetVertexBuffer(slot, buffer, offset);
        }

        void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset) override
        {
            if (m_ended)
            {
                LogError("[Validation] setIndexBuffer: render pass ended");
                return;
            }
            if (!buffer)
            {
                LogError("[Validation] setIndexBuffer: buffer is null");
                return;
            }
            m_inner->SetIndexBuffer(buffer, format, offset);
        }

        void SetViewport(f32 x, f32 y, f32 w, f32 h, f32 minD, f32 maxD) override
        {
            if (m_ended)
            {
                LogError("[Validation] setViewport: render pass ended");
                return;
            }
            m_viewportSet = true;
            m_inner->SetViewport(x, y, w, h, minD, maxD);
        }

        void SetScissor(i32 x, i32 y, u32 w, u32 h) override
        {
            if (m_ended)
            {
                LogError("[Validation] setScissor: render pass ended");
                return;
            }
            m_scissorSet = true;
            m_inner->SetScissor(x, y, w, h);
        }

        void SetBlendConstant(f32 r, f32 g, f32 b, f32 a) override
        {
            if (m_ended)
                return;
            m_inner->SetBlendConstant(r, g, b, a);
        }

        void SetStencilReference(u32 ref) override
        {
            if (m_ended)
                return;
            m_inner->SetStencilReference(ref);
        }

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override
        {
            if (!checkDrawReady("draw"))
                return;
            if (vertexCount == 0)
                LogWarning("[Validation] draw: vertexCount is 0");
            m_inner->Draw(vertexCount, instanceCount, firstVertex, firstInstance);
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 baseVertex,
                         u32 firstInstance) override
        {
            if (!checkDrawReady("drawIndexed"))
                return;
            if (indexCount == 0)
                LogWarning("[Validation] drawIndexed: indexCount is 0");
            m_inner->DrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        }

        void DrawIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            if (!checkDrawReady("drawIndirect"))
                return;
            if (!buffer)
            {
                LogError("[Validation] drawIndirect: buffer is null");
                return;
            }
            m_inner->DrawIndirect(buffer, offset, drawCount, stride);
        }

        void DrawIndexedIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            if (!checkDrawReady("drawIndexedIndirect"))
                return;
            if (!buffer)
            {
                LogError("[Validation] drawIndexedIndirect: buffer is null");
                return;
            }
            m_inner->DrawIndexedIndirect(buffer, offset, drawCount, stride);
        }

        void ExecuteBundles(Span<RenderBundle* const> bundles) override
        {
            if (m_ended)
            {
                LogError("[Validation] executeBundles: render pass ended");
                return;
            }
            if (!m_viewportSet)
                LogWarning("[Validation] executeBundles: viewport not set - bundles inherit "
                           "viewport from the parent pass");
            if (!m_scissorSet)
                LogWarning("[Validation] executeBundles: scissor not set - bundles inherit scissor "
                           "from the parent pass");
            // Unwrap each ValidatedRenderBundle to its inner bundle before forwarding.
            Array<RenderBundle*> inner(bundles.Size());
            for (usize i = 0; i < bundles.Size(); ++i)
            {
                auto* vb = static_cast<ValidatedRenderBundle*>(bundles[i]);
                if (!vb)
                {
                    LogErrorf("[Validation] executeBundles: bundle %d is null",
                              static_cast<int>(i));
                    return;
                }
                inner[i] = vb->inner();
            }
            m_inner->ExecuteBundles(Span<RenderBundle* const>{inner.Data(), inner.Size()});
        }

        void WriteTimestamp(QuerySet* qs, u32 index) override
        {
            if (m_ended)
                return;
            if (!qs)
            {
                LogError("[Validation] writeTimestamp: querySet is null");
                return;
            }
            m_inner->WriteTimestamp(qs, index);
        }

        void BeginOcclusionQuery(QuerySet* qs, u32 index) override
        {
            if (m_ended)
                return;
            if (!qs)
            {
                LogError("[Validation] beginOcclusionQuery: querySet is null");
                return;
            }
            m_inner->BeginOcclusionQuery(qs, index);
        }

        void EndOcclusionQuery(QuerySet* qs, u32 index) override
        {
            if (m_ended)
                return;
            m_inner->EndOcclusionQuery(qs, index);
        }

        void End() override;

        // ---- MeshShaderPassExt ----

        void SetMeshPipeline(MeshPipeline* pipeline) override
        {
            if (m_ended)
            {
                LogError("[Validation] setMeshPipeline: render pass ended");
                return;
            }
            if (!pipeline)
            {
                LogError("[Validation] setMeshPipeline: pipeline is null");
                return;
            }
            m_meshPipelineBound = true;
            m_pipelineBound = false;
            auto* mp = m_inner->AsMeshShaderExt();
            if (mp)
                mp->SetMeshPipeline(pipeline);
            else
                LogError(
                    "[Validation] setMeshPipeline: inner encoder does not support mesh shaders");
        }

        void DrawMeshTasks(u32 gx, u32 gy, u32 gz) override
        {
            if (!checkDrawReady("drawMeshTasks"))
                return;
            auto* mp = m_inner->AsMeshShaderExt();
            if (mp)
                mp->DrawMeshTasks(gx, gy, gz);
        }

        void DrawMeshTasksIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override
        {
            if (!checkDrawReady("drawMeshTasksIndirect"))
                return;
            if (!buf)
            {
                LogError("[Validation] drawMeshTasksIndirect: buffer is null");
                return;
            }
            auto* mp = m_inner->AsMeshShaderExt();
            if (mp)
                mp->DrawMeshTasksIndirect(buf, offset, drawCount, stride);
        }

        void DrawMeshTasksIndirectCount(Buffer* buf, u64 offset, Buffer* countBuf, u64 countOffset,
                                        u32 maxDrawCount, u32 stride) override
        {
            if (!checkDrawReady("drawMeshTasksIndirectCount"))
                return;
            if (!buf || !countBuf)
            {
                LogError("[Validation] drawMeshTasksIndirectCount: buffer is null");
                return;
            }
            auto* mp = m_inner->AsMeshShaderExt();
            if (mp)
                mp->DrawMeshTasksIndirectCount(buf, offset, countBuf, countOffset, maxDrawCount,
                                               stride);
        }

    private:
        bool checkDrawReady(const char* method)
        {
            if (m_ended)
            {
                LogErrorf("[Validation] %s: render pass ended", method);
                return false;
            }
            if (!m_pipelineBound && !m_meshPipelineBound)
            {
                LogErrorf("[Validation] %s: no pipeline bound", method);
                return false;
            }
            if (!m_viewportSet)
            {
                LogErrorf("[Validation] %s: viewport not set", method);
                return false;
            }
            if (!m_scissorSet)
            {
                LogErrorf("[Validation] %s: scissor not set", method);
                return false;
            }
            return true;
        }

        RenderPassEncoder* m_inner = nullptr;
        ValidatedCommandEncoder* m_parent = nullptr;
        bool m_pipelineBound = false;
        bool m_meshPipelineBound = false;
        bool m_viewportSet = false;
        bool m_scissorSet = false;
        bool m_ended = false;
    };

} // namespace draconic::rhi::validation

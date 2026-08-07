/// Validation wrapper for ComputePassEncoder.
/// Ported from Sedulous.RHI.Validation/ValidatedComputePassEncoder.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_compute_pass_encoder;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedCommandEncoder; // forward

    class ValidatedComputePassEncoder : public ComputePassEncoder
    {
    public:
        void begin(ComputePassEncoder* inner, ValidatedCommandEncoder* parent)
        {
            m_inner = inner;
            m_parent = parent;
            m_pipelineBound = false;
            m_ended = false;
        }

        void SetPipeline(ComputePipeline* pipeline) override
        {
            if (m_ended)
            {
                LogError("[Validation] compute setPipeline: pass ended");
                return;
            }
            if (!pipeline)
            {
                LogError("[Validation] compute setPipeline: pipeline is null");
                return;
            }
            m_pipelineBound = true;
            m_inner->SetPipeline(pipeline);
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            if (m_ended)
                return;
            if (!group)
            {
                LogError("[Validation] compute setBindGroup: group is null");
                return;
            }
            if (!m_pipelineBound)
                LogWarning("[Validation] compute setBindGroup: no pipeline bound");
            m_inner->SetBindGroup(index, group, dynOffsets);
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            if (m_ended)
                return;
            if (!m_pipelineBound)
                LogWarning("[Validation] compute setPushConstants: no pipeline bound");
            if (!data && size > 0)
            {
                LogError("[Validation] compute setPushConstants: data is null");
                return;
            }
            if (offset % 4 != 0)
                LogError("[Validation] compute setPushConstants: offset not 4-byte aligned");
            if (size % 4 != 0)
                LogError("[Validation] compute setPushConstants: size not 4-byte aligned");
            m_inner->SetPushConstants(stages, offset, size, data);
        }

        void Dispatch(u32 x, u32 y, u32 z) override
        {
            if (m_ended)
            {
                LogError("[Validation] dispatch: pass ended");
                return;
            }
            if (!m_pipelineBound)
            {
                LogError("[Validation] dispatch: no pipeline bound");
                return;
            }
            if (x == 0 || y == 0 || z == 0)
                LogWarning("[Validation] dispatch: zero dimension");
            m_inner->Dispatch(x, y, z);
        }

        void DispatchIndirect(Buffer* buffer, u64 offset) override
        {
            if (m_ended)
            {
                LogError("[Validation] dispatchIndirect: pass ended");
                return;
            }
            if (!m_pipelineBound)
            {
                LogError("[Validation] dispatchIndirect: no pipeline bound");
                return;
            }
            if (!buffer)
            {
                LogError("[Validation] dispatchIndirect: buffer is null");
                return;
            }
            m_inner->DispatchIndirect(buffer, offset);
        }

        void ComputeBarrier() override
        {
            if (m_ended)
                return;
            m_inner->ComputeBarrier();
        }

        void WriteTimestamp(QuerySet* qs, u32 index) override
        {
            if (m_ended)
                return;
            if (!qs)
            {
                LogError("[Validation] compute writeTimestamp: querySet is null");
                return;
            }
            m_inner->WriteTimestamp(qs, index);
        }

        void End() override;

    private:
        ComputePassEncoder* m_inner = nullptr;
        ValidatedCommandEncoder* m_parent = nullptr;
        bool m_pipelineBound = false;
        bool m_ended = false;
    };

} // namespace draconic::rhi::validation

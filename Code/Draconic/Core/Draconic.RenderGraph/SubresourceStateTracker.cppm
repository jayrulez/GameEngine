// Draconic::RenderGraph - :state_tracker partition
//
// Tracks ResourceState per subresource (mip x layer) with a uniform fast path:
// while all subresources share a state, only one value is stored; a per-
// subresource array is materialized lazily when states diverge, and collapsed
// back when they reconverge. Ported from Sedulous.RenderGraph
// (SubresourceStateTracker.bf); Beef's null-means-uniform becomes an empty Array.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:state_tracker;

import draconic.foundation;
import draconic.rhi;
import :types;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class SubresourceStateTracker
    {
    public:
        SubresourceStateTracker(u32 mipCount, u32 layerCount, rhi::ResourceState initialState)
            : m_mipCount(Max(mipCount, 1u)), m_layerCount(Max(layerCount, 1u)),
              m_uniformState(initialState)
        {
        }

        [[nodiscard]] u32 MipCount() const noexcept { return m_mipCount; }
        [[nodiscard]] u32 LayerCount() const noexcept { return m_layerCount; }
        [[nodiscard]] bool IsUniform() const noexcept { return m_states.IsEmpty(); }
        [[nodiscard]] rhi::ResourceState UniformState() const noexcept { return m_uniformState; }

        [[nodiscard]] rhi::ResourceState GetState(u32 mip, u32 layer) const
        {
            if (m_states.IsEmpty())
            {
                return m_uniformState;
            }
            const u32 idx = mip + layer * m_mipCount;
            if (idx >= m_states.Size())
            {
                return m_uniformState;
            }
            return m_states[idx];
        }

        // count of 0 or ~0u means "all remaining from base".
        void SetState(u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount,
                      rhi::ResourceState state)
        {
            const u32 mipEnd = ResolveEnd(baseMip, mipCount, m_mipCount);
            const u32 layerEnd = ResolveEnd(baseLayer, layerCount, m_layerCount);

            // Whole resource? Collapse to uniform.
            if (baseMip == 0 && mipEnd >= m_mipCount && baseLayer == 0 && layerEnd >= m_layerCount)
            {
                m_uniformState = state;
                m_states.Clear();
                return;
            }

            // Materialize per-subresource storage on first divergence.
            if (m_states.IsEmpty())
            {
                if (state == m_uniformState)
                {
                    return;
                }
                m_states.Resize(m_mipCount * m_layerCount);
                for (u32 i = 0; i < m_states.Size(); ++i)
                {
                    m_states[i] = m_uniformState;
                }
            }

            for (u32 layer = baseLayer; layer < layerEnd; ++layer)
            {
                for (u32 mip = baseMip; mip < mipEnd; ++mip)
                {
                    m_states[mip + layer * m_mipCount] = state;
                }
            }

            TryCollapseToUniform();
        }

        void SetState(RGSubresourceRange range, rhi::ResourceState state)
        {
            const u32 mipCount = range.mipLevelCount == 0 ? 0xFFFFFFFFu : range.mipLevelCount;
            const u32 layerCount = range.arrayLayerCount == 0 ? 0xFFFFFFFFu : range.arrayLayerCount;
            SetState(range.baseMipLevel, mipCount, range.baseArrayLayer, layerCount, state);
        }

        void SetAll(rhi::ResourceState state)
        {
            m_uniformState = state;
            m_states.Clear();
        }

        // Snapshot per-subresource states; empty if uniform. Caller owns the copy.
        [[nodiscard]] Array<rhi::ResourceState> CopyStates() const
        {
            Array<rhi::ResourceState> copy;
            if (!m_states.IsEmpty())
            {
                copy.Resize(m_states.Size());
                for (u32 i = 0; i < m_states.Size(); ++i)
                {
                    copy[i] = m_states[i];
                }
            }
            return copy;
        }

        // Restore from a per-subresource snapshot (empty/mismatched => uniform fallback).
        void InitFromStates(const Array<rhi::ResourceState>& states,
                            rhi::ResourceState uniformFallback)
        {
            if (states.Size() != static_cast<usize>(m_mipCount) * m_layerCount)
            {
                m_uniformState = uniformFallback;
                m_states.Clear();
                return;
            }
            m_states.Resize(states.Size());
            for (usize i = 0; i < states.Size(); ++i)
            {
                m_states[i] = states[i];
            }
            TryCollapseToUniform();
        }

    private:
        void TryCollapseToUniform()
        {
            if (m_states.IsEmpty())
            {
                return;
            }
            const rhi::ResourceState first = m_states[0];
            for (usize i = 1; i < m_states.Size(); ++i)
            {
                if (m_states[i] != first)
                {
                    return;
                }
            }
            m_uniformState = first;
            m_states.Clear();
        }

        static u32 ResolveEnd(u32 base, u32 count, u32 total) noexcept
        {
            if (count == 0 || count == 0xFFFFFFFFu)
            {
                return total;
            }
            return Min(base + count, total);
        }

        u32 m_mipCount;
        u32 m_layerCount;
        rhi::ResourceState m_uniformState;
        Array<rhi::ResourceState> m_states; // empty => uniform
    };
}

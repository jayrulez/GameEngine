// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// DX12 implementation of RenderBundleEncoder and RenderBundle.
///
/// A render bundle is an ID3D12GraphicsCommandList of type D3D12_COMMAND_LIST_TYPE_BUNDLE,
/// recorded once and replayed into a direct command list via ExecuteBundle
/// (RenderPassEncoder::ExecuteBundles). DX12 bundles inherit the parent's descriptor heaps,
/// viewport, scissor, and render targets, but not pipeline state or topology, which the bundle
/// sets itself (handled by SetPipeline). Draw recording is delegated to a DxRenderPassEncoderImpl
/// whose context points at the bundle list, reusing the full root-signature and descriptor-table
/// binding logic.
///
/// This is best effort: the structure and DX12 bundle semantics are in place, but details (heap
/// inheritance, signature caching, lifetime against frames in flight) may need adjustment once
/// exercised on Windows.

module;
#include "Core/Prelude.h"

#include "DxIncludes.h"

export module foundation.rhi.dx12:render_bundle_encoder;

import foundation.core;
import foundation.rhi;
import :render_pass_encoder;

using namespace foundation::core;

export namespace foundation::rhi::dx12
{

    // DxRenderBundleImpl lives in :render_pass_encoder (so ExecuteBundles can use it without a
    // module cycle); this partition imports it from there.

    // Records draws into a bundle command list by delegating to a render-pass encoder pointed at it.
    class DxRenderBundleEncoderImpl : public RenderBundleEncoder
    {
    public:
        DxRenderBundleEncoderImpl(const DxRenderPassContext& ctx,
                                  ComPtr<ID3D12GraphicsCommandList> list,
                                  ComPtr<ID3D12CommandAllocator> alloc, IAllocator& allocator)
            : m_rec(ctx), m_list(Move(list)), m_alloc(Move(alloc)), m_allocator(allocator)
        {
            m_rec.begin(
                RenderPassDesc{}); // reset pipeline-tracking state (no pass attachments needed)
        }
        ~DxRenderBundleEncoderImpl() override { m_allocator.Delete(m_bundle); }

        void SetPipeline(RenderPipeline* p) override
        {
            ++m_recorded.pipelineBinds;
            m_rec.SetPipeline(p);
        }
        void SetBindGroup(u32 i, BindGroup* g, Span<const u32> d) override
        {
            ++m_recorded.bindGroupBinds;
            m_rec.SetBindGroup(i, g, d);
        }
        void SetPushConstants(ShaderStage s, u32 o, u32 sz, const void* d) override
        {
            m_rec.SetPushConstants(s, o, sz, d);
        }
        void SetVertexBuffer(u32 slot, Buffer* b, u64 o) override
        {
            m_rec.SetVertexBuffer(slot, b, o);
        }
        void SetIndexBuffer(Buffer* b, IndexFormat f, u64 o) override
        {
            m_rec.SetIndexBuffer(b, f, o);
        }
        void Draw(u32 v, u32 inst, u32 fv, u32 fi) override
        {
            ++m_recorded.draws;
            m_rec.Draw(v, inst, fv, fi);
        }
        void DrawIndexed(u32 ic, u32 inst, u32 fi, i32 bv, u32 finst) override
        {
            ++m_recorded.draws;
            m_rec.DrawIndexed(ic, inst, fi, bv, finst);
        }
        void DrawIndirect(Buffer* b, u64 o, u32 dc, u32 st) override
        {
            m_recorded.draws += dc;
            m_rec.DrawIndirect(b, o, dc, st);
        }
        void DrawIndexedIndirect(Buffer* b, u64 o, u32 dc, u32 st) override
        {
            m_recorded.draws += dc;
            m_rec.DrawIndexedIndirect(b, o, dc, st);
        }

        RenderBundle* Finish() override
        {
            if (m_bundle)
                return m_bundle;
            m_list->Close();
            m_bundle = m_allocator.New<DxRenderBundleImpl>(m_list, m_alloc, m_rec.currentRootSig(),
                                                           m_rec.currentPso());
            m_bundle->counts = m_recorded;
            return m_bundle;
        }

    private:
        DxRenderPassEncoderImpl m_rec;
        ComPtr<ID3D12GraphicsCommandList> m_list;
        ComPtr<ID3D12CommandAllocator> m_alloc;
        IAllocator& m_allocator;
        DxRenderBundleImpl* m_bundle = nullptr;
        RecordedCounts m_recorded; // what it recorded (its m_rec counts nothing: no counters)
    };

} // namespace foundation::rhi::dx12

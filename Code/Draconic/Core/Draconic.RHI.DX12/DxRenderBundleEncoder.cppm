/// DX12 implementation of RenderBundleEncoder + RenderBundle.
///
/// A render bundle is an ID3D12GraphicsCommandList of type D3D12_COMMAND_LIST_TYPE_BUNDLE,
/// recorded once and replayed into a direct command list via ExecuteBundle
/// (RenderPassEncoder::ExecuteBundles). DX12 bundles inherit the parent's descriptor heaps,
/// viewport, scissor, and render targets - but NOT pipeline state / topology, which the bundle
/// sets itself (handled by SetPipeline). Draw recording is delegated to a DxRenderPassEncoderImpl
/// whose context points at the bundle list, reusing the full root-signature / descriptor-table
/// binding logic.
///
/// BEST-EFFORT (authored without a Windows toolchain to compile against - shake out on Windows):
/// the structure + DX12 bundle semantics are in place; details (heap inheritance, signature
/// caching, lifetime vs frames-in-flight) may need adjustment.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

export module draconic.rhi.dx12:render_bundle_encoder;

import draconic.foundation;
import draconic.rhi;
import :render_pass_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
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

        void SetPipeline(RenderPipeline* p) override { m_rec.SetPipeline(p); }
        void SetBindGroup(u32 i, BindGroup* g, Span<const u32> d) override
        {
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
        void Draw(u32 v, u32 inst, u32 fv, u32 fi) override { m_rec.Draw(v, inst, fv, fi); }
        void DrawIndexed(u32 ic, u32 inst, u32 fi, i32 bv, u32 finst) override
        {
            m_rec.DrawIndexed(ic, inst, fi, bv, finst);
        }
        void DrawIndirect(Buffer* b, u64 o, u32 dc, u32 st) override
        {
            m_rec.DrawIndirect(b, o, dc, st);
        }
        void DrawIndexedIndirect(Buffer* b, u64 o, u32 dc, u32 st) override
        {
            m_rec.DrawIndexedIndirect(b, o, dc, st);
        }

        RenderBundle* Finish() override
        {
            if (m_bundle)
                return m_bundle;
            m_list->Close();
            m_bundle = m_allocator.New<DxRenderBundleImpl>(m_list, m_alloc, m_rec.currentRootSig(),
                                                           m_rec.currentPso());
            return m_bundle;
        }

    private:
        DxRenderPassEncoderImpl m_rec;
        ComPtr<ID3D12GraphicsCommandList> m_list;
        ComPtr<ID3D12CommandAllocator> m_alloc;
        IAllocator& m_allocator;
        DxRenderBundleImpl* m_bundle = nullptr;
    };

} // namespace draconic::rhi::dx12

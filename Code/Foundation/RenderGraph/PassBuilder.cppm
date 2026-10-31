// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - :pass_builder partition
//
// Fluent builder handed to a pass's setup callback to declare reads/writes,
// attachments, dependencies, flags, and the execute callback. Methods return
// *this for chaining.

module;
#include "Core/Prelude.h"

export module foundation.rendergraph:pass_builder;

import foundation.core;
import foundation.rhi;
import :types;
import :descriptors;
import :callbacks;
import :pass;

using namespace foundation::core;

export namespace foundation::rendergraph
{
    namespace rhi = foundation::rhi;

    class PassBuilder
    {
    public:
        explicit PassBuilder(RenderGraphPass& pass) noexcept : m_pass(&pass) {}

        // --- texture / buffer reads ---
        PassBuilder& ReadTexture(RGHandle handle, RGSubresourceRange subresource = {})
        {
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::ReadTexture, subresource});
            return *this;
        }

        // Sample a depth texture in this pass's shader (e.g. a shadow map). Like ReadTexture, this
        // is a read dependency and barrier, not a depth attachment (unlike ReadDepth), but it
        // transitions to DepthStencilRead (DEPTH_STENCIL_READ_ONLY_OPTIMAL), the layout a depth
        // sampler expects.
        PassBuilder& SampleDepth(RGHandle handle, RGSubresourceRange subresource = {})
        {
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::SampleDepthStencil, subresource});
            return *this;
        }

        PassBuilder& ReadDepth(RGHandle handle, RGSubresourceRange subresource = {})
        {
            RGDepthTarget dt{};
            dt.handle = handle;
            dt.depthLoadOp = rhi::LoadOp::Load;
            dt.depthStoreOp = rhi::StoreOp::Store;
            dt.readOnly = true;
            dt.subresource = subresource;
            m_pass->depthTarget = dt;
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::ReadDepthStencil, subresource});
            return *this;
        }

        PassBuilder& ReadBuffer(RGHandle handle)
        {
            m_pass->accesses.PushBack(RGResourceAccess{handle, RGAccessType::ReadBuffer, {}});
            return *this;
        }

        // --- render targets ---
        PassBuilder& SetColorTarget(i32 slot, RGHandle handle,
                                    rhi::LoadOp loadOp = rhi::LoadOp::Clear,
                                    rhi::StoreOp storeOp = rhi::StoreOp::Store,
                                    rhi::ClearColor clearValue = rhi::ClearColor::Black(),
                                    RGSubresourceRange subresource = {})
        {
            RGColorTarget target{};
            target.handle = handle;
            target.loadOp = loadOp;
            target.storeOp = storeOp;
            target.clearValue = clearValue;
            target.subresource = subresource;

            while (static_cast<i32>(m_pass->colorTargets.Size()) <= slot)
            {
                m_pass->colorTargets.PushBack(RGColorTarget{});
            }
            m_pass->colorTargets[static_cast<usize>(slot)] = target;

            if (loadOp == rhi::LoadOp::Load && storeOp == rhi::StoreOp::Store)
            {
                m_pass->accesses.PushBack(
                    RGResourceAccess{handle, RGAccessType::ReadWriteColorTarget, subresource});
            }
            else if (storeOp == rhi::StoreOp::Store)
            {
                m_pass->accesses.PushBack(
                    RGResourceAccess{handle, RGAccessType::WriteColorTarget, subresource});
            }
            return *this;
        }

        // MSAA hardware resolve for a color slot: the slot's multisampled target resolves into
        // `resolveHandle` (single-sample) at pass end, and the MSAA target's storeOp becomes
        // DontCare (its samples are not needed once resolved). Call it after SetColorTarget for
        // the same slot. The resolve target is registered as a write so the graph allocates and
        // transitions it. No-op for the single-sample path (callers only call this under MSAA).
        PassBuilder& SetResolveTarget(i32 slot, RGHandle resolveHandle,
                                      RGSubresourceRange subresource = {})
        {
            while (static_cast<i32>(m_pass->colorTargets.Size()) <= slot)
            {
                m_pass->colorTargets.PushBack(RGColorTarget{});
            }
            RGColorTarget& target = m_pass->colorTargets[static_cast<usize>(slot)];
            target.resolveHandle = resolveHandle;
            target.storeOp = rhi::StoreOp::DontCare; // MSAA samples discarded after resolve
            m_pass->accesses.PushBack(
                RGResourceAccess{resolveHandle, RGAccessType::WriteColorTarget, subresource});
            return *this;
        }

        PassBuilder& SetDepthTarget(RGHandle handle, rhi::LoadOp loadOp = rhi::LoadOp::Clear,
                                    rhi::StoreOp storeOp = rhi::StoreOp::Store,
                                    f32 clearDepth = rhi::depth::ClearValue(),
                                    RGSubresourceRange subresource = {},
                                    rhi::LoadOp stencilLoadOp = rhi::LoadOp::DontCare,
                                    rhi::StoreOp stencilStoreOp = rhi::StoreOp::DontCare,
                                    u32 clearStencil = 0)
        {
            RGDepthTarget dt{};
            dt.handle = handle;
            dt.depthLoadOp = loadOp;
            dt.depthStoreOp = storeOp;
            dt.depthClearValue = clearDepth;
            dt.readOnly = false;
            dt.stencilLoadOp = stencilLoadOp;
            dt.stencilStoreOp = stencilStoreOp;
            dt.stencilClearValue = clearStencil;
            dt.subresource = subresource;
            m_pass->depthTarget = dt;

            if (loadOp == rhi::LoadOp::Load && storeOp == rhi::StoreOp::Store)
            {
                m_pass->accesses.PushBack(
                    RGResourceAccess{handle, RGAccessType::ReadWriteDepthTarget, subresource});
            }
            else
            {
                // Any non-read-only depth attachment is a write access, including
                // StoreOp::DontCare (a scratch depth/stencil used only within the pass, such as
                // the overlay stencil). The access is what keeps the transient referenced
                // (allocated) and drives its layout transition; without it the resource
                // ref-counts to zero, never allocates, and ExecuteRenderPass silently
                // drops the whole pass at the null-attachment guard.
                m_pass->accesses.PushBack(
                    RGResourceAccess{handle, RGAccessType::WriteDepthTarget, subresource});
            }
            return *this;
        }

        // Read-only depth (depth test, no write): transitions to DepthStencilRead.
        PassBuilder& SetReadOnlyDepthTarget(RGHandle handle, RGSubresourceRange subresource = {})
        {
            RGDepthTarget dt{};
            dt.handle = handle;
            dt.depthLoadOp = rhi::LoadOp::Load;
            dt.depthStoreOp = rhi::StoreOp::Store;
            dt.depthClearValue = rhi::depth::ClearValue();
            dt.readOnly = true;
            dt.subresource = subresource;
            m_pass->depthTarget = dt;
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::ReadDepthStencil, subresource});
            return *this;
        }

        // --- storage (UAV) ---
        PassBuilder& WriteStorage(RGHandle handle, RGSubresourceRange subresource = {})
        {
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::WriteStorage, subresource});
            return *this;
        }
        PassBuilder& ReadWriteStorage(RGHandle handle, RGSubresourceRange subresource = {})
        {
            m_pass->accesses.PushBack(
                RGResourceAccess{handle, RGAccessType::ReadWriteStorage, subresource});
            return *this;
        }

        // --- copy ---
        PassBuilder& CopySrc(RGHandle handle)
        {
            m_pass->accesses.PushBack(RGResourceAccess{handle, RGAccessType::ReadCopySrc, {}});
            return *this;
        }
        PassBuilder& CopyDst(RGHandle handle)
        {
            m_pass->accesses.PushBack(RGResourceAccess{handle, RGAccessType::WriteCopyDst, {}});
            return *this;
        }

        // --- dependencies / flags ---
        PassBuilder& DependsOn(PassHandle pass)
        {
            m_pass->dependencies.PushBack(pass);
            return *this;
        }
        // Override the pass's viewport and scissor (a sub-rect of the attachment, e.g. split-screen).
        // Without it the pass covers the full attachment.
        PassBuilder& SetViewport(i32 x, i32 y, u32 w, u32 h)
        {
            m_pass->hasViewport = true;
            m_pass->viewportX = x;
            m_pass->viewportY = y;
            m_pass->viewportW = w;
            m_pass->viewportH = h;
            return *this;
        }
        PassBuilder& NeverCull()
        {
            m_pass->neverCull = true;
            return *this;
        }
        PassBuilder& HasSideEffects()
        {
            m_pass->hasSideEffects = true;
            return *this;
        }
        PassBuilder& EnableIf(Function<bool()> condition)
        {
            m_pass->condition = Move(condition);
            return *this;
        }

        // --- execute callbacks ---
        PassBuilder& SetExecute(RenderPassExecuteCallback callback)
        {
            m_pass->executeCallback = Move(callback);
            return *this;
        }
        // A render pass whose body is supplied by render bundles (parallel command recording).
        // The graph begins the pass with secondary-command-buffer contents and ExecuteBundles them.
        PassBuilder& SetBundleExecute(RenderBundlePassCallback callback)
        {
            m_pass->bundleCallback = Move(callback);
            return *this;
        }
        PassBuilder& SetComputeExecute(ComputePassExecuteCallback callback)
        {
            m_pass->computeCallback = Move(callback);
            return *this;
        }
        PassBuilder& SetCopyExecute(CopyPassExecuteCallback callback)
        {
            m_pass->copyCallback = Move(callback);
            return *this;
        }

    private:
        RenderGraphPass* m_pass;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - :pass partition
//
// A single pass: its declared resource accesses, attachments, dependencies, and
// typed execute callback. GetInputs/GetOutputs fold attachment load/store ops
// into the read/write access set the compiler reasons about.

module;
#include "Core/Prelude.h"

export module foundation.rendergraph:pass;

import foundation.core;
import foundation.rhi;
import :types;
import :descriptors;
import :callbacks;

using namespace foundation::core;

export namespace foundation::rendergraph
{
    namespace rhi = foundation::rhi;

    class RenderGraphPass
    {
    public:
        RenderGraphPass(StringView passName, RGPassType passType) : name(passName), type(passType)
        {
        }

        // Explicitly defaulted so the (move-only, due to Function members) special
        // members are synthesized in this module and usable by importers; GCC's
        // module support otherwise reports the implicit destructor as deleted.
        ~RenderGraphPass() = default;
        RenderGraphPass(RenderGraphPass&&) = default;
        RenderGraphPass& operator=(RenderGraphPass&&) = default;
        RenderGraphPass(const RenderGraphPass&) = delete;
        RenderGraphPass& operator=(const RenderGraphPass&) = delete;

        // Resource handles this pass reads (declared accesses and Load attachments).
        void GetInputs(Array<RGResourceAccess>& out) const
        {
            for (const RGResourceAccess& access : accesses)
            {
                if (access.IsRead())
                {
                    out.PushBack(access);
                }
            }
            for (const RGColorTarget& ct : colorTargets)
            {
                if (ct.loadOp == rhi::LoadOp::Load)
                {
                    out.PushBack(
                        RGResourceAccess{ct.handle, RGAccessType::ReadTexture, ct.subresource});
                }
            }
            if (depthTarget.HasValue())
            {
                const RGDepthTarget& dt = depthTarget.Value();
                if (dt.depthLoadOp == rhi::LoadOp::Load || dt.readOnly)
                {
                    out.PushBack(RGResourceAccess{dt.handle, RGAccessType::ReadDepthStencil,
                                                  dt.subresource});
                }
            }
        }

        // Resource handles this pass writes (declared accesses and Store attachments).
        void GetOutputs(Array<RGResourceAccess>& out) const
        {
            for (const RGResourceAccess& access : accesses)
            {
                if (access.IsWrite())
                {
                    out.PushBack(access);
                }
            }
            for (const RGColorTarget& ct : colorTargets)
            {
                if (ct.storeOp == rhi::StoreOp::Store)
                {
                    out.PushBack(RGResourceAccess{ct.handle, RGAccessType::WriteColorTarget,
                                                  ct.subresource});
                }
            }
            if (depthTarget.HasValue())
            {
                const RGDepthTarget& dt = depthTarget.Value();
                if (dt.depthStoreOp == rhi::StoreOp::Store && !dt.readOnly)
                {
                    out.PushBack(RGResourceAccess{dt.handle, RGAccessType::WriteDepthTarget,
                                                  dt.subresource});
                }
            }
        }

        [[nodiscard]] bool ShouldSurviveCulling() const noexcept
        {
            return neverCull || hasSideEffects;
        }

        // --- identity ---
        String name;
        RGPassType type;
        rhi::QueueType queueType = rhi::QueueType::Graphics;

        // --- declared work ---
        Array<RGResourceAccess> accesses;
        Array<RGColorTarget> colorTargets;
        Optional<RGDepthTarget> depthTarget;
        Array<PassHandle> dependencies;

        // --- optional per-pass viewport/scissor override (else the full attachment is used) ---
        bool hasViewport = false;
        i32 viewportX = 0, viewportY = 0;
        u32 viewportW = 0, viewportH = 0;

        // --- compile flags ---
        bool isCulled = false;
        bool neverCull = false;
        bool hasSideEffects = false;
        Function<bool()> condition; // optional runtime skip condition
        i32 executionOrder = -1;    // assigned during topological sort

        // --- typed execute callbacks (one is set per pass type) ---
        RenderPassExecuteCallback executeCallback;
        RenderBundlePassCallback bundleCallback; // render pass whose body is executed bundles
        ComputePassExecuteCallback computeCallback;
        CopyPassExecuteCallback copyCallback;
    };
}

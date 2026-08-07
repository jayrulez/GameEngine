// Draconic::RenderGraph - :graph partition
//
// The orchestrator. GPU work is declared as passes with resource accesses; the
// graph builds dependencies, culls unused work, topologically sorts, allocates/
// aliases transient resources, inserts barriers (via BarrierSolver), and runs
// the pass callbacks into a command encoder. Ported from Sedulous.RenderGraph
// (RenderGraph.bf). Compile() works without an encoder so graph logic is
// testable headless.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.rendergraph:graph;

import draconic.foundation;
import draconic.rhi;
import :types;
import :descriptors;
import :callbacks;
import :resource;
import :persistent_resource;
import :pass;
import :pass_builder;
import :barrier_solver;
import :profiler;
import :transient_pool;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class RenderGraph
    {
    public:
        explicit RenderGraph(rhi::Device* device, RenderGraphConfig config = {})
            : m_device(device), m_config(config)
        {
            if (device != nullptr)
            {
                m_texturePool = MakeUnique<TransientTexturePool>(DefaultAllocator(), *device);
            }
            const i32 slots = config.frameBufferCount > 0 ? config.frameBufferCount : 1;
            for (i32 i = 0; i < slots; ++i)
            {
                m_deferredDeletions.PushBack(Array<DeferredDeletion>{});
            }
        }

        // Turn on per-pass GPU timestamp profiling (lazy; needs the device). Idempotent.
        void EnableGpuProfiling()
        {
            if (m_gpuProfiler.Get() != nullptr || m_device == nullptr)
            {
                return;
            }
            m_gpuProfiler = MakeUnique<GraphProfiler>(DefaultAllocator());
            if (!m_gpuProfiler->Init(*m_device).IsOk())
            {
                m_gpuProfiler.Reset();
                return;
            }
            if (rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics))
            {
                m_gpuProfiler->SetTimestampPeriod(q->TimestampPeriod());
            }
        }

        // The GPU profiler (null if not enabled). Read its results after the GPU has finished (e.g.
        // after WaitIdle on a P-key dump). `LastProfiledPassCount` is how many passes were timed.
        [[nodiscard]] GraphProfiler* GpuProfiler() noexcept { return m_gpuProfiler.Get(); }
        [[nodiscard]] i32 LastProfiledPassCount() const noexcept { return m_lastProfiledPassCount; }

        // Aggregate this frame's per-pass CPU RECORD time by pass name (most-expensive first). The graph
        // execute is often CPU-bound (recording/bundle build) while the GPU is idle - this shows where.
        void AppendCpuPassReport(String& out) const
        {
            struct Agg
            {
                StringView name;
                u64 ticks = 0;
                i32 n = 0;
            };
            Array<Agg> agg;
            u64 total = 0;
            for (const PassCpu& p : m_passCpu)
            {
                total += p.ticks;
                bool found = false;
                for (Agg& a : agg)
                {
                    if (a.name == p.name)
                    {
                        a.ticks += p.ticks;
                        ++a.n;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    Agg a;
                    a.name = p.name;
                    a.ticks = p.ticks;
                    a.n = 1;
                    agg.PushBack(a);
                }
            }
            for (usize i = 0; i < agg.Size(); ++i)
                for (usize j = i + 1; j < agg.Size(); ++j)
                    if (agg[j].ticks > agg[i].ticks)
                    {
                        const Agg t = agg[i];
                        agg[i] = agg[j];
                        agg[j] = t;
                    }
            out.Append(u8"=== CPU by pass name (record cost, expensive first) ===\n");
            for (const Agg& a : agg)
            {
                AppendFormat(out, u8"  {} ms  (x{})  {}\n", TicksToMilliseconds(a.ticks), a.n,
                             a.name);
            }
            AppendFormat(out, u8"  --------\n  {} ms  TOTAL (pass record)\n",
                         TicksToMilliseconds(total));
        }

        ~RenderGraph()
        {
            for (Array<DeferredDeletion>& list : m_deferredDeletions)
            {
                for (DeferredDeletion& d : list)
                {
                    d.Execute(m_device);
                }
            }
            for (RenderGraphPass* pass : m_passes)
            {
                DefaultAllocator().Delete(pass);
            }
            for (RenderGraphResource* res : m_resources)
            {
                if (res != nullptr)
                {
                    DefaultAllocator().Delete(res);
                }
            }
        }

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        // --- output dimensions (for SizeMode resolution) ---
        void SetOutputSize(u32 width, u32 height) noexcept
        {
            m_outputWidth = width;
            m_outputHeight = height;
        }
        [[nodiscard]] u32 OutputWidth() const noexcept { return m_outputWidth; }
        [[nodiscard]] u32 OutputHeight() const noexcept { return m_outputHeight; }

        // --- frame lifecycle ---
        void BeginFrame(i32 frameIndex)
        {
            m_frameIndex = frameIndex;
            FlushDeferred(frameIndex);
            ClearPasses();
            RecycleNonPersistent();
            m_isCompiled = false;
        }

        // Compile only (cull, sort, allocate). Safe without an encoder (tests).
        [[nodiscard]] Status Compile()
        {
            if (m_passes.IsEmpty())
            {
                return Status{};
            }

            BuildResourceReferences();
            CullPasses();
            BuildDependencies();
            if (!TopologicalSort().IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            AllocateTransientResources();

            m_isCompiled = true;
            return Status{};
        }

        // Compile (if needed) and execute into `encoder` (null = compile-only).
        [[nodiscard]] Status Execute(rhi::CommandEncoder* encoder)
        {
            if (!m_isCompiled)
            {
                if (!Compile().IsOk())
                {
                    return Status{ErrorCode::Unknown};
                }
            }
            if (encoder == nullptr)
            {
                return Status{};
            }

            m_barrierSolver.Reset(ResourceSpan());

            // GPU profiling: reset the timestamp pool up front (must be outside any render pass),
            // then bracket each executed pass with begin/end timestamps.
            const bool prof = (m_gpuProfiler.Get() != nullptr);
            if (prof)
            {
                m_gpuProfiler->BeginFrame(*encoder);
                m_passCpu.Clear();
            }
            i32 profiledPassCount = 0;

            for (i32 passIdx : m_executionOrder)
            {
                RenderGraphPass* pass = m_passes[static_cast<usize>(passIdx)];
                if (pass->isCulled)
                {
                    continue;
                }
                if (static_cast<bool>(pass->condition) && !pass->condition())
                {
                    continue;
                }

                // Per-pass CPU cost (barriers + record/execute, incl. any bundle build/wait): the graph's
                // execute is often CPU-bound while the GPU is idle, so this shows which pass RECORDING is slow.
                const u64 cpuStart = prof ? foundation::GetTicks() : 0;
                encoder->BeginDebugLabel(pass->name.AsView());
                if (prof)
                {
                    m_gpuProfiler->BeginPass(*encoder, profiledPassCount, pass->name.AsView());
                }
                m_barrierSolver.EmitBarriers(*pass, ResourceSpan(), *encoder);

                switch (pass->type)
                {
                case RGPassType::Render:
                    ExecuteRenderPass(*pass, *encoder);
                    break;
                case RGPassType::Compute:
                    ExecuteComputePass(*pass, *encoder);
                    break;
                case RGPassType::Copy:
                    ExecuteCopyPass(*pass, *encoder);
                    break;
                }

                m_barrierSolver.EmitReadableAfterWriteBarriers(*pass, ResourceSpan(), *encoder);
                if (prof)
                {
                    m_gpuProfiler->EndPass(*encoder, profiledPassCount);
                    ++profiledPassCount;
                }
                encoder->EndDebugLabel();
                if (prof)
                {
                    m_passCpu.PushBack(PassCpu{pass->name.AsView(), foundation::GetTicks() - cpuStart});
                }
            }

            if (m_gpuProfiler.Get() != nullptr)
            {
                m_gpuProfiler->Resolve(*encoder, profiledPassCount);
                m_lastProfiledPassCount = profiledPassCount;
            }

            m_barrierSolver.EmitFinalTransitions(ResourceSpan(), *encoder);
            m_barrierSolver.UpdatePersistentStates(ResourceSpan());

            // Defer-delete per-subresource views created this frame.
            if (!m_subresourceViews.IsEmpty())
            {
                Array<DeferredDeletion>& deletions = DeferredSlot();
                for (rhi::TextureView* view : m_subresourceViews)
                {
                    DeferredDeletion d{};
                    d.view = view;
                    deletions.PushBack(d);
                }
                m_subresourceViews.Clear();
            }

            ReturnTransientResources();
            return Status{};
        }

        void EndFrame()
        {
            m_isCompiled = false;
            if (m_texturePool.Get() != nullptr)
            {
                m_texturePool->EndFrame();
            }
        }

        // Clear passes but keep persistent resource state (multi-view rendering).
        void Reset()
        {
            ReturnTransientResources();
            ClearPasses();
            RecycleNonPersistent();
            m_isCompiled = false;
        }

        // --- resource creation ---
        RGHandle CreateTransient(StringView name, RGTextureDesc desc)
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Texture, RGResourceLifetime::Transient);
            desc.Resolve(m_outputWidth, m_outputHeight);
            res->textureDesc = desc;
            return AddResource(res);
        }

        RGHandle CreateTransientBuffer(StringView name, RGBufferDesc desc)
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Buffer, RGResourceLifetime::Transient);
            res->bufferDesc = desc;
            return AddResource(res);
        }

        RGHandle RegisterPersistent(StringView name, rhi::Texture* texture, rhi::TextureView* view)
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Texture, RGResourceLifetime::Persistent);
            res->texture = texture;
            res->textureView = view;
            res->persistentData = MakeUnique<PersistentResource>(DefaultAllocator(), texture, view);
            return AddResource(res);
        }

        RGHandle RegisterPersistentPingPong(StringView name, rhi::Texture* tex0, rhi::Texture* tex1,
                                            rhi::TextureView* view0, rhi::TextureView* view1)
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Texture, RGResourceLifetime::Persistent);
            res->texture = tex0;
            res->textureView = view0;
            res->persistentData =
                MakeUnique<PersistentResource>(DefaultAllocator(), tex0, tex1, view0, view1);
            return AddResource(res);
        }

        RGHandle ImportTarget(StringView name, rhi::Texture* texture, rhi::TextureView* view,
                              Optional<rhi::ResourceState> finalState = {},
                              Optional<rhi::ResourceState> currentState = {})
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Texture, RGResourceLifetime::Imported);
            res->texture = texture;
            res->textureView = view;
            res->finalState = finalState;
            res->lastKnownState =
                currentState.HasValue()
                    ? currentState.Value()
                    : (texture != nullptr ? texture->initialState : rhi::ResourceState::Undefined);
            return AddResource(res);
        }

        // Depth import variant carrying a depth-only view for shader sampling.
        RGHandle ImportTarget(StringView name, rhi::Texture* texture, rhi::TextureView* view,
                              rhi::TextureView* depthOnlyView,
                              Optional<rhi::ResourceState> finalState = {},
                              Optional<rhi::ResourceState> currentState = {})
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Texture, RGResourceLifetime::Imported);
            res->texture = texture;
            res->textureView = view;
            res->depthOnlyView = depthOnlyView;
            res->finalState = finalState;
            res->lastKnownState =
                currentState.HasValue()
                    ? currentState.Value()
                    : (texture != nullptr ? texture->initialState : rhi::ResourceState::Undefined);
            return AddResource(res);
        }

        RGHandle ImportBuffer(StringView name, rhi::Buffer* buffer)
        {
            RenderGraphResource* res = DefaultAllocator().New<RenderGraphResource>(
                name, RGResourceType::Buffer, RGResourceLifetime::Imported);
            res->buffer = buffer;
            return AddResource(res);
        }

        void RequireReadableAfterWrite(RGHandle handle)
        {
            if (RenderGraphResource* res = Resolve(handle))
            {
                res->readableAfterWrite = true;
            }
        }

        // --- pass creation (setup callable receives PassBuilder&) ---
        template <typename Setup>
        PassHandle AddRenderPass(StringView name, Setup&& setup)
        {
            return AddPassOfType(name, RGPassType::Render, setup);
        }
        template <typename Setup>
        PassHandle AddComputePass(StringView name, Setup&& setup)
        {
            return AddPassOfType(name, RGPassType::Compute, setup);
        }
        template <typename Setup>
        PassHandle AddCopyPass(StringView name, Setup&& setup)
        {
            return AddPassOfType(name, RGPassType::Copy, setup);
        }

        // --- resource access (during execute callbacks) ---
        [[nodiscard]] rhi::Texture* GetTexture(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            if (res == nullptr)
            {
                return nullptr;
            }
            return res->persistentData.Get() != nullptr ? res->persistentData->CurrentTexture()
                                                        : res->texture;
        }
        [[nodiscard]] rhi::TextureView* GetTextureView(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            if (res == nullptr)
            {
                return nullptr;
            }
            return res->persistentData.Get() != nullptr ? res->persistentData->CurrentView()
                                                        : res->textureView;
        }

        // A stable id for the GPU texture currently backing `handle`. For a transient it changes when
        // the graph (re)allocates a different physical texture (e.g. on resize) - even if the new view
        // happens to reuse a freed address. A bind-group cache over GetTextureView MUST also key on
        // this to stay correct across resizes. 0 when unresolved.
        [[nodiscard]] u64 GetTextureGeneration(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            return res != nullptr ? res->textureGeneration : 0;
        }
        [[nodiscard]] rhi::TextureView* GetDepthOnlyTextureView(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            return res != nullptr ? res->depthOnlyView : nullptr;
        }
        [[nodiscard]] rhi::Buffer* GetBuffer(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            return res != nullptr ? res->buffer : nullptr;
        }

        void SwapPingPong(RGHandle handle)
        {
            RenderGraphResource* res = Resolve(handle);
            if (res != nullptr && res->persistentData.Get() != nullptr)
            {
                res->persistentData->Swap();
                res->texture = res->persistentData->CurrentTexture();
                res->textureView = res->persistentData->CurrentView();
            }
        }

        // --- queries ---
        [[nodiscard]] usize PassCount() const noexcept { return m_passes.Size(); }
        [[nodiscard]] usize ResourceCount() const noexcept
        {
            usize count = 0;
            for (RenderGraphResource* r : m_resources)
            {
                if (r != nullptr)
                {
                    ++count;
                }
            }
            return count;
        }
        [[nodiscard]] usize CulledPassCount() const noexcept
        {
            usize count = 0;
            for (RenderGraphPass* p : m_passes)
            {
                if (p->isCulled)
                {
                    ++count;
                }
            }
            return count;
        }
        [[nodiscard]] RGHandle GetResource(StringView name) const
        {
            for (usize i = 0; i < m_resources.Size(); ++i)
            {
                RenderGraphResource* res = m_resources[i];
                if (res != nullptr && res->name == name)
                {
                    return RGHandle{static_cast<u32>(i), res->generation};
                }
            }
            return RGHandle::Invalid();
        }
        [[nodiscard]] rhi::ResourceState GetResourceState(RGHandle handle)
        {
            RenderGraphResource* res = ResolveChecked(handle);
            return res != nullptr ? res->lastKnownState : rhi::ResourceState::Undefined;
        }

        [[nodiscard]] const Array<i32>& ExecutionOrder() const noexcept { return m_executionOrder; }
        [[nodiscard]] const Array<RenderGraphPass*>& Passes() const noexcept { return m_passes; }
        [[nodiscard]] const Array<RenderGraphResource*>& Resources() const noexcept
        {
            return m_resources;
        }

    private:
        struct DeferredDeletion
        {
            rhi::Texture* texture = nullptr;
            rhi::TextureView* view = nullptr;
            rhi::TextureView* view2 = nullptr;
            rhi::Buffer* buffer = nullptr;

            void Execute(rhi::Device* device)
            {
                if (device == nullptr)
                {
                    return;
                }
                if (view2 != nullptr)
                {
                    device->DestroyTextureView(view2);
                }
                if (view != nullptr)
                {
                    device->DestroyTextureView(view);
                }
                if (texture != nullptr)
                {
                    device->DestroyTexture(texture);
                }
                if (buffer != nullptr)
                {
                    device->DestroyBuffer(buffer);
                }
            }
        };

        [[nodiscard]] Span<RenderGraphResource* const> ResourceSpan() const
        {
            return Span<RenderGraphResource* const>(m_resources.Data(), m_resources.Size());
        }

        [[nodiscard]] Array<DeferredDeletion>& DeferredSlot()
        {
            const i32 slot = m_frameIndex % static_cast<i32>(m_deferredDeletions.Size());
            return m_deferredDeletions[static_cast<usize>(slot)];
        }

        void FlushDeferred(i32 frameIndex)
        {
            const i32 slot = frameIndex % static_cast<i32>(m_deferredDeletions.Size());
            Array<DeferredDeletion>& deletions = m_deferredDeletions[static_cast<usize>(slot)];
            for (DeferredDeletion& d : deletions)
            {
                d.Execute(m_device);
            }
            deletions.Clear();
        }

        // Validate a handle (bounds + generation); null on mismatch.
        [[nodiscard]] RenderGraphResource* ResolveChecked(RGHandle handle)
        {
            if (!handle.IsValid() || handle.index >= m_resources.Size())
            {
                return nullptr;
            }
            RenderGraphResource* res = m_resources[handle.index];
            if (res == nullptr || res->generation != handle.generation)
            {
                return nullptr;
            }
            return res;
        }
        // Validate a handle (bounds only; ignores generation) - for mutators.
        [[nodiscard]] RenderGraphResource* Resolve(RGHandle handle)
        {
            if (!handle.IsValid() || handle.index >= m_resources.Size())
            {
                return nullptr;
            }
            return m_resources[handle.index];
        }

        RGHandle AddResource(RenderGraphResource* res)
        {
            if (!m_freeResourceSlots.IsEmpty())
            {
                const i32 idx = m_freeResourceSlots.Back();
                m_freeResourceSlots.PopBack();
                m_resources[static_cast<usize>(idx)] = res;
                return RGHandle{static_cast<u32>(idx), res->generation};
            }
            const u32 idx = static_cast<u32>(m_resources.Size());
            m_resources.PushBack(res);
            return RGHandle{idx, res->generation};
        }

        template <typename Setup>
        PassHandle AddPassOfType(StringView name, RGPassType type, Setup&& setup)
        {
            RenderGraphPass* pass = DefaultAllocator().New<RenderGraphPass>(name, type);
            PassBuilder builder(*pass);
            setup(builder);
            const u32 idx = static_cast<u32>(m_passes.Size());
            m_passes.PushBack(pass);
            return PassHandle{idx};
        }

        void ClearPasses()
        {
            for (RenderGraphPass* p : m_passes)
            {
                DefaultAllocator().Delete(p);
            }
            m_passes.Clear();
            m_executionOrder.Clear();
        }

        void RecycleNonPersistent()
        {
            for (usize i = 0; i < m_resources.Size(); ++i)
            {
                RenderGraphResource* res = m_resources[i];
                if (res == nullptr)
                {
                    continue;
                }
                if (res->lifetime != RGResourceLifetime::Persistent)
                {
                    m_freeResourceSlots.PushBack(static_cast<i32>(i));
                    DefaultAllocator().Delete(res);
                    m_resources[i] = nullptr;
                }
                else
                {
                    res->ResetTracking();
                }
            }
        }

        // === compilation pipeline ===
        void BuildResourceReferences()
        {
            for (usize passIdx = 0; passIdx < m_passes.Size(); ++passIdx)
            {
                RenderGraphPass* pass = m_passes[passIdx];
                const PassHandle passHandle{static_cast<u32>(passIdx)};

                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (!access.handle.IsValid() || access.handle.index >= m_resources.Size())
                    {
                        continue;
                    }
                    RenderGraphResource* res = m_resources[access.handle.index];
                    if (res == nullptr)
                    {
                        continue;
                    }

                    ++res->refCount;
                    if (res->firstUsePass < 0 || static_cast<i32>(passIdx) < res->firstUsePass)
                    {
                        res->firstUsePass = static_cast<i32>(passIdx);
                    }
                    if (static_cast<i32>(passIdx) > res->lastUsePass)
                    {
                        res->lastUsePass = static_cast<i32>(passIdx);
                    }
                    if (access.IsWrite())
                    {
                        res->firstWriter = passHandle;
                    }
                    if (access.IsRead())
                    {
                        res->lastReader = passHandle;
                    }
                }
            }
        }

        void CullPasses()
        {
            for (RenderGraphPass* pass : m_passes)
            {
                pass->isCulled = true;
            }
            for (RenderGraphPass* pass : m_passes)
            {
                if (pass->ShouldSurviveCulling())
                {
                    pass->isCulled = false;
                }
            }

            // Passes writing imported resources with a final state stay alive.
            for (RenderGraphPass* pass : m_passes)
            {
                if (!pass->isCulled)
                {
                    continue;
                }
                Array<RGResourceAccess> outputs;
                pass->GetOutputs(outputs);
                for (const RGResourceAccess& output : outputs)
                {
                    if (output.handle.IsValid() && output.handle.index < m_resources.Size())
                    {
                        RenderGraphResource* res = m_resources[output.handle.index];
                        if (res != nullptr && res->finalState.HasValue())
                        {
                            pass->isCulled = false;
                            break;
                        }
                    }
                }
            }

            // Backward propagation: a live pass keeps its overlapping upstream writers alive.
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (RenderGraphPass* pass : m_passes)
                {
                    if (pass->isCulled)
                    {
                        continue;
                    }
                    Array<RGResourceAccess> inputs;
                    pass->GetInputs(inputs);

                    for (const RGResourceAccess& input : inputs)
                    {
                        if (!input.handle.IsValid() || input.handle.index >= m_resources.Size())
                        {
                            continue;
                        }
                        for (usize i = m_passes.Size(); i-- > 0;)
                        {
                            RenderGraphPass* candidate = m_passes[i];
                            if (!candidate->isCulled)
                            {
                                continue;
                            }
                            Array<RGResourceAccess> candidateOutputs;
                            candidate->GetOutputs(candidateOutputs);
                            for (const RGResourceAccess& output : candidateOutputs)
                            {
                                if (output.handle == input.handle &&
                                    (input.subresource.IsAll() || output.subresource.IsAll() ||
                                     input.subresource.Overlaps(output.subresource)))
                                {
                                    candidate->isCulled = false;
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }

        void BuildDependencies()
        {
            for (usize passIdx = 0; passIdx < m_passes.Size(); ++passIdx)
            {
                RenderGraphPass* pass = m_passes[passIdx];
                if (pass->isCulled)
                {
                    continue;
                }

                Array<RGResourceAccess> readAccesses;
                pass->GetInputs(readAccesses);

                for (const RGResourceAccess& readAccess : readAccesses)
                {
                    if (!readAccess.handle.IsValid() ||
                        readAccess.handle.index >= m_resources.Size())
                    {
                        continue;
                    }
                    RenderGraphResource* res = m_resources[readAccess.handle.index];
                    const u32 totalMips = res != nullptr ? res->TotalMipLevels() : 1u;
                    const u32 totalLayers = res != nullptr ? res->TotalArrayLayers() : 1u;

                    for (usize j = passIdx; j-- > 0;)
                    {
                        RenderGraphPass* writer = m_passes[j];
                        if (writer->isCulled)
                        {
                            continue;
                        }
                        Array<RGResourceAccess> writerOutputs;
                        writer->GetOutputs(writerOutputs);

                        bool overlaps = false;
                        for (const RGResourceAccess& writerAccess : writerOutputs)
                        {
                            if (writerAccess.handle == readAccess.handle &&
                                (readAccess.subresource.IsAll() ||
                                 writerAccess.subresource.IsAll() ||
                                 readAccess.subresource.Overlaps(writerAccess.subresource,
                                                                 totalMips, totalLayers)))
                            {
                                overlaps = true;
                                break;
                            }
                        }
                        if (overlaps)
                        {
                            AddDependencyIfNew(*pass, PassHandle{static_cast<u32>(j)});
                            break;
                        }
                    }
                }
            }
        }

        static void AddDependencyIfNew(RenderGraphPass& pass, PassHandle dep)
        {
            for (PassHandle existing : pass.dependencies)
            {
                if (existing == dep)
                {
                    return;
                }
            }
            pass.dependencies.PushBack(dep);
        }

        [[nodiscard]] Status TopologicalSort()
        {
            m_executionOrder.Clear();
            const usize passCount = m_passes.Size();

            Array<i32> inDegree;
            inDegree.Resize(passCount);
            Array<Array<i32>> adjacency;
            for (usize i = 0; i < passCount; ++i)
            {
                adjacency.PushBack(Array<i32>{});
            }

            for (usize i = 0; i < passCount; ++i)
            {
                RenderGraphPass* pass = m_passes[i];
                if (pass->isCulled)
                {
                    continue;
                }
                for (PassHandle dep : pass->dependencies)
                {
                    if (dep.IsValid() && dep.index < passCount)
                    {
                        adjacency[dep.index].PushBack(static_cast<i32>(i));
                        ++inDegree[i];
                    }
                }
            }

            Array<i32> queue;
            for (usize i = 0; i < passCount; ++i)
            {
                if (!m_passes[i]->isCulled && inDegree[i] == 0)
                {
                    queue.PushBack(static_cast<i32>(i));
                }
            }

            while (!queue.IsEmpty())
            {
                const i32 node = queue[0];
                queue.RemoveAt(0);
                m_executionOrder.PushBack(node);
                m_passes[static_cast<usize>(node)]->executionOrder =
                    static_cast<i32>(m_executionOrder.Size()) - 1;

                for (i32 neighbor : adjacency[static_cast<usize>(node)])
                {
                    if (--inDegree[static_cast<usize>(neighbor)] == 0)
                    {
                        queue.PushBack(neighbor);
                    }
                }
            }

            usize nonCulled = 0;
            for (RenderGraphPass* p : m_passes)
            {
                if (!p->isCulled)
                {
                    ++nonCulled;
                }
            }
            return m_executionOrder.Size() == nonCulled ? Status{}
                                                        : Status{ErrorCode::Unknown}; // cycle
        }

        void AllocateTransientResources()
        {
            // Tally transient texture allocations this frame so a failure (GPU out of memory / too many
            // allocations) surfaces with graph context: how many were freshly allocated vs served from
            // the pool, and the first resource + size that failed. See the per-frame warning below.
            u32 freshAllocs = 0, allocFails = 0, poolHits = 0;
            StringView firstFailName;
            u32 firstFailW = 0, firstFailH = 0;

            for (RenderGraphResource* res : m_resources)
            {
                if (res == nullptr || res->lifetime != RGResourceLifetime::Transient ||
                    res->refCount == 0)
                {
                    continue;
                }

                if (res->resourceType == RGResourceType::Texture && m_device != nullptr)
                {
                    rhi::TextureDesc rhiDesc = res->textureDesc.ToTextureDesc(res->name.AsView());
                    rhi::Texture* tex = nullptr;
                    rhi::TextureView* view = nullptr;
                    u64 pooledGen = 0;
                    if (m_texturePool.Get() != nullptr &&
                        m_texturePool->TryAcquire(rhiDesc, tex, view, pooledGen))
                    {
                        ++poolHits;
                        res->texture = tex;
                        res->textureView = view;
                        res->textureGeneration = pooledGen; // reused physical texture keeps its id
                        if (rhi::IsDepthFormat(res->textureDesc.format) &&
                            rhi::HasStencil(res->textureDesc.format))
                        {
                            rhi::TextureViewDesc depthDesc{};
                            depthDesc.aspect = rhi::TextureAspect::DepthOnly;
                            depthDesc.label = u8"RGDepthOnlyView";
                            rhi::TextureView* depthOnly = nullptr;
                            if (m_device->CreateTextureView(tex, depthDesc, depthOnly).IsOk())
                            {
                                res->depthOnlyView = depthOnly;
                            }
                        }
                    }
                    else
                    {
                        ++freshAllocs;
                        if (!res->AllocateTexture(*m_device).IsOk())
                        {
                            ++allocFails;
                            if (firstFailName.IsEmpty())
                            {
                                firstFailName = res->name.AsView();
                                firstFailW = res->textureDesc.width;
                                firstFailH = res->textureDesc.height;
                            }
                        }
                        res->textureGeneration =
                            ++m_nextTransientGeneration; // freshly created -> new id
                    }
                }
                else if (res->resourceType == RGResourceType::Buffer && m_device != nullptr)
                {
                    (void)res->AllocateBuffer(*m_device);
                }
            }

            // One warning per frame that had any transient-texture allocation failure (GPU resource
            // exhaustion). `fresh` = new allocations attempted this frame (pool misses), `poolHit` =
            // reused from the transient pool. Sustained firing indicates a GPU-memory leak somewhere.
            if (allocFails > 0)
            {
                DRACONIC_LOG_WARNING(
                    u8"RenderGraph",
                    u8"transient alloc FAILED {}/{} (poolHit={}); first fail '{}' {}x{}",
                    allocFails, freshAllocs, poolHits, firstFailName, firstFailW, firstFailH);
            }
        }

        void ReturnTransientResources()
        {
            Array<DeferredDeletion>& deletions = DeferredSlot();
            for (RenderGraphResource* res : m_resources)
            {
                if (res == nullptr || res->lifetime != RGResourceLifetime::Transient)
                {
                    continue;
                }

                if (res->resourceType == RGResourceType::Texture && res->texture != nullptr)
                {
                    // Only pool a fully-valid texture. A texture with a NULL view (a partial/failed
                    // allocation) must never re-enter the pool: a later TryAcquire would hand it back and
                    // ExecuteRenderPass would drop that attachment, desyncing the render-pass signature
                    // from the pipelines/bundles - permanently, keyed by size. Destroy it instead.
                    if (m_texturePool.Get() != nullptr && res->textureView != nullptr)
                    {
                        const rhi::TextureDesc rhiDesc =
                            res->textureDesc.ToTextureDesc(res->name.AsView());
                        m_texturePool->ReturnToPool(rhiDesc, res->texture, res->textureView,
                                                    res->textureGeneration);
                        if (res->depthOnlyView != nullptr)
                        {
                            DeferredDeletion d{};
                            d.view = res->depthOnlyView;
                            deletions.PushBack(d);
                        }
                    }
                    else
                    {
                        DeferredDeletion d{};
                        d.texture = res->texture;
                        d.view = res->textureView;
                        d.view2 = res->depthOnlyView;
                        deletions.PushBack(d);
                    }
                    res->texture = nullptr;
                    res->textureView = nullptr;
                    res->depthOnlyView = nullptr;
                }
                else if (res->resourceType == RGResourceType::Buffer && res->buffer != nullptr)
                {
                    DeferredDeletion d{};
                    d.buffer = res->buffer;
                    deletions.PushBack(d);
                    res->buffer = nullptr;
                }
            }
        }

        // === pass execution ===
        rhi::TextureView* CreateSubresourceView(RGHandle handle, RGSubresourceRange subresource)
        {
            if (m_device == nullptr)
            {
                return nullptr;
            }
            rhi::Texture* texture = GetTexture(handle);
            if (texture == nullptr)
            {
                return nullptr;
            }

            rhi::TextureViewDesc viewDesc{};
            viewDesc.baseMipLevel = subresource.baseMipLevel;
            viewDesc.mipLevelCount =
                subresource.mipLevelCount == 0 ? 1u : subresource.mipLevelCount;
            viewDesc.baseArrayLayer = subresource.baseArrayLayer;
            viewDesc.arrayLayerCount =
                subresource.arrayLayerCount == 0 ? 1u : subresource.arrayLayerCount;
            viewDesc.dimension = viewDesc.arrayLayerCount == 1
                                     ? rhi::TextureViewDimension::Texture2D
                                     : rhi::TextureViewDimension::Texture2DArray;
            rhi::TextureView* view = nullptr;
            if (m_device->CreateTextureView(texture, viewDesc, view).IsOk())
            {
                m_subresourceViews.PushBack(view);
                return view;
            }
            return nullptr;
        }

        // The full-target render area for a pass, from an attachment's resolved dimensions (the
        // transient desc, else the backing texture). Used to set the parent pass's viewport +
        // scissor before ExecuteBundles: render bundles INHERIT viewport/scissor from the parent
        // pass (WebGPU + DX12 bundles cannot set them), so the parent must. Returns false if no
        // attachment yields dimensions.
        [[nodiscard]] bool PassRenderArea(RenderGraphPass& pass, u32& outW, u32& outH)
        {
            auto fromHandle = [&](RGHandle h, u32& w, u32& h2) -> bool
            {
                if (RenderGraphResource* res = Resolve(h))
                {
                    if (res->textureDesc.width > 0 && res->textureDesc.height > 0)
                    {
                        w = res->textureDesc.width;
                        h2 = res->textureDesc.height;
                        return true;
                    }
                }
                if (rhi::TextureView* v = GetTextureView(h))
                {
                    if (v->texture != nullptr && v->texture->desc.width > 0 &&
                        v->texture->desc.height > 0)
                    {
                        w = v->texture->desc.width;
                        h2 = v->texture->desc.height;
                        return true;
                    }
                }
                return false;
            };
            for (const RGColorTarget& ct : pass.colorTargets)
            {
                if (fromHandle(ct.handle, outW, outH))
                {
                    return true;
                }
            }
            if (pass.depthTarget.HasValue())
            {
                if (fromHandle(pass.depthTarget.Value().handle, outW, outH))
                {
                    return true;
                }
            }
            return false;
        }

        void ExecuteRenderPass(RenderGraphPass& pass, rhi::CommandEncoder& encoder)
        {
            const bool hasBundles = static_cast<bool>(pass.bundleCallback);
            if (!static_cast<bool>(pass.executeCallback) && !hasBundles)
            {
                return;
            }

            // If any DECLARED color/depth attachment failed to resolve to a view (a transient
            // allocation failure during a rapid viewport resize, or an unready imported target), SKIP
            // the whole pass. Beginning it with only the surviving attachments would shift/reduce the
            // color-attachment count and formats away from the fixed signature the pipelines and
            // secondary bundles were recorded for - the vkCmdExecuteCommands / colorAttachmentCount /
            // renderArea-zero validation cascade and corrupt output. Better to drop one frame's pass.
            for (const RGColorTarget& ct : pass.colorTargets)
            {
                if (GetTextureView(ct.handle) == nullptr)
                {
                    return;
                }
            }
            if (pass.depthTarget.HasValue() &&
                GetTextureView(pass.depthTarget.Value().handle) == nullptr)
            {
                return;
            }

            // A bundle pass records its bundles NOW (encoder in recording state, before the pass
            // begins); the graph then begins with secondary contents + replays them. Done before
            // building the pass desc so the encoder is still recording.
            Array<rhi::RenderBundle*> bundles;
            if (hasBundles)
            {
                pass.bundleCallback(encoder, bundles);
            }

            rhi::RenderPassDesc rpDesc{};
            rpDesc.label = pass.name.AsView();
            if (hasBundles)
            {
                rpDesc.contents = rhi::RenderPassContents::SecondaryCommandBuffers;
            }

            for (usize i = 0; i < pass.colorTargets.Size(); ++i)
            {
                const RGColorTarget& ct = pass.colorTargets[i];
                rhi::TextureView* view = GetTextureView(ct.handle);
                if (view == nullptr)
                {
                    continue;
                }
                if (!ct.subresource.IsAll())
                {
                    if (rhi::TextureView* subView =
                            CreateSubresourceView(ct.handle, ct.subresource))
                    {
                        view = subView;
                    }
                }
                rhi::ColorAttachment attachment{};
                attachment.view = view;
                attachment.loadOp = ct.loadOp;
                attachment.storeOp = ct.storeOp;
                attachment.clearValue = ct.clearValue;
                rpDesc.colorAttachments.Add(attachment);
            }

            if (pass.depthTarget.HasValue())
            {
                const RGDepthTarget& dt = pass.depthTarget.Value();
                rhi::TextureView* view = GetTextureView(dt.handle);
                if (view != nullptr)
                {
                    if (!dt.subresource.IsAll())
                    {
                        if (rhi::TextureView* subView =
                                CreateSubresourceView(dt.handle, dt.subresource))
                        {
                            view = subView;
                        }
                    }
                    rhi::DepthStencilAttachment dsa{};
                    dsa.view = view;
                    dsa.depthLoadOp = dt.depthLoadOp;
                    dsa.depthStoreOp = dt.depthStoreOp;
                    dsa.depthClearValue = dt.depthClearValue;
                    dsa.depthReadOnly = dt.readOnly;
                    dsa.stencilLoadOp = dt.stencilLoadOp;
                    dsa.stencilStoreOp = dt.stencilStoreOp;
                    dsa.stencilClearValue = dt.stencilClearValue;
                    rpDesc.depthStencilAttachment = dsa;
                }
            }

            rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(rpDesc);
            // Viewport/scissor: a per-pass override (split-screen sub-rect) if set, else the full
            // attachment. Set here for bundle passes (bundles inherit it from the parent - WebGPU/
            // DX12 can't set it inside a bundle); a plain execute callback may also rely on it.
            i32 vpX = 0, vpY = 0;
            u32 vpW = 0, vpH = 0;
            if (pass.hasViewport)
            {
                vpX = pass.viewportX;
                vpY = pass.viewportY;
                vpW = pass.viewportW;
                vpH = pass.viewportH;
            }
            else
            {
                (void)PassRenderArea(pass, vpW, vpH);
            }
            if (vpW > 0 && vpH > 0)
            {
                rp->SetViewport(static_cast<f32>(vpX), static_cast<f32>(vpY), static_cast<f32>(vpW),
                                static_cast<f32>(vpH));
                rp->SetScissor(vpX, vpY, vpW, vpH);
            }
            if (hasBundles)
            {
                if (!bundles.IsEmpty())
                {
                    rp->ExecuteBundles(
                        Span<rhi::RenderBundle* const>{bundles.Data(), bundles.Size()});
                }
            }
            else
            {
                pass.executeCallback(*rp);
            }
            rp->End();
        }

        void ExecuteComputePass(RenderGraphPass& pass, rhi::CommandEncoder& encoder)
        {
            if (!static_cast<bool>(pass.computeCallback))
            {
                return;
            }
            rhi::ComputePassEncoder* cp = encoder.BeginComputePass(pass.name.AsView());
            pass.computeCallback(*cp);
            cp->End();
        }

        void ExecuteCopyPass(RenderGraphPass& pass, rhi::CommandEncoder& encoder)
        {
            if (!static_cast<bool>(pass.copyCallback))
            {
                return;
            }
            pass.copyCallback(encoder);
        }

        rhi::Device* m_device;
        RenderGraphConfig m_config;
        Array<RenderGraphResource*> m_resources;
        Array<i32> m_freeResourceSlots;
        Array<RenderGraphPass*> m_passes;
        Array<i32> m_executionOrder;
        bool m_isCompiled = false;
        UniquePtr<GraphProfiler> m_gpuProfiler; // optional per-pass GPU timing
        i32 m_lastProfiledPassCount = 0;
        struct PassCpu
        {
            StringView name;
            u64 ticks = 0;
        }; // per-pass CPU record time (name -> pass->name, valid pre-Reset)
        Array<PassCpu> m_passCpu;
        BarrierSolver m_barrierSolver;
        UniquePtr<TransientTexturePool> m_texturePool;
        Array<Array<DeferredDeletion>> m_deferredDeletions;
        Array<rhi::TextureView*> m_subresourceViews;
        i32 m_frameIndex = 0;
        u32 m_outputWidth = 1920;
        u32 m_outputHeight = 1080;
        u64 m_nextTransientGeneration =
            0; // monotonic id stamped on each freshly created transient texture
    };
}

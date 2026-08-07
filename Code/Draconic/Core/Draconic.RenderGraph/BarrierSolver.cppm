// Draconic::RenderGraph - :barrier_solver partition
//
// Computes and emits resource barriers between passes. State is tracked at two
// levels: per-resource-handle (buffers; convenience for textures) and per-
// Texture with per-subresource granularity (source of truth for textures, keyed
// by the GPU texture pointer so the same texture unifies across handles). Ported
// from Sedulous.RenderGraph (BarrierSolver.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:barrier_solver;

import draconic.foundation;
import draconic.rhi;
import :types;
import :resource;
import :pass;
import :state_tracker;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class BarrierSolver
    {
    public:
        BarrierSolver() = default;
        ~BarrierSolver() { ClearTrackers(); }

        BarrierSolver(const BarrierSolver&) = delete;
        BarrierSolver& operator=(const BarrierSolver&) = delete;

        // Initialize states from the resource list (call once per frame). Persistent
        // resources use their last-known state; transient resources start Undefined.
        void Reset(Span<RenderGraphResource* const> resources)
        {
            m_resourceStates.Clear();
            ClearTrackers();

            for (i32 i = 0; i < static_cast<i32>(resources.Size()); ++i)
            {
                RenderGraphResource* res = resources[static_cast<usize>(i)];
                if (res == nullptr)
                {
                    continue;
                }

                rhi::ResourceState initialState = rhi::ResourceState::Undefined;

                if (res->lifetime == RGResourceLifetime::Persistent &&
                    res->persistentData.Get() != nullptr)
                {
                    initialState = res->persistentData->firstFrame
                                       ? (res->texture != nullptr ? res->texture->initialState
                                                                  : rhi::ResourceState::Undefined)
                                       : res->persistentData->lastKnownState;
                }
                else if (res->lifetime == RGResourceLifetime::Imported)
                {
                    initialState = res->lastKnownState;
                }
                else
                {
                    // Transient: always start Undefined; the backend uses the texture's
                    // actual tracked state for the "before" side, so first access always
                    // gets a correct transition.
                    initialState = rhi::ResourceState::Undefined;
                }

                m_resourceStates.InsertOrAssign(i, initialState);

                if (res->resourceType == RGResourceType::Texture && res->texture != nullptr)
                {
                    if (SubresourceStateTracker** existing = m_textureStates.Find(res->texture))
                    {
                        // Same GPU texture via another handle - unify.
                        SubresourceStateTracker* tracker = *existing;
                        if (initialState == rhi::ResourceState::Undefined && tracker->IsUniform() &&
                            tracker->UniformState() != rhi::ResourceState::Undefined)
                        {
                            m_resourceStates.InsertOrAssign(i, tracker->UniformState());
                        }
                        else if (initialState != rhi::ResourceState::Undefined)
                        {
                            tracker->SetAll(initialState);
                        }
                    }
                    else
                    {
                        const u32 mipCount = res->texture->desc.mipLevelCount;
                        const u32 layerCount = res->texture->desc.arrayLayerCount;
                        SubresourceStateTracker* tracker =
                            DefaultAllocator().New<SubresourceStateTracker>(mipCount, layerCount,
                                                                            initialState);

                        if (res->lifetime == RGResourceLifetime::Persistent &&
                            res->persistentData.Get() != nullptr &&
                            !res->persistentData->firstFrame &&
                            !res->persistentData->subresourceStates.IsEmpty())
                        {
                            tracker->InitFromStates(res->persistentData->subresourceStates,
                                                    initialState);
                        }

                        m_textureStates.InsertOrAssign(res->texture, tracker);
                    }
                }
            }
        }

        // Emit barriers needed before executing `pass`.
        void EmitBarriers(const RenderGraphPass& pass, Span<RenderGraphResource* const> resources,
                          rhi::CommandEncoder& encoder)
        {
            m_textureBarriers.Clear();
            m_bufferBarriers.Clear();

            for (const RGResourceAccess& access : pass.accesses)
            {
                if (!access.handle.IsValid())
                {
                    continue;
                }
                const i32 resIdx = static_cast<i32>(access.handle.index);
                if (static_cast<usize>(resIdx) >= resources.Size())
                {
                    continue;
                }

                RenderGraphResource* res = resources[static_cast<usize>(resIdx)];
                if (res == nullptr)
                {
                    continue;
                }

                const rhi::ResourceState requiredState = access.ToResourceState();
                const bool accessIsReadWrite = IsRead(access.type) && IsWrite(access.type);

                if (res->resourceType == RGResourceType::Texture && res->texture != nullptr)
                {
                    SubresourceStateTracker** found = m_textureStates.Find(res->texture);
                    if (found == nullptr)
                    {
                        continue;
                    }
                    SubresourceStateTracker* tracker = *found;

                    EmitTextureBarriers(*tracker, res->texture, access.subresource, requiredState,
                                        accessIsReadWrite);
                    tracker->SetState(access.subresource, requiredState);
                    m_resourceStates.InsertOrAssign(resIdx, requiredState);
                }
                else if (res->resourceType == RGResourceType::Buffer && res->buffer != nullptr)
                {
                    rhi::ResourceState currentState = rhi::ResourceState::Undefined;
                    if (rhi::ResourceState* p = m_resourceStates.Find(resIdx))
                    {
                        currentState = *p;
                    }
                    if (currentState == requiredState)
                    {
                        continue;
                    }

                    rhi::BufferBarrier bb{};
                    bb.buffer = res->buffer;
                    bb.oldState = currentState;
                    bb.newState = requiredState;
                    m_bufferBarriers.PushBack(bb);

                    m_resourceStates.InsertOrAssign(resIdx, requiredState);
                }
            }

            FlushBarriers(encoder);
        }

        // After a pass executes: transition resources marked ReadableAfterWrite
        // (written by the pass) to ShaderRead so external bind groups can sample them.
        void EmitReadableAfterWriteBarriers(const RenderGraphPass& pass,
                                            Span<RenderGraphResource* const> resources,
                                            rhi::CommandEncoder& encoder)
        {
            m_textureBarriers.Clear();
            m_bufferBarriers.Clear();

            for (const RGResourceAccess& access : pass.accesses)
            {
                if (!access.IsWrite() || !access.handle.IsValid())
                {
                    continue;
                }
                const i32 resIdx = static_cast<i32>(access.handle.index);
                if (static_cast<usize>(resIdx) >= resources.Size())
                {
                    continue;
                }

                RenderGraphResource* res = resources[static_cast<usize>(resIdx)];
                if (res == nullptr || !res->readableAfterWrite)
                {
                    continue;
                }
                if (res->resourceType != RGResourceType::Texture || res->texture == nullptr)
                {
                    continue;
                }

                SubresourceStateTracker** found = m_textureStates.Find(res->texture);
                if (found == nullptr)
                {
                    continue;
                }
                SubresourceStateTracker* tracker = *found;

                EmitTextureBarriers(*tracker, res->texture, access.subresource,
                                    rhi::ResourceState::ShaderRead, false);
                tracker->SetState(access.subresource, rhi::ResourceState::ShaderRead);
                m_resourceStates.InsertOrAssign(resIdx, rhi::ResourceState::ShaderRead);
            }

            FlushBarriers(encoder);
        }

        // Transition imported resources to their requested final state.
        void EmitFinalTransitions(Span<RenderGraphResource* const> resources,
                                  rhi::CommandEncoder& encoder)
        {
            m_textureBarriers.Clear();
            m_bufferBarriers.Clear();

            for (i32 i = 0; i < static_cast<i32>(resources.Size()); ++i)
            {
                RenderGraphResource* res = resources[static_cast<usize>(i)];
                if (res == nullptr || !res->finalState.HasValue())
                {
                    continue;
                }

                const rhi::ResourceState finalState = res->finalState.Value();
                if (res->texture != nullptr)
                {
                    SubresourceStateTracker** found = m_textureStates.Find(res->texture);
                    if (found == nullptr)
                    {
                        continue;
                    }
                    SubresourceStateTracker* tracker = *found;

                    EmitTextureBarriers(*tracker, res->texture, RGSubresourceRange::All(),
                                        finalState, false);
                    tracker->SetAll(finalState);
                    m_resourceStates.InsertOrAssign(i, finalState);
                }
            }

            FlushBarriers(encoder);
        }

        // Write tracked states back into persistent/imported resources for next frame.
        void UpdatePersistentStates(Span<RenderGraphResource* const> resources)
        {
            for (i32 i = 0; i < static_cast<i32>(resources.Size()); ++i)
            {
                RenderGraphResource* res = resources[static_cast<usize>(i)];
                if (res == nullptr)
                {
                    continue;
                }

                if (res->resourceType == RGResourceType::Texture && res->texture != nullptr)
                {
                    SubresourceStateTracker** found = m_textureStates.Find(res->texture);
                    if (found == nullptr)
                    {
                        continue;
                    }
                    SubresourceStateTracker* tracker = *found;

                    if (tracker->IsUniform())
                    {
                        res->lastKnownState = tracker->UniformState();
                        if (res->persistentData.Get() != nullptr)
                        {
                            res->persistentData->lastKnownState = tracker->UniformState();
                            res->persistentData->firstFrame = false;
                            res->persistentData->subresourceStates.Clear();
                        }
                    }
                    else
                    {
                        res->lastKnownState = tracker->GetState(0, 0);
                        if (res->persistentData.Get() != nullptr)
                        {
                            res->persistentData->lastKnownState = tracker->GetState(0, 0);
                            res->persistentData->firstFrame = false;
                            res->persistentData->subresourceStates = tracker->CopyStates();
                        }
                    }
                }
                else if (rhi::ResourceState* p = m_resourceStates.Find(i))
                {
                    res->lastKnownState = *p;
                    if (res->persistentData.Get() != nullptr)
                    {
                        res->persistentData->lastKnownState = *p;
                        res->persistentData->firstFrame = false;
                    }
                }
            }
        }

        [[nodiscard]] rhi::ResourceState GetState(i32 resourceIndex)
        {
            if (rhi::ResourceState* p = m_resourceStates.Find(resourceIndex))
            {
                return *p;
            }
            return rhi::ResourceState::Undefined;
        }

        [[nodiscard]] rhi::ResourceState GetTextureState(rhi::Texture* texture)
        {
            if (texture == nullptr)
            {
                return rhi::ResourceState::Undefined;
            }
            if (SubresourceStateTracker** found = m_textureStates.Find(texture))
            {
                SubresourceStateTracker* tracker = *found;
                return tracker->IsUniform() ? tracker->UniformState() : tracker->GetState(0, 0);
            }
            return rhi::ResourceState::Undefined;
        }

        [[nodiscard]] SubresourceStateTracker* GetTextureTracker(rhi::Texture* texture)
        {
            if (texture == nullptr)
            {
                return nullptr;
            }
            SubresourceStateTracker** found = m_textureStates.Find(texture);
            return found != nullptr ? *found : nullptr;
        }

    private:
        void EmitTextureBarriers(SubresourceStateTracker& tracker, rhi::Texture* texture,
                                 RGSubresourceRange subresource, rhi::ResourceState requiredState,
                                 bool accessIsReadWrite)
        {
            const u32 totalMips = tracker.MipCount();
            const u32 totalLayers = tracker.LayerCount();

            if (tracker.IsUniform())
            {
                const rhi::ResourceState currentState = tracker.UniformState();
                if (currentState == requiredState && !accessIsReadWrite)
                {
                    return;
                }

                rhi::TextureBarrier barrier{};
                barrier.texture = texture;
                barrier.oldState = currentState;
                barrier.newState = requiredState;
                if (!subresource.IsAll())
                {
                    barrier.baseMipLevel = subresource.baseMipLevel;
                    barrier.mipLevelCount =
                        subresource.mipLevelCount == 0 ? 0xFFFFFFFFu : subresource.mipLevelCount;
                    barrier.baseArrayLayer = subresource.baseArrayLayer;
                    barrier.arrayLayerCount = subresource.arrayLayerCount == 0
                                                  ? 0xFFFFFFFFu
                                                  : subresource.arrayLayerCount;
                }
                m_textureBarriers.PushBack(barrier);
            }
            else
            {
                const u32 baseMip = subresource.baseMipLevel;
                const u32 mipEnd = subresource.mipLevelCount == 0
                                       ? totalMips
                                       : Min(baseMip + subresource.mipLevelCount, totalMips);
                const u32 baseLayer = subresource.baseArrayLayer;
                const u32 layerEnd =
                    subresource.arrayLayerCount == 0
                        ? totalLayers
                        : Min(baseLayer + subresource.arrayLayerCount, totalLayers);

                for (u32 layer = baseLayer; layer < layerEnd; ++layer)
                {
                    for (u32 mip = baseMip; mip < mipEnd; ++mip)
                    {
                        const rhi::ResourceState currentState = tracker.GetState(mip, layer);
                        if (currentState == requiredState && !accessIsReadWrite)
                        {
                            continue;
                        }

                        rhi::TextureBarrier barrier{};
                        barrier.texture = texture;
                        barrier.oldState = currentState;
                        barrier.newState = requiredState;
                        barrier.baseMipLevel = mip;
                        barrier.mipLevelCount = 1;
                        barrier.baseArrayLayer = layer;
                        barrier.arrayLayerCount = 1;
                        m_textureBarriers.PushBack(barrier);
                    }
                }
            }
        }

        void FlushBarriers(rhi::CommandEncoder& encoder)
        {
            if (m_textureBarriers.IsEmpty() && m_bufferBarriers.IsEmpty())
            {
                return;
            }

            rhi::BarrierGroup group{};
            if (!m_textureBarriers.IsEmpty())
            {
                group.textureBarriers = Span<const rhi::TextureBarrier>(m_textureBarriers.Data(),
                                                                        m_textureBarriers.Size());
            }
            if (!m_bufferBarriers.IsEmpty())
            {
                group.bufferBarriers = Span<const rhi::BufferBarrier>(m_bufferBarriers.Data(),
                                                                      m_bufferBarriers.Size());
            }
            encoder.Barrier(group);
        }

        void ClearTrackers()
        {
            for (auto& entry : m_textureStates)
            {
                DefaultAllocator().Delete(entry.value);
            }
            m_textureStates.Clear();
        }

        HashMap<i32, rhi::ResourceState> m_resourceStates;
        HashMap<rhi::Texture*, SubresourceStateTracker*> m_textureStates;
        Array<rhi::TextureBarrier> m_textureBarriers;
        Array<rhi::BufferBarrier> m_bufferBarriers;
    };
}

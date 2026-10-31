// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:extract_ctx` partition.
///
/// `RenderContext` provisions the per-worker scratch that parallel extraction writes into: one
/// `FrameArena` and one item list per job-system slot (the worker threads and the caller, the job
/// system's SlotCount). Each worker fills only its own slot (indexed by JobSystem::CurrentSlot),
/// so there is no contention and no locking; a single-threaded merge then gathers every slot's
/// items into the immutable `ExtractedScene`.
///
/// Arenas are reset once per frame (BeginFrame) and accumulate across the frame's scenes; the
/// item lists are cleared per extraction (ResetItems). Extraction is single-buffered; double
/// buffering (extract frame N while submitting N-1) would go here.

module;
#include "Core/Prelude.h"

export module foundation.render:extract_ctx;

import foundation.core;
import :data;

using namespace foundation::core;

export namespace foundation::render
{

    class RenderContext
    {
    public:
        // The allocator (required; the owner decides) backs the per-slot arenas and lists.
        explicit RenderContext(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        // Ensure `slotCount` (>= 1) per-worker slots exist, then reset every arena and item list for a
        // new frame. Call once at frame start (before any extraction).
        void BeginFrame(u32 slotCount)
        {
            if (slotCount < 1)
            {
                slotCount = 1;
            }
            while (static_cast<u32>(m_arenas.Size()) < slotCount)
            {
                m_arenas.PushBack(MakeUnique<FrameArena>(*m_allocator, *m_allocator));
                m_lists.PushBack(MakeUnique<Array<RenderData*>>(*m_allocator));
            }
            m_slots = slotCount;
            for (u32 s = 0; s < m_slots; ++s)
            {
                m_arenas[s]->Reset();
                m_lists[s]->Clear();
            }
        }

        // Clear the per-slot item lists for a fresh extraction (arenas keep accumulating this frame).
        void ResetItems()
        {
            for (u32 s = 0; s < m_slots; ++s)
            {
                m_lists[s]->Clear();
            }
        }

        [[nodiscard]] FrameArena& Arena(u32 slot) noexcept { return *m_arenas[slot]; }
        [[nodiscard]] Array<RenderData*>& Items(u32 slot) noexcept { return *m_lists[slot]; }
        [[nodiscard]] u32 SlotCount() const noexcept { return m_slots; }

        // Gather every slot's items into `out` (single-threaded, after the parallel fill).
        void MergeInto(ExtractedScene& out) const
        {
            for (u32 s = 0; s < m_slots; ++s)
            {
                for (RenderData* rd : *m_lists[s])
                {
                    out.AddExternal(rd);
                }
            }
        }

    private:
        // UniquePtr slots so growing the pools never moves a live FrameArena or item list (the
        // FrameArena is non-movable, and parallel writers hold references for the extraction).
        IAllocator* m_allocator;
        Array<UniquePtr<FrameArena>> m_arenas;
        Array<UniquePtr<Array<RenderData*>>> m_lists;
        u32 m_slots = 0;
    };

} // namespace foundation::render

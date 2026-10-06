// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render - :bind_group_cache partition
//
// The passes build a bind group over this frame's transient textures and keep it while the same
// textures come back. A group is only still right while EVERY view it binds is the same view of
// the same texture: the render graph's pool hands transients out per frame, and a pooled view can
// return over another texture (its generation tells). A cache that checked only some of them kept
// another frame's inputs bound: TAA kept frame 0's motion vectors behind one of its two history
// textures, and the player's edges flickered every other frame (2026-10-02).

module;
#include "Core/Prelude.h"

export module foundation.render:bind_group_cache;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // What a bind group was built from: each view it binds that can change between frames, with
    // its texture's generation, and the one buffer it binds that can (a scene's sky lighting, with
    // the context's generation; none by default). Two are equal only when every input is.
    template <usize N>
    struct BindGroupInputs
    {
        rhi::TextureView* views[N] = {};
        u64 generations[N] = {};
        rhi::Buffer* buffer = nullptr;
        u64 bufferGeneration = 0;

        void Set(usize slot, rhi::TextureView* view, u64 generation) noexcept
        {
            views[slot] = view;
            generations[slot] = generation;
        }
        void SetBuffer(rhi::Buffer* b, u64 generation) noexcept
        {
            buffer = b;
            bufferGeneration = generation;
        }
        // No group can be built while an input is missing.
        [[nodiscard]] bool Complete() const noexcept
        {
            for (usize i = 0; i < N; ++i)
            {
                if (views[i] == nullptr)
                {
                    return false;
                }
            }
            return true;
        }
        [[nodiscard]] bool operator==(const BindGroupInputs&) const = default;
    };

    // Bind groups keyed by the view that selects one (a history texture, an input that is stable
    // per size), each remembering the inputs it was built from.
    template <usize N>
    class BindGroupCache
    {
    public:
        // The group built for `key` from exactly `inputs`, or null. A group built for `key` from
        // anything else is forgotten and handed back in `stale` for the caller to destroy.
        [[nodiscard]] rhi::BindGroup* Find(rhi::TextureView* key, const BindGroupInputs<N>& inputs,
                                           rhi::BindGroup*& stale)
        {
            stale = nullptr;
            Entry* e = m_entries.Find(key);
            if (e == nullptr)
            {
                return nullptr;
            }
            if (e->bg != nullptr && e->inputs == inputs)
            {
                return e->bg;
            }
            stale = e->bg;
            m_entries.Remove(key);
            return nullptr;
        }

        void Store(rhi::TextureView* key, const BindGroupInputs<N>& inputs, rhi::BindGroup* bg)
        {
            m_entries.InsertOrAssign(key, Entry{bg, inputs});
        }

        // Every group still held, for the owner to destroy, and the cache emptied.
        template <typename Fn>
        void Release(Fn&& destroy)
        {
            for (auto& kv : m_entries)
            {
                if (kv.value.bg != nullptr)
                {
                    destroy(kv.value.bg);
                }
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            BindGroupInputs<N> inputs;
        };
        HashMap<rhi::TextureView*, Entry> m_entries;
    };
}

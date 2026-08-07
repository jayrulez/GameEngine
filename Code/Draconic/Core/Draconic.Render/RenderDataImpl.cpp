/// Draconic::Render - the `:data` partition.
///
/// The render-data contract - and the boundary that keeps the renderer scene-agnostic.
/// Render data is *extracted and pushed to* the renderer; the renderer never reaches back
/// into a scene (one-way: the scene-integration layer in draconic.engine.render depends
/// on this, not the reverse).
///
/// A `RenderData` is a unit of renderable work: a `RenderCategory` tag plus the data a
/// draw needs (e.g. `MeshRenderData` = world matrix + mesh + material). It is allocated
/// from a per-frame `FrameArena` (bump allocator), is trivially destructible, and is valid
/// for exactly one frame. An `ExtractedScene` is the per-scene, once-per-frame, immutable
/// snapshot of all a scene's render data; every view of that scene shares it read-only.
///
/// A `RenderData` carries no view-dependent state: the sort key (which depends on the
/// camera) lives on a per-view `DrawItem`, computed during the view's cull+sort against the
/// shared snapshot. (§5/§9 of docs/design/renderer.md.)

module;
#include "Draconic.Foundation/Prelude.h"
#include <new>
#include <type_traits>

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.geometry;
import draconic.materials;

using namespace draconic::foundation;

namespace draconic::render
{
    RenderCategory CategoryRegistry::Register(StringView name, SortMode sort, PassAffinity affinity)
    {
        for (u16 i = 0; i < m_count; ++i)
        {
            if (m_info[i].name == name)
            {
                return i;
            }
        }
        if (m_count >= kMaxCategories)
        {
            return kMaxCategories;
        }
        m_info[m_count] = Info{name, sort, affinity};
        return m_count++;
    }

    SortMode CategoryRegistry::Sort(RenderCategory c) const noexcept
    {
        return (c < m_count) ? m_info[c].sort : SortMode::FrontToBack;
    }

    PassAffinity CategoryRegistry::Affinity(RenderCategory c) const noexcept
    {
        return (c < m_count) ? m_info[c].affinity : PassAffinity::None;
    }

    StringView CategoryRegistry::Name(RenderCategory c) const noexcept
    {
        return (c < m_count) ? m_info[c].name : StringView{};
    }
    void* FrameArena::Allocate(usize size, usize alignment)
    {
        // Walk to a chunk that fits (reusing chunks retained across Reset), else grow.
        for (;;)
        {
            if (m_current < m_chunks.Size())
            {
                Chunk& c = m_chunks[m_current];
                const usize base = reinterpret_cast<usize>(c.data);
                const usize aligned = AlignUp(base + m_offset, alignment) - base;
                if (aligned + size <= c.size)
                {
                    m_offset = aligned + size;
                    return c.data + aligned;
                }
                // doesn't fit this chunk - advance to the next
                ++m_current;
                m_offset = 0;
                continue;
            }
            if (!AddChunk(size > m_chunkSize ? size : m_chunkSize))
            {
                return nullptr;
            }
        }
    }

    void FrameArena::Reset() noexcept
    {
        m_current = 0;
        m_offset = 0;
    }

    bool FrameArena::AddChunk(usize size)
    {
        void* mem = DefaultAllocator().Allocate(size, kChunkAlign);
        if (mem == nullptr)
        {
            return false;
        }
        m_chunks.PushBack(Chunk{static_cast<byte*>(mem), size});
        return true;
    }
    void ExtractedScene::AddExternal(RenderData* data)
    {
        if (data != nullptr)
        {
            m_items.PushBack(data);
        }
    }

    Span<const LocalShadowCaster> ExtractedScene::LocalShadowCasters() const noexcept
    {
        return Span<const LocalShadowCaster>{m_localCasters.Data(), m_localCasters.Size()};
    }

    Span<const DecalInstance> ExtractedScene::Decals() const noexcept
    {
        return Span<const DecalInstance>{m_decals.Data(), m_decals.Size()};
    }

    Span<const ReflectionProbe> ExtractedScene::ReflectionProbes() const noexcept
    {
        return Span<const ReflectionProbe>{m_probes.Data(), m_probes.Size()};
    }

    const DirectionalShadow& ExtractedScene::DirectionalShadowData() const noexcept
    {
        return m_shadow;
    }

    void ExtractedScene::Reset() noexcept
    {
        m_items.Clear();
        m_lights.Clear();
        m_localCasters.Clear();
        m_decals.Clear();
        m_probes.Clear();
        m_ambient = Float3{0.03f, 0.03f, 0.03f};
        m_sky = {};
        m_shadow = {};
        m_arena.Reset();
    }

    Span<RenderData* const> ExtractedScene::Items() const noexcept
    {
        return Span<RenderData* const>{m_items.Data(), m_items.Size()};
    }

    Span<const GpuLight> ExtractedScene::Lights() const noexcept
    {
        return Span<const GpuLight>{m_lights.Data(), m_lights.Size()};
    }
}

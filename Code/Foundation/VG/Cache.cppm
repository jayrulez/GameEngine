// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :cache partition.
//
// Pre-tessellated path reuse across frames: CachedPath (cached fill and stroke
// meshes and the style they were tessellated for) and PathCache (an LRU map from
// path identity to CachedPath). Keyed by Path::InstanceId, so a freed path's entry
// goes stale and ages out rather than being served to a new path at its address.

module;
#include "Core/Prelude.h"

export module foundation.vg:cache;

import foundation.core;
import :enums;
import :vertex;
import :style;
import :path;
import :tessellation;

using namespace foundation::core;

export namespace foundation::vg
{
    /// Pre-tessellated path data for reuse across frames.
    class CachedPath
    {
    public:
        /// Access count for LRU eviction.
        i64 lastAccessTime = 0;

        [[nodiscard]] bool IsFillValid() const { return m_fillValid; }
        [[nodiscard]] bool IsStrokeValid() const { return m_strokeValid; }

        /// Whether the cached fill matches every input of the fill tessellation.
        [[nodiscard]] bool FillMatches(Color color, FillRule fillRule, bool antiAlias,
                                       f32 tolerance) const
        {
            return m_fillValid && m_fillColor == color && m_fillRule == fillRule &&
                   m_fillAA == antiAlias && m_fillTolerance == tolerance;
        }

        /// Whether the cached stroke matches every input of the stroke tessellation: the whole
        /// style (miter limit and dash offset included), the dash pattern and the tolerance, so
        /// an animated dash offset re-tessellates instead of reusing the previous dashing.
        [[nodiscard]] bool StrokeMatches(Color color, StrokeStyle style,
                                         Span<const f32> dashPattern, bool antiAlias,
                                         f32 tolerance) const
        {
            if (!m_strokeValid || m_strokeColor != color || m_strokeAA != antiAlias ||
                m_strokeTolerance != tolerance || m_strokeStyle.width != style.width ||
                m_strokeStyle.cap != style.cap || m_strokeStyle.join != style.join ||
                m_strokeStyle.miterLimit != style.miterLimit ||
                m_strokeStyle.dashOffset != style.dashOffset ||
                m_strokeDash.Size() != dashPattern.Size())
            {
                return false;
            }
            for (usize i = 0; i < dashPattern.Size(); ++i)
            {
                if (m_strokeDash[i] != dashPattern[i])
                {
                    return false;
                }
            }
            return true;
        }

        /// Cached fill mesh data (empty spans if not valid).
        void GetFillMesh(Span<const VGVertex>& vertices, Span<const u32>& meshIndices) const
        {
            if (m_fillValid)
            {
                vertices = Span<const VGVertex>(m_fillVertices.Data(), m_fillVertices.Size());
                meshIndices = Span<const u32>(m_fillIndices.Data(), m_fillIndices.Size());
            }
            else
            {
                vertices = Span<const VGVertex>{};
                meshIndices = Span<const u32>{};
            }
        }

        /// Cached stroke mesh data (empty spans if not valid).
        void GetStrokeMesh(Span<const VGVertex>& vertices, Span<const u32>& meshIndices) const
        {
            if (m_strokeValid)
            {
                vertices = Span<const VGVertex>(m_strokeVertices.Data(), m_strokeVertices.Size());
                meshIndices = Span<const u32>(m_strokeIndices.Data(), m_strokeIndices.Size());
            }
            else
            {
                vertices = Span<const VGVertex>{};
                meshIndices = Span<const u32>{};
            }
        }

        /// Store fill tessellation data.
        void SetFillData(const Array<VGVertex>& vertices, const Array<u32>& fillIndices, f32 tolerance,
                         Color color, FillRule fillRule, bool antiAlias)
        {
            m_fillVertices = vertices;
            m_fillIndices = fillIndices;
            m_fillColor = color;
            m_fillRule = fillRule;
            m_fillAA = antiAlias;
            m_fillTolerance = tolerance;
            m_fillValid = true;
        }

        /// Store stroke tessellation data.
        void SetStrokeData(const Array<VGVertex>& vertices, const Array<u32>& strokeIndices,
                           Span<const f32> dashPattern, f32 tolerance,
                           Color color, StrokeStyle style, bool antiAlias)
        {
            m_strokeVertices = vertices;
            m_strokeIndices = strokeIndices;
            m_strokeColor = color;
            m_strokeStyle = style;
            m_strokeAA = antiAlias;
            m_strokeTolerance = tolerance;
            m_strokeDash.Clear();
            for (usize i = 0; i < dashPattern.Size(); ++i)
                m_strokeDash.PushBack(dashPattern[i]);
            m_strokeValid = true;
        }

        /// Invalidate all cached data.
        void Invalidate()
        {
            m_fillValid = false;
            m_strokeValid = false;
        }

    private:
        Array<VGVertex> m_fillVertices;
        Array<u32> m_fillIndices;
        bool m_fillValid = false;
        Color m_fillColor;
        FillRule m_fillRule = FillRule::NonZero;
        bool m_fillAA = false;
        f32 m_fillTolerance = 0.0f;

        Array<VGVertex> m_strokeVertices;
        Array<u32> m_strokeIndices;
        bool m_strokeValid = false;
        StrokeStyle m_strokeStyle;
        Color m_strokeColor;
        bool m_strokeAA = false;
        f32 m_strokeTolerance = 0.0f;
        Array<f32> m_strokeDash;
    };

    /// Caches pre-tessellated path data for reuse across frames (LRU eviction).
    class PathCache
    {
    public:
        explicit PathCache(i32 capacity = 256) : m_capacity(capacity) {}

        /// Get or tessellate a filled path, appending the mesh to the outputs.
        void GetOrTessellateFill(const Path& path, Color color, FillRule fillRule, bool antiAlias,
                                 Array<VGVertex>& outVertices, Array<u32>& outIndices,
                                 f32 tolerance = 0.25f)
        {
            CachedPath& cached = GetOrCreate(path);
            cached.lastAccessTime = m_accessCounter++;

            if (cached.FillMatches(color, fillRule, antiAlias, tolerance))
            {
                Span<const VGVertex> verts;
                Span<const u32> idx;
                cached.GetFillMesh(verts, idx);
                Append(outVertices, outIndices, verts, idx);
                return;
            }

            Array<VGVertex> tempVerts;
            Array<u32> tempIndices;
            FillTessellator::Tessellate(path, fillRule, color, antiAlias, tempVerts, tempIndices,
                                        tolerance);
            cached.SetFillData(tempVerts, tempIndices, tolerance, color, fillRule, antiAlias);
            Append(outVertices, outIndices,
                   Span<const VGVertex>(tempVerts.Data(), tempVerts.Size()),
                   Span<const u32>(tempIndices.Data(), tempIndices.Size()));
        }

        /// Get or tessellate a stroked path, appending the mesh to the outputs.
        void GetOrTessellateStroke(const Path& path, Color color, StrokeStyle style,
                                   Span<const f32> dashPattern, bool antiAlias,
                                   Array<VGVertex>& outVertices, Array<u32>& outIndices,
                                   f32 tolerance = 0.25f)
        {
            CachedPath& cached = GetOrCreate(path);
            cached.lastAccessTime = m_accessCounter++;

            if (cached.StrokeMatches(color, style, dashPattern, antiAlias, tolerance))
            {
                Span<const VGVertex> verts;
                Span<const u32> idx;
                cached.GetStrokeMesh(verts, idx);
                Append(outVertices, outIndices, verts, idx);
                return;
            }

            Array<VGVertex> tempVerts;
            Array<u32> tempIndices;
            Array<FlattenedSubPath> subPaths;
            PathFlattener::Flatten(path, tolerance, subPaths);

            for (usize s = 0; s < subPaths.Size(); ++s)
            {
                const FlattenedSubPath& subPath = subPaths[s];
                if (subPath.points.Size() >= 2)
                {
                    StrokeTessellator::Tessellate(
                        Span<const Float2>(subPath.points.Data(), subPath.points.Size()),
                        subPath.isClosed, style, dashPattern, antiAlias, color, tempVerts,
                        tempIndices);
                }
            }

            cached.SetStrokeData(tempVerts, tempIndices, dashPattern, tolerance, color, style,
                                 antiAlias);
            Append(outVertices, outIndices,
                   Span<const VGVertex>(tempVerts.Data(), tempVerts.Size()),
                   Span<const u32>(tempIndices.Data(), tempIndices.Size()));
        }

        /// Invalidate cached data for a specific path.
        void Invalidate(const Path& path)
        {
            if (CachedPath* cached = m_cache.Find(path.InstanceId()))
                cached->Invalidate();
        }

        /// Live entries (one per path instance ever tessellated and not yet evicted).
        [[nodiscard]] usize Count() const noexcept { return m_cache.Size(); }

        /// Clear all cached data.
        void Clear()
        {
            m_cache.Clear();
            m_accessCounter = 0;
        }

        /// Set the maximum number of cached paths; a fuller cache trims to exactly `capacity`.
        void SetCapacity(i32 capacity)
        {
            m_capacity = capacity;
            EvictDownTo(m_capacity);
        }

    private:
        static void Append(Array<VGVertex>& outVertices, Array<u32>& outIndices,
                           Span<const VGVertex> verts, Span<const u32> idx)
        {
            const u32 baseIndex = static_cast<u32>(outVertices.Size());
            for (usize i = 0; i < verts.Size(); ++i)
                outVertices.PushBack(verts[i]);
            for (usize i = 0; i < idx.Size(); ++i)
                outIndices.PushBack(baseIndex + idx[i]);
        }

        // Keyed by the path's instance id, never its address: a freed path's entry simply goes
        // stale and ages out by LRU instead of being served to whatever lands at its address.
        CachedPath& GetOrCreate(const Path& path)
        {
            if (CachedPath* existing = m_cache.Find(path.InstanceId()))
                return *existing;

            EvictDownTo(m_capacity - 1); // room for this one: a full cache settles at capacity
            return m_cache.InsertOrAssign(path.InstanceId(), CachedPath());
        }

        /// Evict least-recently-used entries until at most `count` remain.
        void EvictDownTo(i32 count)
        {
            while (static_cast<i32>(m_cache.Size()) > count && !m_cache.IsEmpty())
            {
                u64 oldestId = 0;
                i64 oldestTime = 9223372036854775807LL; // i64 max
                bool found = false;
                for (auto& entry : m_cache)
                {
                    if (!found || entry.value.lastAccessTime < oldestTime)
                    {
                        oldestTime = entry.value.lastAccessTime;
                        oldestId = entry.key;
                        found = true;
                    }
                }
                m_cache.Remove(oldestId);
            }
        }

        HashMap<u64, CachedPath> m_cache;
        i32 m_capacity = 256;
        i64 m_accessCounter = 0;
    };
}

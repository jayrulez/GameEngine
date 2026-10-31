// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Geometry - the `:mesh` partition.
///
/// StaticMesh is the engine's runtime mesh: a static vertex stream (52 bytes a vertex), an
/// index buffer, submeshes, bounds, and the geometry ops (normals, tangents, bounds) that
/// operate purely on the static stream.
///
/// SkinnedMesh derives from StaticMesh: it inherits the whole static stream and its ops
/// and adds only a parallel skinning stream (joints and weights, 24 bytes a vertex) and a
/// skeleton reference. Because the static data is byte-identical and in the same place, a
/// SkinnedMesh can be passed anywhere a StaticMesh& is expected: the static draw path
/// works as is, and the skinned path also binds the skinning stream. The virtual
/// IsSkinned()/SkinningStream() hooks let a consumer holding a StaticMesh& discover and
/// bind the skinning stream without RTTI.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.geometry:mesh;

import foundation.core;
import :types;
import :index_buffer;

using namespace foundation::core;

export namespace foundation::geometry
{

    class StaticMesh : public Object
    {
        RTTI_OBJECT(StaticMesh, Object)
    public:
        // A unique per-object id. Renderer caches key by this, never by pointer: a reloaded or
        // page-local mesh can be allocated at a freed address, and keyed by pointer it would
        // alias the dead mesh's GPU geometry.
        const u64 uid = NextUid();

        String name;
        Array<StaticMeshVertex> vertices;
        IndexBuffer indices{IndexBuffer::Format::U32};
        Array<SubMesh> subMeshes;
        AABB bounds = AABB::Empty();

        // The LOD chain. LOD 0 is `subMeshes`, so a consumer that knows nothing of LODs draws
        // the full mesh. Coarser levels reuse the shared vertex buffer and store their own
        // index ranges inside the one index buffer (concatenated after LOD 0's);
        // `lodSubMeshes` holds levels 1..lodCount-1 flattened as
        // (lod-1) * subMeshes.Size() + submesh. `lodCoverage[l]` is the normalized
        // screen-coverage threshold: level l is eligible while the projected coverage is
        // >= lodCoverage[l]. `lodCoverage[0]` is 1.0 by convention and unused by selection:
        // LOD 0 is the fallback, chosen for any coverage above lodCoverage[1]. A 1-LOD mesh
        // has lodCount == 1 and both arrays empty.
        u32 lodCount = 1;
        Array<SubMesh> lodSubMeshes;
        Array<f32> lodCoverage;

        /// Returns the submesh table of one LOD level: `subMeshes` for level 0, otherwise that
        /// level's slice of `lodSubMeshes`. An out-of-range level or a malformed table falls back
        /// to LOD 0, so bad data never crashes.
        [[nodiscard]] Span<const SubMesh> SubMeshesForLod(u32 lod) const noexcept
        {
            if (lod == 0 || lodCount <= 1 || subMeshes.IsEmpty())
            {
                return Span<const SubMesh>(subMeshes.Data(), subMeshes.Size());
            }
            const u32 level = (lod < lodCount) ? lod : (lodCount - 1);
            const usize per = subMeshes.Size();
            const usize offset = static_cast<usize>(level - 1) * per;
            if (offset + per > lodSubMeshes.Size())
            {
                return Span<const SubMesh>(subMeshes.Data(), subMeshes.Size());
            }
            return Span<const SubMesh>(lodSubMeshes.Data() + offset, per);
        }

        [[nodiscard]] u32 VertexCount() const noexcept { return static_cast<u32>(vertices.Size()); }
        [[nodiscard]] u32 IndexCount() const noexcept { return indices.Count(); }
        [[nodiscard]] static constexpr u32 VertexStride() noexcept
        {
            return sizeof(StaticMeshVertex);
        }

        // Raw static-stream bytes for GPU upload (null when empty).
        [[nodiscard]] const u8* VertexData() const noexcept
        {
            return vertices.IsEmpty() ? nullptr : reinterpret_cast<const u8*>(vertices.Data());
        }
        [[nodiscard]] u32 VertexDataSize() const noexcept { return VertexCount() * VertexStride(); }

        // Substitutability hooks: a consumer with a StaticMesh& can ask whether it is
        // really skinned and bind the parallel stream, no RTTI needed.
        [[nodiscard]] virtual bool IsSkinned() const noexcept { return false; }
        [[nodiscard]] virtual Span<const VertexSkinning> SkinningStream() const noexcept
        {
            return {};
        }

        // Resets the mesh to empty in place, so a hot-reload can re-populate the same
        // instance without invalidating outside references (GPU caches, renderers).
        virtual void ClearForReload()
        {
            name.Clear();
            vertices.Clear();
            indices.Clear();
            subMeshes.Clear();
            bounds = AABB::Empty();
            lodCount = 1;
            lodSubMeshes.Clear();
            lodCoverage.Clear();
        }

        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        // Recomputes `bounds` from the static stream.
        AABB& CalculateBounds()
        {
            if (vertices.IsEmpty())
            {
                bounds = AABB{Float3::Zero, Float3::Zero};
                return bounds;
            }
            bounds = AABB::Empty();
            for (const StaticMeshVertex& v : vertices)
            {
                bounds.Expand(v.position);
            }
            return bounds;
        }

        // Smooth normals: accumulate per-triangle face normals into shared vertices.
        void GenerateNormals()
        {
            const u32 triangles = TriangleCount();
            if (triangles == 0)
            {
                return;
            }
            for (StaticMeshVertex& v : vertices)
            {
                v.normal = Float3::Zero;
            }
            for (u32 t = 0; t < triangles; ++t)
            {
                const u32 i0 = Corner(t, 0), i1 = Corner(t, 1), i2 = Corner(t, 2);
                const Float3 e1 = vertices[i1].position - vertices[i0].position;
                const Float3 e2 = vertices[i2].position - vertices[i0].position;
                const Float3 faceNormal = Cross(e1, e2);
                vertices[i0].normal += faceNormal;
                vertices[i1].normal += faceNormal;
                vertices[i2].normal += faceNormal;
            }
            for (StaticMeshVertex& v : vertices)
            {
                v.normal =
                    (LengthSquared(v.normal) > 0.0001f) ? Normalized(v.normal) : Float3::UnitY;
            }
        }

        // Tangents for normal mapping: per-triangle (deltaUV-weighted) accumulation of tangent
        // and bitangent, then Gram-Schmidt orthogonalization against the normal. The bitangent
        // accumulation determines each vertex's handedness sign (tangent.w): mirrored-UV
        // triangles produce a bitangent opposing cross(N, T), so w flips to -1 there.
        void GenerateTangents()
        {
            GenerateTangentsImpl(Span<StaticMeshVertex>{vertices.Data(), vertices.Size()},
                                 TriangleCount(), [this](u32 t, u32 c) { return Corner(t, c); });
        }

        /// Span overload for raw vertex/index storage (the importer's source blobs, before a
        /// StaticMesh exists). Same math as the member version.
        static void GenerateTangents(Span<StaticMeshVertex> vertices, Span<const u32> indices)
        {
            GenerateTangentsImpl(vertices, static_cast<u32>(indices.Size() / 3),
                                 [indices](u32 t, u32 c) { return indices[t * 3u + c]; });
        }

    private:
        template <typename GetCorner>
        static void GenerateTangentsImpl(Span<StaticMeshVertex> vertices, u32 triangles,
                                         GetCorner corner)
        {
            if (triangles == 0)
            {
                return;
            }
            Array<Float3> tanAccum;
            Array<Float3> bitanAccum;
            tanAccum.Resize(vertices.Size(), Float3::Zero);
            bitanAccum.Resize(vertices.Size(), Float3::Zero);
            for (u32 t = 0; t < triangles; ++t)
            {
                const u32 i0 = corner(t, 0), i1 = corner(t, 1), i2 = corner(t, 2);
                const Float3 dp1 = vertices[i1].position - vertices[i0].position;
                const Float3 dp2 = vertices[i2].position - vertices[i0].position;
                const Float2 du1 = vertices[i1].texCoord - vertices[i0].texCoord;
                const Float2 du2 = vertices[i2].texCoord - vertices[i0].texCoord;
                const f32 denom = du1.x * du2.y - du2.x * du1.y;
                Float3 tangent = Float3::Zero;
                Float3 bitangent = Float3::Zero;
                if (Abs(denom) > 0.0001f)
                {
                    const f32 r = 1.0f / denom;
                    tangent = (dp1 * du2.y - dp2 * du1.y) * r;
                    bitangent = (dp2 * du1.x - dp1 * du2.x) * r;
                }
                tanAccum[i0] += tangent;
                tanAccum[i1] += tangent;
                tanAccum[i2] += tangent;
                bitanAccum[i0] += bitangent;
                bitanAccum[i1] += bitangent;
                bitanAccum[i2] += bitangent;
            }
            for (usize i = 0; i < vertices.Size(); ++i)
            {
                StaticMeshVertex& v = vertices[i];
                Float3 t = tanAccum[i];
                if (LengthSquared(t) > 0.0001f)
                {
                    t = t - v.normal * Dot(v.normal, t); // orthogonalize
                    t = (LengthSquared(t) > 0.0001f) ? Normalized(t) : DefaultTangent(v.normal);
                }
                else
                {
                    t = DefaultTangent(v.normal);
                }
                const f32 w = (Dot(Cross(v.normal, t), bitanAccum[i]) < 0.0f) ? -1.0f : 1.0f;
                v.tangent = Float4{t.x, t.y, t.z, w};
            }
        }

    public:
        // Pack a 0..1 float color to RGBA8 with R in the low byte (Unorm8x4 order).
        [[nodiscard]] static u32 PackColor(Float4 c)
        {
            const u32 r = static_cast<u32>(Clamp(c.x, 0.0f, 1.0f) * 255.0f);
            const u32 g = static_cast<u32>(Clamp(c.y, 0.0f, 1.0f) * 255.0f);
            const u32 b = static_cast<u32>(Clamp(c.z, 0.0f, 1.0f) * 255.0f);
            const u32 a = static_cast<u32>(Clamp(c.w, 0.0f, 1.0f) * 255.0f);
            return r | (g << 8) | (b << 16) | (a << 24);
        }
        [[nodiscard]] static u32 PackColor(Color32 c)
        {
            return static_cast<u32>(c.r) | (static_cast<u32>(c.g) << 8) |
                   (static_cast<u32>(c.b) << 16) | (static_cast<u32>(c.a) << 24);
        }

    protected:
        [[nodiscard]] u32 TriangleCount() const noexcept
        {
            const bool hasIndices = indices.Count() > 0;
            return hasIndices ? indices.Count() / 3u : VertexCount() / 3u;
        }
        // Vertex index of corner `c` (0..2) of triangle `t` (indexed or sequential).
        [[nodiscard]] u32 Corner(u32 t, u32 c) const noexcept
        {
            return (indices.Count() > 0) ? indices.Get(t * 3 + c) : (t * 3 + c);
        }
        [[nodiscard]] static Float3 DefaultTangent(Float3 normal)
        {
            const Float3 t = (Abs(normal.y) < 0.9f) ? Cross(normal, Float3::UnitY)
                                                    : Cross(normal, Float3::UnitX);
            return (LengthSquared(t) > 0.0001f) ? Normalized(t) : Float3::UnitX;
        }
    };

    // A skinned mesh: the static stream, a parallel skinning stream and a skeleton reference.
    class SkinnedMesh final : public StaticMesh
    {
        RTTI_OBJECT(SkinnedMesh, StaticMesh)
    public:
        Array<VertexSkinning> skinning; // parallel to StaticMesh::vertices
        i32 skeletonIndex = -1;         // index into the import skeleton list (-1 = none)

        [[nodiscard]] bool IsSkinned() const noexcept override { return true; }
        [[nodiscard]] Span<const VertexSkinning> SkinningStream() const noexcept override
        {
            return {skinning.Data(), skinning.Size()};
        }

        [[nodiscard]] static constexpr u32 SkinningStride() noexcept
        {
            return sizeof(VertexSkinning);
        }
        [[nodiscard]] const u8* SkinningData() const noexcept
        {
            return skinning.IsEmpty() ? nullptr : reinterpret_cast<const u8*>(skinning.Data());
        }
        [[nodiscard]] u32 SkinningDataSize() const noexcept
        {
            return static_cast<u32>(skinning.Size()) * SkinningStride();
        }

        void ClearForReload() override
        {
            StaticMesh::ClearForReload();
            skinning.Clear();
            skeletonIndex = -1;
        }
    };

    RTTI_DEFINE_OBJECT(StaticMesh, "rtti::geometry")
    RTTI_DEFINE_OBJECT(SkinnedMesh, "rtti::geometry")

} // namespace foundation::geometry

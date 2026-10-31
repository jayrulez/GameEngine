// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Geometry - the `:types` partition.
///
/// Value-type vocabulary for the engine's runtime mesh format (distinct from
/// foundation.model, which is the importer's representation of a loaded file): the
/// primitive topology, a submesh range, and the two vertex streams. A skinned mesh
/// reuses the static vertex stream and adds a *parallel* skinning stream, so its
/// static data is byte-identical to a static mesh (see :mesh).

module;
#include "Core/Prelude.h"

export module foundation.geometry:types;

import foundation.core;

using namespace foundation::core;

export namespace foundation::geometry
{

    // Primitive topology a submesh is drawn with.
    enum class PrimitiveType : u8
    {
        Triangles,
        TriangleStrip,
        TriangleFan,
        Lines,
        LineStrip,
        Points,
    };

    // A contiguous index range with its own material and topology.
    struct SubMesh
    {
        i32 startIndex = 0;
        i32 indexCount = 0;
        i32 materialIndex = 0;
        PrimitiveType primitiveType = PrimitiveType::Triangles;
    };

    // The static vertex stream: 52 bytes, matching VertexLayoutType::Mesh (locations
    // 0..4). Trivially copyable so the array uploads straight to a GPU vertex buffer.
    // tangent.w is the TBN handedness sign (+-1, glTF convention): the shader's bitangent is
    // cross(N, T.xyz) * T.w, so normal maps on mirrored-UV geometry light correctly.
    struct StaticMeshVertex
    {
        Float3 position{0, 0, 0};   // 12
        Float3 normal{0, 1, 0};     // 12
        Float2 texCoord{0, 0};      //  8
        u32 color = 0xFFFFFFFFu;    //  4  packed RGBA, R in the low byte (Unorm8x4)
        Float4 tangent{1, 0, 0, 1}; // 16  xyz = tangent, w = handedness
        // total: 52

        constexpr StaticMeshVertex() noexcept = default;
        constexpr StaticMeshVertex(Float3 pos, Float3 nrm, Float2 uv, u32 col, Float4 tan) noexcept
            : position(pos), normal(nrm), texCoord(uv), color(col), tangent(tan)
        {
        }
        // Float3-tangent convenience (procedural builders): handedness defaults to +1.
        constexpr StaticMeshVertex(Float3 pos, Float3 nrm, Float2 uv, u32 col, Float3 tan) noexcept
            : position(pos), normal(nrm), texCoord(uv), color(col),
              tangent(tan.x, tan.y, tan.z, 1.0f)
        {
        }
    };

    // The parallel skinning stream: 24 bytes (locations 6 and 7). One per static vertex; a
    // skinned mesh stores this alongside the inherited static stream rather than
    // interleaving, so the static stream stays substitutable for a static mesh.
    struct VertexSkinning
    {
        u16 joints[4] = {0, 0, 0, 0}; //  8  bone indices (uint16x4, packed uint32x2)
        Float4 weights{1, 0, 0, 0};   // 16  bone weights (sum to 1)
        // total: 24
    };

    // Size and alignment are a hard GPU-layout contract: these use the packed Float* math types
    // (tight, 4-byte aligned). A stray SIMD Vector* (16-byte aligned) would change both and trip
    // these.
    static_assert(sizeof(StaticMeshVertex) == 52,
                  "static vertex must stay 52 bytes (VertexLayoutType::Mesh)");
    static_assert(alignof(StaticMeshVertex) == 4,
                  "static vertex must stay 4-byte aligned (packed, not SIMD)");
    static_assert(sizeof(VertexSkinning) == 24,
                  "skinning stream must stay 24 bytes (locations 6/7)");
    static_assert(alignof(VertexSkinning) == 4,
                  "skinning stream must stay 4-byte aligned (packed, not SIMD)");

} // namespace foundation::geometry

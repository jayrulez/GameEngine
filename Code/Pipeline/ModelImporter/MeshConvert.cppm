// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter:mesh_convert - Model IR mesh -> geometry *Source.
///
/// Converts a `model::ModelMesh` (raw interleaved vertex bytes described by a
/// VertexElement layout + index buffer + material parts) into a cooked
/// `geometry::StaticMeshSource` / `SkinnedMeshSource`. Vertices are read BY SEMANTIC
/// (position/normal/uv/color/tangent[/joints/weights]) so any loader layout works;
/// the glTF/FBX loaders emit the canonical 48B (static) / 72B (skinned) interleave.
/// The model loaders always produce indexed geometry (non-indexed primitives get
/// sequential indices generated at load), so there is no non-indexed path here.

module;
#include "Core/Prelude.h"

export module modelimporter:mesh_convert;

import foundation.core;
import foundation.model;
import foundation.geometry;
import foundation.geometry.resource;

using namespace foundation::core;
using namespace foundation;
namespace core = foundation::core;
namespace model = foundation::model;
namespace geometry = foundation::geometry;

namespace pipeline
{

    // ---- per-element readers (by semantic; default if the element is absent) ----

    [[nodiscard]] const model::VertexElement* FindElement(Span<const model::VertexElement> elems,
                                                          model::VertexSemantic sem) noexcept
    {
        for (const model::VertexElement& e : elems)
        {
            if (e.semantic == sem)
            {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Float3 ReadVec3(const u8* vtx, const model::VertexElement* e,
                                  Float3 dflt) noexcept
    {
        if (e == nullptr)
        {
            return dflt;
        }
        Float3 r;
        MemCopy(&r, vtx + e->offset, sizeof(Float3));
        return r;
    }
    [[nodiscard]] Float2 ReadVec2(const u8* vtx, const model::VertexElement* e,
                                  Float2 dflt) noexcept
    {
        if (e == nullptr)
        {
            return dflt;
        }
        Float2 r;
        MemCopy(&r, vtx + e->offset, sizeof(Float2));
        return r;
    }
    [[nodiscard]] Float4 ReadVec4(const u8* vtx, const model::VertexElement* e,
                                  Float4 dflt) noexcept
    {
        if (e == nullptr)
        {
            return dflt;
        }
        Float4 r;
        MemCopy(&r, vtx + e->offset, sizeof(Float4));
        return r;
    }
    [[nodiscard]] u32 ReadU32(const u8* vtx, const model::VertexElement* e, u32 dflt) noexcept
    {
        if (e == nullptr)
        {
            return dflt;
        }
        u32 r;
        MemCopy(&r, vtx + e->offset, sizeof(u32));
        return r;
    }

    // ---- index conversion (Model IR is u16 or u32; cooked sources are u32) ----

    void CopyIndices(const model::ModelMesh& mesh, Array<u32>& out)
    {
        const i32 count = mesh.indexCount();
        out.Clear();
        out.Reserve(static_cast<usize>(count));
        const u8* data = mesh.getIndexData();
        if (data == nullptr || count <= 0)
        {
            return;
        }
        if (mesh.use32BitIndices())
        {
            const u32* src = reinterpret_cast<const u32*>(data);
            for (i32 i = 0; i < count; ++i)
            {
                out.PushBack(src[i]);
            }
        }
        else
        {
            const u16* src = reinterpret_cast<const u16*>(data);
            for (i32 i = 0; i < count; ++i)
            {
                out.PushBack(static_cast<u32>(src[i]));
            }
        }
    }

    // ---- parts -> submesh ranges ----

    // The material slots ONE mesh uses: the distinct model-wide material indices of its
    // parts, in first-appearance order. A cooked submesh's materialIndex is the position in
    // THIS list (a per-mesh slot), never the model-wide index - so the entity's material
    // list holds only what the mesh draws (Bistro: 254 materials per node before this,
    // 2909 nodes; the material a submesh needs is one or two of them).
    void CollectMeshMaterialSlotsImpl(const model::ModelMesh& mesh, Array<i32>& out)
    {
        out.Clear();
        for (const model::ModelMeshPart& p : mesh.parts())
        {
            if (p.materialIndex < 0)
            {
                continue;
            }
            bool seen = false;
            for (const i32 slot : out)
            {
                if (slot == p.materialIndex)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                out.PushBack(p.materialIndex);
            }
        }
    }

    void CopyParts(const model::ModelMesh& mesh, geometry::StaticMeshSource& out)
    {
        out.subStart.Clear();
        out.subCount.Clear();
        out.subMaterial.Clear();
        out.subPrim.Clear();
        const Span<const model::ModelMeshPart> parts = mesh.parts();
        if (parts.Size() == 0)
        {
            // No explicit parts: one submesh covering the whole index buffer.
            out.subStart.PushBack(0);
            out.subCount.PushBack(mesh.indexCount());
            out.subMaterial.PushBack(-1);
            out.subPrim.PushBack(static_cast<u8>(geometry::PrimitiveType::Triangles));
            return;
        }
        Array<i32> slots;
        CollectMeshMaterialSlotsImpl(mesh, slots);
        for (const model::ModelMeshPart& p : parts)
        {
            i32 local = -1;
            for (usize i = 0; i < slots.Size(); ++i)
            {
                if (slots[i] == p.materialIndex)
                {
                    local = static_cast<i32>(i);
                    break;
                }
            }
            out.subStart.PushBack(p.indexStart);
            out.subCount.PushBack(p.indexCount);
            out.subMaterial.PushBack(local); // the per-mesh slot (-1 = no material)
            out.subPrim.PushBack(static_cast<u8>(geometry::PrimitiveType::Triangles));
        }
    }

    export {

        /// The model-wide material indices one mesh's parts use, first-appearance order: the
        /// cooked submeshes' materialIndex values are positions in this list, and the manifest
        /// records it per mesh so an entity binds only these materials.
        void CollectMeshMaterialSlots(const model::ModelMesh& mesh, Array<i32>& out)
        {
            CollectMeshMaterialSlotsImpl(mesh, out);
        }

        // model::TextureWrap -> rhi::AddressMode VALUE (as u8; enum orders differ - model has
        // {Repeat, ClampToEdge, MirroredRepeat}, rhi has {Repeat, MirrorRepeat, ClampToEdge}).
        [[nodiscard]] u8 AddressModeFromWrap(model::TextureWrap wrap)
        {
            switch (wrap)
            {
            case model::TextureWrap::ClampToEdge:
                return 2; // rhi::AddressMode::ClampToEdge
            case model::TextureWrap::MirroredRepeat:
                return 1; // rhi::AddressMode::MirrorRepeat
            case model::TextureWrap::Repeat:
            default:
                return 0; // rhi::AddressMode::Repeat
            }
        }

        // The sampler modes a material should use: its base-color texture's sampler (fallback:
        // first slot with one). glTF's empty-sampler default is Repeat/Repeat.
        void MaterialSamplerModes(const model::Model& mdl, const model::ModelMaterial& mat,
                                  u8& outU, u8& outV)
        {
            outU = 0;
            outV = 0;
            const i32 texIndices[] = {mat.baseColorTextureIndex, mat.normalTextureIndex,
                                      mat.metallicRoughnessTextureIndex, mat.emissiveTextureIndex,
                                      mat.occlusionTextureIndex, mat.specularTextureIndex};
            const Span<const model::TextureSampler> samplers = mdl.samplers();
            for (i32 t : texIndices)
            {
                if (t < 0 || static_cast<usize>(t) >= mdl.textures().Size())
                {
                    continue;
                }
                const i32 s = mdl.textures()[static_cast<usize>(t)]->samplerIndex;
                if (s < 0 || static_cast<usize>(s) >= samplers.Size())
                {
                    continue;
                }
                outU = AddressModeFromWrap(samplers[static_cast<usize>(s)].wrapS);
                outV = AddressModeFromWrap(samplers[static_cast<usize>(s)].wrapT);
                return;
            }
        }

        // Asset name for an imported sub-object: the authored name, sanitized for file paths
        // (instance names become envelope/sidecar file names), else "{fallback}.{index}".
        [[nodiscard]] String ImportedAssetName(core::StringView authored, core::StringView fallback,
                                               usize index)
        {
            String out;
            for (usize i = 0; i < authored.Size(); ++i)
            {
                utf8char c = authored.Data()[i];
                if (c == u8'/' || c == u8'\\' || c == u8':' || c == u8'*' || c == u8'?' ||
                    c == u8'"' || c == u8'<' || c == u8'>' || c == u8'|' || c < 0x20)
                {
                    c = u8'_';
                }
                out.PushBack(c);
            }
            if (out.IsEmpty())
            {
                out = Format(u8"{}.{}", fallback, index);
            }
            return out;
        }

        // Display name for an imported texture: the authored texture/image name when present,
        // else the URI's file stem (Default_albedo.jpg -> Default_albedo), else "tex.{index}"
        // (embedded textures with no identity). Callers unique-ify per destination group.
        [[nodiscard]] String ImportedTextureName(const model::ModelTexture& texture, usize index)
        {
            // FILE STEM first, authored image name as the fallback (embedded GLB images have
            // no uri). The file is the identity the user sees on disk and greps for; authored
            // names can lie about it - Poly Haven names its packed arm textures "*_rough",
            // which imported an asset whose name matched neither the file nor its content.
            StringView base;
            StringView uri = texture.uri();
            if (!uri.IsEmpty())
            {
                usize start = 0;
                for (usize i = uri.Size(); i > 0; --i)
                {
                    const utf8char c = uri.Data()[i - 1];
                    if (c == u8'/' || c == u8'\\')
                    {
                        start = i;
                        break;
                    }
                }
                usize end = uri.Size();
                for (usize i = uri.Size(); i > start; --i)
                {
                    if (uri.Data()[i - 1] == u8'.')
                    {
                        end = i - 1;
                        break;
                    }
                }
                if (end > start)
                {
                    base = uri.SubStr(start, end - start);
                }
            }
            if (base.IsEmpty())
            {
                base = texture.name();
            }
            return ImportedAssetName(base, u8"tex", index);
        }

        // Fill a StaticMeshSource from a model mesh's static streams (pos/normal/uv/color/tangent).
        // Authored chains: "Foo_LOD1" / "Foo_lod2" attach as chain levels of
        // "Foo". Returns the level (>= 1) and writes the stripped base name; 0 = a plain
        // name (including "_LOD0" - the base spells itself plainly, never with a suffix).
        [[nodiscard]] inline u32 ParseLodSuffix(StringView name, String& outBase)
        {
            const usize n = name.Size();
            usize digits = 0;
            while (digits < n && name[n - 1 - digits] >= u8'0' && name[n - 1 - digits] <= u8'9')
            {
                ++digits;
            }
            if (digits == 0 || digits > 2 || n < digits + 4)
            {
                return 0;
            }
            const usize tag = n - digits - 4; // "_LOD" (case-insensitive)
            const auto lower = [](utf8char c)
            { return (c >= u8'A' && c <= u8'Z') ? static_cast<utf8char>(c + 32) : c; };
            if (name[tag] != u8'_' || lower(name[tag + 1]) != u8'l' ||
                lower(name[tag + 2]) != u8'o' || lower(name[tag + 3]) != u8'd')
            {
                return 0;
            }
            u32 level = 0;
            for (usize i = 0; i < digits; ++i)
            {
                level = level * 10 + static_cast<u32>(name[n - digits + i] - u8'0');
            }
            if (level == 0)
            {
                return 0; // "_LOD0" is the base itself
            }
            outBase = String(name.SubStr(0, tag));
            return level;
        }

        void StaticMeshSourceFromModel(const model::ModelMesh& mesh,
                                       geometry::StaticMeshSource& out); // defined below

        // Append `lodMesh` as the NEXT chain level of `base`: its vertices join the shared
        // blob, its indices (offset by the pre-append vertex count) join the one index
        // buffer, and one range lands per part. The level's part count must MATCH the
        // base's submesh count (the material layout is per-chain; a level never resorts) -
        // mismatch returns false with `base` unchanged. Coverage thresholds default to a
        // halving ladder (LOD1 at 0.25 of the viewport half-height, LOD2 at 0.125, ...).
        void SkinnedMeshSourceFromModel(const model::ModelMesh& mesh, i32 skeletonIndex,
                                        geometry::SkinnedMeshSource& out); // defined below

        // The shared append core over an ALREADY-CONVERTED level source.
        [[nodiscard]] inline bool AppendConvertedLodLevel(const geometry::StaticMeshSource& level,
                                                          geometry::StaticMeshSource& base)
        {
            if (level.subStart.Size() != base.subStart.Size() || base.subStart.IsEmpty())
            {
                return false;
            }
            constexpr usize kStride = sizeof(geometry::StaticMeshVertex);
            const u32 baseVertexCount = static_cast<u32>(base.vertexBlob.Size() / kStride);
            const u32 indexBase = static_cast<u32>(base.indexData.Size());
            const usize oldBlobSize = base.vertexBlob.Size();
            base.vertexBlob.Resize(oldBlobSize + level.vertexBlob.Size());
            if (!level.vertexBlob.IsEmpty())
            {
                MemCopy(base.vertexBlob.Data() + oldBlobSize, level.vertexBlob.Data(),
                        level.vertexBlob.Size());
            }
            for (const u32 index : level.indexData)
            {
                base.indexData.PushBack(index + baseVertexCount);
            }
            if (base.lodCount <= 1)
            {
                base.lodCount = 1;
                base.lodCoverage.Clear();
                base.lodCoverage.PushBack(1.0f);
            }
            for (usize s = 0; s < level.subStart.Size(); ++s)
            {
                base.lodStart.PushBack(static_cast<i32>(indexBase) + level.subStart[s]);
                base.lodIndexCount.PushBack(level.subCount[s]);
            }
            base.lodCoverage.PushBack(0.25f * Pow(0.5f, static_cast<f32>(base.lodCount - 1)));
            base.lodCount += 1;
            return true;
        }

        [[nodiscard]] inline bool AppendLodLevelFromModel(const model::ModelMesh& lodMesh,
                                                          geometry::StaticMeshSource& base)
        {
            geometry::StaticMeshSource level;
            StaticMeshSourceFromModel(lodMesh, level);
            return AppendConvertedLodLevel(level, base);
        }

        // SKINNED chains: the level's parallel skinning stream appends in lockstep with its
        // vertices (both streams grow by the level's vertex count - they cannot diverge).
        // A level whose stream does not match its vertex count is refused.
        [[nodiscard]] inline bool AppendLodLevelFromModel(const model::ModelMesh& lodMesh,
                                                          geometry::SkinnedMeshSource& base)
        {
            geometry::SkinnedMeshSource level;
            SkinnedMeshSourceFromModel(lodMesh, base.skeletonIndex, level);
            constexpr usize kStride = sizeof(geometry::StaticMeshVertex);
            constexpr usize kSkinStride = sizeof(geometry::VertexSkinning);
            const usize levelVertices = level.vertexBlob.Size() / kStride;
            if (level.skinningBlob.Size() != levelVertices * kSkinStride)
            {
                return false;
            }
            if (!AppendConvertedLodLevel(level, base))
            {
                return false;
            }
            const usize oldSize = base.skinningBlob.Size();
            base.skinningBlob.Resize(oldSize + level.skinningBlob.Size());
            if (!level.skinningBlob.IsEmpty())
            {
                MemCopy(base.skinningBlob.Data() + oldSize, level.skinningBlob.Data(),
                        level.skinningBlob.Size());
            }
            return true;
        }

        void StaticMeshSourceFromModel(const model::ModelMesh& mesh,
                                       geometry::StaticMeshSource& out)
        {
            out.name = String(mesh.name());

            const Span<const model::VertexElement> elems = mesh.vertexElements();
            const model::VertexElement* ePos = FindElement(elems, model::VertexSemantic::Position);
            const model::VertexElement* eNrm = FindElement(elems, model::VertexSemantic::Normal);
            const model::VertexElement* eUv = FindElement(elems, model::VertexSemantic::TexCoord);
            const model::VertexElement* eCol = FindElement(elems, model::VertexSemantic::Color);
            const model::VertexElement* eTan = FindElement(elems, model::VertexSemantic::Tangent);

            const i32 count = mesh.vertexCount();
            const i32 stride = mesh.vertexStride();
            const u8* base = mesh.getVertexData();

            out.vertexBlob.Clear();
            out.vertexBlob.Resize(static_cast<usize>(count) * sizeof(geometry::StaticMeshVertex));
            auto* dst = reinterpret_cast<geometry::StaticMeshVertex*>(out.vertexBlob.Data());
            for (i32 i = 0; i < count; ++i)
            {
                const u8* v = base + static_cast<usize>(i) * static_cast<usize>(stride);
                geometry::StaticMeshVertex sv{};
                sv.position = ReadVec3(v, ePos, Float3{0, 0, 0});
                sv.normal = ReadVec3(v, eNrm, Float3{0, 1, 0});
                sv.texCoord = ReadVec2(v, eUv, Float2{0, 0});
                sv.color = ReadU32(v, eCol, 0xFFFFFFFFu);
                sv.tangent = ReadVec4(v, eTan, Float4{1, 0, 0, 1}); // w = TBN handedness
                dst[i] = sv;
            }

            CopyIndices(mesh, out.indexData);
            CopyParts(mesh, out);

            // No authored tangent stream (e.g. DamagedHelmet.gltf ships only NORMAL+TEXCOORD_0):
            // generate them, or every vertex keeps the {1,0,0,+1} default and the TBN is garbage
            // everywhere - normal-mapped materials then shade wrong across the whole mesh.
            if (eTan == nullptr && eNrm != nullptr && eUv != nullptr && !out.indexData.IsEmpty())
            {
                geometry::StaticMesh::GenerateTangents(
                    Span<geometry::StaticMeshVertex>{dst, static_cast<usize>(count)},
                    Span<const u32>{out.indexData.Data(), out.indexData.Size()});
            }
        }

        // Fill a SkinnedMeshSource: the static streams above + the parallel skinning stream
        // (joints u16x4 + weights) and the owning skeleton index.
        void SkinnedMeshSourceFromModel(const model::ModelMesh& mesh, i32 skeletonIndex,
                                        geometry::SkinnedMeshSource& out)
        {
            StaticMeshSourceFromModel(mesh, out);
            out.skeletonIndex = skeletonIndex;

            const Span<const model::VertexElement> elems = mesh.vertexElements();
            const model::VertexElement* eJnt = FindElement(elems, model::VertexSemantic::Joints);
            const model::VertexElement* eWt = FindElement(elems, model::VertexSemantic::Weights);

            const i32 count = mesh.vertexCount();
            const i32 stride = mesh.vertexStride();
            const u8* base = mesh.getVertexData();

            out.skinningBlob.Clear();
            out.skinningBlob.Resize(static_cast<usize>(count) * sizeof(geometry::VertexSkinning));
            auto* dst = reinterpret_cast<geometry::VertexSkinning*>(out.skinningBlob.Data());
            for (i32 i = 0; i < count; ++i)
            {
                const u8* v = base + static_cast<usize>(i) * static_cast<usize>(stride);
                geometry::VertexSkinning vs{};
                if (eJnt != nullptr)
                {
                    MemCopy(vs.joints, v + eJnt->offset, sizeof(vs.joints));
                }
                vs.weights = ReadVec4(v, eWt, Float4{1, 0, 0, 0});
                dst[i] = vs;
            }
        }

    } // export

    // ============================================================================================
    // Shared texture-import helpers (used by BOTH the drag-drop fan-out and the cook path).
    // ============================================================================================
    export {

        // Classify each model texture's color space by USAGE: data maps (normal / packed or separate
        // metal-rough / occlusion) must stay LINEAR - sRGB-decoding them corrupts the values (a flat
        // normal 0.5 would linearize to ~0.21). Color maps (albedo, emissive) are sRGB-encoded.
        // A texture referenced both ways classifies as linear (data correctness wins; rare).
        // Which textures a material binds in its NORMAL slot (the usage a file-backed texture
        // asset carries; the other linear slots - MR / rough / metal / AO - are data masks).
        inline void ClassifyNormalTextures(const model::Model& mdl, Array<bool>& outNormal)
        {
            outNormal.Clear();
            outNormal.Resize(mdl.textures().Size(), false);
            for (const model::ModelMaterial* m : mdl.materials())
            {
                if (m != nullptr && m->normalTextureIndex >= 0 &&
                    static_cast<usize>(m->normalTextureIndex) < outNormal.Size())
                {
                    outNormal[static_cast<usize>(m->normalTextureIndex)] = true;
                }
            }
        }

        inline void ClassifyLinearTextures(const model::Model& mdl, Array<bool>& outLinear)
        {
            outLinear.Clear();
            outLinear.Resize(mdl.textures().Size(), false);
            const auto mark = [&](i32 index)
            {
                if (index >= 0 && static_cast<usize>(index) < outLinear.Size())
                {
                    outLinear[static_cast<usize>(index)] = true;
                }
            };
            for (const model::ModelMaterial* m : mdl.materials())
            {
                if (m == nullptr)
                {
                    continue;
                }
                mark(m->normalTextureIndex);
                mark(m->metallicRoughnessTextureIndex);
                mark(m->separateRoughnessTextureIndex);
                mark(m->separateMetalnessTextureIndex);
                mark(m->occlusionTextureIndex);
            }
        }

        // Bake a glTF-style PACKED metallic-roughness texture (G = roughness, B = metalness; R = A =
        // 255) from FBX's separate grayscale maps. Missing maps bake as 255 (identity: the factor
        // carries the value). Sizes may differ - the output takes the larger and nearest-samples.
        // Empty result = neither source usable (caller falls back to factors only).
        [[nodiscard]] inline Array<u8> BakePackedMetallicRoughness(const model::Model& mdl,
                                                                   i32 roughnessIdx,
                                                                   i32 metalnessIdx, u32& outW,
                                                                   u32& outH)
        {
            const auto fetch = [&](i32 index) -> const model::ModelTexture*
            {
                if (index < 0 || static_cast<usize>(index) >= mdl.textures().Size())
                {
                    return nullptr;
                }
                const model::ModelTexture* t = mdl.textures()[static_cast<usize>(index)];
                const bool rgba8 = t != nullptr && t->getData() != nullptr && t->width > 0 &&
                                   t->height > 0 && t->getDataSize() == t->width * t->height * 4;
                return rgba8 ? t : nullptr;
            };
            const model::ModelTexture* rough = fetch(roughnessIdx);
            const model::ModelTexture* metal = fetch(metalnessIdx);
            outW = 0;
            outH = 0;
            if (rough == nullptr && metal == nullptr)
            {
                return Array<u8>{};
            }

            outW = static_cast<u32>(
                Max(rough != nullptr ? rough->width : 0, metal != nullptr ? metal->width : 0));
            outH = static_cast<u32>(
                Max(rough != nullptr ? rough->height : 0, metal != nullptr ? metal->height : 0));
            const auto sample = [&](const model::ModelTexture* t, u32 x, u32 y) -> u8
            {
                if (t == nullptr)
                {
                    return 255u;
                } // identity: the scalar factor carries the value
                const u32 sx = (outW > 1) ? (x * static_cast<u32>(t->width)) / outW : 0u;
                const u32 sy = (outH > 1) ? (y * static_cast<u32>(t->height)) / outH : 0u;
                return t->getData()[(static_cast<usize>(sy) * static_cast<usize>(t->width) + sx) *
                                    4]; // grayscale: R
            };
            Array<u8> pixels;
            pixels.Resize(static_cast<usize>(outW) * outH * 4);
            for (u32 y = 0; y < outH; ++y)
            {
                for (u32 x = 0; x < outW; ++x)
                {
                    u8* px = pixels.Data() + (static_cast<usize>(y) * outW + x) * 4;
                    px[0] = 255u;                // R unused (ORM-style occlusion would live here)
                    px[1] = sample(rough, x, y); // G = roughness
                    px[2] = sample(metal, x, y); // B = metalness
                    px[3] = 255u;
                }
            }
            return pixels;
        }
    } // export

} // namespace pipeline

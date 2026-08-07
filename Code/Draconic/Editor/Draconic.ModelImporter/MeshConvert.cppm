/// Draconic::ModelImporter:mesh_convert - Model IR mesh -> geometry *Source.
///
/// Converts a `model::ModelMesh` (raw interleaved vertex bytes described by a
/// VertexElement layout + index buffer + material parts) into a cooked
/// `geometry::StaticMeshSource` / `SkinnedMeshSource`. Vertices are read BY SEMANTIC
/// (position/normal/uv/color/tangent[/joints/weights]) so any loader layout works;
/// the glTF/FBX loaders emit the canonical 48B (static) / 72B (skinned) interleave.
/// The model loaders always produce indexed geometry (non-indexed primitives get
/// sequential indices generated at load), so there is no non-indexed path here.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.modelimporter:mesh_convert;

import draconic.foundation;
import draconic.model;
import draconic.geometry;
import draconic.geometry.resource;

using namespace draconic::foundation;
namespace model = draconic::model;
namespace geometry = draconic::geometry;

namespace draconic::modelimporter
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
        for (const model::ModelMeshPart& p : parts)
        {
            out.subStart.PushBack(p.indexStart);
            out.subCount.PushBack(p.indexCount);
            out.subMaterial.PushBack(p.materialIndex);
            out.subPrim.PushBack(static_cast<u8>(geometry::PrimitiveType::Triangles));
        }
    }

    export {

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
                                      mat.occlusionTextureIndex};
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
        [[nodiscard]] String ImportedAssetName(foundation::StringView authored, foundation::StringView fallback,
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
            StringView base = texture.name();
            if (base.IsEmpty())
            {
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
            }
            return ImportedAssetName(base, u8"tex", index);
        }

        // Fill a StaticMeshSource from a model mesh's static streams (pos/normal/uv/color/tangent).
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

} // namespace draconic::modelimporter

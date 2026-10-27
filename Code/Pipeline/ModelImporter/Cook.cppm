// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter:cook - cook a loaded Model into a content database.
///
/// Cooks a model's textures, materials, and meshes through the editor stack (Asset ->
/// builder -> content Instance) into the output DB, then writes a manifest
/// (ModelManifestSource: resource Guids + node hierarchy + cross-refs) and returns its
/// Guid. The runtime binds the manifest as a ModelResource (a composite pulling in the
/// meshes + materials via dependency edges) and spawns from it.
///
/// v1 cooks every mesh as STATIC (skinned meshes render in bind pose) and binds the
/// albedo texture per material (other PBR maps + skinning land next). Materials use the
/// renderer's builtin "forward" shader by name (no cooked ShaderResource needed yet).

module;
#include "Core/Prelude.h"

export module modelimporter:cook;

import foundation.core;
import foundation.rhi;
import foundation.model;
import foundation.geometry;
import foundation.geometry.resource;
import geometry.pipeline;
import foundation.materials;
import foundation.materials.resource;
import materials.pipeline;
import foundation.texture.resource;
import foundation.image;
import foundation.image.io;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.content;
import pipeline.core;
import :mesh_convert;
import :anim_convert;
import foundation.model.resource;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace model = foundation::model;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace texture = foundation::texture;
namespace animation = foundation::animation;
namespace content = foundation::content;

export namespace pipeline
{
    // The cooked-model runtime types now live in foundation::model (foundation.model.resource).
    using foundation::model::ModelManifestSource;
    using foundation::model::ModelNode;
    using foundation::model::ModelResource;

    // True if the model mesh carries skinning (Joints/Weights vertex elements).
    [[nodiscard]] inline bool IsSkinnedMesh(const model::ModelMesh& mesh) noexcept
    {
        for (const model::VertexElement& e : mesh.vertexElements())
        {
            if (e.semantic == model::VertexSemantic::Joints)
            {
                return true;
            }
        }
        return false;
    }

    // Cook the model's textures into outDb. The model loaders DECODE every texture into raw
    // RGBA8 pixels (storeImageData) for external files, data-URIs, AND embedded GLB buffer-views
    // alike, so we cook directly from ModelTexture's pixel bytes - no file re-read, and embedded
    // textures work. Returns one Guid per model texture (nil if it has no usable RGBA8 data).
    inline void CookTextures(const model::Model& model, content::Group* root,
                             StringView namePrefix, Array<Guid>& outGuids)
    {
        Array<bool> linear;
        ClassifyLinearTextures(model, linear);
        const Span<model::ModelTexture* const> textures = model.textures();
        for (usize i = 0; i < textures.Size(); ++i)
        {
            const model::ModelTexture& t = *textures[i];
            const u8* data = t.getData();
            i32 size = t.getDataSize();
            u32 width = t.width > 0 ? static_cast<u32>(t.width) : 0u;
            u32 height = t.height > 0 ? static_cast<u32>(t.height) : 0u;
            // A texture the loader left on disk (a DDS) decodes here: this direct path has no
            // pass-through (that is the asset pipeline's), so level 0 cooks as RGBA8.
            foundation::image::Image decoded;
            if (data == nullptr && !t.sourceFile().IsEmpty())
            {
                Result<Array<byte>> bytes = ReadFile(t.sourceFile());
                if (bytes.HasValue() &&
                    foundation::image::io::LoadImageFromMemory(
                        Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                       bytes.Value().Size()),
                        decoded)
                        .IsOk() &&
                    decoded.Format() == foundation::image::PixelFormat::RGBA8)
                {
                    data = decoded.PixelData().Data();
                    size = static_cast<i32>(decoded.PixelData().Size());
                    width = decoded.Width();
                    height = decoded.Height();
                }
            }
            // The loaders decode to RGBA8 (4 bpp); guard against any other layout.
            const bool rgba8 = (data != nullptr && width > 0 && height > 0 &&
                                static_cast<u64>(size) == static_cast<u64>(width) * height * 4);
            if (!rgba8)
            {
                outGuids.PushBack(Guid{});
                continue;
            }

            texture::TextureResource res;
            res.width = width;
            res.height = height;
            // Color space follows USAGE: data maps (normal/MR/AO) stay linear (sRGB-decoding
            // corrupts them); color maps (albedo/emissive) are sRGB-encoded.
            res.format = (i < linear.Size() && linear[i]) ? rhi::TextureFormat::RGBA8Unorm
                                                          : rhi::TextureFormat::RGBA8UnormSrgb;
            res.mipLevels = 1; // factory uploads mip 0 (no mip gen yet)
            res.generateMipmaps = false;

            String texName = ImportedTextureName(t, i);
            const String name =
                root->UniqueInstanceName(Format(u8"{}.{}", namePrefix, texName).AsView());
            content::Instance* inst =
                root->CreateInstance(name.AsView(), texture::TextureResource::StaticType());
            if (inst == nullptr)
            {
                outGuids.PushBack(Guid{});
                continue;
            }
            if (!inst->WriteObject(res).IsOk())
            {
                outGuids.PushBack(Guid{});
                continue;
            }
            const Status ds =
                inst->WriteData(u8"data", Span<const byte>{reinterpret_cast<const byte*>(data),
                                                           static_cast<usize>(size)});
            outGuids.PushBack(ds.IsOk() ? inst->Id() : Guid{});
        }
    }

    // A model's material as a material asset on the built-in its shading model names
    // (Specs/phong-material.md), as its file authored it: PBR its factors and its five maps (FBX's
    // separate metal and rough maps baked into one packed map by `packedMetallicRoughness`, the
    // caller's, as each cook makes textures its own way); Phong its diffuse and highlight, Lambert
    // its diffuse alone, each with its normal and emissive maps; Unlit its colour and map. Then
    // the authored pipeline state and the samplers. Every texture the material reads is wired into
    // its source, so a material picked on its own draws fully textured. Both cooks (the file
    // import's and the direct one) make their materials here.
    inline void BuildModelMaterial(const model::Model& model, const model::ModelMaterial& m, StringView name,
                                   const Array<Guid>& textureGuids,
                                   const Function<Guid(i32 roughIndex, i32 metalIndex)>& packedMetallicRoughness,
                                   pipeline::MaterialAsset& asset)
    {
        RefPtr<materials::Material> built;
        switch (m.shadingModel)
        {
        case model::ShadingModel::Phong:
        case model::ShadingModel::Lambert:
        {
            const bool highlight = m.shadingModel == model::ShadingModel::Phong;
            const Float4 specular = highlight ? Float4{m.specularColor.x, m.specularColor.y, m.specularColor.z, 1.0f} : Float4{0.0f, 0.0f, 0.0f, 1.0f};
            built = materials::CreatePhong(name, m.baseColorFactor, specular, m.shininess);
            break;
        }
        case model::ShadingModel::Unlit:
            built = materials::CreateUnlit(name, m.baseColorFactor);
            break;
        case model::ShadingModel::Pbr:
        default:
            built = materials::CreatePBR(name, m.baseColorFactor, m.metallicFactor, m.roughnessFactor);
            built->SetDefaultFloat(u8"OcclusionStrength", m.occlusionStrength);
            break;
        }
        // What the built-ins share by name (a property a built-in lacks is passed by).
        built->SetDefaultColor(u8"EmissiveColor", Float4{m.emissiveFactor.x, m.emissiveFactor.y, m.emissiveFactor.z,
                                                         m.emissiveIntensity}); // a ColorHdr: sRGB colour, intensity in w
        built->SetDefaultFloat(u8"NormalScale", m.normalScale);
        built->SetDefaultFloat(u8"AlphaCutoff", m.alphaCutoff);
        pipeline::MaterialImporter::Import(*built, Guid{}, asset); // nil shaderId: the built-in by name
        asset.source.shaderName = String(built->shaderName.AsView());

        const auto wire = [&](i32 texIdx, StringView slot)
        {
            if (texIdx >= 0 && static_cast<usize>(texIdx) < textureGuids.Size() && !textureGuids[static_cast<usize>(texIdx)].IsNil())
            {
                asset.source.textureSlots.PushBack(String(slot));
                asset.source.textureIds.PushBack(textureGuids[static_cast<usize>(texIdx)]);
            }
        };
        wire(m.baseColorTextureIndex, u8"AlbedoMap");
        if (m.shadingModel != model::ShadingModel::Unlit)
        {
            wire(m.normalTextureIndex, u8"NormalMap");
            wire(m.emissiveTextureIndex, u8"EmissiveMap");
        }
        if (m.shadingModel == model::ShadingModel::Phong)
            wire(m.specularTextureIndex, u8"SpecularMap");
        if (m.shadingModel == model::ShadingModel::Pbr)
        {
            wire(m.occlusionTextureIndex, u8"OcclusionMap");
            // Metallic-roughness: glTF's packed texture wires directly; FBX's separate grayscale
            // maps bake into a packed one (G = rough, B = metal): feeding either into the packed
            // slot directly would bleed across channels.
            if (m.metallicRoughnessTextureIndex >= 0)
                wire(m.metallicRoughnessTextureIndex, u8"MetallicRoughnessMap");
            else if ((m.separateRoughnessTextureIndex >= 0 || m.separateMetalnessTextureIndex >= 0) && packedMetallicRoughness)
            {
                const Guid packed = packedMetallicRoughness(m.separateRoughnessTextureIndex, m.separateMetalnessTextureIndex);
                if (!packed.IsNil())
                {
                    asset.source.textureSlots.PushBack(String(u8"MetallicRoughnessMap"));
                    asset.source.textureIds.PushBack(packed);
                }
            }
        }
        // Authored pipeline state: alpha mode -> blend (Mask = alpha-tested cutout with holey
        // shadows; Blend = the transparent pass) and double-sided -> no culling.
        if (m.alphaMode == model::AlphaMode::Mask)
            asset.source.blendMode = materials::BlendMode::Masked;
        else if (m.alphaMode == model::AlphaMode::Blend)
            asset.source.blendMode = materials::BlendMode::AlphaBlend;
        if (m.doubleSided)
            asset.source.cullMode = materials::CullModeConfig::None;
        MaterialSamplerModes(model, m, asset.source.samplerU, asset.source.samplerV);
    }

    // Cook the model's materials into outDb (each on its built-in, BuildModelMaterial, then the
    // editor cook). Records, per material, its cooked Guid + the albedo texture Guid (resolved
    // from textureGuids).
    inline void CookMaterials(IAllocator& allocator, const model::Model& model,
                              content::Group* root, StringView namePrefix,
                              const Array<Guid>& textureGuids, Array<Guid>& outMatGuids,
                              Array<Guid>& outAlbedo)
    {
        pipeline::MaterialAssetBuilder builder;
        HashMap<u64, Guid> bakedMR; // per-pair bake cache (materials often share maps)
        // FBX's separate metal and rough maps baked into one packed texture product, once a pair.
        const Function<Guid(i32, i32)> packedMR = [&](i32 roughIndex, i32 metalIndex) -> Guid
        {
            const u64 key = (static_cast<u64>(static_cast<u32>(roughIndex)) << 32) | static_cast<u64>(static_cast<u32>(metalIndex));
            if (const Guid* hit = bakedMR.Find(key))
                return *hit;
            Guid packed;
            u32 w = 0, h = 0;
            Array<u8> pixels = BakePackedMetallicRoughness(model, roughIndex, metalIndex, w, h);
            if (!pixels.IsEmpty())
            {
                texture::TextureResource res;
                res.width = w;
                res.height = h;
                res.format = rhi::TextureFormat::RGBA8Unorm; // data map: linear
                res.mipLevels = 1;
                res.generateMipmaps = false;
                const String texName = Format(u8"{}.mr.packed.{}.{}", namePrefix, roughIndex, metalIndex);
                content::Instance* texInst = root->CreateInstance(texName.AsView(), texture::TextureResource::StaticType());
                if (texInst != nullptr && texInst->WriteObject(res).IsOk() &&
                    texInst->WriteData(u8"data", Span<const byte>{reinterpret_cast<const byte*>(pixels.Data()), pixels.Size()}).IsOk())
                    packed = texInst->Id();
            }
            bakedMR.InsertOrAssign(key, packed);
            return packed;
        };
        const Span<model::ModelMaterial* const> materials = model.materials();
        for (usize i = 0; i < materials.Size(); ++i)
        {
            const model::ModelMaterial& m = *materials[i];
            pipeline::MaterialAsset asset;
            BuildModelMaterial(model, m, Format(u8"{}.{}", namePrefix, ImportedAssetName(m.name(), u8"mat", i)).AsView(), textureGuids,
                               packedMR, asset);

            const String name = root->UniqueInstanceName(
                Format(u8"{}.{}", namePrefix, ImportedAssetName(m.name(), u8"mat", i)).AsView());
            content::Instance* inst =
                root->CreateInstance(name.AsView(), materials::MaterialSource::StaticType());
            if (inst == nullptr)
            {
                outMatGuids.PushBack(Guid{});
                outAlbedo.PushBack(Guid{});
                continue;
            }
            pipeline::AssetBuildContext ctx{allocator};
            ctx.output = inst;
            if (builder.Build(asset, ctx).IsOk())
            {
                outMatGuids.PushBack(inst->Id());
            }
            else
            {
                outMatGuids.PushBack(Guid{});
            }

            const i32 tIdx = m.baseColorTextureIndex;
            outAlbedo.PushBack((tIdx >= 0 && static_cast<usize>(tIdx) < textureGuids.Size())
                                   ? textureGuids[static_cast<usize>(tIdx)]
                                   : Guid{});
        }
    }

    // Cook a model into outDb (textures + materials + static meshes + manifest). `namePrefix`
    // namespaces the created instances. On success `outModelGuid` is the manifest
    // (ModelResource) Guid to Bind at runtime.
    [[nodiscard]] inline Status CookModel(const model::Model& model,
                                          content::ContentDatabase& outDb, StringView namePrefix,
                                          Guid& outModelGuid)
    {
        content::Group* root = outDb.RootGroup();
        if (root == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }

        ModelManifestSource manifest;
        const_cast<model::Model&>(model).calculateBounds(); // ensure model-space AABB is populated
        manifest.boundsMin = model.bounds().min;
        manifest.boundsMax = model.bounds().max;

        Array<Guid> textureGuids;
        CookTextures(model, root, namePrefix, textureGuids);
        CookMaterials(outDb.Allocator(), model, root, namePrefix, textureGuids, manifest.materialGuids,
                      manifest.materialAlbedo);

        // Skeleton + animations (skin 0 only). The skeleton's bone order = the skin's joint order;
        // animation channels + bone parents are remapped from model bone indices into joint indices.
        HashMap<i32, i32> boneToJoint;
        const bool hasSkin = model.skins().Size() > 0;
        if (hasSkin)
        {
            const model::ModelSkin& skin = *model.skins()[0];
            boneToJoint = BuildBoneToJoint(skin);
            manifest.skeletonParentNode = SkeletonParentNode(model, skin, boneToJoint);

            pipeline::SkeletonAsset skelAsset;
            SkeletonSourceFromModel(model, skin, boneToJoint, skelAsset.source);
            pipeline::SkeletonAssetBuilder skelBuilder;
            content::Instance* skelInst =
                root->CreateInstance(Format(u8"{}.skeleton", namePrefix).AsView(),
                                     animation::SkeletonSource::StaticType());
            if (skelInst != nullptr)
            {
                pipeline::AssetBuildContext ctx{outDb.Allocator()};
                ctx.output = skelInst;
                if (skelBuilder.Build(skelAsset, ctx).IsOk())
                {
                    manifest.skeletonGuid = skelInst->Id();
                }
            }

            pipeline::AnimationClipAssetBuilder clipBuilder;
            const Span<model::ModelAnimation* const> animations = model.animations();
            for (usize a = 0; a < animations.Size(); ++a)
            {
                pipeline::AnimationClipAsset clipAsset;
                AnimationClipSourceFromModel(
                    *animations[a], boneToJoint,
                    Format(u8"{}.{}", namePrefix,
                           ImportedAssetName(animations[a]->name(), u8"anim", a))
                        .AsView(),
                    clipAsset.source, manifest.skeletonParentNode);
                content::Instance* clipInst = root->CreateInstance(
                    Format(u8"{}.{}", namePrefix,
                           ImportedAssetName(animations[a]->name(), u8"anim", a))
                        .AsView(),
                    animation::AnimationClipSource::StaticType());
                if (clipInst == nullptr)
                {
                    continue;
                }
                pipeline::AssetBuildContext ctx{outDb.Allocator()};
                ctx.output = clipInst;
                if (clipBuilder.Build(clipAsset, ctx).IsOk())
                {
                    manifest.animationGuids.PushBack(clipInst->Id());
                }
            }
        }

        pipeline::StaticMeshAssetBuilder meshBuilder;
        pipeline::SkinnedMeshAssetBuilder skinnedBuilder;
        const Span<model::ModelMesh* const> meshes = model.meshes();
        for (usize i = 0; i < meshes.Size(); ++i)
        {
            const model::ModelMesh& m = *meshes[i];
            const bool skinned = IsSkinnedMesh(m) && hasSkin;
            const String name =
                Format(u8"{}.{}", namePrefix, ImportedAssetName(m.name(), u8"mesh", i));

            content::Instance* inst = root->CreateInstance(
                name.AsView(), skinned ? geometry::SkinnedMeshSource::StaticType()
                                       : geometry::StaticMeshSource::StaticType());
            if (inst == nullptr)
            {
                return Status{ErrorCode::Unknown};
            }
            pipeline::AssetBuildContext ctx{outDb.Allocator()};
            ctx.output = inst;

            Status s;
            if (skinned)
            {
                pipeline::SkinnedMeshAsset asset;
                SkinnedMeshSourceFromModel(m, /*skeletonIndex*/ 0, asset.source);
                s = skinnedBuilder.Build(asset, ctx);
            }
            else
            {
                pipeline::StaticMeshAsset asset;
                StaticMeshSourceFromModel(m, asset.source);
                s = meshBuilder.Build(asset, ctx);
            }
            if (!s.IsOk())
            {
                return s;
            }

            manifest.meshGuids.PushBack(inst->Id());
            manifest.meshSkinned.PushBack(skinned ? u8{1} : u8{0});
            // One material per mesh: the first submesh's material (single-material models).
            const Span<const model::ModelMeshPart> parts = m.parts();
            manifest.meshMaterial.PushBack(parts.Size() > 0 ? parts[0].materialIndex : -1);
            {
                foundation::model::ModelMeshMaterialSlots slots;
                CollectMeshMaterialSlots(m, slots.slots);
                manifest.meshMaterialSlots.PushBack(Move(slots));
            }
        }

        const Span<model::ModelBone* const> bones = model.bones();
        for (usize i = 0; i < bones.Size(); ++i)
        {
            const model::ModelBone& b = *bones[i];
            ModelNode n;
            n.name = String(b.name());
            n.parentIndex = b.parentIndex;
            n.localTransform.position = b.translation;
            n.localTransform.rotation = b.rotation;
            n.localTransform.scale = b.scale;
            n.meshIndex = b.meshIndex;
            manifest.nodes.PushBack(Move(n));
        }

        const String manifestName = Format(u8"{}.model", namePrefix);
        content::Instance* manifestInst =
            root->CreateInstance(manifestName.AsView(), ModelManifestSource::StaticType());
        if (manifestInst == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }
        const Status ms = manifestInst->WriteObject(manifest);
        if (!ms.IsOk())
        {
            return ms;
        }

        outModelGuid = manifestInst->Id();
        return Status{};
    }

} // namespace pipeline

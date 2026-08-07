/// Draconic::ModelImporter:cook - cook a loaded Model into a content database.
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
#include "Draconic.Foundation/Prelude.h"

export module draconic.modelimporter:cook;

import draconic.foundation;
import draconic.rhi;
import draconic.model;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.geometry.editor;
import draconic.materials;
import draconic.materials.resource;
import draconic.materials.editor;
import draconic.texture.resource;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.content;
import draconic.editor;
import :mesh_convert;
import :anim_convert;
import draconic.model.resource;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace model = draconic::model;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace texture = draconic::texture;
namespace animation = draconic::animation;
namespace content = draconic::content;
namespace editor = draconic::editor;

export namespace draconic::modelimporter
{
    // The cooked-model runtime types now live in draconic::model (draconic.model.resource).
    using draconic::model::ModelManifestSource;
    using draconic::model::ModelNode;
    using draconic::model::ModelResource;

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
    inline void CookTextures(const model::Model& model, content::Group* root, StringView namePrefix,
                             Array<Guid>& outGuids)
    {
        Array<bool> linear;
        ClassifyLinearTextures(model, linear);
        const Span<model::ModelTexture* const> textures = model.textures();
        for (usize i = 0; i < textures.Size(); ++i)
        {
            const model::ModelTexture& t = *textures[i];
            const u8* data = t.getData();
            const i32 size = t.getDataSize();
            // The loaders decode to RGBA8 (4 bpp); guard against any other layout for now.
            const bool rgba8 =
                (data != nullptr && t.width > 0 && t.height > 0 && size == t.width * t.height * 4);
            if (!rgba8)
            {
                outGuids.PushBack(Guid{});
                continue;
            }

            texture::TextureResource res;
            res.width = static_cast<u32>(t.width);
            res.height = static_cast<u32>(t.height);
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

    // Cook the model's materials into outDb (as MaterialSource via CreatePBR + the editor cook).
    // Records, per material, its cooked Guid + the albedo texture Guid (resolved from textureGuids).
    inline void CookMaterials(const model::Model& model, content::Group* root,
                              StringView namePrefix, const Array<Guid>& textureGuids,
                              Array<Guid>& outMatGuids, Array<Guid>& outAlbedo)
    {
        materials::MaterialAssetBuilder builder;
        HashMap<u64, Guid> bakedMR; // per-pair bake cache (materials often share maps)
        const Span<model::ModelMaterial* const> materials = model.materials();
        for (usize i = 0; i < materials.Size(); ++i)
        {
            const model::ModelMaterial& m = *materials[i];

            // Build a standard PBR material carrying the model's factors, capture it into a source that
            // names the builtin "forward" shader (no cooked ShaderResource needed).
            RefPtr<materials::Material> built = materials::CreatePBR(
                Format(u8"{}.{}", namePrefix, ImportedAssetName(m.name(), u8"mat", i)).AsView(),
                m.baseColorFactor, m.metallicFactor, m.roughnessFactor);
            built->SetDefaultColor(u8"EmissiveColor", Float4{m.emissiveFactor.x, m.emissiveFactor.y,
                                                             m.emissiveFactor.z, 1.0f});
            built->SetDefaultFloat(u8"OcclusionStrength", m.occlusionStrength);
            built->SetDefaultFloat(u8"NormalScale", m.normalScale);
            built->SetDefaultFloat(u8"AlphaCutoff", m.alphaCutoff);
            materials::MaterialAsset asset;
            materials::MaterialImporter::Import(*built, Guid{},
                                                asset); // nil shaderId -> use shaderName
            asset.source.shaderName = String(u8"forward");
            // Wire every authored texture (see FileImport::ImportMaterials - same self-contained rule).
            {
                const auto wire = [&](i32 texIdx, foundation::StringView slot)
                {
                    if (texIdx >= 0 && static_cast<usize>(texIdx) < textureGuids.Size() &&
                        !textureGuids[static_cast<usize>(texIdx)].IsNil())
                    {
                        asset.source.textureSlots.PushBack(String(slot));
                        asset.source.textureIds.PushBack(textureGuids[static_cast<usize>(texIdx)]);
                    }
                };
                wire(m.baseColorTextureIndex, u8"AlbedoMap");
                wire(m.normalTextureIndex, u8"NormalMap");
                wire(m.occlusionTextureIndex, u8"OcclusionMap");
                wire(m.emissiveTextureIndex, u8"EmissiveMap");
                // Metallic-roughness: glTF's packed texture wires directly; FBX's separate maps
                // bake into a packed product (see FileImport - same rule, product-side here).
                if (m.metallicRoughnessTextureIndex >= 0)
                {
                    wire(m.metallicRoughnessTextureIndex, u8"MetallicRoughnessMap");
                }
                else if (m.separateRoughnessTextureIndex >= 0 ||
                         m.separateMetalnessTextureIndex >= 0)
                {
                    const u64 key =
                        (static_cast<u64>(static_cast<u32>(m.separateRoughnessTextureIndex))
                         << 32) |
                        static_cast<u64>(static_cast<u32>(m.separateMetalnessTextureIndex));
                    Guid packed;
                    if (const Guid* hit = bakedMR.Find(key))
                    {
                        packed = *hit;
                    }
                    else
                    {
                        u32 w = 0, h = 0;
                        Array<u8> pixels =
                            BakePackedMetallicRoughness(model, m.separateRoughnessTextureIndex,
                                                        m.separateMetalnessTextureIndex, w, h);
                        if (!pixels.IsEmpty())
                        {
                            texture::TextureResource res;
                            res.width = w;
                            res.height = h;
                            res.format = rhi::TextureFormat::RGBA8Unorm; // data map: linear
                            res.mipLevels = 1;
                            res.generateMipmaps = false;
                            const String texName = Format(u8"{}.mr.packed.{}.{}", namePrefix,
                                                          m.separateRoughnessTextureIndex,
                                                          m.separateMetalnessTextureIndex);
                            content::Instance* texInst = root->CreateInstance(
                                texName.AsView(), texture::TextureResource::StaticType());
                            if (texInst != nullptr && texInst->WriteObject(res).IsOk() &&
                                texInst
                                    ->WriteData(u8"data",
                                                Span<const byte>{
                                                    reinterpret_cast<const byte*>(pixels.Data()),
                                                    pixels.Size()})
                                    .IsOk())
                            {
                                packed = texInst->Id();
                            }
                        }
                        bakedMR.InsertOrAssign(key, packed);
                    }
                    if (!packed.IsNil())
                    {
                        asset.source.textureSlots.PushBack(String(u8"MetallicRoughnessMap"));
                        asset.source.textureIds.PushBack(packed);
                    }
                }
                // Authored pipeline state: alpha mode -> blend (Mask = alpha-tested cutout w/ holey
                // shadows; Blend = transparent pass) and double-sided -> no culling.
                if (m.alphaMode == draconic::model::AlphaMode::Mask)
                {
                    asset.source.blendMode =
                        draconic::materials::BlendMode::Masked;
                }
                else if (m.alphaMode == draconic::model::AlphaMode::Blend)
                {
                    asset.source.blendMode =
                        draconic::materials::BlendMode::AlphaBlend;
                }
                if (m.doubleSided)
                {
                    asset.source.cullMode =
                        draconic::materials::CullModeConfig::None;
                }
                MaterialSamplerModes(model, m, asset.source.samplerU, asset.source.samplerV);
            }

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
            editor::AssetBuildContext ctx;
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
        CookMaterials(model, root, namePrefix, textureGuids, manifest.materialGuids,
                      manifest.materialAlbedo);

        // Skeleton + animations (skin 0 only for now). The skeleton's bone order = the skin's joint order;
        // animation channels + bone parents are remapped from model bone indices into joint indices.
        HashMap<i32, i32> boneToJoint;
        const bool hasSkin = model.skins().Size() > 0;
        if (hasSkin)
        {
            const model::ModelSkin& skin = *model.skins()[0];
            boneToJoint = BuildBoneToJoint(skin);

            animation::SkeletonAsset skelAsset;
            SkeletonSourceFromModel(model, skin, boneToJoint, skelAsset.source);
            animation::SkeletonAssetBuilder skelBuilder;
            content::Instance* skelInst =
                root->CreateInstance(Format(u8"{}.skeleton", namePrefix).AsView(),
                                     animation::SkeletonSource::StaticType());
            if (skelInst != nullptr)
            {
                editor::AssetBuildContext ctx;
                ctx.output = skelInst;
                if (skelBuilder.Build(skelAsset, ctx).IsOk())
                {
                    manifest.skeletonGuid = skelInst->Id();
                }
            }

            animation::AnimationClipAssetBuilder clipBuilder;
            const Span<model::ModelAnimation* const> animations = model.animations();
            for (usize a = 0; a < animations.Size(); ++a)
            {
                animation::AnimationClipAsset clipAsset;
                AnimationClipSourceFromModel(
                    *animations[a], boneToJoint,
                    Format(u8"{}.{}", namePrefix,
                           ImportedAssetName(animations[a]->name(), u8"anim", a))
                        .AsView(),
                    clipAsset.source);
                content::Instance* clipInst = root->CreateInstance(
                    Format(u8"{}.{}", namePrefix,
                           ImportedAssetName(animations[a]->name(), u8"anim", a))
                        .AsView(),
                    animation::AnimationClipSource::StaticType());
                if (clipInst == nullptr)
                {
                    continue;
                }
                editor::AssetBuildContext ctx;
                ctx.output = clipInst;
                if (clipBuilder.Build(clipAsset, ctx).IsOk())
                {
                    manifest.animationGuids.PushBack(clipInst->Id());
                }
            }
        }

        geometry::StaticMeshAssetBuilder meshBuilder;
        geometry::SkinnedMeshAssetBuilder skinnedBuilder;
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
            editor::AssetBuildContext ctx;
            ctx.output = inst;

            Status s;
            if (skinned)
            {
                geometry::SkinnedMeshAsset asset;
                SkinnedMeshSourceFromModel(m, /*skeletonIndex*/ 0, asset.source);
                s = skinnedBuilder.Build(asset, ctx);
            }
            else
            {
                geometry::StaticMeshAsset asset;
                StaticMeshSourceFromModel(m, asset.source);
                s = meshBuilder.Build(asset, ctx);
            }
            if (!s.IsOk())
            {
                return s;
            }

            manifest.meshGuids.PushBack(inst->Id());
            manifest.meshSkinned.PushBack(skinned ? u8{1} : u8{0});
            // One material per mesh for now: the first submesh's material (single-material models).
            const Span<const model::ModelMeshPart> parts = m.parts();
            manifest.meshMaterial.PushBack(parts.Size() > 0 ? parts[0].materialIndex : -1);
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

} // namespace draconic::modelimporter

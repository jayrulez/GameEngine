// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::ModelImporter - :file_import partition.
//
// The source-side model importer for the editor pipeline: a dropped model file fans out into
// source instances in the content DB (textures as embedded-pixel TextureAssets, materials as
// MaterialAssets, meshes as StaticMeshAssets or SkinnedMeshAssets, the skeleton and clips as
// animation assets, and a ModelManifestAsset tying them together), so the pipeline cooks each
// incrementally and each fanned-out asset is editable on its own (tweak one material without
// touching the model). Product guids equal source guids, which keeps the manifest's recorded
// guids valid in the cooked DB, where the runtime's ModelResource composite binds them.
//
// This is the editor path; CookModel in :cook is the direct path that bakes a loaded model
// straight into a runtime DB.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

export module modelimporter:file_import;

import foundation.core;
import foundation.model;
import foundation.model.io;
import foundation.model.gltf;
import foundation.model.fbx;
import foundation.geometry;
import foundation.geometry.resource;
import geometry.pipeline;
import foundation.materials;
import foundation.materials.resource;
import materials.pipeline;
import foundation.texture;
import texture.pipeline;
import foundation.image;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import pipeline.importer;
import physics.pipeline;
import :mesh_convert;
import :anim_convert;
import foundation.model.resource;
import :cook; // the conversion helpers' home

using namespace foundation::core;

export namespace pipeline
{
    // The cooked-model runtime types live in foundation::model (foundation.model.resource).
    using foundation::model::ModelManifestSource;
    using foundation::model::ModelNode;
    using foundation::model::ModelResource;

    namespace content = foundation::content;

    // Source asset embedding a ModelManifestSource (built at import; the cook writes it
    // through). Guids inside are source guids, which equal product guids.
    class ModelManifestAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ModelManifestAsset, pipeline::Asset)
    public:
        ModelManifestSource manifest;
        // Re-import memory: the review dialog's decisions from the import that wrote this
        // manifest. A re-import merges them onto the fresh plan instead of asking again.
        pipeline::ImportPlan importSelection;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName = the imported model file (re-import seed)
            manifest.Serialize(ar);
            foundation::core::Serialize(ar, "importSelection", importSelection);
        }
    };

    class ModelManifestAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ModelManifestAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ModelManifestSource::StaticType();
        }

        // Everything the manifest points at is a runtime reference: the products must exist,
        // but their content never re-cooks the manifest.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const ModelManifestAsset& ma = static_cast<const ModelManifestAsset&>(asset);
            for (const Guid& g : ma.manifest.meshGuids)
            {
                out.references.PushBack(g);
            }
            for (const Guid& g : ma.manifest.materialGuids)
            {
                out.references.PushBack(g);
            }
            for (const Guid& g : ma.manifest.materialAlbedo)
            {
                if (!g.IsNil())
                {
                    out.references.PushBack(g);
                }
            }
            for (const Guid& g : ma.manifest.animationGuids)
            {
                out.references.PushBack(g);
            }
            if (!ma.manifest.skeletonGuid.IsNil())
            {
                out.references.PushBack(ma.manifest.skeletonGuid);
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const ModelManifestAsset& ma = static_cast<const ModelManifestAsset&>(asset);
            return ctx.output->WriteObject(const_cast<ModelManifestSource&>(ma.manifest));
        }
    };

    /// Which way is up in a model file whose loader reads it as Y up but whose content is not (an
    /// export whose Z-up axis was not written into the file).
    enum class ModelUpAxis : u8
    {
        Y, ///< as the file is read
        Z, ///< turned so its Z points up
    };

    /// Options for one model import: its settings, reflected (ModelImporterImpl.cpp), which the
    /// import window draws and an agent sets by label.
    class ModelImportOptions final : public pipeline::ImportOptions
    {
        RTTI_OBJECT(ModelImportOptions, pipeline::ImportOptions)
    public:
        bool importTextures = true;     // embedded and sidecar images as TextureAssets
        bool importMaterials = true;    // PBR materials (texture slots wired when textures import)
        bool importAnimations = true;   // skeleton and clips
        bool generatePrefab = true;     // hierarchy prefab beside the manifest (post-import step)
        bool generateScene = false;     // standalone scene of the same hierarchy (post-import step)
        bool generateCollision = false; // CollisionShapeAsset per mesh and colliders on the prefab
        bool collisionConvex = false;   // hull (dynamic-capable) instead of exact triangle mesh
        bool generateLods = true;       // auto-LOD chains for big static meshes (authored _LODn wins)
        bool rootMotion = false; // new clips extract their root's travel and turn
        f32 scale = 1.0f;                     // every length times this (a file in centimetres: 0.01)
        ModelUpAxis upAxis = ModelUpAxis::Y; // the file's up, turned to the engine's Y

        void Serialize(ISerializer& ar) override
        {
            u8 textures = importTextures ? 1u : 0u;
            u8 materials = importMaterials ? 1u : 0u;
            u8 animations = importAnimations ? 1u : 0u;
            u8 prefab = generatePrefab ? 1u : 0u;
            u8 sceneOut = generateScene ? 1u : 0u;
            u8 collision = generateCollision ? 1u : 0u;
            u8 convex = collisionConvex ? 1u : 0u;
            u8 lods = generateLods ? 1u : 0u;
            foundation::core::Serialize(ar, "textures", textures);
            foundation::core::Serialize(ar, "materials", materials);
            foundation::core::Serialize(ar, "animations", animations);
            foundation::core::Serialize(ar, "prefab", prefab);
            foundation::core::Serialize(ar, "collision", collision);
            foundation::core::Serialize(ar, "collisionConvex", convex);
            foundation::core::Serialize(ar, "scene", sceneOut);
            foundation::core::Serialize(ar, "generateLods", lods);
            u8 motion = rootMotion ? 1u : 0u;
            SerializeAppended(ar, "rootMotion", motion); // options saved before it read as off
            SerializeAppended(ar, "scale", scale);       // before it: 1
            u8 up = static_cast<u8>(upAxis);
            SerializeAppended(ar, "upAxis", up);         // before it: Y
            upAxis = up == static_cast<u8>(ModelUpAxis::Z) ? ModelUpAxis::Z : ModelUpAxis::Y;
            importTextures = textures != 0;
            importMaterials = materials != 0;
            importAnimations = animations != 0;
            generatePrefab = prefab != 0;
            generateScene = sceneOut != 0;
            generateCollision = collision != 0;
            collisionConvex = convex != 0;
            generateLods = lods != 0;
            rootMotion = motion != 0;
        }
    };

    /// One clip's own settings in an import (IFileImporter::PartSettings): which of its root's
    /// motion moves the character rather than playing in place. Reflected (ModelImporterImpl.cpp),
    /// drawn in the import window's clip details.
    class ClipImportSettings final : public Object
    {
        RTTI_OBJECT(ClipImportSettings, Object)
    public:
        bool travel = false;   // the ground-plane translation
        bool vertical = false; // height (a climb; off for a walk, which keeps its bob)
        bool turn = false;     // the turn about up
    };

    /// Scale and up axis applied to a loaded model before its import fans out: each root node's
    /// transform, and the keys of the clips that move a root node, so its meshes, skeleton and
    /// clips all come in scaled and turned. A skinned mesh follows its skeleton's root.
    struct ModelImportFrame
    {
        /// The turn that brings `up` to the engine's Y.
        [[nodiscard]] static Quaternion TurnFor(ModelUpAxis up)
        {
            return up == ModelUpAxis::Z ? Quaternion::FromAxisAngle(Float3{1.0f, 0.0f, 0.0f}, -kPi * 0.5f) : Quaternion::Identity;
        }
        /// Applies `scale` and `up` to `model`; nothing when they are 1 and Y. A scale of zero or
        /// less (a field typed over) is taken as 1: a model scaled to nothing imports nothing useful.
        static void Apply(foundation::model::Model& model, f32 scale, ModelUpAxis up)
        {
            if (!(scale > 0.0f))
                scale = 1.0f;
            if (scale == 1.0f && up == ModelUpAxis::Y)
                return;
            const Quaternion turn = TurnFor(up);
            for (foundation::model::ModelBone* bone : model.bones())
            {
                if (bone == nullptr || bone->parentIndex >= 0)
                    continue;
                bone->translation = RotateVector(turn, bone->translation) * scale;
                bone->rotation = turn * bone->rotation;
                bone->scale = bone->scale * scale;
                bone->updateLocalTransform();
            }
            for (foundation::model::ModelAnimation* clip : model.animations())
            {
                if (clip == nullptr)
                    continue;
                for (foundation::model::AnimationChannel* channel : clip->channels())
                {
                    if (channel == nullptr || channel->targetBone < 0 || static_cast<usize>(channel->targetBone) >= model.bones().Size() ||
                        model.bones()[static_cast<usize>(channel->targetBone)]->parentIndex >= 0)
                        continue;
                    for (foundation::model::AnimationKeyframe& key : channel->keyframes())
                    {
                        if (channel->path == foundation::model::AnimationPath::Translation)
                        {
                            const Float3 moved = RotateVector(turn, Float3{key.value.x, key.value.y, key.value.z}) * scale;
                            key.value = Float4{moved.x, moved.y, moved.z, key.value.w};
                        }
                        else if (channel->path == foundation::model::AnimationPath::Rotation)
                        {
                            const Quaternion turned = turn * Quaternion{key.value.x, key.value.y, key.value.z, key.value.w};
                            key.value = Float4{turned.x, turned.y, turned.z, turned.w};
                        }
                        else if (channel->path == foundation::model::AnimationPath::Scale)
                            key.value = Float4{key.value.x * scale, key.value.y * scale, key.value.z * scale, key.value.w};
                    }
                }
            }
            model.calculateBounds();
        }
    };

    /// PrepareOnWorker's payload: the fully loaded model (parsing and texture decoding, the
    /// slow part of a model import, done off the UI thread).
    class LoadedModel final : public Object
    {
        RTTI_OBJECT(LoadedModel, Object)
    public:
        foundation::model::Model model;
    };

    /// OS-file importer for model files: loads through foundation.model and fans out source
    /// instances into a subgroup named after the file stem.
    class ModelFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Model"; }
        [[nodiscard]] StringView ListedTypeName(StringView) const override { return u8"ModelManifestAsset"; }

        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator& allocator) const override
        {
            return RefPtr<pipeline::ImportOptions>(MakeRef<ModelImportOptions>(allocator).Get());
        }

        /// What a part holds, read from the model the worker read: a mesh's triangles and its parts
        /// by material; a material's shading, colour, metallic, roughness, textures and the parts
        /// that draw with it; a texture's size and the materials using it; the skeleton's bones; a
        /// clip's length; a collision shape's mesh and kind. Nothing without that read.
        [[nodiscard]] Array<pipeline::ImportPartFact> DescribePart(StringView, const pipeline::ImportOptions* options, Object* prepared,
                                                                    const pipeline::ImportPlanEntry& part) const override
        {
            Array<pipeline::ImportPartFact> facts;
            const LoadedModel* loaded = Cast<LoadedModel>(prepared);
            if (loaded == nullptr)
                return facts;
            const foundation::model::Model& model = loaded->model;
            const auto fact = [&facts](StringView label, String value) { facts.PushBack(pipeline::ImportPartFact{String(label), Move(value)}); };
            const auto grouped = [](u64 n)
            {
                String digits = Format(u8"{}", n);
                String out;
                for (usize i = 0; i < digits.Size(); ++i)
                {
                    if (i > 0 && (digits.Size() - i) % 3 == 0)
                        out.Append(u8',');
                    out.Append(digits[i]);
                }
                return out;
            };
            const auto materialName = [&model](i32 index) -> String
            {
                return index >= 0 && static_cast<usize>(index) < model.materials().Size() ? String(model.materials()[static_cast<usize>(index)]->name())
                                                                                          : String(u8"No material");
            };
            const auto partTriangles = [](const foundation::model::ModelMeshPart& p) { return static_cast<u64>(Max(p.indexCount, 0)) / 3u; };
            const auto meshTriangles = [](const foundation::model::ModelMesh& mesh)
            {
                const i32 corners = mesh.indexCount() > 0 ? mesh.indexCount() : mesh.vertexCount();
                return static_cast<u64>(Max(corners, 0)) / 3u;
            };
            const StringView name = part.sourceName.AsView();
            switch (part.kind)
            {
            case pipeline::ImportResourceKind::Mesh:
            case pipeline::ImportResourceKind::Collision:
            {
                const bool collision = part.kind == pipeline::ImportResourceKind::Collision;
                const Span<foundation::model::ModelMesh* const> meshes = model.meshes();
                for (usize i = 0; i < meshes.Size(); ++i)
                {
                    String key = ImportedAssetName(meshes[i]->name(), u8"mesh", i);
                    if (collision)
                        key.Append(u8".collision");
                    if (key.AsView() != name)
                        continue;
                    const foundation::model::ModelMesh& mesh = *meshes[i];
                    if (collision)
                    {
                        const auto* modelOptions = Cast<ModelImportOptions>(options);
                        fact(u8"From the mesh", ImportedAssetName(mesh.name(), u8"mesh", i));
                        fact(u8"Shape", String(modelOptions != nullptr && modelOptions->collisionConvex ? StringView(u8"Convex hull") : StringView(u8"Triangle mesh")));
                        fact(u8"Triangles", grouped(meshTriangles(mesh)));
                        break;
                    }
                    fact(u8"Triangles", grouped(meshTriangles(mesh)));
                    fact(u8"Skinned", String(IsSkinnedMesh(mesh) ? StringView(u8"Yes") : StringView(u8"No")));
                    fact(u8"Parts", grouped(mesh.parts().Size()));
                    for (usize pi = 0; pi < mesh.parts().Size(); ++pi)
                    {
                        const foundation::model::ModelMeshPart& p = mesh.parts()[pi];
                        fact(Format(u8"Part {}", pi + 1).AsView(), Format(u8"{}, {} triangles", materialName(p.materialIndex), grouped(partTriangles(p))));
                    }
                    break;
                }
                break;
            }
            case pipeline::ImportResourceKind::Material:
            {
                const Span<foundation::model::ModelMaterial* const> materials = model.materials();
                for (usize i = 0; i < materials.Size(); ++i)
                {
                    if (ImportedAssetName(materials[i]->name(), u8"mat", i).AsView() != name)
                        continue;
                    const foundation::model::ModelMaterial& m = *materials[i];
                    using Shading = foundation::model::ShadingModel;
                    fact(u8"Shading", String(m.shadingModel == Shading::Phong     ? StringView(u8"Phong")
                                             : m.shadingModel == Shading::Lambert ? StringView(u8"Lambert")
                                             : m.shadingModel == Shading::Unlit   ? StringView(u8"Unlit")
                                                                                  : StringView(u8"PBR, metallic and roughness")));
                    fact(u8"Base colour", Format(u8"R {} G {} B {}", FormatFixed(m.baseColorFactor.x, 2), FormatFixed(m.baseColorFactor.y, 2), FormatFixed(m.baseColorFactor.z, 2)));
                    fact(u8"Metallic", FormatFixed(m.metallicFactor, 2));
                    fact(u8"Roughness", FormatFixed(m.roughnessFactor, 2));
                    String textures;
                    const struct
                    {
                        StringView slot;
                        i32 index;
                    } slots[] = {{u8"base colour", m.baseColorTextureIndex}, {u8"normal", m.normalTextureIndex},
                                 {u8"metal rough", m.metallicRoughnessTextureIndex}, {u8"occlusion", m.occlusionTextureIndex},
                                 {u8"emissive", m.emissiveTextureIndex}};
                    for (const auto& slot : slots)
                        if (slot.index >= 0 && static_cast<usize>(slot.index) < model.textures().Size())
                        {
                            if (!textures.IsEmpty())
                                textures.Append(u8", ");
                            textures.Append(Format(u8"{} ({})", ImportedTextureName(*model.textures()[static_cast<usize>(slot.index)], static_cast<usize>(slot.index)), slot.slot).AsView());
                        }
                    fact(u8"Textures", textures.IsEmpty() ? String(u8"None") : Move(textures));
                    String users;
                    usize count = 0;
                    const Span<foundation::model::ModelMesh* const> meshes = model.meshes();
                    for (usize mi = 0; mi < meshes.Size(); ++mi)
                        for (usize pi = 0; pi < meshes[mi]->parts().Size(); ++pi)
                            if (meshes[mi]->parts()[pi].materialIndex == static_cast<i32>(i))
                            {
                                if (count < 3)
                                {
                                    if (!users.IsEmpty())
                                        users.Append(u8", ");
                                    users.Append(Format(u8"{}, part {}", ImportedAssetName(meshes[mi]->name(), u8"mesh", mi), pi + 1).AsView());
                                }
                                ++count;
                            }
                    if (count > 3)
                        users.Append(Format(u8", and {} more", count - 3).AsView());
                    fact(u8"Used by", count == 0 ? String(u8"No part") : Move(users));
                    break;
                }
                break;
            }
            case pipeline::ImportResourceKind::Texture:
            {
                const Span<foundation::model::ModelTexture* const> textures = model.textures();
                for (usize i = 0; i < textures.Size(); ++i)
                {
                    if (ImportedTextureName(*textures[i], i).AsView() != name)
                        continue;
                    fact(u8"Size", Format(u8"{} × {}", textures[i]->width, textures[i]->height));
                    String users;
                    for (const foundation::model::ModelMaterial* m : model.materials())
                    {
                        if (m == nullptr)
                            continue;
                        StringView slot;
                        if (m->baseColorTextureIndex == static_cast<i32>(i))
                            slot = u8"base colour";
                        else if (m->normalTextureIndex == static_cast<i32>(i))
                            slot = u8"normal";
                        else if (m->metallicRoughnessTextureIndex == static_cast<i32>(i))
                            slot = u8"metal rough";
                        else if (m->occlusionTextureIndex == static_cast<i32>(i))
                            slot = u8"occlusion";
                        else if (m->emissiveTextureIndex == static_cast<i32>(i))
                            slot = u8"emissive";
                        if (slot.IsEmpty())
                            continue;
                        if (!users.IsEmpty())
                            users.Append(u8", ");
                        users.Append(Format(u8"{} ({})", m->name(), slot).AsView());
                    }
                    fact(u8"Used by", users.IsEmpty() ? String(u8"No material") : Move(users));
                    break;
                }
                break;
            }
            case pipeline::ImportResourceKind::Skeleton:
            {
                if (model.skins().Size() == 0)
                    break;
                const foundation::model::ModelSkin& skin = *model.skins()[0];
                fact(u8"Bones", grouped(skin.joints().Size()));
                if (!skin.joints().IsEmpty() && skin.joints()[0] >= 0 && static_cast<usize>(skin.joints()[0]) < model.bones().Size())
                    fact(u8"Root bone", String(model.bones()[static_cast<usize>(skin.joints()[0])]->name()));
                break;
            }
            case pipeline::ImportResourceKind::AnimationClip:
            {
                const Span<foundation::model::ModelAnimation* const> animations = model.animations();
                for (usize a = 0; a < animations.Size(); ++a)
                {
                    if (ImportedAssetName(animations[a]->name(), u8"anim", a).AsView() != name)
                        continue;
                    fact(u8"Length", Format(u8"{} s", FormatFixed(animations[a]->duration, 2)));
                    fact(u8"Channels", grouped(animations[a]->channels().Size()));
                    break;
                }
                break;
            }
            case pipeline::ImportResourceKind::Asset:
                break;
            }
            return facts;
        }

        /// A clip's root motion: as its asset has it from an earlier import into `group`, else the
        /// file's Root motion default (travel and turn).
        [[nodiscard]] RefPtr<Object> PartSettings(StringView sourcePath, const pipeline::ImportOptions* options, const pipeline::ImportPlanEntry& part,
                                                  content::Group* group, IAllocator& allocator) const override
        {
            if (part.kind != pipeline::ImportResourceKind::AnimationClip)
                return {};
            RefPtr<ClipImportSettings> settings = MakeRef<ClipImportSettings>(allocator);
            const auto* opt = Cast<ModelImportOptions>(options);
            settings->travel = settings->turn = opt != nullptr && opt->rootMotion;
            // The clip as an earlier import made it: its folder group, named after the file.
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(sourcePath));
            content::Group* folder = group != nullptr ? group->GetGroup(stem) : nullptr;
            const StringView name = part.targetName.IsEmpty() ? part.sourceName.AsView() : part.targetName.AsView();
            if (content::Instance* clip = folder != nullptr ? folder->GetInstance(name) : nullptr)
            {
                RefPtr<ISerializable> object = clip->ReadObject();
                if (const auto* before = Cast<pipeline::AnimationClipAsset>(object.Get()))
                {
                    settings->travel = before->source.rootMotion.horizontal;
                    settings->vertical = before->source.rootMotion.vertical;
                    settings->turn = before->source.rootMotion.yaw;
                }
            }
            return settings;
        }

        /// Root motion is each clip's own; the file's setting is the default for new clips.
        [[nodiscard]] bool SettingIsPerPart(StringView setting) const override { return setting == StringView(u8"rootMotion"); }

        /// Textures, materials, and the skeleton with its clips each have a setting that leaves them out.
        [[nodiscard]] StringView KindSetting(pipeline::ImportResourceKind kind) const override
        {
            switch (kind)
            {
            case pipeline::ImportResourceKind::Texture: return u8"importTextures";
            case pipeline::ImportResourceKind::Material: return u8"importMaterials";
            case pipeline::ImportResourceKind::Skeleton:
            case pipeline::ImportResourceKind::AnimationClip: return u8"importAnimations";
            default: return {};
            }
        }

        // The import makes the model's folder group, named after the file.
        [[nodiscard]] bool ImportsIntoItsOwnGroup() const override { return true; }

        /// The scale's note: how tall the model comes in with it (and the up axis), from the
        /// model read on the worker.
        [[nodiscard]] pipeline::ImportSettingNote DescribeSetting(StringView, const pipeline::ImportOptions& options, Object* prepared,
                                                                  const PropertyInfo& setting) const override
        {
            pipeline::ImportSettingNote note;
            const LoadedModel* loaded = Cast<LoadedModel>(prepared);
            const ModelImportOptions* chosen = Cast<ModelImportOptions>(&options);
            if (loaded == nullptr || chosen == nullptr || StringView(reinterpret_cast<const utf8char*>(setting.name)) != StringView(u8"scale"))
                return note;
            const AABB box = loaded->model.bounds();
            if (!box.IsValid())
                return note;
            const f32 scale = chosen->scale > 0.0f ? chosen->scale : 1.0f;
            const f32 height = (chosen->upAxis == ModelUpAxis::Z ? box.max.z - box.min.z : box.max.y - box.min.y) * scale;
            note.effect = Format(u8"Makes it {} m tall", FormatFixed(height, height < 1.0f ? 3 : 2));
            return note;
        }

        [[nodiscard]] bool WantsWorkerPrepare() const override { return true; }

        [[nodiscard]] RefPtr<Object> PrepareOnWorker(StringView sourcePath,
                                                     IAllocator& allocator) override
        {
            RefPtr<LoadedModel> loaded = MakeRef<LoadedModel>(allocator);
            if (LoadModelFrom(sourcePath, loaded->model) != foundation::model::ModelLoadResult::Ok)
            {
                return {};
            }
            return RefPtr<Object>(loaded.Get());
        }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView ext : {u8"glb", u8"gltf", u8"fbx", u8"obj"})
            {
                if (extension == ext)
                {
                    return true;
                }
            }
            return false;
        }

        /// Everything Import would create, in fan-out order: the review dialog's data.
        /// Reuses the worker-prepared model when supplied; only mirrors the naming logic of
        /// the fan-out helpers (ComputeLodFold is shared so the mesh set matches exactly).
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions* options,
                                                          Object* prepared) override
        {
            const ModelImportOptions defaults;
            const ModelImportOptions& opt =
                (options != nullptr) ? static_cast<const ModelImportOptions&>(*options) : defaults;
            foundation::model::Model inlineModel;
            foundation::model::Model* modelPtr = nullptr;
            if (auto* loadedPayload = Cast<LoadedModel>(prepared))
            {
                modelPtr = &loadedPayload->model;
            }
            else
            {
                if (LoadModelFrom(sourcePath, inlineModel) !=
                    foundation::model::ModelLoadResult::Ok)
                {
                    return {};
                }
                modelPtr = &inlineModel;
            }
            const foundation::model::Model& model = *modelPtr;

            pipeline::ImportPlan plan;
            const auto add = [&plan](pipeline::ImportResourceKind kind, String name)
            {
                pipeline::ImportPlanEntry e;
                e.kind = kind;
                e.targetName = name;
                e.sourceName = Move(name);
                e.enabled = true;
                plan.entries.PushBack(Move(e));
            };

            if (opt.importTextures)
            {
                const Span<foundation::model::ModelTexture* const> textures = model.textures();
                for (usize i = 0; i < textures.Size(); ++i)
                {
                    const foundation::model::ModelTexture& t = *textures[i];
                    const bool rgba8 =
                        (t.getData() != nullptr && t.width > 0 && t.height > 0 &&
                         t.getDataSize() == t.width * t.height * 4);
                    if (rgba8) // undecodable textures never become assets, so keep them out
                    {
                        add(pipeline::ImportResourceKind::Texture, ImportedTextureName(t, i));
                    }
                }
            }
            if (opt.importMaterials)
            {
                const Span<foundation::model::ModelMaterial* const> materials = model.materials();
                for (usize i = 0; i < materials.Size(); ++i)
                {
                    add(pipeline::ImportResourceKind::Material,
                        ImportedAssetName(materials[i]->name(), u8"mat", i));
                }
            }
            if (opt.importAnimations && model.skins().Size() > 0)
            {
                add(pipeline::ImportResourceKind::Skeleton, SkeletonBaseName(*model.skins()[0]));
                const Span<foundation::model::ModelAnimation* const> animations =
                    model.animations();
                for (usize a = 0; a < animations.Size(); ++a)
                {
                    add(pipeline::ImportResourceKind::AnimationClip,
                        ImportedAssetName(animations[a]->name(), u8"anim", a));
                }
            }
            const bool hasSkin = model.skins().Size() > 0;
            const Span<foundation::model::ModelMesh* const> meshes = model.meshes();
            Array<i32> lodOf;
            Array<Array<usize>> lodLevels;
            ComputeLodFold(model, /*logMismatch*/ false, lodOf, lodLevels);
            for (usize i = 0; i < meshes.Size(); ++i)
            {
                if (lodOf[i] >= 0)
                {
                    continue; // folds into its base's LOD chain, not an asset of its own
                }
                add(pipeline::ImportResourceKind::Mesh,
                    ImportedAssetName(meshes[i]->name(), u8"mesh", i));
                if (opt.generateCollision && !(IsSkinnedMesh(*meshes[i]) && hasSkin))
                {
                    String key = ImportedAssetName(meshes[i]->name(), u8"mesh", i);
                    key.Append(u8".collision");
                    add(pipeline::ImportResourceKind::Collision, Move(key));
                }
            }
            return plan;
        }

        /// The selection the previous import of this source stored on its manifest (the
        /// fan-out lands in a subgroup named after the file stem; the manifest carries the
        /// review decisions that produced it).
        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(sourcePath));
            content::Group* modelGroup = group.GetGroup(stem);
            content::Instance* manifestInst =
                (modelGroup != nullptr) ? modelGroup->GetInstance(stem) : nullptr;
            if (manifestInst == nullptr ||
                manifestInst->TypeName() != StringView(u8"ModelManifestAsset"))
            {
                return {};
            }
            RefPtr<ISerializable> object = manifestInst->ReadObject();
            auto* asset = Cast<ModelManifestAsset>(object.Get());
            return (asset != nullptr)
                       ? static_cast<pipeline::ImportPlan&&>(asset->importSelection)
                       : pipeline::ImportPlan{};
        }

        /// A model claims its folder (named after the file) and the manifest in it: another
        /// file's model there is a clash of its type; anything else in the folder keeps the name
        /// apart as before (the manifest takes the next free one).
        [[nodiscard]] pipeline::ImportClash FindClash(content::Group& group, StringView sourcePath, const pipeline::ImportPlan&,
                                                      StringView ledgerPath) const override
        {
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(sourcePath));
            content::Group* modelGroup = group.GetGroup(stem);
            if (modelGroup == nullptr)
            {
                return {};
            }
            pipeline::ImportClash clash = pipeline::FindImportClash(*modelGroup, stem, u8"ModelManifestAsset", sourcePath, ledgerPath);
            if (clash.kind == pipeline::ImportClashKind::OtherType)
            {
                clash = {};
            }
            return clash;
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
               const pipeline::ImportOptions* options, Object* prepared,
               Array<pipeline::DeferredImportWrite>* deferredWrites) override
        {
            const ModelImportOptions defaults;
            const ModelImportOptions& opt =
                (options != nullptr) ? static_cast<const ModelImportOptions&>(*options) : defaults;
            // Source provenance copy: the file name is known without copying; the copy
            // itself (and the .gltf sidecars below) is bulk file IO, deferred when possible.
            const StringView sourceFileName = pipeline::FileNameOf(sourcePath);
            if (sourceFileName.IsEmpty())
            {
                return Err(ErrorCode::InvalidArgument);
            }
            // The clash rule (pipeline::FindImportClash) over the model's folder: another file's
            // model there is refused, replaced or kept beside under the next folder name, as the
            // context says; this file's own earlier import is re-imported in place.
            const pipeline::ImportClash clash = FindClash(group, sourcePath, {}, context.ledgerPath.AsView());
            if (!pipeline::ImportClashAllows(clash, context.onConflict))
            {
                return Err(ErrorCode::AlreadyExists);
            }
            String folderName(pipeline::FileStemOf(sourceFileName));
            bool overwriteOwn = false; // a new model never writes over a file another one copied in
            if (clash.kind == pipeline::ImportClashKind::Reimport ||
                (clash.kind == pipeline::ImportClashKind::SameType && context.onConflict == pipeline::ImportConflictChoice::Replace))
            {
                RefPtr<ISerializable> previous = clash.existing->ReadObject();
                const auto* asset = Cast<pipeline::Asset>(previous.Get());
                overwriteOwn = asset != nullptr && asset->fileName.View() == sourceFileName;
            }
            else if (clash.kind == pipeline::ImportClashKind::SameType)
            {
                folderName = group.UniqueGroupName(folderName.AsView()); // KeepBoth
                overwriteOwn = false;
            }
            Result<String> fileName = Result<String>(String(sourceFileName));
            if (deferredWrites != nullptr)
            {
                // Named now (from what is there and the ledger), copied on the worker.
                pipeline::DeferredImportWrite copy;
                copy.copyFrom = String(sourcePath);
                fileName = pipeline::SourcesNameFor(context, sourcePath, overwriteOwn);
                copy.copyTo = PathJoin(context.sourcesRoot.AsView(), fileName.Value().AsView());
                deferredWrites->PushBack(static_cast<pipeline::DeferredImportWrite&&>(copy));
            }
            else
            {
                fileName = pipeline::CopyIntoSources(context, sourcePath, overwriteOwn);
                if (!fileName.HasValue())
                {
                    return Err(fileName.Error());
                }
            }

            // The slow load either arrived pre-baked from the worker phase, or runs inline
            // (headless and tests). Loading uses the original dropped path: .gltf files
            // reference sibling sidecars living next to the original, not in Sources/.
            foundation::model::Model inlineModel;
            foundation::model::Model* modelPtr = nullptr;
            if (auto* loadedPayload = Cast<LoadedModel>(prepared))
            {
                modelPtr = &loadedPayload->model;
            }
            else
            {
                if (LoadModelFrom(sourcePath, inlineModel) != foundation::model::ModelLoadResult::Ok)
                {
                    LOG_ERROR(u8"Import", u8"model load failed: {}", fileName.Value());
                    return Err(ErrorCode::InvalidArgument);
                }
                modelPtr = &inlineModel;
            }
            foundation::model::Model& model = *modelPtr;
            ModelImportFrame::Apply(model, opt.scale, opt.upAxis); // before anything reads a length or a direction

            // .gltf: copy the referenced sidecars (buffers and images by relative uri) into
            // Sources/ so the imported source set is complete.
            const bool sidecarsCopied =
                pipeline::FileExtensionLower(sourcePath) == StringView(u8"gltf");
            if (sidecarsCopied)
            {
                CopyGltfSidecars(sourcePath, context, deferredWrites);
            }

            // Phase timings on the calling thread (the editor's UI thread): the import's own
            // profile, so a regression in any phase names itself in the console.
            const Stopwatch phaseClock = Stopwatch::StartNew();
            i64 phaseTexturesMs = 0;
            i64 phaseMaterialsMs = 0;
            i64 phaseAnimationsMs = 0;
            i64 phaseMeshesMs = 0;
            i64 phaseCollisionMs = 0;
            const auto lap = [&phaseClock](i64& into)
            {
                into = static_cast<i64>(phaseClock.Elapsed().AsMilliseconds());
            };
            const StringView stem = folderName.AsView(); // the folder's and the manifest's name
            content::Group* modelGroup = group.CreateGroup(stem);
            if (modelGroup == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }

            ModelManifestAsset manifestAsset;
            manifestAsset.fileName = foundation::vfs::SourcePath(fileName.Value().AsView());
            manifestAsset.importSelection = opt.selection; // re-import memory
            ModelManifestSource& manifest = manifestAsset.manifest;
            manifest.boundsMin = model.bounds().min;
            manifest.boundsMax = model.bounds().max;

            Array<String> claimed; // names claimed this run (ClaimInstance's dedup scope)
            // The manifest is named for the file, and is written last: its name is reserved
            // first, so a sub-asset named like the model (a material "Gem_Blue" in
            // Gem_Blue.gltf) takes a suffix rather than the manifest's instance.
            claimed.PushBack(String(stem));
            Array<Guid> textureGuids;
            if (opt.importTextures)
            {
                ImportTextures(model, context, sidecarsCopied, *modelGroup, textureGuids, claimed,
                               deferredWrites, opt);
            }
            else
            {
                for (usize i = 0; i < model.textures().Size(); ++i)
                {
                    textureGuids.PushBack(Guid{});
                }
            }
            lap(phaseTexturesMs);
            if (opt.importMaterials)
            {
                ImportMaterials(model, *modelGroup, textureGuids, manifest, claimed,
                                deferredWrites, opt);
            }
            lap(phaseMaterialsMs);
            if (opt.importAnimations)
            {
                ImportSkeletonAndClips(model, *modelGroup, manifest, claimed, opt);
            }
            lap(phaseAnimationsMs);
            Array<String> meshSourceNames; // per manifest mesh slot (collision's plan keys)
            // The prepared model outlives the deferred flush (the editor's job captures it),
            // so the mesh conversion and LOD chains run inside the deferred writes; an inline
            // model dies with this call, so its writes are produced here.
            const Status meshes =
                ImportMeshes(*context.allocator, model, *modelGroup, manifest, claimed,
                             deferredWrites, opt.generateLods, opt, meshSourceNames,
                             /*modelOutlivesWrites*/ modelPtr != &inlineModel);
            if (!meshes.IsOk())
            {
                return Err(meshes.Code());
            }
            lap(phaseMeshesMs);
            if (opt.generateCollision)
            {
                ImportCollisionShapes(*modelGroup, manifest, opt.collisionConvex, claimed, opt,
                                      meshSourceNames);
            }
            lap(phaseCollisionMs);
            ImportNodes(model, manifest);

            // Reuse a previous import's manifest (its guid survives a re-import); a different type
            // holding the name (left by an import that did not reserve it) keeps its instance, and
            // the manifest takes the next free name.
            content::Instance* instance = modelGroup->GetInstance(stem);
            if (instance == nullptr)
            {
                instance = modelGroup->CreateInstance(stem, ModelManifestAsset::StaticType());
            }
            else if (instance->TypeName() !=
                     StringView(reinterpret_cast<const utf8char*>(ModelManifestAsset::StaticType().name)))
            {
                instance = modelGroup->CreateInstance(modelGroup->UniqueInstanceName(stem).AsView(),
                                                      ModelManifestAsset::StaticType());
            }
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            const Status written = instance->WriteObject(manifestAsset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            LOG_INFO(u8"Import",
                     u8"'{}' phases (this thread, ms): textures {} materials {} animations {} "
                     u8"meshes {} collision {} nodes+manifest {}",
                     fileName.Value().AsView(), phaseTexturesMs,
                     phaseMaterialsMs - phaseTexturesMs, phaseAnimationsMs - phaseMaterialsMs,
                     phaseMeshesMs - phaseAnimationsMs, phaseCollisionMs - phaseMeshesMs,
                     static_cast<i64>(phaseClock.Elapsed().AsMilliseconds()) - phaseCollisionMs);
            return instance;
        }

    private:
        [[nodiscard]] static foundation::model::ModelLoadResult
        LoadModelFrom(StringView sourcePath, foundation::model::Model& model)
        {
            foundation::model::gltf::GltfLoader gltfLoader;
            foundation::model::fbx::FbxLoader fbxLoader;
            foundation::model::io::registerLoader(&gltfLoader);
            foundation::model::io::registerLoader(&fbxLoader);
            const foundation::model::ModelLoadResult loaded =
                foundation::model::io::loadModel(sourcePath, model);
            foundation::model::io::unregisterLoader(&fbxLoader);
            foundation::model::io::unregisterLoader(&gltfLoader);
            if (loaded == foundation::model::ModelLoadResult::Ok)
            {
                model.calculateBounds();
            }
            return loaded;
        }

        // Copy every relative "uri" the .gltf references (buffers, images) from next to the
        // original file into Sources/, preserving relative subpaths. Data URIs and
        // parent-escaping paths are skipped. A plain text scan (the uris live in JSON string
        // values); failures only log, since the import itself already succeeded from the original.
        static void CopyGltfSidecars(StringView originalPath, const pipeline::ImportContext& context,
                                     Array<pipeline::DeferredImportWrite>* deferredWrites)
        {
            Result<Array<byte>> bytes = ReadFile(originalPath);
            if (!bytes.HasValue())
            {
                return;
            }
            const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()),
                                  bytes.Value().Size());

            // Directory of the original file.
            usize dirEnd = 0;
            for (usize i = originalPath.Size(); i > 0; --i)
            {
                const utf8char c = originalPath[i - 1];
                if (c == utf8char('/') || c == utf8char('\\'))
                {
                    dirEnd = i;
                    break;
                }
            }
            const StringView dir = originalPath.SubStr(0, dirEnd);

            foundation::vfs::NativeFileSystem sources(context.sourcesRoot.AsView(),
                                                      *context.allocator);
            const StringView key = u8"\"uri\"";
            for (usize i = 0; i + key.Size() < text.Size(); ++i)
            {
                if (text.SubStr(i, key.Size()) != key)
                {
                    continue;
                }
                usize j = i + key.Size();
                while (j < text.Size() && (text[j] == utf8char(':') || text[j] == utf8char(' ') ||
                                           text[j] == utf8char('\t')))
                {
                    ++j;
                }
                if (j >= text.Size() || text[j] != utf8char('"'))
                {
                    continue;
                }
                const usize begin = ++j;
                while (j < text.Size() && text[j] != utf8char('"'))
                {
                    ++j;
                }
                if (j >= text.Size())
                {
                    break;
                }
                const StringView uri = text.SubStr(begin, j - begin);
                i = j;

                if (uri.IsEmpty())
                {
                    continue;
                }
                if (uri.Size() >= 5 && uri.SubStr(0, 5) == StringView(u8"data:"))
                {
                    continue;
                }
                bool escapes = false;
                for (usize k = 0; k + 1 < uri.Size(); ++k)
                {
                    if (uri[k] == utf8char('.') && uri[k + 1] == utf8char('.'))
                    {
                        escapes = true;
                        break;
                    }
                }
                if (escapes)
                {
                    continue;
                }

                String from(dir);
                from.Append(uri);
                // A uri the package does not ship (a package can name a PNG twin for every DDS
                // and include only the DDS) is a warning, never a failed write: the deferred
                // flush must not fail the whole import over it, same as the inline copy below.
                if (!FileExists(from.AsView()))
                {
                    LOG_WARNING(u8"Import", u8"gltf sidecar missing: {}", uri);
                    continue;
                }
                if (deferredWrites != nullptr)
                {
                    pipeline::DeferredImportWrite copy;
                    copy.copyFrom = from;
                    copy.copyTo = PathJoin(context.sourcesRoot.AsView(), uri);
                    deferredWrites->PushBack(static_cast<pipeline::DeferredImportWrite&&>(copy));
                    continue;
                }
                Result<Array<byte>> payload = ReadFile(from.AsView());
                if (!payload.HasValue())
                {
                    LOG_WARNING(u8"Import", u8"gltf sidecar missing: {}", uri);
                    continue;
                }
                const Status saved = sources.AsWritable()->Save(
                    uri, Span<const byte>(payload.Value().Data(), payload.Value().Size()));
                if (!saved.IsOk())
                {
                    LOG_WARNING(u8"Import", u8"gltf sidecar copy failed: {}", uri);
                }
            }
        }

        // Reuse or claim (re-import semantics): a same-named instance of the same type from a
        // previous import is reused. Its guid survives, so cooked products overwrite in place
        // and everything referencing it (materials, prefabs, placed scenes) follows the
        // re-imported content. Names already claimed this run (two source textures named
        // alike) or held by a different type get numeric suffixes.
        [[nodiscard]] static content::Instance* ClaimInstance(content::Group& group,
                                                              StringView base, const TypeInfo& type,
                                                              Array<String>& claimed)
        {
            String name(base);
            for (u32 n = 2;; ++n)
            {
                bool taken = false;
                for (const String& c : claimed)
                {
                    if (c.AsView() == name.AsView())
                    {
                        taken = true;
                        break;
                    }
                }
                if (!taken)
                {
                    content::Instance* existing = group.GetInstance(name.AsView());
                    const StringView typeName(reinterpret_cast<const utf8char*>(type.name));
                    if (existing == nullptr || existing->TypeName() == typeName)
                    {
                        content::Instance* instance =
                            (existing != nullptr) ? existing
                                                  : group.CreateInstance(name.AsView(), type);
                        if (instance != nullptr)
                        {
                            claimed.PushBack(Move(name));
                        }
                        return instance;
                    }
                }
                name = Format(u8"{}.{}", base, n);
            }
        }

        // A texture the loader left on disk (a DDS): copied into Sources/ and referenced by a
        // file-backed asset, so the cook passes its GPU-ready levels through instead of embedding
        // decoded pixels. Usage comes from the material slot (normal or data mask), the rest from
        // the file's own header facts. Returns the instance (null = deselected or failed).
        // A model-relative uri ("textures/shared/x.dds": no root, no drive, no "..") keeps its
        // path under Sources/, the way the glTF sidecar copy lays files out; anything else
        // (an FBX's resolved absolute path) lands flat by file name.
        [[nodiscard]] static bool IsModelRelativeUri(StringView uri)
        {
            if (uri.IsEmpty() || uri[0] == utf8char('/') || uri[0] == utf8char('\\'))
            {
                return false;
            }
            if (uri.Size() >= 2 && uri[1] == utf8char(':'))
            {
                return false; // a drive
            }
            for (usize k = 0; k + 1 < uri.Size(); ++k)
            {
                if (uri[k] == utf8char('.') && uri[k + 1] == utf8char('.'))
                {
                    return false;
                }
            }
            return true;
        }

        static content::Instance* ImportFileBackedTexture(
            const foundation::model::ModelTexture& t, usize index, bool normalSlot, bool linear,
            const pipeline::ImportContext& context, bool sidecarsCopied, content::Group& group,
            Array<String>& claimed, Array<pipeline::DeferredImportWrite>* deferredWrites,
            const pipeline::ImportOptions& sel)
        {
            const StringView sourceFile = t.sourceFile();
            const bool relative = IsModelRelativeUri(t.uri());
            // The name the asset references under Sources/: the model-relative uri, else the file.
            const StringView fileName = relative ? t.uri() : pipeline::FileNameOf(sourceFile);
            if (fileName.IsEmpty())
            {
                return nullptr;
            }
            const String texBase = ImportedTextureName(t, index);
            if (!sel.SelectionEnabled(pipeline::ImportResourceKind::Texture, texBase.AsView()))
            {
                return nullptr;
            }
            // The .gltf sidecar pass already copies every relative uri (the same bytes to the
            // same place), and copying again here would double a large package's IO. Only what
            // it did not cover is copied: an absolute reference, or a container with no sidecar pass.
            if (!(relative && sidecarsCopied))
            {
                if (deferredWrites != nullptr)
                {
                    pipeline::DeferredImportWrite copy;
                    copy.copyFrom = String(sourceFile);
                    copy.copyTo = PathJoin(context.sourcesRoot.AsView(), fileName);
                    deferredWrites->PushBack(static_cast<pipeline::DeferredImportWrite&&>(copy));
                }
                else
                {
                    Result<Array<byte>> bytes = ReadFile(sourceFile);
                    if (!bytes.HasValue())
                    {
                        return nullptr;
                    }
                    foundation::vfs::NativeFileSystem sources(context.sourcesRoot.AsView(),
                                                              *context.allocator);
                    if (!sources.AsWritable()
                             ->Save(fileName, Span<const byte>(bytes.Value().Data(),
                                                               bytes.Value().Size()))
                             .IsOk())
                    {
                        return nullptr;
                    }
                }
            }
            pipeline::TextureAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName);
            asset.sourceHint = String(t.uri());
            // The file's facts first (BC5 is a normal, BC4 a mask, a DX10 colour space), then the
            // slot, which knows what the material does with the map.
            pipeline::TextureFileImporter::SetupForDds(asset, sourceFile,
                                                       pipeline::FileStemOf(fileName));
            if (normalSlot)
            {
                asset.SetupForNormalMap();
            }
            else if (linear)
            {
                asset.SetupForDataMask();
            }
            content::Instance* inst = ClaimInstance(
                group, sel.SelectionName(pipeline::ImportResourceKind::Texture, texBase.AsView()),
                pipeline::TextureAsset::StaticType(), claimed);
            if (inst == nullptr || !inst->WriteObject(asset).IsOk())
            {
                return nullptr;
            }
            return inst;
        }

        static void ImportTextures(const foundation::model::Model& model,
                                   const pipeline::ImportContext& context, bool sidecarsCopied,
                                   content::Group& group, Array<Guid>& outGuids,
                                   Array<String>& claimed,
                                   Array<pipeline::DeferredImportWrite>* deferredWrites,
                                   const pipeline::ImportOptions& sel)
        {
            // Color space follows usage: data maps (normal, metal-rough, AO) stay linear, since
            // sRGB-decoding them corrupts the values (a flat normal 0.5 would linearize to ~0.21).
            Array<bool> linear;
            ClassifyLinearTextures(model, linear);
            Array<bool> normal;
            ClassifyNormalTextures(model, normal);

            const Span<foundation::model::ModelTexture* const> textures = model.textures();
            for (usize i = 0; i < textures.Size(); ++i)
            {
                const foundation::model::ModelTexture& t = *textures[i];
                const u8* data = t.getData();
                const i32 size = t.getDataSize();
                const bool rgba8 = (data != nullptr && t.width > 0 && t.height > 0 &&
                                    size == t.width * t.height * 4);
                if (!rgba8)
                {
                    content::Instance* fileBacked =
                        t.sourceFile().IsEmpty()
                            ? nullptr
                            : ImportFileBackedTexture(t, i, i < normal.Size() && normal[i],
                                                      i < linear.Size() && linear[i], context,
                                                      sidecarsCopied, group, claimed,
                                                      deferredWrites, sel);
                    outGuids.PushBack(fileBacked != nullptr ? fileBacked->Id() : Guid{});
                    continue;
                }

                pipeline::TextureAsset asset;
                asset.embeddedWidth = static_cast<u32>(t.width);
                asset.embeddedHeight = static_cast<u32>(t.height);
                asset.colorSpace =
                    (i < linear.Size() && linear[i])
                        ? foundation::image::ImageColorSpace::Linear // data maps (normal, MR, AO)
                        : foundation::image::ImageColorSpace::Srgb;  // color maps (albedo, emissive)
                asset.generateMipmaps = true; // mips at cook; without them minified textures shimmer
                // Provenance for the texture page: the model's image uri (display only;
                // asset names prefer the file stem, but a review-dialog rename or an
                // authored-name fallback can still diverge from the file).
                asset.sourceHint = String(t.uri());

                // Asset names prefer the file stem (authored image names can misdescribe the
                // file, such as packed arm maps named "*_rough"); embedded images fall
                // back to the authored name.
                const String texBase = ImportedTextureName(t, i);
                if (!sel.SelectionEnabled(pipeline::ImportResourceKind::Texture, texBase.AsView()))
                {
                    outGuids.PushBack(Guid{}); // deselected: dependents wire no texture
                    continue;
                }
                content::Instance* inst = ClaimInstance(
                    group,
                    sel.SelectionName(pipeline::ImportResourceKind::Texture, texBase.AsView()),
                    pipeline::TextureAsset::StaticType(), claimed);
                if (inst == nullptr || !inst->WriteObject(asset).IsOk())
                {
                    outGuids.PushBack(Guid{});
                    continue;
                }
                const Span<const byte> pixels{reinterpret_cast<const byte*>(data),
                                              static_cast<usize>(size)};
                if (deferredWrites != nullptr)
                {
                    // Decoded pixels are the import's bulk (hundreds of MB for a big model):
                    // park them for the worker flush; the view borrows from the prepared
                    // model, which the caller keeps alive until the flush completes.
                    pipeline::DeferredImportWrite write;
                    write.instance = inst;
                    write.streamName = String(u8"pixels");
                    write.view = pixels;
                    deferredWrites->PushBack(static_cast<pipeline::DeferredImportWrite&&>(write));
                    outGuids.PushBack(inst->Id());
                    continue;
                }
                const Status ds = inst->WriteData(u8"pixels", pixels);
                outGuids.PushBack(ds.IsOk() ? inst->Id() : Guid{});
            }
        }

        // Bake-once cache for FBX's separate metal and rough pairs (materials often share maps).
        static Guid GetOrBakePackedMR(const foundation::model::Model& model, content::Group& group,
                                      HashMap<u64, Guid>& cache, i32 roughIdx, i32 metalIdx,
                                      Array<String>& claimed,
                                      Array<pipeline::DeferredImportWrite>* deferredWrites)
        {
            const u64 key = (static_cast<u64>(static_cast<u32>(roughIdx)) << 32) |
                            static_cast<u64>(static_cast<u32>(metalIdx));
            if (const Guid* hit = cache.Find(key))
            {
                return *hit;
            }

            u32 w = 0, h = 0;
            Array<u8> pixels = BakePackedMetallicRoughness(model, roughIdx, metalIdx, w, h);
            if (pixels.IsEmpty())
            {
                cache.InsertOrAssign(key, Guid{});
                return Guid{};
            }

            pipeline::TextureAsset asset;
            asset.embeddedWidth = w;
            asset.embeddedHeight = h;
            asset.colorSpace = foundation::image::ImageColorSpace::Linear; // data map
            asset.generateMipmaps = true; // mips at cook; without them minified textures shimmer
            const String name = Format(u8"mr.packed.{}.{}", roughIdx, metalIdx);
            content::Instance* inst = ClaimInstance(
                group, name.AsView(), pipeline::TextureAsset::StaticType(), claimed);
            if (inst == nullptr || !inst->WriteObject(asset).IsOk())
            {
                cache.InsertOrAssign(key, Guid{});
                return Guid{};
            }
            if (deferredWrites != nullptr)
            {
                // Baked pixels are produced here, so the deferred write owns them.
                pipeline::DeferredImportWrite write;
                write.instance = inst;
                write.streamName = String(u8"pixels");
                for (u8 b : pixels)
                {
                    write.owned.PushBack(static_cast<byte>(b));
                }
                deferredWrites->PushBack(static_cast<pipeline::DeferredImportWrite&&>(write));
            }
            else if (!inst->WriteData(u8"pixels",
                                      Span<const byte>{reinterpret_cast<const byte*>(pixels.Data()),
                                                       pixels.Size()})
                          .IsOk())
            {
                cache.InsertOrAssign(key, Guid{});
                return Guid{};
            }
            cache.InsertOrAssign(key, inst->Id());
            return inst->Id();
        }

        static void ImportMaterials(const foundation::model::Model& model, content::Group& group,
                                    const Array<Guid>& textureGuids, ModelManifestSource& manifest,
                                    Array<String>& claimed,
                                    Array<pipeline::DeferredImportWrite>* deferredWrites,
                                    const pipeline::ImportOptions& sel)
        {
            HashMap<u64, Guid> bakedMR; // per-pair bake cache (see GetOrBakePackedMR)
            const Span<foundation::model::ModelMaterial* const> materials = model.materials();
            for (usize i = 0; i < materials.Size(); ++i)
            {
                const foundation::model::ModelMaterial& m = *materials[i];
                const String matBase = ImportedAssetName(m.name(), u8"mat", i);
                if (!sel.SelectionEnabled(pipeline::ImportResourceKind::Material,
                                          matBase.AsView()))
                {
                    // Deselected: hold the manifest slots (nil) so submesh materialIndex
                    // stays aligned.
                    manifest.materialGuids.PushBack(Guid{});
                    manifest.materialAlbedo.PushBack(Guid{});
                    continue;
                }
                // On the built-in its shading model names, every texture it reads wired in
                // (Cook's BuildModelMaterial, shared with the direct cook); FBX's separate metal
                // and rough maps baked into one packed texture asset here, once a pair.
                pipeline::MaterialAsset asset;
                BuildModelMaterial(model, m, matBase.AsView(), textureGuids,
                                   [&](i32 roughIndex, i32 metalIndex)
                                   { return GetOrBakePackedMR(model, group, bakedMR, roughIndex, metalIndex, claimed, deferredWrites); },
                                   asset);

                content::Instance* inst = ClaimInstance(
                    group,
                    sel.SelectionName(pipeline::ImportResourceKind::Material, matBase.AsView()),
                    pipeline::MaterialAsset::StaticType(), claimed);
                if (inst == nullptr || !inst->WriteObject(asset).IsOk())
                {
                    manifest.materialGuids.PushBack(Guid{});
                    manifest.materialAlbedo.PushBack(Guid{});
                    continue;
                }
                manifest.materialGuids.PushBack(inst->Id());

                const i32 tIdx = m.baseColorTextureIndex;
                manifest.materialAlbedo.PushBack(
                    (tIdx >= 0 && static_cast<usize>(tIdx) < textureGuids.Size())
                        ? textureGuids[static_cast<usize>(tIdx)]
                        : Guid{});
            }
        }

        // The importer's deterministic skeleton base name (also the review plan's key).
        [[nodiscard]] static String SkeletonBaseName(const foundation::model::ModelSkin& skin)
        {
            return skin.name().IsEmpty() ? String(u8"skeleton")
                                         : ImportedAssetName(skin.name(), u8"skeleton", 0);
        }

        static void ImportSkeletonAndClips(const foundation::model::Model& model,
                                           content::Group& group, ModelManifestSource& manifest,
                                           Array<String>& claimed,
                                           const ModelImportOptions& sel)
        {
            if (model.skins().Size() == 0)
            {
                return;
            }
            const foundation::model::ModelSkin& skin = *model.skins()[0];
            const Array<i32> joints = SkeletonJoints(model, skin); // the skin's, then the bones it hangs from
            const HashMap<i32, i32> boneToJoint = BuildBoneToJoint(joints.AsSpan());
            manifest.skeletonParentNode = SkeletonParentNode(model, joints.AsSpan());

            const String skelBase = SkeletonBaseName(skin);
            if (sel.SelectionEnabled(pipeline::ImportResourceKind::Skeleton, skelBase.AsView()))
            {
                pipeline::SkeletonAsset skeleton;
                SkeletonSourceFromModel(model, skin, joints.AsSpan(), boneToJoint, skeleton.source);
                content::Instance* skelInst = ClaimInstance(
                    group,
                    sel.SelectionName(pipeline::ImportResourceKind::Skeleton, skelBase.AsView()),
                    pipeline::SkeletonAsset::StaticType(), claimed);
                if (skelInst != nullptr && skelInst->WriteObject(skeleton).IsOk())
                {
                    manifest.skeletonGuid = skelInst->Id();
                }
            }

            const Span<foundation::model::ModelAnimation* const> animations = model.animations();
            for (usize a = 0; a < animations.Size(); ++a)
            {
                const String clipBase = ImportedAssetName(animations[a]->name(), u8"anim", a);
                if (!sel.SelectionEnabled(pipeline::ImportResourceKind::AnimationClip,
                                          clipBase.AsView()))
                {
                    continue; // animationGuids is a plain list, with no slot to hold
                }
                content::Instance* clipInst = ClaimInstance(
                    group,
                    sel.SelectionName(pipeline::ImportResourceKind::AnimationClip,
                                      clipBase.AsView()),
                    pipeline::AnimationClipAsset::StaticType(), claimed);
                pipeline::AnimationClipAsset clip;
                AnimationClipSourceFromModel(
                    *animations[a], boneToJoint,
                    (clipInst != nullptr) ? clipInst->Name() : StringView(u8"anim"), clip.source,
                    manifest.skeletonParentNode);
                // For the root motion cook: the skeleton, and the armature's rest (its own channels
                // are the clip's model tracks).
                clip.skeleton = manifest.skeletonGuid;
                const Span<foundation::model::ModelBone* const> nodes = model.bones();
                if (manifest.skeletonParentNode >= 0 &&
                    static_cast<usize>(manifest.skeletonParentNode) < nodes.Size())
                {
                    const foundation::model::ModelBone& armature =
                        *nodes[static_cast<usize>(manifest.skeletonParentNode)];
                    clip.modelRest.position = armature.translation;
                    clip.modelRest.rotation = armature.rotation;
                    clip.modelRest.scale = armature.scale;
                }
                // A re-import keeps what was authored on the clip (its page): its root motion
                // settings, whether it loops (a model file does not say; a Die plays once) and its
                // events, each held inside the clip's new length.
                if (clipInst != nullptr)
                {
                    RefPtr<ISerializable> previous = clipInst->ReadObject();
                    if (const auto* before = Cast<pipeline::AnimationClipAsset>(previous.Get()))
                    {
                        clip.source.rootMotion = before->source.rootMotion;
                        clip.source.isLooping = before->source.isLooping;
                        clip.source.eventTimes = before->source.eventTimes;
                        clip.source.eventNames = before->source.eventNames;
                        for (f32& time : clip.source.eventTimes)
                        {
                            time = Clamp(time, 0.0f, clip.source.duration);
                        }
                    }
                    else if (sel.rootMotion)
                    {
                        clip.source.rootMotion.horizontal = true;
                        clip.source.rootMotion.yaw = true;
                    }
                }
                // Set for this clip in the review (the import window's clip details): that, over both.
                if (const auto* own = Cast<ClipImportSettings>(sel.SelectionSettings(pipeline::ImportResourceKind::AnimationClip, clipBase.AsView())))
                {
                    clip.source.rootMotion.horizontal = own->travel;
                    clip.source.rootMotion.vertical = own->vertical;
                    clip.source.rootMotion.yaw = own->turn;
                }
                if (clipInst != nullptr && clipInst->WriteObject(clip).IsOk())
                {
                    manifest.animationGuids.PushBack(clipInst->Id());
                }
            }
        }

        // Authored LOD collapse: "Foo_LOD1" and "Foo_LOD2" meshes become chain levels of the mesh
        // named "Foo" instead of assets of their own. lodOf[i] is the base mesh index a
        // suffixed mesh folds into (or -1); levels are gathered per base sorted by their
        // suffix number. A mixed pair (a skinned base with a static level or the reverse) is
        // refused, since the parallel-stream contract cannot hold across the mismatch. Shared by
        // the import fan-out and DescribeImport so the review plan lists exactly the mesh
        // assets the import creates.
        static void ComputeLodFold(const foundation::model::Model& model, bool logMismatch,
                                   Array<i32>& lodOf, Array<Array<usize>>& lodLevels)
        {
            const bool hasSkin = model.skins().Size() > 0;
            const Span<foundation::model::ModelMesh* const> meshes = model.meshes();
            lodOf.Resize(meshes.Size());
            lodLevels.Resize(meshes.Size());
            for (usize i = 0; i < meshes.Size(); ++i)
            {
                lodOf[i] = -1;
            }
            for (usize i = 0; i < meshes.Size(); ++i)
            {
                String lodBase;
                const u32 level = pipeline::ParseLodSuffix(StringView(meshes[i]->name()), lodBase);
                if (level == 0)
                {
                    continue;
                }
                i32 baseIndex = -1;
                for (usize j = 0; j < meshes.Size(); ++j)
                {
                    if (j != i && StringView(meshes[j]->name()) == lodBase.AsView())
                    {
                        baseIndex = static_cast<i32>(j);
                        break;
                    }
                }
                if (baseIndex < 0)
                {
                    continue; // no base of that name: a plain mesh that happens to end _LODn
                }
                if ((IsSkinnedMesh(*meshes[baseIndex]) && hasSkin) !=
                    (IsSkinnedMesh(*meshes[i]) && hasSkin))
                {
                    if (logMismatch)
                    {
                        LOG_WARNING(u8"Import",
                                    u8"mesh '{}': LOD level and base disagree on skinning - "
                                    u8"importing as a separate mesh",
                                    meshes[i]->name());
                    }
                    continue;
                }
                lodOf[i] = baseIndex;
                // Insert sorted by suffix number so LOD2 lands after LOD1 regardless of node order.
                Array<usize>& levels = lodLevels[static_cast<usize>(baseIndex)];
                String otherBase;
                usize at = levels.Size();
                for (usize k = 0; k < levels.Size(); ++k)
                {
                    if (level < pipeline::ParseLodSuffix(StringView(meshes[levels[k]]->name()),
                                                        otherBase))
                    {
                        at = k;
                        break;
                    }
                }
                levels.Insert(at, i);
            }
        }

        // Fold the authored _LODn levels into a source and auto-generate a chain for a big
        // chainless mesh. This is the import's CPU bulk: on the editor path it runs inside the
        // deferred geometry write, on the worker.
        static void FoldLevelsAndChain(const foundation::model::ModelMesh& m,
                                       Span<const foundation::model::ModelMesh* const> levels,
                                       bool generateLods, foundation::geometry::StaticMeshSource& source)
        {
            for (const foundation::model::ModelMesh* level : levels)
            {
                if (!pipeline::AppendLodLevelFromModel(*level, source))
                {
                    LOG_WARNING(u8"Import",
                                u8"mesh '{}': LOD level '{}' has a different submesh count - "
                                u8"level skipped",
                                m.name(), level->name());
                }
            }
            if (source.lodCount > 1)
            {
                LOG_INFO(u8"Import", u8"mesh '{}': authored LOD chain with {} level(s)", m.name(),
                         source.lodCount);
            }
            // Auto-generation: big static meshes with no authored chain get a simplified
            // ladder (GenerateLodChain no-ops on chains).
            else if (generateLods && source.indexData.Size() >= 3u * 10000u)
            {
                (void)pipeline::GenerateLodChain(source);
            }
        }
        static void FoldLevelsAndChain(const foundation::model::ModelMesh& m,
                                       Span<const foundation::model::ModelMesh* const> levels,
                                       bool generateLods, foundation::geometry::SkinnedMeshSource& source)
        {
            for (const foundation::model::ModelMesh* level : levels)
            {
                if (!pipeline::AppendLodLevelFromModel(*level, source))
                {
                    LOG_WARNING(u8"Import",
                                u8"mesh '{}': LOD level '{}' mismatched (submesh count or skin "
                                u8"stream) - level skipped",
                                m.name(), level->name());
                }
            }
            if (source.lodCount > 1)
            {
                LOG_INFO(u8"Import", u8"mesh '{}': authored LOD chain with {} level(s)", m.name(),
                         source.lodCount);
            }
            // Simplification only drops indices, so the parallel skin stream is untouched.
            else if (generateLods && source.indexData.Size() >= 3u * 10000u)
            {
                (void)pipeline::GenerateLodChain(source);
            }
        }
        static void BuildMeshSource(const foundation::model::ModelMesh& m,
                                    Span<const foundation::model::ModelMesh* const> levels,
                                    bool generateLods, pipeline::StaticMeshAsset& asset)
        {
            StaticMeshSourceFromModel(m, asset.source);
            FoldLevelsAndChain(m, levels, generateLods, asset.source);
        }
        static void BuildMeshSource(const foundation::model::ModelMesh& m,
                                    Span<const foundation::model::ModelMesh* const> levels,
                                    bool generateLods, pipeline::SkinnedMeshAsset& asset)
        {
            SkinnedMeshSourceFromModel(m, 0, asset.source);
            FoldLevelsAndChain(m, levels, generateLods, asset.source);
        }

        // Queue a mesh asset's two deferred writes: the geometry stream (lazy when the model
        // outlives the flush: the conversion, LODs and serialization run on the worker) then
        // the envelope, in that order so the envelope's XML sees the final source.
        template <typename AssetT>
        static void QueueMeshWrites(content::Instance& inst, RefPtr<AssetT> asset,
                                    const foundation::model::ModelMesh& m,
                                    Array<const foundation::model::ModelMesh*> levels,
                                    bool generateLods, bool lazy,
                                    Array<pipeline::DeferredImportWrite>& deferredWrites)
        {
            pipeline::DeferredImportWrite geometry;
            geometry.instance = &inst;
            geometry.streamName = String(pipeline::kMeshGeometryStreamName);
            if (lazy)
            {
                const foundation::model::ModelMesh* mesh = &m;
                geometry.produce = [asset, mesh, meshLevels = Move(levels),
                                    generateLods](Array<byte>& bytes) -> Status
                {
                    BuildMeshSource(*mesh,
                                    Span<const foundation::model::ModelMesh* const>{meshLevels.Data(),
                                                                                        meshLevels.Size()},
                                    generateLods, *asset);
                    pipeline::detail::MeshSourceToBytes(asset->source, bytes);
                    return Status{};
                };
            }
            else
            {
                BuildMeshSource(m,
                                Span<const foundation::model::ModelMesh* const>{levels.Data(),
                                                                                levels.Size()},
                                generateLods, *asset);
                pipeline::detail::MeshSourceToBytes(asset->source, geometry.owned);
            }
            deferredWrites.PushBack(static_cast<pipeline::DeferredImportWrite&&>(geometry));
            pipeline::DeferredImportWrite envelope;
            envelope.instance = &inst;
            envelope.object = RefPtr<ISerializable>(asset.Get());
            deferredWrites.PushBack(static_cast<pipeline::DeferredImportWrite&&>(envelope));
        }

        [[nodiscard]] static Status ImportMeshes(IAllocator& allocator,
                                                 const foundation::model::Model& model,
                                                 content::Group& group,
                                                 ModelManifestSource& manifest,
                                                 Array<String>& claimed,
                                                 Array<pipeline::DeferredImportWrite>* deferredWrites,
                                                 bool generateLods,
                                                 const pipeline::ImportOptions& sel,
                                                 Array<String>& meshSourceNames,
                                                 bool modelOutlivesWrites)
        {
            const bool hasSkin = model.skins().Size() > 0;
            const Span<foundation::model::ModelMesh* const> meshes = model.meshes();
            Array<i32> lodOf;
            Array<Array<usize>> lodLevels; // per mesh: consumed level indices, suffix order
            ComputeLodFold(model, /*logMismatch*/ true, lodOf, lodLevels);

            for (usize i = 0; i < meshes.Size(); ++i)
            {
                if (lodOf[i] >= 0)
                {
                    // Consumed as a chain level of its base: no asset of its own, but the
                    // manifest slot must hold (nil guid). node.meshIndex is a model mesh
                    // index, and dropping the entry would shift every later mesh's slot, so
                    // nodes would reference the wrong mesh or fall out of range and the
                    // generated prefab or scene would lose meshes on authored-LOD models.
                    const foundation::model::ModelMesh& folded = *meshes[i];
                    manifest.meshGuids.PushBack(Guid{});
                    manifest.meshSkinned.PushBack(
                        (IsSkinnedMesh(folded) && hasSkin) ? u8{1} : u8{0});
                    const Span<const foundation::model::ModelMeshPart> foldedParts =
                        folded.parts();
                    manifest.meshMaterial.PushBack(
                        foldedParts.Size() > 0 ? foldedParts[0].materialIndex : -1);
                    manifest.meshMaterialSlots.PushBack(foundation::model::ModelMeshMaterialSlots{}); // held slot: no mesh
                    meshSourceNames.PushBack(ImportedAssetName(folded.name(), u8"mesh", i));
                    continue;
                }
                const foundation::model::ModelMesh& m = *meshes[i];
                const bool skinned = IsSkinnedMesh(m) && hasSkin;
                const String baseName = ImportedAssetName(m.name(), u8"mesh", i);
                if (!sel.SelectionEnabled(pipeline::ImportResourceKind::Mesh, baseName.AsView()))
                {
                    // Deselected in the review dialog: hold the manifest slot (nil guid) so
                    // parallel arrays and node mesh indices keep their shape.
                    manifest.meshGuids.PushBack(Guid{});
                    manifest.meshSkinned.PushBack(skinned ? u8{1} : u8{0});
                    const Span<const foundation::model::ModelMeshPart> skippedParts = m.parts();
                    manifest.meshMaterial.PushBack(
                        skippedParts.Size() > 0 ? skippedParts[0].materialIndex : -1);
                    manifest.meshMaterialSlots.PushBack(foundation::model::ModelMeshMaterialSlots{}); // held slot: no mesh
                    meshSourceNames.PushBack(baseName);
                    continue;
                }
                const StringView name =
                    sel.SelectionName(pipeline::ImportResourceKind::Mesh, baseName.AsView());

                // Mesh envelopes are the import's largest serialization cost (a big mesh
                // source rendered to XML), so defer the object and its write to the worker flush.
                content::Instance* inst = nullptr;
                Status written;
                Array<const foundation::model::ModelMesh*> levels; // authored _LODn siblings
                for (const usize levelIndex : lodLevels[i])
                {
                    levels.PushBack(meshes[levelIndex]);
                }
                const Span<const foundation::model::ModelMesh* const> levelSpan{levels.Data(),
                                                                               levels.Size()};
                if (skinned)
                {
                    auto asset = MakeRef<pipeline::SkinnedMeshAsset>(allocator);
                    inst = ClaimInstance(
                        group, name, pipeline::SkinnedMeshAsset::StaticType(), claimed);
                    if (inst == nullptr)
                    {
                        return Status{ErrorCode::Unknown};
                    }
                    if (deferredWrites != nullptr)
                    {
                        QueueMeshWrites(*inst, asset, m, Move(levels), generateLods,
                                        modelOutlivesWrites, *deferredWrites);
                    }
                    else
                    {
                        BuildMeshSource(m, levelSpan, generateLods, *asset);
                        written = pipeline::WriteMeshAsset(*inst, *asset);
                    }
                }
                else
                {
                    auto asset = MakeRef<pipeline::StaticMeshAsset>(allocator);
                    inst = ClaimInstance(
                        group, name, pipeline::StaticMeshAsset::StaticType(), claimed);
                    if (inst == nullptr)
                    {
                        return Status{ErrorCode::Unknown};
                    }
                    if (deferredWrites != nullptr)
                    {
                        QueueMeshWrites(*inst, asset, m, Move(levels), generateLods,
                                        modelOutlivesWrites, *deferredWrites);
                    }
                    else
                    {
                        BuildMeshSource(m, levelSpan, generateLods, *asset);
                        written = pipeline::WriteMeshAsset(*inst, *asset);
                    }
                }
                if (!written.IsOk())
                {
                    return written;
                }

                manifest.meshGuids.PushBack(inst->Id());
                manifest.meshSkinned.PushBack(skinned ? u8{1} : u8{0});
                const Span<const foundation::model::ModelMeshPart> parts = m.parts();
                manifest.meshMaterial.PushBack(parts.Size() > 0 ? parts[0].materialIndex : -1);
                {
                    foundation::model::ModelMeshMaterialSlots slots;
                    CollectMeshMaterialSlots(m, slots.slots);
                    manifest.meshMaterialSlots.PushBack(Move(slots));
                }
                meshSourceNames.PushBack(baseName);
            }
            return Status{};
        }

        // One CollisionShapeAsset per static mesh (skinned meshes don't get collision);
        // the guid lands in manifest.collisionGuids (parallel; nil means none) for the
        // prefab generator to wire colliders from.
        static void ImportCollisionShapes(content::Group& group, ModelManifestSource& manifest,
                                          bool convex, Array<String>& claimed,
                                          const pipeline::ImportOptions& sel,
                                          const Array<String>& meshSourceNames)
        {
            for (usize i = 0; i < manifest.meshGuids.Size(); ++i)
            {
                if (manifest.meshSkinned[i] != 0 || manifest.meshGuids[i].IsNil())
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                // Plan key: the mesh's source base and ".collision" (stable under mesh renames;
                // the created name below still derives from the live mesh instance name, so a
                // renamed mesh cascades to its shape's name).
                String selKey(i < meshSourceNames.Size() ? meshSourceNames[i].AsView()
                                                         : StringView(u8"mesh"));
                selKey.Append(u8".collision");
                if (!sel.SelectionEnabled(pipeline::ImportResourceKind::Collision,
                                          selKey.AsView()))
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                content::Instance* meshInstance = nullptr;
                for (content::Instance* candidate : group.Instances())
                {
                    if (candidate->Id() == manifest.meshGuids[i])
                    {
                        meshInstance = candidate;
                        break;
                    }
                }
                String name(meshInstance != nullptr ? meshInstance->Name() : StringView(u8"mesh"));
                name.Append(u8".collision");
                const StringView renamed =
                    sel.SelectionName(pipeline::ImportResourceKind::Collision, selKey.AsView());
                if (renamed != selKey.AsView())
                {
                    name = String(renamed); // explicit user rename wins over the derived name
                }
                content::Instance* inst =
                    ClaimInstance(group, name.AsView(),
                                  pipeline::CollisionShapeAsset::StaticType(), claimed);
                if (inst == nullptr)
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                pipeline::CollisionShapeAsset asset;
                asset.sourceMesh = manifest.meshGuids[i];
                asset.cook = convex ? pipeline::CollisionCookKind::ConvexHull
                                    : pipeline::CollisionCookKind::TriangleMesh;
                if (!inst->WriteObject(asset).IsOk())
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                manifest.collisionGuids.PushBack(inst->Id());
            }
        }

        static void ImportNodes(const foundation::model::Model& model, ModelManifestSource& manifest)
        {
            const Span<foundation::model::ModelBone* const> bones = model.bones();
            for (usize i = 0; i < bones.Size(); ++i)
            {
                const foundation::model::ModelBone& b = *bones[i];
                ModelNode n;
                n.name = String(b.name());
                n.parentIndex = b.parentIndex;
                n.localTransform.position = b.translation;
                n.localTransform.rotation = b.rotation;
                n.localTransform.scale = b.scale;
                n.meshIndex = b.meshIndex;
                manifest.nodes.PushBack(Move(n));
            }
        }
    };

    // Registers the manifest asset type for content-DB construction and deserialization.
    inline void RegisterModelManifestAsset()
    {
        // Data version 3: the skeleton's parent node (v2: per-mesh material slots; v1:
        // importSelection, the re-import memory). v1 and v2 still read (legacy readers): a v1
        // manifest's meshes were cooked with model-wide submesh indices and the prefab builder
        // keeps the whole list for them; a v2 one knows no skeleton parent and its prefab keeps
        // the file's placement. No REFLECT block owns this type, so it is set on the TypeInfo.
        const_cast<TypeInfo&>(ModelManifestAsset::StaticType()).dataVersion = 3;
        KeepLegacyReader(const_cast<TypeInfo&>(ModelManifestAsset::StaticType()), 1);
        GlobalTypeRegistry().Register(ModelManifestAsset::StaticType());
        RegisterSerializable<ModelManifestAsset>();
    }

    RTTI_DEFINE_OBJECT(LoadedModel, "rtti::pipeline::modelimporter")
}

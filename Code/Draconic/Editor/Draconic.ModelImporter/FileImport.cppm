// Draconic::ModelImporter - :file_import partition.
//
// The SOURCE-side model importer for the editor pipeline (asset-pipeline design §4/§7): a
// dropped model file fans out into REAL source instances in the content DB - textures
// (embedded-pixels TextureAssets), materials (MaterialAssets), meshes (Static/SkinnedMeshAssets),
// skeleton + clips (animation assets), and a ModelManifestAsset tying them together - so the
// pipeline owns every cook incrementally and each fanned-out asset is individually editable
// (tweak one material without touching the model). Product guid = source guid keeps the
// manifest's recorded guids valid in the cooked DB, where the runtime's ModelResource composite
// binds them.
//
// (This is the editor path; CookModel in :cook remains the direct/sample path that bakes a
// loaded model straight into a runtime DB.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <initializer_list>

export module draconic.modelimporter:file_import;

import draconic.foundation;
import draconic.model;
import draconic.model.io;
import draconic.model.gltf;
import draconic.model.fbx;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.geometry.editor;
import draconic.materials;
import draconic.materials.resource;
import draconic.materials.editor;
import draconic.texture;
import draconic.texture.editor;
import draconic.image;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.vfs;
import draconic.content;
import draconic.editor;
import draconic.editor.core;
import draconic.physics.editor;
import :mesh_convert;
import :anim_convert;
import draconic.model.resource;
import :cook; // IsSkinnedMesh + the conversion helpers' home

using namespace draconic::foundation;

export namespace draconic::modelimporter
{
    // The cooked-model runtime types now live in draconic::model (draconic.model.resource).
    using draconic::model::ModelManifestSource;
    using draconic::model::ModelNode;
    using draconic::model::ModelResource;

    namespace content = draconic::content;
    namespace editor = draconic::editor;

    // Source asset embedding a ModelManifestSource (built at import; the cook writes it
    // through). Guids inside are source guids == product guids.
    class ModelManifestAsset final : public editor::Asset
    {
        DRACONIC_OBJECT(ModelManifestAsset, editor::Asset)
    public:
        ModelManifestSource manifest;

        void Serialize(ISerializer& ar) override
        {
            editor::Asset::Serialize(ar); // fileName = the imported model file (re-import seed)
            manifest.Serialize(ar);
        }
    };

    class ModelManifestAssetBuilder final : public editor::DefaultAssetBuilder
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

        // Everything the manifest points at is a runtime REFERENCE: the products must exist,
        // but their content never re-cooks the manifest.
        void ScanDependencies(const editor::Asset& asset, editor::AssetBuildContext&,
                              editor::AssetDependencies& out) override
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

        [[nodiscard]] Status Build(const editor::Asset& asset,
                                   editor::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const ModelManifestAsset& ma = static_cast<const ModelManifestAsset&>(asset);
            return ctx.output->WriteObject(const_cast<ModelManifestSource&>(ma.manifest));
        }
    };

    /// Options for one model import (the import dialog renders the toggles).
    class ModelImportOptions final : public editor::ImportOptions
    {
        DRACONIC_OBJECT(ModelImportOptions, editor::ImportOptions)
    public:
        bool importTextures = true;     // embedded/sidecar images -> TextureAssets
        bool importMaterials = true;    // PBR materials (texture slots wired when textures import)
        bool importAnimations = true;   // skeleton + clips
        bool generatePrefab = true;     // hierarchy prefab beside the manifest (post-import step)
        bool generateCollision = false; // CollisionShapeAsset per mesh + colliders on the prefab
        bool collisionConvex = false;   // hull (dynamic-capable) instead of exact triangle mesh

        [[nodiscard]] Array<Toggle> Toggles() override
        {
            Array<Toggle> toggles;
            toggles.PushBack(Toggle{u8"Textures", u8"Import the model's images as texture assets",
                                    &importTextures});
            toggles.PushBack(Toggle{
                u8"Materials", u8"Import PBR materials (textures wire in when they import too)",
                &importMaterials});
            toggles.PushBack(Toggle{u8"Animations", u8"Import the skeleton and animation clips",
                                    &importAnimations});
            toggles.PushBack(Toggle{u8"Generate prefab",
                                    u8"Create a spawnable prefab of the model's node hierarchy; "
                                    u8"re-import regenerates it",
                                    &generatePrefab});
            toggles.PushBack(Toggle{u8"Generate collision",
                                    u8"Cook a collision shape per mesh and add colliders (+ a "
                                    u8"static rigid body) to the generated prefab",
                                    &generateCollision});
            toggles.PushBack(Toggle{
                u8"Convex collision",
                u8"Simplified convex hulls (dynamic-capable) instead of exact triangle meshes",
                &collisionConvex});
            return toggles;
        }

        void Serialize(ISerializer& ar) override
        {
            u8 textures = importTextures ? 1u : 0u;
            u8 materials = importMaterials ? 1u : 0u;
            u8 animations = importAnimations ? 1u : 0u;
            u8 prefab = generatePrefab ? 1u : 0u;
            u8 collision = generateCollision ? 1u : 0u;
            u8 convex = collisionConvex ? 1u : 0u;
            draconic::foundation::Serialize(ar, "textures", textures);
            draconic::foundation::Serialize(ar, "materials", materials);
            draconic::foundation::Serialize(ar, "animations", animations);
            draconic::foundation::Serialize(ar, "prefab", prefab);
            draconic::foundation::Serialize(ar, "collision", collision);
            draconic::foundation::Serialize(ar, "collisionConvex", convex);
            importTextures = textures != 0;
            importMaterials = materials != 0;
            importAnimations = animations != 0;
            generatePrefab = prefab != 0;
            generateCollision = collision != 0;
            collisionConvex = convex != 0;
        }
    };

    /// PrepareOnWorker's payload: the fully loaded model (parse + texture decode = the slow
    /// 95% of a model import, safely off the UI thread).
    class LoadedModel final : public Object
    {
        DRACONIC_OBJECT(LoadedModel, Object)
    public:
        draconic::model::Model model;
    };

    /// OS-file importer for model files: loads through draconic.model and fans out source
    /// instances into a subgroup named after the file stem.
    class ModelFileImporter final : public editor::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Model"; }

        [[nodiscard]] RefPtr<editor::ImportOptions> CreateOptions() const override
        {
            return RefPtr<editor::ImportOptions>(
                MakeRef<ModelImportOptions>(DefaultAllocator()).Get());
        }

        [[nodiscard]] bool WantsWorkerPrepare() const override { return true; }

        [[nodiscard]] RefPtr<Object> PrepareOnWorker(StringView sourcePath) override
        {
            RefPtr<LoadedModel> loaded = MakeRef<LoadedModel>(DefaultAllocator());
            if (LoadModelFrom(sourcePath, loaded->model) != draconic::model::ModelLoadResult::Ok)
            {
                return {};
            }
            return RefPtr<Object>(loaded.Get());
        }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView ext : {u8"glb", u8"gltf", u8"fbx"})
            {
                if (extension == ext)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, editor::EditorProject& project, content::Group& group,
               const editor::ImportOptions* options, Object* prepared,
               Array<editor::DeferredImportWrite>* deferredWrites) override
        {
            const ModelImportOptions defaults;
            const ModelImportOptions& opt =
                (options != nullptr) ? static_cast<const ModelImportOptions&>(*options) : defaults;
            // Source provenance copy: the file NAME is known without copying; the copy
            // itself (and the .gltf sidecars below) is bulk file IO - deferred when possible.
            const StringView sourceFileName = editor::FileNameOf(sourcePath);
            if (sourceFileName.IsEmpty())
            {
                return Err(ErrorCode::InvalidArgument);
            }
            Result<String> fileName = Result<String>(String(sourceFileName));
            if (deferredWrites != nullptr)
            {
                editor::DeferredImportWrite copy;
                copy.copyFrom = String(sourcePath);
                copy.copyTo = PathJoin(project.SourcesRoot().AsView(), sourceFileName);
                deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(copy));
            }
            else
            {
                fileName = editor::CopyIntoSources(project, sourcePath);
                if (!fileName.HasValue())
                {
                    return Err(fileName.Error());
                }
            }

            // The slow load either arrived pre-baked from the worker phase, or runs inline
            // (headless/tests). Loading uses the ORIGINAL dropped path: .gltf files
            // reference sibling sidecars living next to the original, not in Sources/.
            draconic::model::Model inlineModel;
            draconic::model::Model* modelPtr = nullptr;
            if (auto* loadedPayload = Cast<LoadedModel>(prepared))
            {
                modelPtr = &loadedPayload->model;
            }
            else
            {
                if (LoadModelFrom(sourcePath, inlineModel) != draconic::model::ModelLoadResult::Ok)
                {
                    DRACONIC_LOG_ERROR(u8"Import", u8"model load failed: {}", fileName.Value());
                    return Err(ErrorCode::InvalidArgument);
                }
                modelPtr = &inlineModel;
            }
            draconic::model::Model& model = *modelPtr;

            // .gltf: copy the referenced sidecars (buffers/images by relative uri) into
            // Sources/ so the imported source set is complete.
            if (editor::FileExtensionLower(sourcePath) == StringView(u8"gltf"))
            {
                CopyGltfSidecars(sourcePath, project, deferredWrites);
            }

            const StringView stem = editor::FileStemOf(fileName.Value().AsView());
            content::Group* modelGroup = group.CreateGroup(stem);
            if (modelGroup == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }

            ModelManifestAsset manifestAsset;
            manifestAsset.fileName = draconic::vfs::SourcePath(fileName.Value().AsView());
            ModelManifestSource& manifest = manifestAsset.manifest;
            manifest.boundsMin = model.bounds().min;
            manifest.boundsMax = model.bounds().max;

            Array<String> claimed; // names claimed THIS run (ClaimInstance's dedup scope)
            Array<Guid> textureGuids;
            if (opt.importTextures)
            {
                ImportTextures(model, *modelGroup, textureGuids, claimed, deferredWrites);
            }
            else
            {
                for (usize i = 0; i < model.textures().Size(); ++i)
                {
                    textureGuids.PushBack(Guid{});
                }
            }
            if (opt.importMaterials)
            {
                ImportMaterials(model, *modelGroup, textureGuids, manifest, claimed,
                                deferredWrites);
            }
            if (opt.importAnimations)
            {
                ImportSkeletonAndClips(model, *modelGroup, manifest, claimed);
            }
            const Status meshes =
                ImportMeshes(model, *modelGroup, manifest, claimed, deferredWrites);
            if (!meshes.IsOk())
            {
                return Err(meshes.Code());
            }
            if (opt.generateCollision)
            {
                ImportCollisionShapes(*modelGroup, manifest, opt.collisionConvex, claimed);
            }
            ImportNodes(model, manifest);

            content::Instance* instance =
                modelGroup->CreateInstance(stem, ModelManifestAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            const Status written = instance->WriteObject(manifestAsset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }

    private:
        [[nodiscard]] static draconic::model::ModelLoadResult
        LoadModelFrom(StringView sourcePath, draconic::model::Model& model)
        {
            draconic::model::gltf::GltfLoader gltfLoader;
            draconic::model::fbx::FbxLoader fbxLoader;
            draconic::model::io::registerLoader(&gltfLoader);
            draconic::model::io::registerLoader(&fbxLoader);
            const draconic::model::ModelLoadResult loaded =
                draconic::model::io::loadModel(sourcePath, model);
            draconic::model::io::unregisterLoader(&fbxLoader);
            draconic::model::io::unregisterLoader(&gltfLoader);
            if (loaded == draconic::model::ModelLoadResult::Ok)
            {
                model.calculateBounds();
            }
            return loaded;
        }

        // Copy every relative "uri" the .gltf references (buffers, images) from next to the
        // original file into Sources/, preserving relative subpaths. Data URIs and
        // parent-escaping paths are skipped. A plain text scan (the uris live in JSON string
        // values); failures only log - the import itself already succeeded from the original.
        static void CopyGltfSidecars(StringView originalPath, editor::EditorProject& project,
                                     Array<editor::DeferredImportWrite>* deferredWrites)
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

            draconic::vfs::NativeFileSystem sources(project.SourcesRoot().AsView());
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
                if (deferredWrites != nullptr)
                {
                    editor::DeferredImportWrite copy;
                    copy.copyFrom = from;
                    copy.copyTo = PathJoin(project.SourcesRoot().AsView(), uri);
                    deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(copy));
                    continue;
                }
                Result<Array<byte>> payload = ReadFile(from.AsView());
                if (!payload.HasValue())
                {
                    DRACONIC_LOG_WARNING(u8"Import", u8"gltf sidecar missing: {}", uri);
                    continue;
                }
                const Status saved = sources.AsWritable()->Save(
                    uri, Span<const byte>(payload.Value().Data(), payload.Value().Size()));
                if (!saved.IsOk())
                {
                    DRACONIC_LOG_WARNING(u8"Import", u8"gltf sidecar copy failed: {}", uri);
                }
            }
        }

        // Reuse-or-claim (re-import semantics): a same-named instance OF THE SAME TYPE from a
        // previous import is REUSED - its guid survives, so cooked products overwrite in place
        // and everything referencing it (materials, prefabs, placed scenes) follows the
        // re-imported content. Names already claimed THIS run (two source textures named
        // alike) or squatted by a DIFFERENT type get numeric suffixes, like UniqueName did.
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

        static void ImportTextures(const draconic::model::Model& model, content::Group& group,
                                   Array<Guid>& outGuids, Array<String>& claimed,
                                   Array<editor::DeferredImportWrite>* deferredWrites)
        {
            // Color space follows USAGE: data maps (normal/MR/AO) stay linear - sRGB-decoding
            // them corrupts the values (a flat normal 0.5 would linearize to ~0.21).
            Array<bool> linear;
            ClassifyLinearTextures(model, linear);

            const Span<draconic::model::ModelTexture* const> textures = model.textures();
            for (usize i = 0; i < textures.Size(); ++i)
            {
                const draconic::model::ModelTexture& t = *textures[i];
                const u8* data = t.getData();
                const i32 size = t.getDataSize();
                const bool rgba8 = (data != nullptr && t.width > 0 && t.height > 0 &&
                                    size == t.width * t.height * 4);
                if (!rgba8)
                {
                    outGuids.PushBack(Guid{});
                    continue;
                }

                draconic::texture::TextureAsset asset;
                asset.embeddedWidth = static_cast<u32>(t.width);
                asset.embeddedHeight = static_cast<u32>(t.height);
                asset.colorSpace =
                    (i < linear.Size() && linear[i])
                        ? draconic::image::ImageColorSpace::Linear // data maps (normal/MR/AO)
                        : draconic::image::ImageColorSpace::Srgb;  // color maps (albedo/emissive)
                asset.generateMipmaps = false;

                // Real names when the source has them (rules out slot mix-ups at a glance).
                content::Instance* inst =
                    ClaimInstance(group, ImportedTextureName(t, i).AsView(),
                                  draconic::texture::TextureAsset::StaticType(), claimed);
                if (inst == nullptr || !inst->WriteObject(asset).IsOk())
                {
                    outGuids.PushBack(Guid{});
                    continue;
                }
                const Span<const byte> pixels{reinterpret_cast<const byte*>(data),
                                              static_cast<usize>(size)};
                if (deferredWrites != nullptr)
                {
                    // Decoded pixels are the import's bulk (100s of MB for a big model) -
                    // park them for the worker flush; the view borrows from the prepared
                    // model, which the caller keeps alive until the flush completes.
                    editor::DeferredImportWrite write;
                    write.instance = inst;
                    write.streamName = String(u8"pixels");
                    write.view = pixels;
                    deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(write));
                    outGuids.PushBack(inst->Id());
                    continue;
                }
                const Status ds = inst->WriteData(u8"pixels", pixels);
                outGuids.PushBack(ds.IsOk() ? inst->Id() : Guid{});
            }
        }

        // PBR factors -> MaterialAsset instances (builtin "forward" shader by name).
        // Bake-once cache for FBX separate metal/rough pairs (materials often share maps).
        static Guid GetOrBakePackedMR(const draconic::model::Model& model, content::Group& group,
                                      HashMap<u64, Guid>& cache, i32 roughIdx, i32 metalIdx,
                                      Array<String>& claimed,
                                      Array<editor::DeferredImportWrite>* deferredWrites)
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

            draconic::texture::TextureAsset asset;
            asset.embeddedWidth = w;
            asset.embeddedHeight = h;
            asset.colorSpace = draconic::image::ImageColorSpace::Linear; // data map
            asset.generateMipmaps = false;
            const String name = Format(u8"mr.packed.{}.{}", roughIdx, metalIdx);
            content::Instance* inst = ClaimInstance(
                group, name.AsView(), draconic::texture::TextureAsset::StaticType(), claimed);
            if (inst == nullptr || !inst->WriteObject(asset).IsOk())
            {
                cache.InsertOrAssign(key, Guid{});
                return Guid{};
            }
            if (deferredWrites != nullptr)
            {
                // Baked pixels are produced HERE, so the deferred write owns them.
                editor::DeferredImportWrite write;
                write.instance = inst;
                write.streamName = String(u8"pixels");
                for (u8 b : pixels)
                {
                    write.owned.PushBack(static_cast<byte>(b));
                }
                deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(write));
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

        static void ImportMaterials(const draconic::model::Model& model, content::Group& group,
                                    const Array<Guid>& textureGuids, ModelManifestSource& manifest,
                                    Array<String>& claimed,
                                    Array<editor::DeferredImportWrite>* deferredWrites)
        {
            HashMap<u64, Guid> bakedMR; // per-pair bake cache (see GetOrBakePackedMR)
            const Span<draconic::model::ModelMaterial* const> materials = model.materials();
            for (usize i = 0; i < materials.Size(); ++i)
            {
                const draconic::model::ModelMaterial& m = *materials[i];
                RefPtr<draconic::materials::Material> built = draconic::materials::CreatePBR(
                    ImportedAssetName(m.name(), u8"mat", i).AsView(), m.baseColorFactor,
                    m.metallicFactor, m.roughnessFactor);
                built->SetDefaultColor(
                    u8"EmissiveColor",
                    Float4{m.emissiveFactor.x, m.emissiveFactor.y, m.emissiveFactor.z, 1.0f});
                built->SetDefaultFloat(u8"OcclusionStrength", m.occlusionStrength);
                built->SetDefaultFloat(u8"NormalScale", m.normalScale);
                built->SetDefaultFloat(u8"AlphaCutoff", m.alphaCutoff);
                draconic::materials::MaterialAsset asset;
                draconic::materials::MaterialImporter::Import(*built, Guid{}, asset);
                asset.source.shaderName = String(u8"forward");
                MaterialSamplerModes(model, m, asset.source.samplerU, asset.source.samplerV);

                // Wire EVERY authored texture INTO the material source (self-contained cooked
                // material - a directly-picked material renders fully textured, not just via
                // the model-spawn composite). Previously only the albedo made it across.
                const auto wire = [&](i32 texIdx, StringView slot)
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
                // Metallic-roughness: glTF's packed texture wires directly; FBX's separate
                // grayscale maps BAKE into a packed one (G=rough, B=metal) - feeding either
                // into the packed slot directly would bleed across channels.
                if (m.metallicRoughnessTextureIndex >= 0)
                {
                    wire(m.metallicRoughnessTextureIndex, u8"MetallicRoughnessMap");
                }
                else if (m.separateRoughnessTextureIndex >= 0 ||
                         m.separateMetalnessTextureIndex >= 0)
                {
                    const Guid packed =
                        GetOrBakePackedMR(model, group, bakedMR, m.separateRoughnessTextureIndex,
                                          m.separateMetalnessTextureIndex, claimed, deferredWrites);
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

                content::Instance* inst =
                    ClaimInstance(group, ImportedAssetName(m.name(), u8"mat", i).AsView(),
                                  draconic::materials::MaterialAsset::StaticType(), claimed);
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

        static void ImportSkeletonAndClips(const draconic::model::Model& model,
                                           content::Group& group, ModelManifestSource& manifest,
                                           Array<String>& claimed)
        {
            if (model.skins().Size() == 0)
            {
                return;
            }
            const draconic::model::ModelSkin& skin = *model.skins()[0];
            const HashMap<i32, i32> boneToJoint = BuildBoneToJoint(skin);

            draconic::animation::SkeletonAsset skeleton;
            SkeletonSourceFromModel(model, skin, boneToJoint, skeleton.source);
            content::Instance* skelInst = ClaimInstance(
                group,
                skin.name().IsEmpty() ? StringView(u8"skeleton")
                                      : ImportedAssetName(skin.name(), u8"skeleton", 0).AsView(),
                draconic::animation::SkeletonAsset::StaticType(), claimed);
            if (skelInst != nullptr && skelInst->WriteObject(skeleton).IsOk())
            {
                manifest.skeletonGuid = skelInst->Id();
            }

            const Span<draconic::model::ModelAnimation* const> animations = model.animations();
            for (usize a = 0; a < animations.Size(); ++a)
            {
                content::Instance* clipInst = ClaimInstance(
                    group, ImportedAssetName(animations[a]->name(), u8"anim", a).AsView(),
                    draconic::animation::AnimationClipAsset::StaticType(), claimed);
                draconic::animation::AnimationClipAsset clip;
                AnimationClipSourceFromModel(
                    *animations[a], boneToJoint,
                    (clipInst != nullptr) ? clipInst->Name() : StringView(u8"anim"), clip.source);
                if (clipInst != nullptr && clipInst->WriteObject(clip).IsOk())
                {
                    manifest.animationGuids.PushBack(clipInst->Id());
                }
            }
        }

        [[nodiscard]] static Status ImportMeshes(const draconic::model::Model& model,
                                                 content::Group& group,
                                                 ModelManifestSource& manifest,
                                                 Array<String>& claimed,
                                                 Array<editor::DeferredImportWrite>* deferredWrites)
        {
            const bool hasSkin = model.skins().Size() > 0;
            const Span<draconic::model::ModelMesh* const> meshes = model.meshes();
            for (usize i = 0; i < meshes.Size(); ++i)
            {
                const draconic::model::ModelMesh& m = *meshes[i];
                const bool skinned = IsSkinnedMesh(m) && hasSkin;
                const String baseName = ImportedAssetName(m.name(), u8"mesh", i);
                const StringView name = baseName.AsView();

                // Mesh envelopes are the import's largest SERIALIZATION cost (a big mesh
                // source rendered to XML) - defer object + write to the worker flush.
                content::Instance* inst = nullptr;
                Status written;
                if (skinned)
                {
                    auto asset = MakeRef<draconic::geometry::SkinnedMeshAsset>(DefaultAllocator());
                    SkinnedMeshSourceFromModel(m, 0, asset->source);
                    inst = ClaimInstance(
                        group, name, draconic::geometry::SkinnedMeshAsset::StaticType(), claimed);
                    if (inst == nullptr)
                    {
                        return Status{ErrorCode::Unknown};
                    }
                    if (deferredWrites != nullptr)
                    {
                        editor::DeferredImportWrite write;
                        write.instance = inst;
                        write.object = RefPtr<ISerializable>(asset.Get());
                        deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(write));
                    }
                    else
                    {
                        written = inst->WriteObject(*asset);
                    }
                }
                else
                {
                    auto asset = MakeRef<draconic::geometry::StaticMeshAsset>(DefaultAllocator());
                    StaticMeshSourceFromModel(m, asset->source);
                    inst = ClaimInstance(
                        group, name, draconic::geometry::StaticMeshAsset::StaticType(), claimed);
                    if (inst == nullptr)
                    {
                        return Status{ErrorCode::Unknown};
                    }
                    if (deferredWrites != nullptr)
                    {
                        editor::DeferredImportWrite write;
                        write.instance = inst;
                        write.object = RefPtr<ISerializable>(asset.Get());
                        deferredWrites->PushBack(static_cast<editor::DeferredImportWrite&&>(write));
                    }
                    else
                    {
                        written = inst->WriteObject(*asset);
                    }
                }
                if (!written.IsOk())
                {
                    return written;
                }

                manifest.meshGuids.PushBack(inst->Id());
                manifest.meshSkinned.PushBack(skinned ? u8{1} : u8{0});
                const Span<const draconic::model::ModelMeshPart> parts = m.parts();
                manifest.meshMaterial.PushBack(parts.Size() > 0 ? parts[0].materialIndex : -1);
            }
            return Status{};
        }

        // One CollisionShapeAsset per STATIC mesh (skinned meshes don't get collision);
        // the guid lands in manifest.collisionGuids (parallel; nil = none) for the
        // prefab generator to wire colliders from.
        static void ImportCollisionShapes(content::Group& group, ModelManifestSource& manifest,
                                          bool convex, Array<String>& claimed)
        {
            for (usize i = 0; i < manifest.meshGuids.Size(); ++i)
            {
                if (manifest.meshSkinned[i] != 0)
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
                content::Instance* inst =
                    ClaimInstance(group, name.AsView(),
                                  draconic::physics::CollisionShapeAsset::StaticType(), claimed);
                if (inst == nullptr)
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                draconic::physics::CollisionShapeAsset asset;
                asset.sourceMesh = manifest.meshGuids[i];
                asset.cook = convex ? draconic::physics::CollisionCookKind::ConvexHull
                                    : draconic::physics::CollisionCookKind::TriangleMesh;
                if (!inst->WriteObject(asset).IsOk())
                {
                    manifest.collisionGuids.PushBack(Guid{});
                    continue;
                }
                manifest.collisionGuids.PushBack(inst->Id());
            }
        }

        static void ImportNodes(const draconic::model::Model& model, ModelManifestSource& manifest)
        {
            const Span<draconic::model::ModelBone* const> bones = model.bones();
            for (usize i = 0; i < bones.Size(); ++i)
            {
                const draconic::model::ModelBone& b = *bones[i];
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

    // Registers the manifest asset type for content-DB construction + deserialization.
    inline void RegisterModelManifestAsset()
    {
        GlobalTypeRegistry().Register(ModelManifestAsset::StaticType());
        RegisterSerializable<ModelManifestAsset>();
    }

    DRACONIC_DEFINE_OBJECT(ModelManifestAsset, "draconic::modelimporter")
    DRACONIC_DEFINE_OBJECT(ModelImportOptions, "draconic::modelimporter")
    DRACONIC_DEFINE_OBJECT(LoadedModel, "draconic::modelimporter")
}

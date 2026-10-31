// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:library`: a model file opened for viewing without a project. The file
// loads through the model loaders (glTF and GLB; ufbx for FBX and OBJ) into the in-memory model,
// whose stats the viewer shows. The model cooks into its own content database in the viewer's
// cache, and the cooked model binds through a resource manager with the engine's factories. The
// cache folder is named by the file's path, size and modification time, so reopening a model binds
// what was cooked before while a changed file cooks again. A record beside the cooked files keeps
// the model's guid and its stats.

module;
#include "Core/Prelude.h"

export module modelviewer:library;

import foundation.core;
import foundation.rhi;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.model;
import foundation.model.io;
import foundation.model.gltf;
import foundation.model.fbx;
import foundation.model.resource;
import engine.composition;
import modelimporter;

using namespace foundation::core;

export namespace modelviewer
{
    namespace content = foundation::content;
    namespace resource = foundation::resource;

    /// A material as the file defines it: its name, how it shades, and the textures in its slots.
    struct ModelMaterialInfo
    {
        struct Slot
        {
            String slot;  // "base color", "normal", ...
            u32 texture;  // index into ModelStats::textureList
        };
        String name;
        String shading;   // "PBR", "Phong", "Lambert", "Unlit"
        Array<Slot> slots;
    };

    /// A texture as the file defines it: its name, its size in pixels, and where its image is.
    struct ModelTextureInfo
    {
        String name;
        u32 width = 0;
        u32 height = 0;
        String file;      // the image's file name; empty when the image is inside the model file
    };

    /// What the side panel shows about a model, counted from the file as it loaded.
    struct ModelStats
    {
        u32 meshes = 0;
        u32 vertices = 0;
        u32 triangles = 0;
        u32 materials = 0;
        u32 textures = 0;
        u32 bones = 0;
        u32 skins = 0;
        Array<String> clips;    // the animations' names, in the file's order
        Array<f32> clipSeconds; // and their lengths
        Float3 size{};          // the extent of the bounds, in the file's units
        Array<ModelMaterialInfo> materialList; // each material and its textures (after the loose texture rule)
        Array<ModelTextureInfo> textureList;
    };

    /// The file types the loaders read, for a dialog's filter and a folder scan.
    inline constexpr StringView kModelExtensions = u8"gltf;glb;fbx;obj";

    /// Whether `path` names a file the viewer opens (by its extension, any case).
    [[nodiscard]] inline bool IsModelFile(StringView path)
    {
        usize dot = path.Size();
        while (dot > 0 && path[dot - 1] != u8'.' && path[dot - 1] != u8'/')
            --dot;
        if (dot == 0 || path[dot - 1] != u8'.')
            return false;
        String extension;
        for (const utf8char c : path.SubStr(dot, path.Size() - dot))
            extension.Append(c >= u8'A' && c <= u8'Z' ? static_cast<utf8char>(c - u8'A' + u8'a') : c);
        const StringView known[] = {u8"gltf", u8"glb", u8"fbx", u8"obj"};
        for (const StringView type : known)
            if (extension.AsView() == type)
                return true;
        return false;
    }

    /// The version of the cache's cooked files. Raise it when the import changes what it makes, so
    /// a model opened again cooks afresh.
    inline constexpr u32 kCacheVersion = 6; // 6: the record lists the materials and textures

    /// A name for what is made from the file at `path`: 16 hex digits hashed from its path, size,
    /// modification time and `version`, so a changed file or a raised version gets a new name.
    /// Empty when the file does not exist.
    [[nodiscard]] inline String FileKey(StringView path, u32 version)
    {
        u64 size = 0;
        i64 modified = 0;
        if (!FileStat(path, size, modified))
            return {};
        u64 hash = HashBytes(path.Data(), path.Size());
        hash = HashBytes(&size, sizeof(size), hash);
        hash = HashBytes(&modified, sizeof(modified), hash);
        hash = HashBytes(&version, sizeof(version), hash);
        utf8char name[16];
        for (u32 i = 0; i < 16; ++i)
            name[i] = static_cast<utf8char>(u8"0123456789abcdef"[(hash >> (60 - 4 * i)) & 0xFu]);
        return String(StringView(name, 16));
    }

    /// The folder under `cacheRoot` that holds the cooked files for `path`, named by FileKey with
    /// the cache version and by whether loose textures were applied, since they change what cooks.
    /// Empty when the file does not exist.
    [[nodiscard]] inline String CacheFolderFor(StringView cacheRoot, StringView path, bool looseTextures = true)
    {
        const String key = FileKey(path, kCacheVersion * 2u + (looseTextures ? 1u : 0u));
        return key.IsEmpty() ? String{} : PathJoin(cacheRoot, key.AsView());
    }

    namespace library_detail
    {
        [[nodiscard]] inline bool EqualsIgnoreCase(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char x = a[i];
                utf8char y = b[i];
                if (x >= u8'A' && x <= u8'Z')
                    x = static_cast<utf8char>(x - u8'A' + u8'a');
                if (y >= u8'A' && y <= u8'Z')
                    y = static_cast<utf8char>(y - u8'A' + u8'a');
                if (x != y)
                    return false;
            }
            return true;
        }
        [[nodiscard]] inline bool Less(StringView a, StringView b)
        {
            const usize n = Min(a.Size(), b.Size());
            for (usize i = 0; i < n; ++i)
                if (a[i] != b[i])
                    return a[i] < b[i];
            return a.Size() < b.Size();
        }
        [[nodiscard]] inline StringView Stem(StringView name)
        {
            usize dot = name.Size();
            while (dot > 0 && name[dot - 1] != u8'.')
                --dot;
            return dot > 0 ? name.SubStr(0, dot - 1) : name;
        }
        [[nodiscard]] inline bool IsImageFile(StringView name)
        {
            usize dot = name.Size();
            while (dot > 0 && name[dot - 1] != u8'.')
                --dot;
            if (dot == 0)
                return false;
            const StringView extension = name.SubStr(dot, name.Size() - dot);
            return EqualsIgnoreCase(extension, u8"png") || EqualsIgnoreCase(extension, u8"jpg") || EqualsIgnoreCase(extension, u8"jpeg");
        }
    }

    /// The loose texture rule: a material that names no colour texture (the link is often lost on
    /// export, common with FBX files from packs and extractions) takes an image beside the model
    /// file: the one named like the material, else the one named like the model, else the first
    /// PNG or JPEG in name order. Returns how many materials took an image.
    inline u32 ApplyLooseTextures(foundation::model::Model& model, StringView modelPath)
    {
        usize slash = modelPath.Size();
        while (slash > 0 && modelPath[slash - 1] != u8'/' && modelPath[slash - 1] != u8'\\')
            --slash;
        const StringView folder = slash > 0 ? modelPath.SubStr(0, slash - 1) : StringView(u8".");
        const StringView modelStem = library_detail::Stem(modelPath.SubStr(slash, modelPath.Size() - slash));
        Array<String> images;
        (void)ListDirectory(folder,
                            [](void* context, StringView name, bool isDirectory)
                            {
                                if (!isDirectory && library_detail::IsImageFile(name))
                                    static_cast<Array<String>*>(context)->PushBack(String(name));
                            },
                            &images);
        if (images.IsEmpty())
            return 0;
        for (usize i = 1; i < images.Size(); ++i) // sort by name so "the first" is the same each time
            for (usize j = i; j > 0 && library_detail::Less(images[j].AsView(), images[j - 1].AsView()); --j)
            {
                String moved = foundation::core::Move(images[j]);
                images[j] = foundation::core::Move(images[j - 1]);
                images[j - 1] = foundation::core::Move(moved);
            }
        auto find = [&](StringView stem) -> const String*
        {
            for (const String& image : images)
                if (library_detail::EqualsIgnoreCase(library_detail::Stem(image.AsView()), stem))
                    return &image;
            return nullptr;
        };
        Array<String> added;  // the image file each added texture was made for
        Array<i32> indices;   // and that texture's index in the model
        u32 applied = 0;
        for (foundation::model::ModelMaterial* material : model.materials())
        {
            if (material == nullptr || material->baseColorTextureIndex >= 0)
                continue;
            const String* chosen = find(material->name());
            if (chosen == nullptr)
                chosen = find(modelStem);
            if (chosen == nullptr)
                chosen = &images[0];
            i32 index = -1;
            for (usize i = 0; i < added.Size(); ++i)
                if (added[i] == *chosen)
                    index = indices[i];
            if (index < 0)
            {
                auto* texture = new foundation::model::ModelTexture();
                texture->setName(library_detail::Stem(chosen->AsView()));
                texture->setUri(chosen->AsView());
                texture->setSourceFile(PathJoin(folder, chosen->AsView()).AsView());
                index = model.addTexture(texture);
                added.PushBack(*chosen);
                indices.PushBack(index);
            }
            material->baseColorTextureIndex = index;
            material->baseColorFactor = Float4(1.0f, 1.0f, 1.0f, 1.0f);
            ++applied;
        }
        return applied;
    }

    /// A file's name: what follows its last slash or backslash.
    [[nodiscard]] inline String FileTitle(StringView path)
    {
        usize start = path.Size();
        while (start > 0 && path[start - 1] != u8'/' && path[start - 1] != u8'\\')
            --start;
        return String(path.SubStr(start, path.Size() - start));
    }

    /// Counts the stats of a loaded model.
    [[nodiscard]] inline ModelStats CountModel(foundation::model::Model& model)
    {
        ModelStats stats;
        for (const foundation::model::ModelMesh* mesh : model.meshes())
        {
            ++stats.meshes;
            stats.vertices += static_cast<u32>(Max(mesh->vertexCount(), 0));
            // A mesh with no index buffer draws its vertices three at a time (glTF allows it).
            const i32 corners = mesh->indexCount() > 0 ? mesh->indexCount() : mesh->vertexCount();
            stats.triangles += static_cast<u32>(Max(corners, 0)) / 3u;
        }
        stats.materials = static_cast<u32>(model.materials().Size());
        stats.textures = static_cast<u32>(model.textures().Size());
        for (const foundation::model::ModelTexture* texture : model.textures())
        {
            ModelTextureInfo info;
            if (texture != nullptr)
            {
                info.name = String(texture->name());
                info.width = static_cast<u32>(Max(texture->width, 0));
                info.height = static_cast<u32>(Max(texture->height, 0));
                // The texture's own file (FBX, a DDS, the loose texture rule), else a glTF uri beside
                // the model; none when the image is inside the model file.
                if (!texture->sourceFile().IsEmpty())
                    info.file = FileTitle(texture->sourceFile());
                else if (!texture->uri().IsEmpty() && !texture->uri().StartsWith(u8"data:"))
                    info.file = FileTitle(texture->uri());
            }
            stats.textureList.PushBack(Move(info));
        }
        for (const foundation::model::ModelMaterial* material : model.materials())
        {
            ModelMaterialInfo info;
            if (material != nullptr)
            {
                using Shading = foundation::model::ShadingModel;
                info.name = String(material->name());
                info.shading = String(material->shadingModel == Shading::Phong     ? StringView(u8"Phong")
                                      : material->shadingModel == Shading::Lambert ? StringView(u8"Lambert")
                                      : material->shadingModel == Shading::Unlit   ? StringView(u8"Unlit")
                                                                                   : StringView(u8"PBR"));
                const struct
                {
                    StringView slot;
                    i32 index;
                } slots[] = {{u8"base color", material->baseColorTextureIndex},
                             {u8"normal", material->normalTextureIndex},
                             {u8"metal rough", material->metallicRoughnessTextureIndex},
                             {u8"roughness", material->separateRoughnessTextureIndex},
                             {u8"metalness", material->separateMetalnessTextureIndex},
                             {u8"occlusion", material->occlusionTextureIndex},
                             {u8"emissive", material->emissiveTextureIndex},
                             {u8"specular", material->specularTextureIndex}};
                for (const auto& slot : slots)
                    if (slot.index >= 0 && static_cast<usize>(slot.index) < stats.textureList.Size())
                        info.slots.PushBack(ModelMaterialInfo::Slot{String(slot.slot), static_cast<u32>(slot.index)});
            }
            stats.materialList.PushBack(Move(info));
        }
        // Count the joints of the skin the viewer animates (the cook takes the first skin), not
        // the model's nodes, which include meshes and groups.
        stats.bones = model.skins().Size() > 0 ? static_cast<u32>(model.skins()[0]->joints().Size()) : 0u;
        stats.skins = static_cast<u32>(model.skins().Size());
        for (const foundation::model::ModelAnimation* clip : model.animations())
        {
            stats.clips.PushBack(String(clip->name()));
            stats.clipSeconds.PushBack(clip->duration);
        }
        model.calculateBounds();
        const auto bounds = model.bounds();
        stats.size = bounds.max - bounds.min;
        return stats;
    }

    /// The record kept beside a model's cooked files: its guid, then its stats, one per line.
    [[nodiscard]] inline String WriteRecord(const Guid& model, const ModelStats& stats)
    {
        String text = Format(u8"model {}\nmeshes {}\nvertices {}\ntriangles {}\nmaterials {}\ntextures {}\nbones {}\nskins {}\nsize {} {} {}\n",
                             model, stats.meshes, stats.vertices, stats.triangles, stats.materials, stats.textures,
                             stats.bones, stats.skins, stats.size.x, stats.size.y, stats.size.z);
        for (usize i = 0; i < stats.clips.Size(); ++i)
            text += Format(u8"clip {} {}\n", stats.clipSeconds[i], stats.clips[i]).AsView();
        // A texture line holds its size, then its name and file separated by a tab (a name may
        // hold spaces). A material line holds its shading and name, followed by a line per slot.
        for (const ModelTextureInfo& t : stats.textureList)
            text += Format(u8"texture {} {} {}\t{}\n", t.width, t.height, t.name.AsView(), t.file.AsView()).AsView();
        for (const ModelMaterialInfo& m : stats.materialList)
        {
            text += Format(u8"material {} {}\n", m.shading.AsView(), m.name.AsView()).AsView();
            for (const ModelMaterialInfo::Slot& slot : m.slots)
                text += Format(u8"slot {} {}\n", slot.texture, slot.slot.AsView()).AsView();
        }
        return text;
    }

    /// Parses a record; false when the text is not a valid record, in which case the model cooks again.
    [[nodiscard]] inline bool ReadRecord(StringView text, Guid& model, ModelStats& stats)
    {
        bool haveModel = false;
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != u8'\n')
                ++end;
            const StringView line = text.SubStr(start, end - start);
            start = end + 1;
            usize space = 0;
            while (space < line.Size() && line[space] != u8' ')
                ++space;
            if (space >= line.Size())
                continue;
            const StringView key = line.SubStr(0, space);
            const StringView value = line.SubStr(space + 1, line.Size() - space - 1);
            const auto number = [&value]() { return static_cast<u32>(ParseInt(value).ValueOr(0)); };
            if (key == StringView(u8"model"))
            {
                haveModel = Guid::TryParse(value, model);
            }
            else if (key == StringView(u8"meshes"))
                stats.meshes = number();
            else if (key == StringView(u8"vertices"))
                stats.vertices = number();
            else if (key == StringView(u8"triangles"))
                stats.triangles = number();
            else if (key == StringView(u8"materials"))
                stats.materials = number();
            else if (key == StringView(u8"textures"))
                stats.textures = number();
            else if (key == StringView(u8"bones"))
                stats.bones = number();
            else if (key == StringView(u8"skins"))
                stats.skins = number();
            else if (key == StringView(u8"size"))
            {
                f32 v[3] = {};
                usize at = 0;
                for (u32 i = 0; i < 3 && at <= value.Size(); ++i)
                {
                    usize next = at;
                    while (next < value.Size() && value[next] != u8' ')
                        ++next;
                    v[i] = static_cast<f32>(ParseFloat(value.SubStr(at, next - at)).ValueOr(0.0));
                    at = next + 1;
                }
                stats.size = Float3{v[0], v[1], v[2]};
            }
            else if (key == StringView(u8"texture"))
            {
                ModelTextureInfo t;
                usize at = 0;
                u32 parts[2] = {};
                for (u32 i = 0; i < 2; ++i)
                {
                    usize next = at;
                    while (next < value.Size() && value[next] != u8' ')
                        ++next;
                    parts[i] = static_cast<u32>(ParseInt(value.SubStr(at, next - at)).ValueOr(0));
                    at = Min(next + 1, value.Size());
                }
                t.width = parts[0];
                t.height = parts[1];
                const StringView rest = value.SubStr(at, value.Size() - at);
                usize tab = 0;
                while (tab < rest.Size() && rest[tab] != u8'\t')
                    ++tab;
                t.name = String(rest.SubStr(0, tab));
                if (tab < rest.Size())
                    t.file = String(rest.SubStr(tab + 1, rest.Size() - tab - 1));
                stats.textureList.PushBack(Move(t));
            }
            else if (key == StringView(u8"material"))
            {
                usize gap = 0;
                while (gap < value.Size() && value[gap] != u8' ')
                    ++gap;
                ModelMaterialInfo m;
                m.shading = String(value.SubStr(0, gap));
                m.name = String(gap < value.Size() ? value.SubStr(gap + 1, value.Size() - gap - 1) : StringView{});
                stats.materialList.PushBack(Move(m));
            }
            else if (key == StringView(u8"slot") && !stats.materialList.IsEmpty())
            {
                usize gap = 0;
                while (gap < value.Size() && value[gap] != u8' ')
                    ++gap;
                const u32 texture = static_cast<u32>(ParseInt(value.SubStr(0, gap)).ValueOr(0));
                stats.materialList[stats.materialList.Size() - 1].slots.PushBack(
                    ModelMaterialInfo::Slot{String(gap < value.Size() ? value.SubStr(gap + 1, value.Size() - gap - 1) : StringView{}), texture});
            }
            else if (key == StringView(u8"clip"))
            {
                usize gap = 0;
                while (gap < value.Size() && value[gap] != u8' ')
                    ++gap;
                stats.clipSeconds.PushBack(static_cast<f32>(ParseFloat(value.SubStr(0, gap)).ValueOr(0.0)));
                stats.clips.PushBack(String(gap < value.Size() ? value.SubStr(gap + 1, value.Size() - gap - 1) : StringView{}));
            }
        }
        return haveModel;
    }

    /// A model file open for viewing: its stats and the bound cooked model, which lives as long as this.
    class OpenModel
    {
    public:
        enum class Outcome : u8
        {
            Opened,   // cooked by this open
            FromCache, // bound what an earlier open cooked
            NotFound,
            LoadFailed, // the loaders could not read it
            CookFailed,
            BindFailed,
        };

        OpenModel(IAllocator& allocator) : m_allocator(&allocator) {}
        /// Whether a material without its texture takes an image beside the model (ApplyLooseTextures).
        /// On by default. It changes what cooks, so each setting has its own cache folder.
        void SetLooseTextures(bool on) noexcept { m_looseTextures = on; }
        [[nodiscard]] bool LooseTextures() const noexcept { return m_looseTextures; }
        OpenModel(const OpenModel&) = delete;
        OpenModel& operator=(const OpenModel&) = delete;
        ~OpenModel() { Close(); }

        /// Opens `path`, with its cooked files under `cacheRoot`. The texture factory uploads with
        /// `device`; when it is null there are no textures. This runs Begin, LoadFile when the file
        /// must be read, then Finish, all on this thread; a window runs the same steps with the
        /// read on a worker.
        Outcome Open(StringView path, StringView cacheRoot, foundation::rhi::Device* device)
        {
            Outcome outcome = Outcome::NotFound;
            if (Begin(path, cacheRoot, device, outcome) != Begun::NeedsFile)
                return outcome;
            foundation::model::Model model;
            ModelStats stats;
            outcome = LoadFile(path, m_looseTextures, model, stats);
            if (outcome != Outcome::Opened)
                return outcome;
            return Finish(model, stats);
        }

        /// Reads the file into `model` and counts its stats, applying loose textures when asked.
        /// This is the slow part of an open and is safe on any thread, since it creates its own
        /// loaders rather than using the registry's. Returns Opened on success.
        static Outcome LoadFile(StringView path, bool looseTextures, foundation::model::Model& model, ModelStats& stats)
        {
            if (!FileExists(path))
                return Outcome::NotFound;
            foundation::model::gltf::GltfLoader gltf;
            foundation::model::fbx::FbxLoader fbx;
            StringView extension; // with its dot, as the loaders expect it
            for (usize i = path.Size(); i > 0 && path[i - 1] != u8'/' && path[i - 1] != u8'\\'; --i)
                if (path[i - 1] == u8'.')
                {
                    extension = path.SubStr(i - 1, path.Size() - i + 1);
                    break;
                }
            foundation::model::io::ModelLoader* loader = nullptr;
            if (gltf.supportsExtension(extension))
                loader = &gltf;
            else if (fbx.supportsExtension(extension))
                loader = &fbx;
            if (loader == nullptr || loader->load(path, model) != foundation::model::ModelLoadResult::Ok)
                return Outcome::LoadFailed;
            if (looseTextures)
                (void)ApplyLooseTextures(model, path);
            stats = CountModel(model);
            return Outcome::Opened;
        }

        enum class Begun : u8
        {
            Bound,     // cooked before: bound from the cache and open
            NeedsFile, // the file must be read (LoadFile, on any thread), then Finish called
            Failed,
        };
        /// The first step, run on the thread that owns the resources. It creates the cache folder,
        /// its database and resources, and binds the model when an earlier open cooked it.
        /// `outcome` reports the result when the return is not NeedsFile.
        Begun Begin(StringView path, StringView cacheRoot, foundation::rhi::Device* device, Outcome& outcome)
        {
            Close();
            m_path = String(path);
            const String folder = CacheFolderFor(cacheRoot, path, m_looseTextures);
            outcome = Outcome::NotFound;
            if (folder.IsEmpty())
                return Begun::Failed;
            outcome = Outcome::CookFailed;
            if (!CreateDirectories(folder.AsView()))
                return Begun::Failed;
            m_folder = folder;
            m_fileSystem = MakeUnique<foundation::vfs::NativeFileSystem>(*m_allocator, folder.AsView(), *m_allocator);
            m_database = MakeUnique<content::ContentDatabase>(*m_allocator, *m_allocator, *m_fileSystem, BinarySerializerFactory(),
                                                              u8".rasset");
            m_resources = MakeUnique<resource::ResourceManager>(*m_allocator, *m_allocator, *m_database);
            resource::ResourceServiceTable services;
            if (device != nullptr)
                services.Add<foundation::rhi::Device>(device);
            engine::FullComposition().CreateFactories(m_factories, *m_allocator, services);
            m_factories.Register(*m_resources);
            engine::RegisterAllResourceTypes();

            String text;
            if (Result<Array<byte>> bytes = ReadFile(RecordPath().AsView()); bytes.HasValue())
                text = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            Guid guid;
            if (text.IsEmpty() || !ReadRecord(text.AsView(), guid, m_stats))
            {
                m_stats = {};
                return Begun::NeedsFile;
            }
            outcome = BindCooked(guid, Outcome::FromCache);
            return outcome == Outcome::FromCache ? Begun::Bound : Begun::Failed;
        }

        /// The last step, after Begin returned NeedsFile: cooks `model` (as LoadFile read it) into
        /// the cache, writes its record, and binds it.
        Outcome Finish(foundation::model::Model& model, const ModelStats& stats)
        {
            if (!m_database)
                return Outcome::CookFailed;
            m_stats = stats;
            Guid guid;
            if (!pipeline::CookModel(model, *m_database, u8"model", guid).IsOk())
                return Outcome::CookFailed;
            const String record = WriteRecord(guid, m_stats);
            (void)WriteFile(RecordPath().AsView(), Span<const byte>(reinterpret_cast<const byte*>(record.Data()), record.Size()));
            return BindCooked(guid, Outcome::Opened);
        }

        /// Releases the model and everything it bound.
        void Close()
        {
            m_model = {};
            m_resources.Reset();
            m_factories.Clear();
            m_database.Reset();
            m_fileSystem.Reset();
        }

        [[nodiscard]] bool IsOpen() const noexcept { return static_cast<bool>(m_model); }
        [[nodiscard]] foundation::model::ModelResource* Model() const noexcept { return m_model.Get(); }
        [[nodiscard]] const ModelStats& Stats() const noexcept { return m_stats; }
        [[nodiscard]] StringView Path() const noexcept { return m_path.AsView(); }
        [[nodiscard]] StringView CacheFolder() const noexcept { return m_folder.AsView(); }
        /// The resource manager; its loads finish as it is pumped.
        [[nodiscard]] resource::ResourceManager* Resources() const noexcept { return m_resources.Get(); }

        // Exported data: the viewer executable reads it through ModelBrowse in the shared build.
        static constexpr ENGINE_EXPORT_DATA const char8_t* kRecordName = u8"viewer-record.txt";

    private:
        [[nodiscard]] String RecordPath() const { return PathJoin(m_folder.AsView(), StringView(kRecordName)); }
        Outcome BindCooked(const Guid& guid, Outcome done)
        {
            m_model = m_resources->Bind<foundation::model::ModelResource>(guid);
            return m_model ? done : Outcome::BindFailed;
        }

        IAllocator* m_allocator;
        String m_path;
        bool m_looseTextures = true;
        String m_folder;
        ModelStats m_stats;
        UniquePtr<foundation::vfs::NativeFileSystem> m_fileSystem;
        UniquePtr<content::ContentDatabase> m_database;
        UniquePtr<resource::ResourceManager> m_resources;
        resource::ResourceFactorySet m_factories;
        resource::Proxy<foundation::model::ModelResource> m_model;
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:library`: a model file opened for viewing, without a project
// (Documentation/Specs/model-viewer.md). The file loads through the model loaders (glTF and GLB;
// ufbx for FBX and OBJ) into the in-memory model, whose numbers the viewer shows; the model cooks
// into a content database of its own in the viewer's cache, and the cooked model binds through a
// resource manager with the engine's factories. The cache folder is named by the file's path, size
// and modification time, so a model opened again binds what it cooked before and a changed file
// cooks afresh. A record beside the cooked files keeps the model's guid and its numbers.

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

    /// What the side panel says about a model: counted from the file as it loaded.
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
        Float3 size{};          // the bounds' extent, in the file's units
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

    /// What the cache's cooked files were made by: raised when the import changes what it makes, so
    /// a model opened again cooks afresh.
    inline constexpr u32 kCacheVersion = 4;

    /// The folder `path`'s cooked files live in under `cacheRoot`: named by the path, its size, its
    /// modification time and the cache version. Empty when the file is not there.
    [[nodiscard]] inline String CacheFolderFor(StringView cacheRoot, StringView path)
    {
        u64 size = 0;
        i64 modified = 0;
        if (!FileStat(path, size, modified))
            return {};
        u64 hash = HashBytes(path.Data(), path.Size());
        hash = HashBytes(&size, sizeof(size), hash);
        hash = HashBytes(&modified, sizeof(modified), hash);
        hash = HashBytes(&kCacheVersion, sizeof(kCacheVersion), hash);
        utf8char name[16];
        for (u32 i = 0; i < 16; ++i)
            name[i] = static_cast<utf8char>(u8"0123456789abcdef"[(hash >> (60 - 4 * i)) & 0xFu]);
        return PathJoin(cacheRoot, StringView(name, 16));
    }

    /// The numbers of a loaded model.
    [[nodiscard]] inline ModelStats CountModel(foundation::model::Model& model)
    {
        ModelStats stats;
        for (const foundation::model::ModelMesh* mesh : model.meshes())
        {
            ++stats.meshes;
            stats.vertices += static_cast<u32>(Max(mesh->vertexCount(), 0));
            stats.triangles += static_cast<u32>(Max(mesh->indexCount(), 0)) / 3u;
        }
        stats.materials = static_cast<u32>(model.materials().Size());
        stats.textures = static_cast<u32>(model.textures().Size());
        stats.bones = static_cast<u32>(model.bones().Size());
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

    /// The record kept beside a model's cooked files: its guid, then its numbers, a line each.
    [[nodiscard]] inline String WriteRecord(const Guid& model, const ModelStats& stats)
    {
        String text = Format(u8"model {}\nmeshes {}\nvertices {}\ntriangles {}\nmaterials {}\ntextures {}\nbones {}\nskins {}\nsize {} {} {}\n",
                             model, stats.meshes, stats.vertices, stats.triangles, stats.materials, stats.textures,
                             stats.bones, stats.skins, stats.size.x, stats.size.y, stats.size.z);
        for (usize i = 0; i < stats.clips.Size(); ++i)
            text += Format(u8"clip {} {}\n", stats.clipSeconds[i], stats.clips[i]).AsView();
        return text;
    }

    /// A record read back; false when it is not one (the model cooks again).
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

    /// A model file open for viewing: its numbers, and the cooked model bound (alive while this is).
    class OpenModel
    {
    public:
        enum class Outcome : u8
        {
            Opened,   // cooked now
            FromCache, // bound what an earlier open cooked
            NotFound,
            LoadFailed, // the loaders could not read it
            CookFailed,
            BindFailed,
        };

        OpenModel(IAllocator& allocator) : m_allocator(&allocator) {}
        OpenModel(const OpenModel&) = delete;
        OpenModel& operator=(const OpenModel&) = delete;
        ~OpenModel() { Close(); }

        /// Open `path`, its cooked files under `cacheRoot`. `device` (may be null: no textures)
        /// is what the texture factory uploads with.
        Outcome Open(StringView path, StringView cacheRoot, foundation::rhi::Device* device)
        {
            Close();
            m_path = String(path);
            const String folder = CacheFolderFor(cacheRoot, path);
            if (folder.IsEmpty())
                return Outcome::NotFound;
            if (!CreateDirectories(folder.AsView()))
                return Outcome::CookFailed;
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

            const String recordPath = PathJoin(folder.AsView(), StringView(kRecordName));
            Guid guid;
            Outcome result = Outcome::FromCache;
            String text;
            if (Result<Array<byte>> bytes = ReadFile(recordPath.AsView()); bytes.HasValue())
                text = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            if (text.IsEmpty() || !ReadRecord(text.AsView(), guid, m_stats))
            {
                m_stats = {};
                foundation::model::gltf::GltfLoader gltf;
                foundation::model::fbx::FbxLoader fbx;
                foundation::model::io::registerLoader(&gltf);
                foundation::model::io::registerLoader(&fbx);
                foundation::model::Model model;
                const foundation::model::ModelLoadResult loaded = foundation::model::io::loadModel(path, model);
                foundation::model::io::unregisterLoader(&fbx);
                foundation::model::io::unregisterLoader(&gltf);
                if (loaded != foundation::model::ModelLoadResult::Ok)
                    return Outcome::LoadFailed;
                m_stats = CountModel(model);
                if (!pipeline::CookModel(model, *m_database, u8"model", guid).IsOk())
                    return Outcome::CookFailed;
                const String record = WriteRecord(guid, m_stats);
                (void)WriteFile(recordPath.AsView(), Span<const byte>(reinterpret_cast<const byte*>(record.Data()), record.Size()));
                result = Outcome::Opened;
            }
            m_model = m_resources->Bind<foundation::model::ModelResource>(guid);
            if (!m_model)
                return Outcome::BindFailed;
            return result;
        }

        /// The model and everything it bound let go.
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
        /// Its resources (the loads finish as they are pumped).
        [[nodiscard]] resource::ResourceManager* Resources() const noexcept { return m_resources.Get(); }

        static constexpr const char8_t* kRecordName = u8"viewer-record.txt";

    private:
        IAllocator* m_allocator;
        String m_path;
        String m_folder;
        ModelStats m_stats;
        UniquePtr<foundation::vfs::NativeFileSystem> m_fileSystem;
        UniquePtr<content::ContentDatabase> m_database;
        UniquePtr<resource::ResourceManager> m_resources;
        resource::ResourceFactorySet m_factories;
        resource::Proxy<foundation::model::ModelResource> m_model;
    };
}

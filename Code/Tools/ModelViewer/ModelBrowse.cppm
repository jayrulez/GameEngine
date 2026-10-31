// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:browse`: the browser's logic, kept apart from the window. It lists a
// folder's subfolders and model files and collects the models under a folder for the grid. Each
// model's thumbnail file in the thumbnail cache is named like its cooked folder (by path, size and
// modification time), so a changed file gets a new thumbnail. The stats of a model cooked before
// are read from its cache record, so the filters (name, rigged, animated) work without cooking it
// again.

module;
#include "Core/Prelude.h"

export module modelviewer:browse;

import foundation.core;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    /// Raise this when the way thumbnails are drawn changes, so every thumbnail is drawn again.
    inline constexpr u32 kThumbnailVersion = 1;

    /// A folder's subfolders and model files, each as a full path, sorted by name ignoring case.
    /// Hidden entries (a leading dot) are left out.
    struct FolderListing
    {
        Array<String> folders;
        Array<String> models;
    };

    namespace browse_detail
    {
        [[nodiscard]] inline bool LessIgnoreCase(StringView a, StringView b)
        {
            const usize n = Min(a.Size(), b.Size());
            for (usize i = 0; i < n; ++i)
            {
                utf8char x = a[i];
                utf8char y = b[i];
                if (x >= u8'A' && x <= u8'Z')
                    x = static_cast<utf8char>(x - u8'A' + u8'a');
                if (y >= u8'A' && y <= u8'Z')
                    y = static_cast<utf8char>(y - u8'A' + u8'a');
                if (x != y)
                    return x < y;
            }
            return a.Size() < b.Size();
        }

        inline void SortIgnoreCase(Array<String>& paths)
        {
            for (usize i = 1; i < paths.Size(); ++i)
            {
                String moving = foundation::core::Move(paths[i]);
                usize j = i;
                for (; j > 0 && LessIgnoreCase(moving.AsView(), paths[j - 1].AsView()); --j)
                    paths[j] = foundation::core::Move(paths[j - 1]);
                paths[j] = foundation::core::Move(moving);
            }
        }
    }

    /// Lists the immediate subfolders and model files of `folder`; empty when it cannot be opened.
    [[nodiscard]] inline FolderListing ListFolder(StringView folder)
    {
        struct Collect
        {
            StringView folder;
            FolderListing* out;
        };
        FolderListing listing;
        Collect collect{folder, &listing};
        (void)ListDirectory(folder,
                            [](void* context, StringView name, bool isDirectory)
                            {
                                auto* c = static_cast<Collect*>(context);
                                if (name.IsEmpty() || name[0] == u8'.')
                                    return;
                                if (isDirectory)
                                    c->out->folders.PushBack(PathJoin(c->folder, name));
                                else if (IsModelFile(name))
                                    c->out->models.PushBack(PathJoin(c->folder, name));
                            },
                            &collect);
        browse_detail::SortIgnoreCase(listing.folders);
        browse_detail::SortIgnoreCase(listing.models);
        return listing;
    }

    /// Every model file under `folder` and its subfolders, folder by folder in name order. At most
    /// `limit` are collected, so pointing it at a whole disk by mistake stays bounded.
    inline void CollectModels(StringView folder, Array<String>& out, usize limit = 10000)
    {
        if (out.Size() >= limit)
            return;
        const FolderListing listing = ListFolder(folder);
        for (const String& model : listing.models)
        {
            if (out.Size() >= limit)
                return;
            out.PushBack(model);
        }
        for (const String& sub : listing.folders)
            CollectModels(sub.AsView(), out, limit);
    }

    /// The thumbnail file for `path` under `thumbRoot`, named by the model file's path, size and
    /// time (FileKey) so a changed file is drawn again. Empty when the model file does not exist.
    [[nodiscard]] inline String ThumbnailPathFor(StringView thumbRoot, StringView path)
    {
        const String key = FileKey(path, kThumbnailVersion);
        return key.IsEmpty() ? String{} : PathJoin(thumbRoot, Format(u8"{}.png", key).AsView());
    }

    /// Reads the stats of a cooked model from its record in the cache, for the given loose texture
    /// setting. Returns false when the model has not been cooked, or was cooked by an older viewer.
    [[nodiscard]] inline bool ReadCachedStats(StringView cacheRoot, StringView path, ModelStats& out, bool looseTextures = true)
    {
        const String folder = CacheFolderFor(cacheRoot, path, looseTextures);
        if (folder.IsEmpty())
            return false;
        Result<Array<byte>> bytes = ReadFile(PathJoin(folder.AsView(), StringView(OpenModel::kRecordName)).AsView());
        if (!bytes.HasValue())
            return false;
        Guid model;
        return ReadRecord(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()), model, out);
    }

    /// The grid's filter: names containing `name` (ignoring case), and, for each of the rigged,
    /// animated and in-library boxes that is ticked, only models known to have that property.
    struct BrowseFilter
    {
        String name;
        bool riggedOnly = false;
        bool animatedOnly = false;
        bool inLibraryOnly = false;
    };

    /// Whether the model at `path` passes `filter`. `stats` holds its stats when known (the model
    /// was cooked before); a model not cooked yet fails the rigged and animated boxes until it is.
    /// `inLibrary` says whether the library holds the model.
    [[nodiscard]] inline bool Matches(const BrowseFilter& filter, StringView path, const ModelStats* stats, bool inLibrary = false)
    {
        if (filter.inLibraryOnly && !inLibrary)
            return false;
        if (!filter.name.IsEmpty())
        {
            const String title = FileTitle(path);
            if (!title.AsView().ContainsIgnoreCase(filter.name.AsView()))
                return false;
        }
        if (filter.riggedOnly && (stats == nullptr || stats->skins == 0))
            return false;
        if (filter.animatedOnly && (stats == nullptr || stats->clips.IsEmpty()))
            return false;
        return true;
    }
}

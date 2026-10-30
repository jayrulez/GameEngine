// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:browse`: the browsing half that is not the window
// (Documentation/Specs/model-viewer.md, P3). A folder lists its subfolders and model files; the
// models under a folder are collected for the grid; each model's thumbnail has a file in the
// viewer's thumbnail cache named like its cooked folder (by its path, size and modification time),
// so a changed file gets a new one; a model's numbers are read from its cache record when it was
// cooked before, for the filters (name, rigged, animated) without cooking it again.

module;
#include "Core/Prelude.h"

export module modelviewer:browse;

import foundation.core;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    /// Raised when the thumbnails change how they are drawn, so every one is drawn again.
    inline constexpr u32 kThumbnailVersion = 1;

    /// A folder's contents: its subfolders and its model files, each by full path, sorted by name
    /// (any case). Hidden entries (a leading dot) are left out.
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

    /// Lists `folder`'s immediate subfolders and model files (an empty listing when it does not open).
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

    /// Every model file under `folder`, its subfolders' too, folder by folder in name order; at
    /// most `limit` (a whole disk named by mistake stays bounded).
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

    /// The thumbnail file for `path` under `thumbRoot`: named by the file's path, size and time
    /// (FileKey), so a changed file is drawn again. Empty when the file is not there.
    [[nodiscard]] inline String ThumbnailPathFor(StringView thumbRoot, StringView path)
    {
        const String key = FileKey(path, kThumbnailVersion);
        return key.IsEmpty() ? String{} : PathJoin(thumbRoot, Format(u8"{}.png", key).AsView());
    }

    /// The numbers a model was cooked with, read from its record in the cache (false when it has
    /// not been cooked, or was cooked by an older viewer), with loose textures applied or not.
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

    /// What the grid shows: names containing `name` (any case), and with the rigged, animated or
    /// in-library box ticked only models known to be so.
    struct BrowseFilter
    {
        String name;
        bool riggedOnly = false;
        bool animatedOnly = false;
        bool inLibraryOnly = false;
    };

    /// Whether the model at `path` passes `filter`. `stats` is its numbers when known (cooked
    /// before); a model not cooked yet passes the rigged and animated boxes only once it is.
    /// `inLibrary`: the library has it.
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

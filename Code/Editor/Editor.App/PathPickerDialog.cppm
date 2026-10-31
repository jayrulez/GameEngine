// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :path_picker_dialog partition
//
// The path picker: the picker over the files under a folder whose extension is one of those
// requested (ignoring case; with none requested, every file), so a field that names a file can only
// name one that exists (a font's source, a profiler capture). Each file is a row with its name,
// under its folder (root-relative), sorted by path; its key is its root-relative path. With no
// files it says so, naming the extensions and the folder.
module;
#include "Core/Prelude.h"

export module editor.app:path_picker_dialog;

import foundation.core;
import foundation.vfs;
import :picker;

using namespace foundation::core;

export namespace editor::app
{
    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct PathPicker
    {
        /// The picker's request over the files under `root` with one of `extensions` (".ttf");
        /// `current` is the root-relative path the field names now.
        [[nodiscard]] static PickerRequest Request(IAllocator& allocator, StringView root, Span<const String> extensions, StringView current = {})
        {
            Array<String> files = Files(allocator, root, extensions);
            files.Sort([](const String& a, const String& b) { return a.AsView().Compare(b.AsView()) < 0; });
            PickerRequest request;
            request.Placeholder = String(u8"Search files");
            request.Current = String(current);
            for (const String& file : files)
            {
                const StringView path = file.AsView();
                usize slash = path.Size();
                for (usize i = 0; i < path.Size(); ++i)
                    if (path[i] == u8'/')
                        slash = i;
                const bool nested = slash < path.Size();
                PickerItem item;
                item.Key = file;
                item.Name = String(nested ? path.SubStr(slash + 1, path.Size() - slash - 1) : path);
                item.Section = String(nested ? path.SubStr(0, slash) : StringView{});
                item.Glyph = String(u8"Editor/Icons/file.svg");
                request.Items.PushBack(Move(item));
            }
            String kinds;
            for (const String& extension : extensions)
            {
                if (!kinds.IsEmpty())
                    kinds += u8" or ";
                kinds += extension.AsView();
            }
            request.EmptyTitle = kinds.IsEmpty() ? String(u8"No files here") : Format(u8"No {} files here", kinds);
            request.EmptyText = Format(u8"Nothing under {}.", root);
            return request;
        }

        /// Every file under `root` with one of `extensions`, root-relative, in the walk's order.
        [[nodiscard]] static Array<String> Files(IAllocator& allocator, StringView root, Span<const String> extensions)
        {
            Array<String> files;
            foundation::vfs::NativeFileSystem fs(root, allocator);
            auto* enumerable = fs.AsEnumerable();
            if (enumerable == nullptr)
                return files;
            Array<String> pending;
            pending.PushBack(String{});
            while (!pending.IsEmpty())
            {
                const String folder = Move(pending.Back());
                pending.RemoveAt(pending.Size() - 1);
                Array<foundation::vfs::DirEntry> entries;
                if (!enumerable->Enumerate(folder.AsView(), entries).IsOk())
                    continue;
                for (const foundation::vfs::DirEntry& entry : entries)
                {
                    String path = folder.IsEmpty() ? entry.name : PathJoin(folder.AsView(), entry.name.AsView());
                    if (entry.isDirectory)
                        pending.PushBack(Move(path));
                    else if (Takes(path.AsView(), extensions))
                        files.PushBack(Move(path));
                }
            }
            return files;
        }

        /// Whether `name` ends with one of `extensions`, ignoring case; with none, any name matches.
        [[nodiscard]] static bool Takes(StringView name, Span<const String> extensions)
        {
            if (extensions.IsEmpty())
                return true;
            for (const String& extension : extensions)
                if (name.Size() >= extension.Size() && name.SubStr(name.Size() - extension.Size(), extension.Size()).ContainsIgnoreCase(extension.AsView()))
                    return true;
            return false;
        }
    };
}

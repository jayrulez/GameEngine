// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :filesystem partition
//
// Whole-file convenience helpers over FileStream. Directory queries live in
// :system (DirectoryExists/CreateDirectory/RemoveDirectory) and are re-exported
// here for a single filesystem surface.

module;
#include "Core/Prelude.h"

export module foundation.core:filesystem;

import :base;
import :allocator;
import :array;
import :span;
import :string;
import :system;
import :io;

export namespace foundation::core
{
    // Reads an entire file into a byte buffer.
    [[nodiscard]] inline Result<Array<byte>> ReadFile(StringView path,
                                                      IAllocator& allocator = DefaultAllocator())
    {
        FileStream file(path, FileMode::Read);
        if (!file.IsValid())
        {
            return Err(ErrorCode::NotFound);
        }

        const i64 size = file.Size();
        if (size < 0)
        {
            return Err(ErrorCode::Internal);
        }

        Array<byte> data(allocator);
        data.Resize(static_cast<usize>(size));
        if (size > 0)
        {
            const u64 read = file.Read(data.Data(), static_cast<u64>(size));
            if (read != static_cast<u64>(size))
            {
                return Err(ErrorCode::Internal);
            }
        }
        return data;
    }

    // Writes a byte buffer to a file, replacing any existing contents.
    [[nodiscard]] inline Status WriteFile(StringView path, Span<const byte> data)
    {
        FileStream file(path, FileMode::Write);
        if (!file.IsValid())
        {
            return Status{ErrorCode::Internal};
        }
        if (!data.IsEmpty())
        {
            const u64 written = file.Write(data.Data(), data.Size());
            if (written != data.Size())
            {
                return Status{ErrorCode::Internal};
            }
        }
        return Status{};
    }

    // Writes a byte buffer to a file so that a reader (or the next run, after a crash) sees the old
    // contents or the new ones, never a torn mix: the bytes go to `<path>.tmp` beside the target,
    // which is then renamed over it (a rename on one volume replaces the target atomically). A
    // failed write leaves the target as it was.
    [[nodiscard]] inline Status WriteFileAtomic(StringView path, Span<const byte> data,
                                                IAllocator& allocator = DefaultAllocator())
    {
        String temporary(path, allocator);
        temporary.Append(u8".tmp");
        if (const Status written = WriteFile(temporary.AsView(), data); !written.IsOk())
        {
            (void)FileDelete(temporary.AsView());
            return written;
        }
        if (!FileMove(temporary.AsView(), path))
        {
            (void)FileDelete(temporary.AsView());
            return Status{ErrorCode::Internal};
        }
        return Status{};
    }

    // Removes a directory and everything under it. `RemoveDirectory` is the raw backend
    // primitive (rmdir), which removes only empty directories and silently fails on a
    // populated one. This composes the portable primitives, with no per-platform code.
    // Returns true when the directory is gone afterwards (a missing dir counts as gone).
    inline bool RemoveDirectoryRecursive(StringView path) // NOLINT(misc-no-recursion)
    {
        if (!DirectoryExists(path))
        {
            return true;
        }
        // Snapshot the children first: entry names are only valid during the callback,
        // and deleting while the backend iterates the directory is undefined.
        struct Entry
        {
            String name;
            bool isDirectory;
        };
        struct Collect
        {
            Array<Entry>* entries;
        };
        Array<Entry> entries;
        Collect collect{&entries};
        if (!ListDirectory(
                path,
                [](void* ctx, StringView name, bool isDirectory)
                {
                    auto* c = static_cast<Collect*>(ctx);
                    c->entries->PushBack(Entry{String(name), isDirectory});
                },
                &collect))
        {
            return false;
        }
        for (const Entry& entry : entries)
        {
            String child(path);
            child.Append(u8'/');
            child.Append(entry.name.AsView());
            if (entry.isDirectory)
            {
                if (!RemoveDirectoryRecursive(child.AsView()))
                {
                    return false;
                }
            }
            else if (!FileDelete(child.AsView()))
            {
                return false;
            }
        }
        return RemoveDirectory(path);
    }
}

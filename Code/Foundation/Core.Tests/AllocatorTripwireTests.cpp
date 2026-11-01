// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The allocator tripwire: `DefaultAllocator()` may appear only at composition roots and
// inside the value and ABI bounds that need it. This test pins the allowed-file set
// (allocator-allowlist.txt, next to this test) and fails when a non-test file outside it
// starts referencing the ambient default. Thread the owner's allocator instead, or, for a
// genuine new composition root, add the file to the allowlist. It also fails on stale
// entries so the list stays accurate as files are cleaned up.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;

using namespace foundation::core;

#ifndef RAPTOR_SOURCE_DIR
#error "Core.Tests must define RAPTOR_SOURCE_DIR (see Core.Tests/CMakeLists.txt)"
#endif

namespace
{
    bool IsSourceFile(StringView path)
    {
        const StringView ext = PathExtension(path);
        return ext == u8".cppm" || ext == u8".cpp" || ext == u8".h";
    }

    bool IsTestPath(StringView rel)
    {
        return rel.Contains(u8"Tests") || rel.Contains(u8"TestMain");
    }

    bool FileContainsToken(StringView path)
    {
        const Result<Array<byte>> bytes = ReadFile(path);
        if (!bytes.HasValue())
        {
            return false;
        }
        return StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size())
            .Contains(u8"DefaultAllocator()");
    }

    // Every file under `root`/`rel`, as paths relative to `root` with '/' separators.
    void CollectFiles(StringView root, StringView rel, Array<String>& files) // NOLINT(misc-no-recursion)
    {
        struct Entry
        {
            String name;
            bool isDirectory;
        };
        Array<Entry> entries;
        (void)ListDirectory(
            PathJoin(root, rel).AsView(),
            [](void* ctx, StringView name, bool isDirectory)
            {
                static_cast<Array<Entry>*>(ctx)->PushBack(Entry{String(name), isDirectory});
            },
            &entries);
        for (const Entry& entry : entries)
        {
            String child = PathJoin(rel, entry.name.AsView());
            if (entry.isDirectory)
            {
                CollectFiles(root, child.AsView(), files);
            }
            else
            {
                files.PushBack(Move(child));
            }
        }
    }

    // The allowlist's entries: one repo-relative path per line, blank lines skipped.
    bool ReadAllowlist(StringView path, HashSet<String>& allowed)
    {
        const Result<Array<byte>> bytes = ReadFile(path);
        if (!bytes.HasValue())
        {
            return false;
        }
        const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != u8'\n')
            {
                ++end;
            }
            StringView line = text.SubStr(start, end - start);
            while (!line.IsEmpty() && line[line.Size() - 1] == u8'\r')
            {
                line = line.SubStr(0, line.Size() - 1);
            }
            if (!line.IsEmpty())
            {
                allowed.Insert(String(line));
            }
            start = end + 1;
        }
        return true;
    }
}

TEST_CASE("allocators: DefaultAllocator() stays confined to the pinned allowlist")
{
    const StringView root(reinterpret_cast<const utf8char*>(RAPTOR_SOURCE_DIR));
    const String codeDir = PathJoin(root, u8"Code");
    const String allowlistPath = PathJoin(root, u8"Code/Foundation/Core.Tests/allocator-allowlist.txt");

    REQUIRE_MESSAGE(DirectoryExists(codeDir.AsView()), "Code/ not found under RAPTOR_SOURCE_DIR");

    // Load the pinned list (repo-relative paths, one per line).
    HashSet<String> allowed;
    REQUIRE_MESSAGE(ReadAllowlist(allowlistPath.AsView(), allowed), "allocator-allowlist.txt missing");

    // Fresh scan: every non-test source file referencing the ambient default.
    Array<String> files;
    CollectFiles(root, u8"Code", files);
    HashSet<String> current;
    for (const String& rel : files)
    {
        if (!IsSourceFile(rel.AsView()) || IsTestPath(rel.AsView()))
        {
            continue;
        }
        if (FileContainsToken(PathJoin(root, rel.AsView()).AsView()))
        {
            current.Insert(rel);
        }
    }
    REQUIRE_MESSAGE(!current.IsEmpty(),
                    "scan found nothing at all - RAPTOR_SOURCE_DIR is likely wrong");

    // New offenders: a file outside the allowlist references DefaultAllocator().
    for (const String& file : current)
    {
        if (!allowed.Contains(file))
        {
            FAIL_CHECK("NEW ambient-allocator reference (thread an owner's allocator, or "
                       "consciously add to allocator-allowlist.txt): "
                       << reinterpret_cast<const char*>(file.CStr()));
        }
    }
    // Stale entries: keep the list accurate as files get cleaned.
    for (const String& file : allowed)
    {
        if (!current.Contains(file))
        {
            FAIL_CHECK("STALE allowlist entry (file no longer references "
                       "DefaultAllocator() - remove it from allocator-allowlist.txt): "
                       << reinterpret_cast<const char*>(file.CStr()));
        }
    }
}

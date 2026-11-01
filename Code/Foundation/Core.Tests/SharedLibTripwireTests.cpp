// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The shared-libraries rendezvous tripwire: process-wide state must not live in module
// interface units as vague-linkage definitions, because each shared library would get its
// own copy (a registry registered in one and empty in another, or a thread_local handshake
// crossing wires at a boundary). Patterns caught in .cppm files:
//   1. `inline thread_local` / `inline static thread_local` variables
//   2. `static inline` data members holding state
//   3. Meyers-style accessors (declare a function-local static, return it), inline
//      or in-class (implicitly inline), both duplicate per library
// The fix is the GlobalLogger/DefaultAllocator shape: declare in the interface,
// define in the library's .cpp implementation unit. Genuinely safe cases (such as
// module-linkage definitions in a non-exported namespace block, or single-library
// state) go on the pinned allowlist, which also fails when entries go stale.

#include <doctest/doctest.h>

#include <regex>

#include "Core/Prelude.h"

import foundation.core;

using namespace foundation::core;

#ifndef RAPTOR_SOURCE_DIR
#error "Core.Tests must define RAPTOR_SOURCE_DIR (see Core.Tests/CMakeLists.txt)"
#endif

namespace
{
    bool IsTestPath(StringView rel)
    {
        return rel.Contains(u8"Tests") || rel.Contains(u8"TestMain");
    }

    bool FileHasHazard(StringView path)
    {
        const Result<Array<byte>> bytes = ReadFile(path);
        if (!bytes.HasValue())
        {
            return false;
        }
        const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        if (text.Contains(u8"inline thread_local") || text.Contains(u8"inline static thread_local") ||
            text.Contains(u8"static inline"))
        {
            return true;
        }
        // Meyers accessor: a function-local static immediately returned. Matches both
        // `static Foo instance; ... return instance;` and brace/paren-initialized forms.
        static const std::regex meyers(
            R"(static\s+(?:thread_local\s+)?[\w:<>,\s*&]+?\b(\w+)\s*(?:\{[^;{}]*\}|\([^;()]*\)|=[^;]+)?;\s*(?://[^\n]*\n|\n)*\s*return\s+(?:\*|&)?\1\s*;)");
        const char* begin = reinterpret_cast<const char*>(text.Data());
        return std::regex_search(begin, begin + text.Size(), meyers);
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

    // The allowlist's entries: one repo-relative path per line, blank and '#' lines skipped.
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
            if (!line.IsEmpty() && line[0] != u8'#')
            {
                allowed.Insert(String(line));
            }
            start = end + 1;
        }
        return true;
    }
}

TEST_CASE("shared-libs: module interfaces hold no duplicating process-wide state")
{
    const StringView root(reinterpret_cast<const utf8char*>(RAPTOR_SOURCE_DIR));
    const String codeDir = PathJoin(root, u8"Code");
    const String allowlistPath = PathJoin(root, u8"Code/Foundation/Core.Tests/sharedlib-allowlist.txt");

    REQUIRE_MESSAGE(DirectoryExists(codeDir.AsView()), "Code/ not found under RAPTOR_SOURCE_DIR");

    HashSet<String> allowed;
    REQUIRE_MESSAGE(ReadAllowlist(allowlistPath.AsView(), allowed), "sharedlib-allowlist.txt missing");

    Array<String> files;
    CollectFiles(root, u8"Code", files);
    HashSet<String> current;
    for (const String& rel : files)
    {
        if (PathExtension(rel.AsView()) != u8".cppm" || IsTestPath(rel.AsView()))
        {
            continue;
        }
        if (FileHasHazard(PathJoin(root, rel.AsView()).AsView()))
        {
            current.Insert(rel);
        }
    }
    REQUIRE_MESSAGE(!current.IsEmpty(),
                    "scan found nothing at all - RAPTOR_SOURCE_DIR is likely wrong");

    for (const String& file : current)
    {
        if (!allowed.Contains(file))
        {
            FAIL_CHECK("NEW process-wide state in a module interface (move the definition "
                       "to the library's impl unit - shared-libraries.md - or consciously "
                       "add to sharedlib-allowlist.txt): "
                       << reinterpret_cast<const char*>(file.CStr()));
        }
    }
    for (const String& file : allowed)
    {
        if (!current.Contains(file))
        {
            FAIL_CHECK("STALE allowlist entry (hazard gone - remove from "
                       "sharedlib-allowlist.txt): "
                       << reinterpret_cast<const char*>(file.CStr()));
        }
    }
}

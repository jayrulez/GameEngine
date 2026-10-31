// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The allocator tripwire: `DefaultAllocator()` may appear only at composition roots and
// inside the value and ABI bounds that need it. This test pins the allowed-file set
// (allocator-allowlist.txt, next to this test) and fails when a non-test file outside it
// starts referencing the ambient default. Thread the owner's allocator instead, or, for a
// genuine new composition root, add the file to the allowlist. It also fails on stale
// entries so the list stays accurate as files are cleaned up.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#ifndef RAPTOR_SOURCE_DIR
#error "Core.Tests must define RAPTOR_SOURCE_DIR (see Core.Tests/CMakeLists.txt)"
#endif

namespace
{
    namespace fs = std::filesystem;

    bool IsSourceFile(const fs::path& p)
    {
        const std::string ext = p.extension().string();
        return ext == ".cppm" || ext == ".cpp" || ext == ".h";
    }

    bool IsTestPath(const std::string& rel)
    {
        return rel.find("Tests") != std::string::npos ||
               rel.find("TestMain") != std::string::npos;
    }

    bool FileContainsToken(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
        {
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str().find("DefaultAllocator()") != std::string::npos;
    }
}

TEST_CASE("allocators: DefaultAllocator() stays confined to the pinned allowlist")
{
    const fs::path root = fs::path(RAPTOR_SOURCE_DIR);
    const fs::path codeDir = root / "Code";
    const fs::path allowlistPath =
        root / "Code" / "Foundation" / "Core.Tests" / "allocator-allowlist.txt";

    std::error_code ec;
    REQUIRE_MESSAGE(fs::exists(codeDir, ec), "Code/ not found under RAPTOR_SOURCE_DIR");

    // Load the pinned list (repo-relative paths, one per line).
    std::set<std::string> allowed;
    {
        std::ifstream in(allowlistPath);
        REQUIRE_MESSAGE(in.good(), "allocator-allowlist.txt missing");
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (!line.empty())
            {
                allowed.insert(line);
            }
        }
    }

    // Fresh scan: every non-test source file referencing the ambient default.
    std::set<std::string> current;
    for (fs::recursive_directory_iterator it(codeDir, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!it->is_regular_file(ec) || !IsSourceFile(it->path()))
        {
            continue;
        }
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (IsTestPath(rel))
        {
            continue;
        }
        if (FileContainsToken(it->path()))
        {
            current.insert(rel);
        }
    }
    REQUIRE_MESSAGE(!current.empty(),
                    "scan found nothing at all - RAPTOR_SOURCE_DIR is likely wrong");

    // New offenders: a file outside the allowlist references DefaultAllocator().
    for (const std::string& file : current)
    {
        if (allowed.find(file) == allowed.end())
        {
            FAIL_CHECK("NEW ambient-allocator reference (thread an owner's allocator, or "
                       "consciously add to allocator-allowlist.txt): "
                       << file);
        }
    }
    // Stale entries: keep the list accurate as files get cleaned.
    for (const std::string& file : allowed)
    {
        if (current.find(file) == current.end())
        {
            FAIL_CHECK("STALE allowlist entry (file no longer references "
                       "DefaultAllocator() - remove it from allocator-allowlist.txt): "
                       << file);
        }
    }
}

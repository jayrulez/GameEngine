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

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#ifndef RAPTOR_SOURCE_DIR
#error "Core.Tests must define RAPTOR_SOURCE_DIR (see Core.Tests/CMakeLists.txt)"
#endif

namespace
{
    namespace fs = std::filesystem;

    bool IsTestPath(const std::string& rel)
    {
        return rel.find("Tests") != std::string::npos ||
               rel.find("TestMain") != std::string::npos;
    }

    bool FileHasHazard(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in)
        {
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        if (text.find("inline thread_local") != std::string::npos ||
            text.find("inline static thread_local") != std::string::npos ||
            text.find("static inline") != std::string::npos)
        {
            return true;
        }
        // Meyers accessor: a function-local static immediately returned. Matches both
        // `static Foo instance; ... return instance;` and brace/paren-initialized forms.
        static const std::regex meyers(
            R"(static\s+(?:thread_local\s+)?[\w:<>,\s*&]+?\b(\w+)\s*(?:\{[^;{}]*\}|\([^;()]*\)|=[^;]+)?;\s*(?://[^\n]*\n|\n)*\s*return\s+(?:\*|&)?\1\s*;)");
        return std::regex_search(text, meyers);
    }
}

TEST_CASE("shared-libs: module interfaces hold no duplicating process-wide state")
{
    const fs::path root = fs::path(RAPTOR_SOURCE_DIR);
    const fs::path codeDir = root / "Code";
    const fs::path allowlistPath =
        root / "Code" / "Foundation" / "Core.Tests" / "sharedlib-allowlist.txt";

    std::error_code ec;
    REQUIRE_MESSAGE(fs::exists(codeDir, ec), "Code/ not found under RAPTOR_SOURCE_DIR");

    std::set<std::string> allowed;
    {
        std::ifstream in(allowlistPath);
        REQUIRE_MESSAGE(in.good(), "sharedlib-allowlist.txt missing");
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (!line.empty() && line[0] != '#')
            {
                allowed.insert(line);
            }
        }
    }

    std::set<std::string> current;
    for (fs::recursive_directory_iterator it(codeDir, ec), end; it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != ".cppm")
        {
            continue;
        }
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (IsTestPath(rel))
        {
            continue;
        }
        if (FileHasHazard(it->path()))
        {
            current.insert(rel);
        }
    }
    REQUIRE_MESSAGE(!current.empty(),
                    "scan found nothing at all - RAPTOR_SOURCE_DIR is likely wrong");

    for (const std::string& file : current)
    {
        if (allowed.find(file) == allowed.end())
        {
            FAIL_CHECK("NEW process-wide state in a module interface (move the definition "
                       "to the library's impl unit - shared-libraries.md - or consciously "
                       "add to sharedlib-allowlist.txt): "
                       << file);
        }
    }
    for (const std::string& file : allowed)
    {
        if (current.find(file) == current.end())
        {
            FAIL_CHECK("STALE allowlist entry (hazard gone - remove from "
                       "sharedlib-allowlist.txt): "
                       << file);
        }
    }
}

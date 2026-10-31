// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shared doctest entry point for all test binaries.
//
// Redirects scratch data that tests write (content DBs, cooked projects, temp files, all created
// through paths relative to the working directory, like "scratch_xxx_db") into a single
// gitignored ".test-scratch" directory, so test runs never litter the repo root or the build
// directory. Fixture reads are unaffected: they use absolute ${CMAKE_SOURCE_DIR} paths from
// compile definitions.
#pragma once

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <filesystem>

// ASAN detection, spelled so MSVC can parse it. `defined(__has_feature) && __has_feature(x)`
// in one expression is not portable: MSVC has no __has_feature, and its preprocessor still has
// to parse the call rather than short-circuiting past it, which it reports as
// "C1012: unmatched parenthesis: missing ')'". Nesting the test means the inner line is only
// ever lexed, never evaluated, on compilers without __has_feature.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TESTMAIN_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(TESTMAIN_ASAN)
#define TESTMAIN_ASAN 1
#endif

#if defined(TESTMAIN_ASAN)
#include <dlfcn.h>
// LeakSanitizer: suppress third-party leaks outside our control. libdbus caches a connection for
// the process lifetime (reached via SDL), and GPU user-mode drivers retain per-instance state.
// First-party leaks stay fatal.
extern "C" const char* __lsan_default_suppressions()
{
    return "leak:libdbus-1\n"
           "leak:nvidia\n"
           "leak:libvulkan_\n";
}

// The Vulkan loader dlcloses ICDs on instance destroy; a leak whose frames live in an unloaded
// module reports as "<unknown module>", which no name suppression can match. Pinning an extra
// dlopen reference keeps the drivers mapped through the end-of-process leak check so their
// frames symbolize and the suppressions above apply.
namespace testmain
{
    inline void PinGpuDriverModules()
    {
        static const char* const kModules[] = {
            "libGLX_nvidia.so.0",
            "libvulkan_intel.so",
            "libvulkan_lvp.so",
            "libvulkan_radeon.so",
        };
        for (const char* name : kModules)
        {
            (void)dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }
    }
} // namespace testmain
#endif

int main(int argc, char** argv)
{
#if defined(TESTMAIN_ASAN)
    testmain::PinGpuDriverModules();
#endif
    std::error_code ec;
    std::filesystem::create_directories(".test-scratch", ec);
    std::filesystem::current_path(".test-scratch", ec); // scratch writes land here, not the CWD root

    doctest::Context context(argc, argv);
    return context.run();
}

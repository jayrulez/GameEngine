// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VFS - :data_root partition.
//
// THE one mechanism by which an executable finds engine data (runtime assets, shaders, fonts):
// the "Data" directory identified by a ".dataroot" marker file. Discovery is anchored at the
// EXECUTABLE (robust for a relocated distribution - the working directory is unreliable when an
// app is launched from a menu or a .desktop file), walking up for a dev source tree, then
// falling back to the working directory. An explicit `--data-root <dir>` override wins, and is
// validated (it must hold the marker). There is deliberately no compile-time path fallback:
// a build that cannot find its data fails loudly instead of reading the build machine's tree.
//
// The application resolves the root ONCE at startup (ResolveDataRoot), mounts a
// NativeFileSystem over it, and hands that IFileSystem to whatever reads engine data (the
// shader host reads "Engine/Shaders/...", the GUI its default font under "Engine/Fonts/..."). Nothing
// below the application knows where the root is - only what it reads relative to it.
//
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h" // the no-data-root warning (a silent miss = unexplained missing assets)

export module foundation.vfs:data_root;

import foundation.core;

using namespace foundation::core;

export namespace foundation::vfs
{
    // Marker file that identifies a Data root directory.
    inline constexpr StringView kDataRootMarker = u8".dataroot";

    // A directory is a Data root when it holds the marker file.
    [[nodiscard]] inline bool IsDataRoot(StringView dir)
    {
        return FileExists(PathJoin(dir, kDataRootMarker).AsView());
    }

    // Walks up from `startDir`, returning the first "<ancestor>/Data" that is a data root, or
    // empty. The first iteration also covers "<startDir>/Data" - the relocated-dist case where
    // Data/ sits beside the executable.
    [[nodiscard]] inline String FindDataRootFrom(StringView startDir)
    {
        String dir(startDir);
        while (!dir.IsEmpty())
        {
            String candidate = PathJoin(dir.AsView(), u8"Data");
            if (IsDataRoot(candidate.AsView()))
            {
                return candidate;
            }
            dir = String(PathParent(dir.AsView()));
        }
        return String();
    }

    // Finds the absolute path of the data root, or empty if none is found. Search order:
    //   1. <exeDir>/Data, then walk up from <exeDir>   (dist beside the exe; dev under Bin/)
    //   2. <cwd>/Data,    then walk up from <cwd>       (dev convenience)
    [[nodiscard]] inline String FindDataRoot()
    {
        if (String r = FindDataRootFrom(GetExecutableDirectory().AsView()); !r.IsEmpty())
        {
            return r;
        }
        String fromCwd = FindDataRootFrom(GetCurrentDirectory().AsView());
        if (fromCwd.IsEmpty())
        {
            // Loud, not silent: there is no compile-time fallback, so a botched extraction
            // otherwise surfaces as unexplained missing assets. Name what was searched so the
            // fix is self-service (put Data/ beside the exe, or pass --data-root).
            LOG_ERROR(u8"VFS",
                      u8"no data root found (searched up from exe dir '{}' and cwd '{}' for a "
                      u8"Data/.dataroot marker) - put Data/ beside the executable or pass "
                      u8"--data-root <dir>",
                      GetExecutableDirectory(), GetCurrentDirectory());
        }
        return fromCwd;
    }

    // The command-line override every entry point honors: `--data-root <dir>` or
    // `--data-root=<dir>`.
    inline constexpr StringView kDataRootArgument = u8"--data-root";

    // Scans argv for the data-root override. Empty when absent (or given without a value).
    [[nodiscard]] inline String DataRootFromArguments(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const StringView arg(reinterpret_cast<const utf8char*>(argv[i]));
            if (arg == kDataRootArgument)
            {
                if (i + 1 < argc)
                {
                    return String(StringView(reinterpret_cast<const utf8char*>(argv[i + 1])));
                }
                return String();
            }
            if (arg.Size() > kDataRootArgument.Size() + 1 &&
                arg.SubStr(0, kDataRootArgument.Size()) == kDataRootArgument &&
                arg[kDataRootArgument.Size()] == utf8char('='))
            {
                return String(arg.SubStr(kDataRootArgument.Size() + 1,
                                         arg.Size() - kDataRootArgument.Size() - 1));
            }
        }
        return String();
    }

    // Resolves the data root: an explicit `overrideDir` (validated - it must hold the marker,
    // else an error and empty), otherwise the discovery walk. Empty = no data root; callers
    // fail loudly (an application exits, a test REQUIREs).
    [[nodiscard]] inline String ResolveDataRoot(StringView overrideDir)
    {
        if (!overrideDir.IsEmpty())
        {
            if (IsDataRoot(overrideDir))
            {
                return String(overrideDir);
            }
            LOG_ERROR(u8"VFS", u8"--data-root '{}' is not a data root (no {} marker inside it)",
                      overrideDir, kDataRootMarker);
            return String();
        }
        return FindDataRoot();
    }

    // The entry-point form: `--data-root` from argv, else the discovery walk.
    [[nodiscard]] inline String ResolveDataRoot(int argc, char** argv)
    {
        return ResolveDataRoot(DataRootFromArguments(argc, argv).AsView());
    }

    // Joins a data-root-relative path onto `dataRoot` (a plain PathJoin - kept so call sites
    // read as "data path", and so an empty root yields the relative path unchanged).
    [[nodiscard]] inline String DataPath(StringView dataRoot, StringView relative)
    {
        return dataRoot.IsEmpty() ? String(relative) : PathJoin(dataRoot, relative);
    }
}

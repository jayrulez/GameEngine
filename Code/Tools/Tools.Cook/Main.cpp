// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.Cook - the command-line cooker. Headless: opens the project, registers every builder, then
// plans and executes the incremental cook.
//
// Usage: Tools.Cook <projectDirectory> [--rebuild] [--dry-run] [--target <id>]
//                   [--data-root <dir>]
//   --rebuild        force-cook every buildable asset (for when a builder changed without a
//                    version bump)
//   --dry-run        print the plan (dirty set and orphans) without cooking
//   --target <id>    also cook a per-target DB under Cooked-<id>/ beside the host DB. Cooks the
//                    host first, then carries platform-invariant products forward and recooks only
//                    variant products (textures: BC or ASTC) for the target. "web-astc" is the
//                    ASTC mobile-web target; everything else is BC desktop. "host" is the default.
// The exit code is the number of failed cooks (0 on success).

#include <cstdio>
#include <cstring>
#include "Core/Log/Log.h"

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.registration;
import engine.composition; // RegisterAllScriptFacades: the complete engine facade surface
import editor.project;
import pipeline.cook;

using namespace foundation::core;
namespace vfs = foundation::vfs;

namespace
{
    // This binary's composition root: the one place it chooses the ambient allocator.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: Tools.Cook <projectDirectory> [--rebuild] [--dry-run] "
                             "[--target <id>] [--data-root <dir>]\n");
        return 1;
    }
    bool rebuild = false;
    bool dryRun = false;
    StringView targetId(u8"host");
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rebuild") == 0)
        {
            rebuild = true;
        }
        else if (std::strcmp(argv[i], "--dry-run") == 0)
        {
            dryRun = true;
        }
        else if (std::strcmp(argv[i], "--target") == 0)
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--target needs an id (e.g. web-astc)\n");
                return 1;
            }
            targetId = StringView(reinterpret_cast<const utf8char*>(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--data-root") == 0 && i + 1 < argc)
        {
            ++i; // ResolveDataRoot reads it below; the loop only has to step over it
        }
        else
        {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    ConsoleSink consoleSink;
    GlobalLogger().AddSink(&consoleSink);

    const StringView projectDir(reinterpret_cast<const utf8char*>(argv[1]));
    UniquePtr<editor::EditorProject> project =
        editor::EditorProject::Open(AppRoot(), projectDir);
    if (!project)
    {
        std::fprintf(stderr, "Tools.Cook: failed to open project '%s'\n", argv[1]);
        return 1;
    }

    // The engine data root (a custom shader's cook compiles against <dataRoot>/Shaders): the
    // --data-root argument, else the Data/.dataroot walk up from this tool's executable.
    const String dataRoot = foundation::vfs::ResolveDataRoot(argc, argv);
    if (dataRoot.IsEmpty())
    {
        std::fprintf(stderr, "Tools.Cook: no data root (put Data/ with its .dataroot marker "
                             "beside the tool, or pass --data-root <dir>)\n");
        return 1;
    }

    pipeline::BuilderRegistry registry{AppRoot()};
    pipeline::RegisterPipelineTypes(); // every asset, product and resource type, and the script cooks
    pipeline::RegisterAllBuilders(registry, dataRoot.AsView());
    // The complete engine script surface (the run, ui, physics, audio and other facades) goes into
    // the global registry, so script cooks compile against the same surface a running game has.
    // RegisterPipelineTypes registers only the foundation-level surface, without the Engine-level
    // facades. This registers metadata only (no device, GPU or world) and is idempotent.
    engine::RegisterAllScriptFacades();

    vfs::NativeFileSystem sourcesMount(project->SourcesRoot().AsView(), AppRoot());
    vfs::NativeFileSystem cacheMount(project->CacheRoot().AsView(), AppRoot());
    JobSystem jobs(AppRoot()); // owned by the tool's composition root

    pipeline::CookProgress progress;
    progress.onItem = [](usize done, usize total, StringView path, bool ok)
    {
        std::printf("[%zu/%zu] %s %.*s\n", done, total, ok ? "ok  " : "FAIL",
                    static_cast<int>(path.Size()), reinterpret_cast<const char*>(path.Data()));
    };

    const bool hostOnly = (targetId == StringView(u8"host"));

    // The host cook always runs: it is the editor's DB and the copy-forward source for any target.
    pipeline::CookDriver host(AppRoot(), project->SourceDb(), project->CookedDb(), registry,
                              &sourcesMount, &cacheMount, &jobs);
    pipeline::CookPlan hostPlan = host.Plan(rebuild);
    std::printf("cook plan (host): %zu dirty, %zu up to date, %zu orphan(s), %zu without builders\n",
                hostPlan.dirty.Size(), hostPlan.upToDate, hostPlan.orphans.Size(),
                hostPlan.unbuildable);
    if (dryRun && hostOnly)
    {
        for (const pipeline::CookItem& item : hostPlan.dirty)
        {
            std::printf("  dirty: %.*s\n", static_cast<int>(item.path.Size()),
                        reinterpret_cast<const char*>(item.path.Data()));
        }
        return 0;
    }
    const pipeline::CookStats hostStats = host.Execute(hostPlan, &progress);
    std::printf("host cooked %zu, failed %zu, swept %zu orphan(s)\n", hostStats.cooked,
                hostStats.failed, hostStats.orphansSwept);

    usize totalFailed = hostStats.failed;

    if (!hostOnly)
    {
        // Create the per-target DB on demand. It lives in a sibling Cooked-<id>/, not Cooked/<id>/:
        // the desktop pack walks Cooked/ recursively, so a nested target folder would leak into the
        // desktop Content.pak, and keeping it outside guarantees the desktop pack is unchanged by
        // target cooks. Its cook.db lives under .cache/<id>/.
        String cookedDir(project->Directory());
        cookedDir.Append(u8"/Cooked-");
        cookedDir.Append(targetId);
        const String cacheDir = PathJoin(PathJoin(project->Directory(), u8".cache"), targetId);
        (void)CreateDirectory(cookedDir.AsView());
        (void)CreateDirectory(PathJoin(project->Directory(), u8".cache").AsView());
        (void)CreateDirectory(cacheDir.AsView());

        vfs::NativeFileSystem targetCookedMount(cookedDir.AsView(), AppRoot());
        vfs::NativeFileSystem targetCacheMount(cacheDir.AsView(), AppRoot());
        foundation::content::ContentDatabase targetDb(AppRoot(), targetCookedMount, BinarySerializerFactory(),
                                                      u8".rasset");

        const pipeline::CookStats targetStats = pipeline::CookForTarget(
            AppRoot(), project->SourceDb(), targetDb, project->CookedDb(), host.Db(), registry,
            &sourcesMount,
            &targetCacheMount, pipeline::CookTargetFor(targetId), &jobs, rebuild);
        std::printf("target '%.*s' cooked %zu, copied-forward %zu, failed %zu\n",
                    static_cast<int>(targetId.Size()),
                    reinterpret_cast<const char*>(targetId.Data()), targetStats.cooked,
                    targetStats.copiedForward, targetStats.failed);
        totalFailed += targetStats.failed;
    }

    GlobalLogger().RemoveSink(&consoleSink);
    return static_cast<int>(totalFailed);
}

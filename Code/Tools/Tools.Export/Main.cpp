// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.Export - packages a project into shippable dists. A thin CLI over the editor's export
// driver: the editor's Export menu calls the same ExportOne and ExportAll, and tests drive it
// headlessly. The whole dist (the content, Content.pak and player.xml, plus the player and its
// runtime sidecars) comes from an export preset that resolves to an export template, so a preset
// produces the same result whichever front end runs it. No editor code goes into the result.
//
// Usage:
//   Tools.Export <projectDir> [--out <dir>] [--preset <name> | --all] [--rebuild] [--data-root <dir>]
//   Tools.Export --template list
//   Tools.Export --template import <templateDir>
//   Tools.Export --template create <configDir> [--install | --out <folder>] [--id <id>] [--name <name>]
//                                  [--notes <text>] [--icon <built-in name | file.svg>]
//
// Without an export_presets.xml in the project, a host preset for the current platform is
// synthesized, so a quick development export works out of the box (the host template is the
// player next to this tool).

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.animation;
import foundation.particles;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;
import pipeline.core;
import pipeline.registration;
import editor.project;
import texture.pipeline;
import fonts.pipeline;
import image.pipeline;
import foundation.image.resource;
import geometry.pipeline;
import animation.pipeline;
import materials.pipeline;
import shaders.pipeline;
import particles.pipeline;
import foundation.input;
import foundation.input.resource;
import input.pipeline;
import modelimporter;
import engine.composition; // AddAllSceneManagers + RegisterAllSceneComponentReflection
import foundation.physics;
import foundation.physics.resource;
import physics.pipeline;
import foundation.audio;
import foundation.audio.resource;
import audio.pipeline;
import foundation.script;
import foundation.script.resource;
import script.pipeline; // the per-language script cooks (AngelScript, Luau) come from
                        // RegisterPipelineTypes: the composition root registers every enabled
                        // backend, so export cooks scripts of either language without naming one here.

using namespace foundation::core;
namespace scene = foundation::scene;
namespace vfs = foundation::vfs;
namespace fs = std::filesystem;

namespace
{
    // This binary's composition root: the one place it chooses the ambient allocator.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




namespace
{
    [[nodiscard]] StringView Sv(const char* s)
    {
        return StringView(reinterpret_cast<const utf8char*>(s));
    }
    // Takes const String&, not StringView: a StringView argument binds an implicit String temporary to
    // this reference parameter, which lives to the end of the full-expression, so the returned CStr()
    // stays valid for the enclosing printf. A by-value StringView would return a dangling pointer.
    [[nodiscard]] const char* Cs(const String& s)
    {
        return reinterpret_cast<const char*>(s.CStr());
    }

    // The builder and type registrations live in Pipeline::Registration (the composition root);
    // main() calls RegisterPipelineTypes and RegisterAllBuilders directly.

    // The directory holding this executable (under Bin/, where Engine.Player and its .runtime-libs
    // live, which makes it the host template's source). argv[0] can be bare or relative, so it is
    // canonicalized.
    [[nodiscard]] String ToolDir(const char* argv0)
    {
        std::error_code ec;
        const fs::path self = fs::weakly_canonical(fs::absolute(fs::path(argv0), ec), ec);
        const std::string dir = self.parent_path().string();
        return String(StringView(reinterpret_cast<const utf8char*>(dir.c_str())));
    }

    // The templates root: $ENV_TEMPLATES_DIR, else <user-data-dir>/templates. The CLI has no editor
    // settings, so it passes no override, which matches the editor with an empty setting.
    [[nodiscard]] String TemplatesRoot() { return editor::ResolveTemplatesRoot(); }

    // Builds the registry from the templates root (if it exists) and the host template (from toolDir).
    void BuildRegistry(editor::TemplateRegistry& registry, StringView templatesRoot,
                       StringView toolDir)
    {
        std::error_code ec;
        UniquePtr<vfs::NativeFileSystem> rootFs;
        if (fs::is_directory(Cs(templatesRoot), ec))
        {
            rootFs = MakeUnique<vfs::NativeFileSystem>(AppRoot(), templatesRoot,
                                                       AppRoot());
        }
        vfs::NativeFileSystem toolFs(toolDir, AppRoot());
        registry.Refresh(templatesRoot, rootFs.Get(), toolDir,
                         &toolFs); // reads runtime-libs synchronously
    }

    int Usage()
    {
        std::fprintf(
            stderr,
            "usage:\n"
            "  Tools.Export <projectDir> [--out <dir>] [--preset <name> | --all] [--rebuild]\n"
            "  Tools.Export --template list\n"
            "  Tools.Export --template import <templateDir>\n"
            "  Tools.Export --template create <configDir> [--install | --out <folder>]\n"
            "                                 [--id <id>] [--name <name>] [--notes <text>]\n"
            "                                 [--icon <desktop|windows|linux|handheld|web|phone | file.svg>]\n");
        return 1;
    }

    // --- template management ---------------------------------------------------------------------

    int TemplateList(const char* argv0)
    {
        editor::TemplateRegistry registry;
        const String toolDir = ToolDir(argv0);
        const String root = TemplatesRoot();
        BuildRegistry(registry, root.AsView(), toolDir.AsView());
        std::printf("export templates (root: %s):\n", Cs(root.AsView()));
        for (usize i = 0; i < registry.Count(); ++i)
        {
            const editor::ExportTemplate* t = registry.At(i);
            std::printf("  %-24s %-8s %s%s\n", Cs(t->id.AsView()), Cs(t->platform.AsView()),
                        Cs(t->name.AsView()), t->isHost ? "  [host]" : "");
        }
        return 0;
    }

    int TemplateImport(const char* argv0, const char* srcDir)
    {
        const String root = TemplatesRoot();
        String importedId;
        if (!editor::ImportTemplate(Sv(srcDir), root.AsView(), &importedId).IsOk())
        {
            std::fprintf(stderr, "Tools.Export: failed to import '%s' (no valid template.xml?)\n",
                         srcDir);
            return 1;
        }
        const String dst = PathJoin(root.AsView(), importedId.AsView());
        std::printf("imported template '%s' -> %s\n", Cs(importedId), Cs(dst));
        (void)argv0;
        return 0;
    }

    // --template create <configDir> [--install | --out <folder>] [--id] [--name] [--notes]. By
    // default the template is installed into the templates root, ready to use. --out <folder>
    // writes a self-contained bundle to that folder, to zip. --id, --name and --notes replace the
    // canonical identity, so a second bundle for one platform (the Steam Deck build) can sit beside
    // the first. --icon names the template's icon (a built-in one, `handheld` for the Deck, or an
    // .svg); without it the platform's icon is used.
    int TemplateCreate(int argc, char** argv)
    {
        // argv[3] = configDir; optional argv[4..] = --install | --out <folder>.
        if (argc < 4)
        {
            return Usage();
        }
        const char* configDir = argv[3];
        bool install = true;
        const char* outFolder = nullptr;
        editor::TemplateIdentity identity;
        for (int i = 4; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--install") == 0)
            {
                install = true;
            }
            else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            {
                install = false;
                outFolder = argv[++i];
            }
            else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc)
            {
                identity.id = Sv(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            {
                identity.name = Sv(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--notes") == 0 && i + 1 < argc)
            {
                identity.notes = Sv(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--icon") == 0 && i + 1 < argc)
            {
                identity.icon = Sv(argv[++i]);
                if (!identity.icon.EndsWith(u8".svg") && editor::BuiltInTemplateIconSvg(identity.icon).IsEmpty())
                {
                    std::fprintf(stderr, "unknown icon: %s (desktop, windows, linux, handheld, web, phone, "
                                         "or an .svg file)\n",
                                 argv[i]);
                    return Usage();
                }
            }
            else
            {
                std::fprintf(stderr, "unknown option: %s\n", argv[i]);
                return Usage();
            }
        }

        const String root = TemplatesRoot();
        const String destRoot = install ? root : String(Sv(outFolder));
        const editor::TemplateOutput mode =
            install ? editor::TemplateOutput::Install : editor::TemplateOutput::ExportFolder;
        String createdId, createdDir;
        if (!editor::CreateTemplate(Sv(configDir), destRoot.AsView(), mode, &createdId, &createdDir, identity)
                 .IsOk())
        {
            std::fprintf(stderr,
                         "Tools.Export: failed to create a template from '%s' "
                         "(missing player binary or runtime-libs?)\n",
                         configDir);
            return 1;
        }
        std::printf("created template '%s' -> %s\n", Cs(createdId), Cs(createdDir));
        return 0;
    }
}

int main(int argc, char** argv)
{
    ConsoleSink consoleSink;
    GlobalLogger().AddSink(&consoleSink);

    // --- template mode ---
    if (argc >= 2 && std::strcmp(argv[1], "--template") == 0)
    {
        if (argc < 3)
        {
            return Usage();
        }
        if (std::strcmp(argv[2], "list") == 0)
        {
            return TemplateList(argv[0]);
        }
        if (std::strcmp(argv[2], "import") == 0)
        {
            if (argc < 4)
            {
                return Usage();
            }
            return TemplateImport(argv[0], argv[3]);
        }
        if (std::strcmp(argv[2], "create") == 0)
        {
            return TemplateCreate(argc, argv);
        }
        return Usage();
    }

    // --- export mode ---
    if (argc < 2)
    {
        return Usage();
    }
    const char* projectDir = argv[1];
    const char* outArg = nullptr;
    const char* presetName = nullptr;
    bool all = false;
    bool rebuild = false;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
        {
            outArg = argv[++i];
        }
        else if (std::strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            presetName = argv[++i];
        }
        else if (std::strcmp(argv[i], "--all") == 0)
        {
            all = true;
        }
        else if (std::strcmp(argv[i], "--rebuild") == 0)
        {
            rebuild = true;
        }
        else if (std::strcmp(argv[i], "--data-root") == 0 && i + 1 < argc)
        {
            ++i; // ResolveDataRoot reads it below; the loop only has to step over it
        }
        else
        {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            return Usage();
        }
    }
    if (all && presetName != nullptr)
    {
        std::fprintf(stderr, "--all and --preset are mutually exclusive\n");
        return 1;
    }

    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(AppRoot(), Sv(projectDir));
    if (!project)
    {
        std::fprintf(stderr, "Tools.Export: failed to open project '%s'\n", projectDir);
        return 1;
    }

    // The engine data root (the shader cook reads <dataRoot>/Shaders): the --data-root argument,
    // else the Data/.dataroot walk up from this tool's executable, as every executable finds it.
    const String dataRoot = vfs::ResolveDataRoot(argc, argv);
    if (dataRoot.IsEmpty())
    {
        std::fprintf(stderr, "Tools.Export: no data root (put Data/ with its .dataroot marker "
                             "beside the tool, or pass --data-root <dir>)\n");
        return 1;
    }

    pipeline::BuilderRegistry builders{AppRoot()};
    pipeline::RegisterPipelineTypes(); // every asset, product and resource type, and the script cooks
    pipeline::RegisterAllBuilders(builders, dataRoot.AsView());
    // The complete engine script surface (the run, ui, physics, audio and other facades) goes into
    // the global registry, so the export cook compiles scripts against the same surface a running
    // game has. RegisterPipelineTypes registers only the foundation-level surface, without the
    // Engine-level facades. This registers metadata only (no device, GPU or world) and is idempotent.
    engine::RegisterAllScriptFacades();

    // Component reflection (with its data-version gates) for every domain, through the scene
    // composition root, before any scene stream deserializes. One shared list keeps this host
    // from missing a domain.
    engine::RegisterAllSceneComponentReflection();
    HashMap<Guid, Array<byte>> sceneStreams;
    engine::CollectSceneStreams(AppRoot(), *project->SourceDb().RootGroup(), sceneStreams);

    // The presets come from <project>/export_presets.xml, else a synthesized host preset.
    editor::ExportPresetSet presets;
    {
        vfs::NativeFileSystem projectFs(project->Directory(), AppRoot());
        if (!editor::LoadExportPresets(projectFs, presets).IsOk())
        {
            editor::DefaultExportPresets(presets);
        }
    }

    editor::TemplateRegistry registry;
    BuildRegistry(registry, TemplatesRoot().AsView(), ToolDir(argv[0]).AsView());

    const String outRoot =
        (outArg != nullptr) ? String(Sv(outArg)) : PathJoin(project->Directory(), u8"Dist");

    const editor::SceneReferenceScanner scanner =
        [](foundation::content::Instance& instance, foundation::content::ContentDatabase& db,
           editor::SceneReferences& out)
    { (void)engine::ScanSceneReferences(AppRoot(), instance, db, out.resources, out.prefabs); };


    if (all)
    {
        const Span<const editor::ExportPreset> span(presets.presets.Data(), presets.presets.Size());
        if (!editor::ExportAll(*project, span, registry, builders, outRoot.AsView(),
                               dataRoot.AsView(), rebuild, {}, true, &sceneStreams, &scanner)
                 .IsOk())
        {
            std::fprintf(stderr, "Tools.Export: one or more presets failed (see log)\n");
            return 1;
        }
        std::printf("export done: %zu preset(s) -> %s\n", presets.presets.Size(),
                    Cs(outRoot.AsView()));
        return 0;
    }

    const editor::ExportPreset* preset =
        (presetName != nullptr) ? presets.Find(Sv(presetName))
                                : (presets.presets.Size() > 0 ? &presets.presets[0] : nullptr);
    if (preset == nullptr)
    {
        std::fprintf(stderr, "Tools.Export: no preset%s%s\n", presetName ? " named " : "",
                     presetName ? presetName : " defined");
        return 1;
    }

    editor::ExportResult result;
    if (!editor::ExportOne(*project, *preset, registry, builders, outRoot.AsView(),
                           dataRoot.AsView(), rebuild, &result, {}, true, &sceneStreams, &scanner)
             .IsOk())
    {
        std::fprintf(stderr, "Tools.Export: export failed (see log)\n");
        return 1;
    }
    std::printf(
        "exported '%s' -> %s\n  cook: %zu cooked, %zu scene(s), %zu packed | staged %zu file(s)\n",
        Cs(preset->name.AsView()), Cs(result.outputDir.AsView()), result.content.cooked,
        result.content.scenesStaged, result.content.filesPacked, result.filesStaged);
    if (!result.engineVersionWarning.IsEmpty())
    {
        std::printf("  warning: %s\n", Cs(result.engineVersionWarning.AsView()));
    }
    if (result.pruning.pruned)
    {
        const String reportText = editor::FormatPruningReport(result.pruning);
        std::printf("  pruned: %zu kept, %zu dropped -> %s/export-report.txt\n",
                    result.pruning.keptCount, result.pruning.dropped.Size(),
                    Cs(result.outputDir.AsView()));
        std::printf("%s", Cs(reportText.AsView()));
    }

    GlobalLogger().RemoveSink(&consoleSink);
    return 0;
}

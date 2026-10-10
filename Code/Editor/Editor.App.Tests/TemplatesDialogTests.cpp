// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the export templates dialog over a scratch templates root: it lists what the
// root holds and the host build, shows the selected one's details (its engine version, warned when
// not this editor's), installs a bundle from a folder, and removes one (never the host build).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    /// A template bundle in `dir`: its manifest and a player file.
    void WriteBundle(StringView dir, StringView id, StringView name, StringView engineVersion)
    {
        REQUIRE(CreateDirectories(dir));
        foundation::vfs::NativeFileSystem fs(dir, DefaultAllocator());
        ExportTemplate tmpl;
        tmpl.id = String(id);
        tmpl.name = String(name);
        tmpl.platform = String(u8"Linux64");
        tmpl.config = String(u8"Release");
        tmpl.engineVersion = String(engineVersion);
        tmpl.playerBinary = String(u8"Engine.Player");
        tmpl.sidecars.PushBack(String(u8"libwgpu_native.so"));
        tmpl.notes = String(u8"for the test");
        REQUIRE(SaveTemplateManifest(*fs.AsWritable(), tmpl).IsOk());
        const StringView player = u8"#!player\n";
        REQUIRE(fs.AsWritable()
                    ->Save(u8"Engine.Player", Span<const byte>(reinterpret_cast<const byte*>(player.Data()),
                                                               player.Size()))
                    .IsOk());
    }

    struct Fixture
    {
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_root");
        String source = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_source");
        String hostDir = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_host");
        EditorContext context{DefaultAllocator()};
        String picked; // what the folder picker answers
        Array<String> revealed{DefaultAllocator()};

        Fixture()
        {
            (void)RemoveDirectoryRecursive(root.AsView());
            (void)RemoveDirectoryRecursive(source.AsView());
            (void)RemoveDirectoryRecursive(hostDir.AsView());
            REQUIRE(CreateDirectories(root.AsView()));
            WriteBundle(PathJoin(root.AsView(), u8"t-linux").AsView(), u8"t-linux", u8"Linux Release",
                        kEngineVersionString);
            WriteBundle(PathJoin(root.AsView(), u8"t-old").AsView(), u8"t-old", u8"Old Linux", u8"0.0.1");
            // The host build: a player beside the (pretend) editor.
            REQUIRE(CreateDirectories(hostDir.AsView()));
            foundation::vfs::NativeFileSystem hostFs(hostDir.AsView(), DefaultAllocator());
            const StringView player = u8"#!player\n";
            REQUIRE(hostFs.AsWritable()
                        ->Save(GetExecutableName(u8"Engine.Player").AsView(),
                               Span<const byte>(reinterpret_cast<const byte*>(player.Data()), player.Size()))
                        .IsOk());
        }
        ~Fixture()
        {
            (void)RemoveDirectoryRecursive(root.AsView());
            (void)RemoveDirectoryRecursive(source.AsView());
            (void)RemoveDirectoryRecursive(hostDir.AsView());
        }

        app::TemplatesDialogSeams Seams()
        {
            app::TemplatesDialogSeams seams;
            seams.templatesRoot = [this]() { return root; };
            seams.refresh = [this](TemplateRegistry& registry)
            {
                foundation::vfs::NativeFileSystem rootFs(root.AsView(), DefaultAllocator());
                foundation::vfs::NativeFileSystem hostFs(hostDir.AsView(), DefaultAllocator());
                registry.Refresh(root.AsView(), &rootFs, hostDir.AsView(), &hostFs);
            };
            seams.pickFolder = [this](Function<void(String)> chosen)
            {
                if (!picked.IsEmpty())
                {
                    chosen(picked);
                }
            };
            seams.revealFolder = [this](StringView folder) { revealed.PushBack(String(folder)); };
            return seams;
        }

        static i32 IndexOf(const app::TemplatesDialog& dialog, StringView id)
        {
            for (usize i = 0; i < dialog.TemplateCount(); ++i)
            {
                if (dialog.TemplateAt(i)->id == id)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }
    };
}

TEST_CASE("templates dialog: lists the root's templates and the host build, the selected one's details")
{
    Fixture f;
    auto dialog = MakeRef<app::TemplatesDialog>(DefaultAllocator(), f.context, f.Seams());
    REQUIRE(dialog->TemplateCount() == 3u); // two installed, the host build
    REQUIRE(dialog->Selected() != nullptr); // the first, selected on open

    const i32 linuxIndex = Fixture::IndexOf(*dialog, u8"t-linux");
    REQUIRE(linuxIndex >= 0);
    dialog->Select(static_cast<usize>(linuxIndex));
    CHECK(dialog->DetailText(u8"Platform") == StringView(u8"Linux64"));
    CHECK(dialog->DetailText(u8"Engine") == kEngineVersionString);
    CHECK(dialog->DetailText(u8"Runtime files") == StringView(u8"libwgpu_native.so"));
    CHECK(dialog->DetailText(u8"Notes") == StringView(u8"for the test"));
    CHECK(dialog->RemoveButton().IsEnabled);
    dialog->RevealButton().FireClick();
    REQUIRE(f.revealed.Size() == 1u);
    CHECK(f.revealed[0].AsView().EndsWith(u8"t-linux"));

    // Built for another engine: it says so, and that an export still runs.
    dialog->Select(static_cast<usize>(Fixture::IndexOf(*dialog, u8"t-old")));
    CHECK(dialog->DetailText(u8"Engine").StartsWith(u8"0.0.1 (this editor is"));

    // The host build is this editor's own: never removable.
    for (usize i = 0; i < dialog->TemplateCount(); ++i)
    {
        if (dialog->TemplateAt(i)->isHost)
        {
            dialog->Select(i);
        }
    }
    REQUIRE(dialog->Selected()->isHost);
    CHECK_FALSE(dialog->RemoveButton().IsEnabled);
    dialog->RemoveSelected(true);
    CHECK(dialog->TemplateCount() == 3u);
}

TEST_CASE("templates dialog: installs a bundle from a folder and removes one, the list following")
{
    Fixture f;
    auto dialog = MakeRef<app::TemplatesDialog>(DefaultAllocator(), f.context, f.Seams());
    REQUIRE(dialog->TemplateCount() == 3u);

    // Install: the picked folder's bundle is copied into the root, listed and selected.
    WriteBundle(f.source.AsView(), u8"t-new", u8"New Linux", kEngineVersionString);
    f.picked = f.source;
    dialog->InstallFromFolder();
    CHECK(dialog->TemplateCount() == 4u);
    REQUIRE(dialog->Selected() != nullptr);
    CHECK(dialog->Selected()->id == StringView(u8"t-new"));
    CHECK(DirectoryExists(PathJoin(f.root.AsView(), u8"t-new").AsView()));

    // A cancelled pick does nothing.
    f.picked = String();
    dialog->InstallFromFolder();
    CHECK(dialog->TemplateCount() == 4u);

    // Remove (confirmed): its folder goes, and so does its row.
    dialog->RemoveSelected(true);
    CHECK(dialog->TemplateCount() == 3u);
    CHECK_FALSE(DirectoryExists(PathJoin(f.root.AsView(), u8"t-new").AsView()));
    CHECK(Fixture::IndexOf(*dialog, u8"t-new") < 0);
}

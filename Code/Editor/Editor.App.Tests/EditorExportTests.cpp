// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorExport: an export asks for a project and runs one at a time; the templates'
// folder is the user's override when there is one; a resource report with no project is a line
// in the log, not a crash; a project's presets saved are read back when it opens again.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.settings;
import pipeline.core;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

TEST_CASE("export: one asked for with no project says to open one; the templates follow the user's override")
{
    RegisterEditorSettingsTypes();
    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    EditorCookService cook;
    EditorJobService jobs{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    foundation::settings::Settings user(DefaultAllocator());

    app::EditorExport::Services services;
    services.context = &context;
    services.cook = &cook;
    services.jobs = &jobs;
    services.builders = &builders;
    services.userSettings = &user;
    app::EditorExport exporter(DefaultAllocator(), Move(services));

    exporter.Run(u8"Linux", false);
    CHECK(said == String(u8"Open a project first."));
    CHECK_FALSE(exporter.IsPending());

    user.Section<EditorExportSettings>().templatesRoot = String(u8"/shared/templates");
    CHECK(exporter.TemplatesRoot() == String(u8"/shared/templates"));

    app::EditorExport::ReportResourceMemory(nullptr); // no project: said in the log
    jobs.Shutdown();
}

TEST_CASE("export: a project's presets are read back when it opens again")
{
    RegisterEditorSettingsTypes();
    const String dir(u8"scratch_export_presets_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    EditorCookService cook;
    EditorJobService jobs{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    const auto make = [&]()
    {
        app::EditorExport::Services services;
        services.context = &context;
        services.cook = &cook;
        services.jobs = &jobs;
        services.builders = &builders;
        return MakeUnique<app::EditorExport>(DefaultAllocator(), DefaultAllocator(), Move(services));
    };
    {
        UniquePtr<app::EditorExport> first = make();
        first->SetProject(project.Get());
        const usize defaults = first->Presets().Count();
        ExportPreset deck;
        deck.name = String(u8"Steam Deck");
        deck.platform = String(u8"Linux64");
        (void)first->Presets().Add(deck);
        first->SavePresets();
        REQUIRE(first->Presets().Count() == defaults + 1);
    }
    // The next session: a new export over the same project.
    UniquePtr<app::EditorExport> again = make();
    again->SetProject(project.Get());
    bool found = false;
    for (const ExportPreset& preset : again->Presets().Set().presets)
        found = found || preset.name == StringView(u8"Steam Deck");
    CHECK(found);
    project.Reset();
    jobs.Shutdown();
    (void)RemoveDirectoryRecursive(dir.AsView());
}

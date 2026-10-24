// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorExport: an export asks for a project and runs one at a time; the templates'
// folder is the user's override when there is one; a resource report with no project is a line
// in the log, not a crash.
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

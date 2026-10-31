// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Closing the editor session: the views' state is written once, while the views are still up, and
// kept as they close. A page that saves its own state as it closes must not cause the open pages to
// be written again, or the last page to close would leave none to reopen, and a project closed with
// a scene open would reopen with none.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.settings;
import pipeline.core;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

TEST_CASE("editor session: a project closed with a page open writes that page; a page's own save as it closes keeps it")
{
    app::RegisterEditorProjectSettingsTypes(); // the store's sections, as the editor registers them
    const String dir(u8"scratch_editor_session");
    std::error_code ec;
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Session").IsOk());

    EditorContext context{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorCookService cook;
    EditorJobService jobs{DefaultAllocator()};
    ThumbnailService thumbnails;
    Random rng(7);
    const Guid page = Guid::Generate(rng);
    bool open = true; // the page is up until the views close

    app::EditorSession::Services services;
    services.context = &context;
    services.cook = &cook;
    services.jobs = &jobs;
    services.builders = &builders;
    services.thumbnails = &thumbnails;
    // The views' state: the page while it is up, and nothing once it has closed.
    services.captureState = [&](foundation::settings::Settings& store)
    {
        Array<Guid> pages;
        if (open)
            pages.PushBack(page);
        app::CaptureOpenPages(store, pages, open ? page : Guid{});
    };
    // The views close: the page goes, saving its own state as a scene page does.
    services.closing = [&]()
    {
        open = false;
        context.RequestProjectEditorSettingsSave();
    };
    {
        app::EditorSession session(DefaultAllocator(), Move(services));
        REQUIRE(session.Open(dir.AsView(), app::EditorSession::OpenOptions{}));
        session.Close();
    }
    jobs.Shutdown();

    // Opened again: the page is there to reopen.
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(project);
    foundation::settings::Settings store{DefaultAllocator()};
    REQUIRE(app::LoadProjectEditorSettings(store, project->EditorStateRoot().AsView()).IsOk());
    Array<Guid> pages;
    Guid active;
    REQUIRE(app::ApplyOpenPages(store, pages, active).IsOk());
    REQUIRE(pages.Size() == 1u);
    CHECK(pages[0] == page);
    CHECK(active == page);
    project.Reset();
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Closing the editor session: the views' state is written once, while the views are still up, and
// kept as they close. Opening a project loads the plugins it enables into the embedded runtime's
// plugin host, and closing it unloads them. A page that saves its own state as it closes must not cause the open pages to
// be written again, or the last page to close would leave none to reopen, and a project closed with
// a scene open would reopen with none.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.settings;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import pipeline.core;
import pipeline.importer;
import engine.project;
import engine.defaultapp;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

TEST_CASE("editor session: a project closed with a page open writes that page; a page's own save as it closes keeps it")
{
    app::RegisterEditorProjectSettingsTypes(); // the store's sections, as the editor registers them
    const String dir(u8"scratch_editor_session");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Session").IsOk());

    EditorContext context{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorCookService cook;
    ThumbnailService thumbnails;
    Random rng(7);
    const Guid page = Guid::Generate(rng);
    bool open = true; // the page is up until the views close

    app::EditorSession::Services services;
    services.context = &context;
    services.cook = &cook;
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
    context.Tasks().Finish();

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
    (void)RemoveDirectoryRecursive(dir.AsView());
}

namespace
{
    // A headless application host: no shell, no graphics, no windows.
    class StubHost final : public foundation::runtime::IApplicationHost
    {
    public:
        foundation::runtime::SubsystemManager& Subsystems() noexcept override { return m_subsystems; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        TaskPool& Tasks() noexcept override
        {
            static TaskPool tasks(DefaultAllocator(), 0u);
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override { return nullptr; }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        foundation::runtime::SubsystemManager m_subsystems{DefaultAllocator()};
    };

    // A plugin with all four parts: each records its kind as it loads and unloads, and the Editor
    // part whether the editor's context was offered.
    constexpr char8_t kFourParts[] = u8"four_parts";
    Array<String> g_partLog;
    EditorContext* g_contextSeen = nullptr;
    pipeline::PipelinePluginServices* g_pipelineSeen = nullptr;
    template <foundation::runtime::PluginPartKind Kind>
    void PartLoad(foundation::runtime::PluginContext& context)
    {
        g_partLog.PushBack(Format(u8"load:{}", foundation::runtime::PluginPartKindName(Kind)));
        if (Kind == foundation::runtime::PluginPartKind::Editor)
            g_contextSeen = context.Find<EditorContext>();
        if (Kind == foundation::runtime::PluginPartKind::Pipeline)
            g_pipelineSeen = context.Find<pipeline::PipelinePluginServices>();
    }
    template <foundation::runtime::PluginPartKind Kind>
    void PartUnload(foundation::runtime::PluginContext&)
    {
        g_partLog.PushBack(Format(u8"unload:{}", foundation::runtime::PluginPartKindName(Kind)));
    }
    template <foundation::runtime::PluginPartKind Kind>
    const foundation::runtime::PluginPart* Part()
    {
        static constexpr foundation::runtime::PluginPart part{kFourParts, Kind, &PartLoad<Kind>, &PartUnload<Kind>};
        return &part;
    }
}

TEST_CASE("editor session: a project's plugins load with it into the embedded runtime, every part an editor takes, and unload as it closes")
{
    app::RegisterEditorProjectSettingsTypes();
    const String dir(u8"scratch_editor_session_plugins");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Plugins").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
        REQUIRE(project);
        foundation::runtime::EnabledPlugin entry;
        entry.id = String(kFourParts);
        project->Settings().plugins.PushBack(Move(entry));
        REQUIRE(project->SaveSettings().IsOk());
    }

    foundation::runtime::PluginManifest manifest;
    manifest.id = String(kFourParts);
    manifest.version = String(u8"1.0");
    manifest.engineVersion = String(engine::project::kEngineVersionString);
    for (foundation::runtime::PluginPartKind kind : foundation::runtime::kPluginPartKinds)
        manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(foundation::runtime::PluginPartKindName(kind))});
    manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(GetHostPlatformName())});
    MemoryStream xml;
    REQUIRE(foundation::runtime::WritePluginManifest(manifest, xml).IsOk());
    Array<byte> text;
    for (byte b : xml.Bytes())
        text.PushBack(b);
    text.PushBack(byte{0});
    const foundation::runtime::PluginPartEntryFn parts[] = {
        &Part<foundation::runtime::PluginPartKind::Engine>, &Part<foundation::runtime::PluginPartKind::Pipeline>,
        &Part<foundation::runtime::PluginPartKind::Tooling>, &Part<foundation::runtime::PluginPartKind::Editor>};
    const foundation::runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(text.Data()), parts, 4}};

    StubHost host;
    engine::runtime::DefaultApplication embedded;
    embedded.Configure(host);
    EditorContext context{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorCookService cook;
    ThumbnailService thumbnails;
    app::EditorSession::Services services;
    services.context = &context;
    services.cook = &cook;
    services.builders = &builders;
    services.thumbnails = &thumbnails;
    services.embeddedHost = &host;
    services.embeddedApp = &embedded;
    services.linkedPlugins = Span<const foundation::runtime::LinkedPlugin>(linked, 1);
    g_partLog.Clear();
    g_contextSeen = nullptr;
    {
        app::EditorSession session(DefaultAllocator(), Move(services));
        REQUIRE(session.Open(dir.AsView(), app::EditorSession::OpenOptions{}));
        REQUIRE(context.Plugins() != nullptr);
        CHECK(context.Plugins() == embedded.Plugins());
        CHECK(context.Plugins()->Role() == foundation::runtime::HostRole::Editor);
        CHECK(context.Plugins()->IsLoaded(kFourParts));
        REQUIRE(g_partLog.Size() == 4u);
        CHECK(g_partLog[0] == String(u8"load:Engine"));
        CHECK(g_partLog[3] == String(u8"load:Editor"));
        CHECK(g_contextSeen == &context);
        REQUIRE(g_pipelineSeen != nullptr); // a Pipeline part adds to the editor's registries
        CHECK(g_pipelineSeen->Registries().builders == &builders);
        CHECK(g_pipelineSeen->Registries().importers == &context.Importers());
        session.Close();
        CHECK(context.Plugins() == nullptr);
        CHECK(embedded.Plugins() == nullptr);
        REQUIRE(g_partLog.Size() == 8u);
        CHECK(g_partLog[4] == String(u8"unload:Editor")); // the reverse of the load
    }
    context.Tasks().Finish();
    embedded.OnShutdown(host);
    (void)RemoveDirectoryRecursive(dir.AsView());
}

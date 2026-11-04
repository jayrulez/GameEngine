// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the reference game's own plugin package (Data/SampleProjects/NativeSample)
// in every host. Its four parts are built in this tree and linked here as a host links its
// plugins; a project that enables it loads, in each host, the parts that host's role takes, and
// each part's contribution arrives with it and leaves with it:
//   - Engine: the spin manager, in every scene (where the host offers the engine's service);
//   - Pipeline: the note asset type, its builder and its creator, in the host's registries;
//   - Tooling: the nativesample_notes MCP tool, in every MCP server the host serves;
//   - Editor: the Project > Native Sample Notes action, in the editor.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.shell;
import foundation.graphics;
import foundation.settings;
import pipeline.core;
import pipeline.importer;
import engine.project;
import engine.defaultapp;
import editor.project;
import editor.core;
import editor.mcp;
import editor.app;
import nativesample.engine;
import nativesample.pipeline;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
namespace runtime = foundation::runtime;
namespace app = editor::app;
using foundation::json::JsonValue;

namespace
{
    constexpr char8_t kNativeSample[] = u8"nativesample";

    // A headless application host: no shell, no graphics, no windows.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::SubsystemManager& Subsystems() noexcept override { return m_subsystems; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        TaskPool& Tasks() noexcept override
        {
            static TaskPool tasks(DefaultAllocator(), 0u);
            return tasks;
        }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&,
                                                       const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::SubsystemManager m_subsystems{DefaultAllocator()};
    };

    // A scratch project that enables the reference game's package (linked here, so no path).
    void MakeProject(StringView dir, StringView name)
    {
        (void)RemoveDirectoryRecursive(dir);
        REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, name).IsOk());
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        runtime::EnabledPlugin entry;
        entry.id = String(kNativeSample);
        project->Settings().plugins.PushBack(Move(entry));
        REQUIRE(project->SaveSettings().IsOk());
    }

    [[nodiscard]] bool HasCreator(const pipeline::AssetCreatorRegistry& creators, StringView label)
    {
        return creators.FindByLabel(label) != nullptr;
    }

    [[nodiscard]] bool SpinContributed()
    {
        return foundation::scene::SceneModuleContributions::Global().Contains(TypeOf<nativesample::SpinManager>().id);
    }

    struct Called
    {
        JsonValue value;
        String text;
        bool error = false;
    };

    Called Call(McpServer& server, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = server.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue response = json::Parse(line.response.AsView()).value;
        Called called;
        if (response.Has(u8"error"))
        {
            called.error = true;
            called.text = response.Get(u8"error").Get(u8"message").AsString();
            return called;
        }
        JsonValue result = response.Get(u8"result");
        called.error = result.Get(u8"isError").AsBool();
        called.text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (!called.error)
        {
            called.value = JsonValue::Parse(called.text.AsView());
        }
        return called;
    }
}

TEST_CASE("plugins: the reference game's package is linked here with its four parts, and its manifest reads")
{
    const runtime::LinkedPlugin* found = nullptr;
    for (const runtime::LinkedPlugin& linked : runtime::LinkedPluginList())
    {
        runtime::PluginManifest manifest;
        MemoryStream stream;
        const StringView xml{linked.manifestXml};
        (void)stream.Write(xml.Data(), xml.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        REQUIRE(runtime::ReadPluginManifest(stream, manifest).IsOk());
        if (manifest.id == StringView(kNativeSample))
        {
            found = &linked;
            for (runtime::PluginPartKind kind : runtime::kPluginPartKinds)
            {
                CHECK(manifest.HasPart(kind));
            }
            String why;
            CHECK(runtime::CheckPluginManifest(manifest, engine::project::kEngineVersionString, why));
        }
    }
    REQUIRE(found != nullptr);
    CHECK(found->partCount == 4u); // an editor's role links every part
}

TEST_CASE("plugins: in the editor, a project's package brings every part's contribution and takes it away at close")
{
    app::RegisterEditorProjectSettingsTypes();
    const StringView dir = u8"scratch_plugins_editor";
    MakeProject(dir, u8"Editor Notes");

    StubHost host;
    engine::runtime::DefaultApplication embedded;
    embedded.Configure(host);
    editor::EditorContext context{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    editor::EditorCookService cook;
    editor::ThumbnailService thumbnails;
    editor::mcp::McpPluginServices pluginTools{DefaultAllocator()};
    editor::mcp::ProjectSession mcpSession;
    McpServer server;
    pluginTools.AttachServer(server, mcpSession); // the editor's MCP host's server, serving
    app::EditorSession::Services services;
    services.context = &context;
    services.cook = &cook;
    services.builders = &builders;
    services.thumbnails = &thumbnails;
    services.embeddedHost = &host;
    services.embeddedApp = &embedded;
    services.runtime = &host.Subsystems();
    services.linkedPlugins = runtime::LinkedPluginList();
    services.pluginTools = &pluginTools;
    {
        app::EditorSession session(DefaultAllocator(), Move(services));
        REQUIRE(session.Open(dir, app::EditorSession::OpenOptions{}));
        REQUIRE(context.Plugins() != nullptr);
        CHECK(context.Plugins()->IsLoaded(kNativeSample));
        CHECK(SpinContributed());                                    // Engine
        CHECK(builders.Find(&nativesample::NoteAsset::StaticType()) != nullptr); // Pipeline
        CHECK(HasCreator(context.Creators(), nativesample::kNoteCreator));
        CHECK(server.HasTool(u8"nativesample_notes"));               // Tooling
        CHECK(context.Actions().Find(u8"nativesample.notes") != nullptr); // Editor
        session.Close();
        CHECK(context.Plugins() == nullptr);
        CHECK_FALSE(SpinContributed());
        CHECK(builders.Find(&nativesample::NoteAsset::StaticType()) == nullptr);
        CHECK_FALSE(HasCreator(context.Creators(), nativesample::kNoteCreator));
        CHECK_FALSE(server.HasTool(u8"nativesample_notes"));
        CHECK(context.Actions().Find(u8"nativesample.notes") == nullptr);
    }
    pluginTools.DetachServer(server);
    context.Tasks().Finish();
    embedded.OnShutdown(host);
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("plugins: the cook loads the package's Engine and Pipeline parts, its builder cooking and nothing else arriving")
{
    const StringView dir = u8"scratch_plugins_cook";
    MakeProject(dir, u8"Cook Notes");
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(project);
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    {
        editor::ToolPlugins plugins(DefaultAllocator(), runtime::HostRole::Cook, *project, runtime::LinkedPluginList(),
                                    pipeline::PipelineRegistries{&builders});
        CHECK(plugins.Host().IsLoaded(kNativeSample));
        CHECK(builders.Find(&nativesample::NoteAsset::StaticType()) != nullptr);
        CHECK_FALSE(SpinContributed()); // a cook offers no engine service, so no scene manager
    }
    CHECK(builders.Find(&nativesample::NoteAsset::StaticType()) == nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("plugins: through the standalone MCP host, an agent creates and cooks the game's notes and reads them with its tool")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::McpPluginServices pluginTools{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    pluginTools.AttachServer(server, session);
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, StringView{},
                                          editor::mcp::ProjectPlugins{runtime::LinkedPluginList(),
                                                                      pipeline::PipelineRegistries{&builders, &importers, &creators},
                                                                      &pluginTools});
    const StringView dir = u8"scratch_plugins_mcp";
    const StringView plain = u8"scratch_plugins_mcp_plain";
    MakeProject(dir, u8"Mcp Notes");
    (void)RemoveDirectoryRecursive(plain);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), plain, u8"Plain").IsOk());
    const auto open = [&server](StringView project)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"directory", JsonValue::MakeString(String(project)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(args)).error);
    };

    open(dir);
    REQUIRE(session.plugins != nullptr);
    CHECK(session.plugins->IsLoaded(kNativeSample));
    JsonValue create = JsonValue::MakeObject();
    create.Set(u8"creator", JsonValue::MakeString(String(nativesample::kNoteCreator)));
    create.Set(u8"name", JsonValue::MakeString(String(u8"First")));
    const Called made = Call(server, u8"asset_create", Move(create));
    REQUIRE_FALSE(made.error);
    CHECK(made.value.Get(u8"type").AsString() == String(u8"NoteAsset"));
    const Called cooked = Call(server, u8"asset_cook", JsonValue::MakeObject());
    REQUIRE_FALSE(cooked.error);
    CHECK(cooked.value.Get(u8"failed").AsNumber() == doctest::Approx(0.0));
    Guid id;
    REQUIRE(Guid::TryParse(made.value.Get(u8"guid").AsString().AsView(), id));
    RefPtr<ISerializable> read = session.project->CookedDb().ReadObject(id);
    auto* product = read ? Cast<nativesample::NoteProduct>(read.Get()) : nullptr;
    REQUIRE(product != nullptr);
    CHECK(product->text == String(u8"A note from the game's own code"));

    const Called notes = Call(server, u8"nativesample_notes", JsonValue::MakeObject());
    REQUIRE_FALSE(notes.error);
    CHECK(notes.value.Get(u8"count").AsNumber() == doctest::Approx(1.0));
    CHECK(notes.value.Get(u8"notes").At(0).Get(u8"name").AsString() == String(u8"First"));
    CHECK(notes.value.Get(u8"notes").At(0).Get(u8"text").AsString() == String(u8"A note from the game's own code"));

    // A project that does not enable it: the plugin's tool and creator leave with it.
    open(plain);
    CHECK_FALSE(server.HasTool(u8"nativesample_notes"));
    CHECK_FALSE(HasCreator(creators, nativesample::kNoteCreator));

    session.plugins = nullptr;
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    pluginTools.DetachServer(server);
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Plugins: the reference game's own plugin package (Data/SampleProjects/NativeSample)
// in every host. Its four parts are built in this tree and linked here as a host links its
// plugins; a project that enables it loads, in each host, the parts that host's role takes, and
// each part's contribution arrives with it and leaves with it:
//   - Engine: the spin component's reflection, and the spin manager, in every scene (where the host
//     offers the engine's service), turning its entities;
//   - Pipeline: the note asset type, its builder and its creator, in the host's registries;
//   - Tooling: the nativesample_notes MCP tool, in every MCP server the host serves;
//   - Editor: the Project > Native Sample Notes action, in the editor.
// The sample project itself is checked too: its content reads, its scene's spinning entities are the
// Engine part's, and it cooks.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.scene.resource;
import foundation.shell;
import foundation.vfs;
import foundation.graphics;
import foundation.settings;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
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

TEST_CASE("plugins: in the player, the Engine part reflects the spin component and its manager turns the scene's entities")
{
    StubHost host;
    engine::runtime::DefaultApplication embedded;
    embedded.Configure(host);
    runtime::PluginHost* plugins = embedded.CreatePluginHost(runtime::HostRole::Player);
    REQUIRE(plugins != nullptr);
    engine::project::ProjectSettings settings;
    runtime::EnabledPlugin entry;
    entry.id = String(kNativeSample);
    settings.plugins.PushBack(Move(entry));
    const runtime::PluginLoadReport report =
        engine::project::LoadProjectPlugins(*plugins, runtime::LinkedPluginList(), settings);
    REQUIRE(report.loaded.Size() == 1u);

    // The reflection the inspector and the component tools read.
    const TypeInfo& type = TypeOf<nativesample::SpinComponent>();
    CHECK(GlobalTypeRegistry().FindById(type.id) != nullptr);
    CHECK(TypeAttrString(type, "displayName", {}) == StringView(u8"Spin"));
    CHECK(FindProperty(type, "turnsPerSecond") != nullptr);
    CHECK(FindProperty(type, "axis") != nullptr);

    {
        foundation::scene::Scene scene(DefaultAllocator());
        foundation::scene::SceneModuleContributions::Global().InstallAll(scene);
        nativesample::SpinManager* spins = scene.GetSystem<nativesample::SpinManager>();
        REQUIRE(spins != nullptr);
        const foundation::scene::EntityHandle upright = scene.CreateEntity(u8"Upright");
        const foundation::scene::EntityHandle sideways = scene.CreateEntity(u8"Sideways");
        const foundation::scene::EntityHandle still = scene.CreateEntity(u8"Still");
        spins->Add(upright).turnsPerSecond = 0.25f; // a quarter turn a second about its up axis
        nativesample::SpinComponent& side = spins->Add(sideways);
        side.turnsPerSecond = 0.5f;
        side.axis = Float3{1.0f, 0.0f, 0.0f};
        spins->Add(still);
        scene.SetActive(still, false);

        // A second in ten steps: the upright entity turned a quarter about Y, its forward (-Z) now
        // pointing along -X; the sideways one turned half about X, its up pointing down.
        for (int i = 0; i < 10; ++i)
        {
            scene.Update(0.1f);
        }
        const Float3 forward = RotateVector(scene.GetLocalTransform(upright).rotation, Float3{0.0f, 0.0f, -1.0f});
        CHECK(forward.x == doctest::Approx(-1.0f).epsilon(1e-3));
        CHECK(forward.z == doctest::Approx(0.0f).epsilon(1e-3));
        const Float3 up = RotateVector(scene.GetLocalTransform(sideways).rotation, Float3{0.0f, 1.0f, 0.0f});
        CHECK(up.y == doctest::Approx(-1.0f).epsilon(1e-3));
        const Quaternion rest = scene.GetLocalTransform(still).rotation;
        CHECK(rest.w == doctest::Approx(1.0f)); // inactive: not turned

        // A scene that does not simulate (the editor's edit mode) does not turn them.
        scene.SetSimulationEnabled(false);
        const Quaternion before = scene.GetLocalTransform(upright).rotation;
        scene.Update(0.5f);
        CHECK(scene.GetLocalTransform(upright).rotation.y == doctest::Approx(before.y));
    }

    embedded.DestroyPluginHost();
    CHECK(GlobalTypeRegistry().FindById(type.id) == nullptr); // taken out with the plugin
    embedded.OnShutdown(host);
}

TEST_CASE("plugins: the cook loads the package's Engine and Pipeline parts, its builder and scene manager arriving and leaving with it")
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
        CHECK(SpinContributed()); // a tool offers the engine's service, so its scenes get the manager
    }
    CHECK(builders.Find(&nativesample::NoteAsset::StaticType()) == nullptr);
    CHECK_FALSE(SpinContributed());
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

    // The scene tools know the Engine part's component: the schema has it, and a scene holding one
    // validates without a warning.
    const auto spinSchema = [&server]()
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"type", JsonValue::MakeString(u8"nativesample.Spin"));
        return Call(server, u8"component_schema", Move(args));
    };
    const Called schema = spinSchema();
    REQUIRE_FALSE(schema.error);
    CHECK(schema.value.Get(u8"wireName").AsString() == String(u8"nativesample.Spin"));
    const JsonValue version = schema.value.Get(u8"dataVersions").At(0);
    const String sceneXml = Format(
        u8"<root><u32 name=\"magic\">3586350318</u32><u32 name=\"version\">3</u32><string name=\"name\">Spin</string>"
        u8"<array name=\"entities\" count=\"1\"><string name=\"id\">6d1c1a52-3a51-4f0e-9a1b-2a6c7d8e9f01</string>"
        u8"<string name=\"name\">Spinner</string><u8 name=\"active\">1</u8>"
        u8"<string name=\"parent\">00000000-0000-0000-0000-000000000000</string>"
        u8"<object name=\"position\"><f32 name=\"x\">0</f32><f32 name=\"y\">0</f32><f32 name=\"z\">0</f32></object>"
        u8"<object name=\"rotation\"><f32 name=\"x\">0</f32><f32 name=\"y\">0</f32><f32 name=\"z\">0</f32><f32 name=\"w\">1</f32></object>"
        u8"<object name=\"scale\"><f32 name=\"x\">1</f32><f32 name=\"y\">1</f32><f32 name=\"z\">1</f32></object></array>"
        u8"<array name=\"components\" count=\"1\"><object><string name=\"owner\">6d1c1a52-3a51-4f0e-9a1b-2a6c7d8e9f01</string>"
        u8"<string name=\"type\">nativesample.Spin</string><object name=\"data\"><array name=\"dataVersions\" count=\"1\">"
        u8"<u64 name=\"type\">{}</u64><u32 name=\"version\">{}</u32></array><f32 name=\"turnsPerSecond\">0.5</f32>"
        u8"<object name=\"axis\"><f32 name=\"x\">0</f32><f32 name=\"y\">1</f32><f32 name=\"z\">0</f32></object></object></object></array>"
        u8"<array name=\"systemSettings\" count=\"0\"/><u8 name=\"prefabMode\">4</u8><array name=\"prefabInstances\" count=\"0\"/></root>",
        version.Get(u8"type").AsString().AsView(), static_cast<u32>(version.Get(u8"version").AsNumber()));
    JsonValue validateArgs = JsonValue::MakeObject();
    validateArgs.Set(u8"xml", JsonValue::MakeString(sceneXml));
    const Called validated = Call(server, u8"scene_validate", Move(validateArgs));
    REQUIRE_FALSE(validated.error);
    CHECK(validated.value.Get(u8"valid").AsBool());
    CHECK(validated.value.Get(u8"warnings").Count() == 0u);

    // A package extracted into the project's Plugins folder is available, not enabled: plugin_list
    // offers it, and once plugin_enable names it, it is an entry and no longer offered. The linked
    // plugin the project enables is not offered.
    {
        runtime::PluginManifest lantern;
        lantern.id = String(u8"lantern");
        lantern.name = String(u8"Lantern");
        lantern.version = String(u8"0.3.0");
        lantern.engineVersion = String(engine::project::kEngineVersionString);
        lantern.parts.PushBack(runtime::PluginManifestPart{String(u8"Engine")});
        lantern.platforms.PushBack(runtime::PluginPlatform{String(GetHostPlatformName())});
        const String packageDir = PathJoin(dir, u8"Plugins/Lantern");
        REQUIRE(CreateDirectories(packageDir.AsView()));
        MemoryStream xml;
        REQUIRE(runtime::WritePluginManifest(lantern, xml).IsOk());
        REQUIRE(WriteFile(PathJoin(packageDir.AsView(), u8"Plugin.xml").AsView(), xml.Bytes()).IsOk());
    }
    const auto offered = [&server](StringView id)
    {
        const Called list = Call(server, u8"plugin_list", JsonValue::MakeObject());
        REQUIRE_FALSE(list.error);
        const JsonValue available = list.value.Get(u8"available");
        for (i64 i = 0; i < available.Count(); ++i)
        {
            if (available.At(i).Get(u8"id").AsString() == String(id))
            {
                return available.At(i).Get(u8"offered").AsBool();
            }
        }
        return false;
    };
    CHECK(offered(u8"lantern"));
    CHECK_FALSE(offered(kNativeSample)); // enabled already
    {
        JsonValue enable = JsonValue::MakeObject();
        enable.Set(u8"id", JsonValue::MakeString(u8"lantern"));
        enable.Set(u8"path", JsonValue::MakeString(u8"Plugins/Lantern"));
        REQUIRE_FALSE(Call(server, u8"plugin_enable", Move(enable)).error);
    }
    CHECK_FALSE(offered(u8"lantern"));

    // A project that does not enable it: the plugin's tool, creator and component leave with it, and
    // the linked plugin is offered there.
    open(plain);
    CHECK_FALSE(server.HasTool(u8"nativesample_notes"));
    CHECK_FALSE(HasCreator(creators, nativesample::kNoteCreator));
    CHECK(spinSchema().error);
    CHECK(offered(kNativeSample));

    session.plugins = nullptr;
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    pluginTools.DetachServer(server);
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
}

TEST_CASE("plugins: the exporter loads the package's Tooling part, whose export hook writes into the dist and leaves with it")
{
    const StringView dir = u8"scratch_plugins_export";
    const StringView dist = u8"scratch_plugins_export_dist";
    MakeProject(dir, u8"Export Notes");
    (void)RemoveDirectoryRecursive(dist);
    REQUIRE(CreateDirectories(dist));
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(project);
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    editor::ExportPluginServices hooks(DefaultAllocator());
    {
        editor::ToolPlugins plugins(DefaultAllocator(), runtime::HostRole::Export, *project, runtime::LinkedPluginList(),
                                    pipeline::PipelineRegistries{&builders},
                                    [&hooks](runtime::PluginHost& host) { (void)hooks.Attach(host); });
        CHECK(plugins.Host().IsLoaded(kNativeSample));
        REQUIRE(hooks.Count() == 1u);
        editor::ExportPreset preset;
        preset.name = String(u8"Host Build");
        CHECK(hooks.Run(editor::ExportHookContext{project.Get(), &preset, dist}).IsEmpty());
        Result<Array<byte>> written = ReadFile(PathJoin(dist, u8"nativesample-notes.txt").AsView());
        REQUIRE(written.HasValue());
        CHECK(StringView(reinterpret_cast<const utf8char*>(written.Value().Data()), written.Value().Size()) == StringView(u8"0\n"));
    }
    CHECK(hooks.Count() == 0u); // gone with its plugin
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(dist);
}

// The sample project in Data/SampleProjects reads and cooks with its package: its default scene's
// spin records resolve to the Engine part's manager (none left unresolved), and the MCP host cooks
// the primitives and the note with the Pipeline part's builder. A scratch copy of its project
// settings and content keeps the tracked tree untouched.
TEST_CASE("plugins: the NativeSample project reads with its package, its scene's entities spin, and it cooks")
{
    pipeline::RegisterPipelineTypes();
    engine::RegisterAllSceneComponentReflection();
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String source = PathJoin(dataRoot.AsView(), u8"SampleProjects/NativeSample");
    const StringView dir = u8"scratch_plugins_sample";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(CreateDirectory(dir));
    REQUIRE(CopyDirectoryRecursive(PathJoin(source.AsView(), u8"Content").AsView(), PathJoin(dir, u8"Content").AsView()));
    Result<Array<byte>> settingsFile = ReadFile(PathJoin(source.AsView(), u8"Project.xml").AsView());
    REQUIRE(settingsFile.HasValue());
    REQUIRE(WriteFile(PathJoin(dir, u8"Project.xml").AsView(),
                      Span<const byte>(settingsFile.Value().Data(), settingsFile.Value().Size()))
                .IsOk());
    {
        // The package is linked into this suite, so the copy's entry names no package path.
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        REQUIRE(project->Settings().plugins.Size() == 1u);
        project->Settings().plugins[0].path = String();
        REQUIRE(project->SaveSettings().IsOk());
    }

    {
        StubHost host;
        engine::runtime::DefaultApplication embedded;
        embedded.Configure(host);
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        runtime::PluginHost* plugins = embedded.CreatePluginHost(runtime::HostRole::Player);
        REQUIRE(plugins != nullptr);
        REQUIRE(engine::project::LoadProjectPlugins(*plugins, runtime::LinkedPluginList(), project->Settings())
                    .loaded.Size() == 1u);
        foundation::content::Instance* level = project->SourceDb().GetInstance(project->Settings().defaultSceneId);
        REQUIRE(level != nullptr);
        CHECK(level->Name() == StringView(u8"Spinners"));
        {
            foundation::scene::Scene world(DefaultAllocator(), u8"Spinners");
            engine::AddAllSceneManagers(world);
            REQUIRE(foundation::scene::LoadScene(*level, world).IsOk());
            CHECK(world.UnresolvedComponents().IsEmpty());
            nativesample::SpinManager* spins = world.GetSystem<nativesample::SpinManager>();
            REQUIRE(spins != nullptr);
            CHECK(spins->Count() == 10u); // the row of five, the carousel and its four riders
        }
        project.Reset();
        embedded.DestroyPluginHost();
        embedded.OnShutdown(host);
    }

    {
        pipeline::BuilderRegistry builders{DefaultAllocator()};
        pipeline::RegisterAllBuilders(builders, dataRoot.AsView());
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
        JsonValue open = JsonValue::MakeObject();
        open.Set(u8"directory", JsonValue::MakeString(String(dir)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(open)).error);
        JsonValue force = JsonValue::MakeObject();
        force.Set(u8"force", JsonValue::MakeBool(true));
        const Called cooked = Call(server, u8"asset_cook", Move(force));
        REQUIRE_FALSE(cooked.error);
        CHECK(cooked.value.Get(u8"cooked").AsNumber() >= 3.0); // the Cube, the Plane and the note
        CHECK(cooked.value.Get(u8"failed").AsNumber() == doctest::Approx(0.0));
        const Called notes = Call(server, u8"nativesample_notes", JsonValue::MakeObject());
        REQUIRE_FALSE(notes.error);
        CHECK(notes.value.Get(u8"count").AsNumber() == doctest::Approx(1.0));
        CHECK(notes.value.Get(u8"notes").At(0).Get(u8"name").AsString() == String(u8"Welcome"));
        session.plugins = nullptr;
        owner.plugins.Reset();
        session.project = nullptr;
        owner.project.Reset();
        pluginTools.DetachServer(server);
    }
    (void)RemoveDirectoryRecursive(dir);
}

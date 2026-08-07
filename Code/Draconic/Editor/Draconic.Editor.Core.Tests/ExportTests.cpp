// End-to-end export pipeline: author a project programmatically (scene + a cooked mesh asset
// + a resource ref between them + a game script), ExportProject it, then consume the dist the
// way Draconic.Engine.Player does - ONE binary ContentDatabase over the pak for products AND scenes, the
// script as a raw pak entry, the manifest via the lean project helpers. Everything under the
// versioned-payload formats.
#include <atomic> // gcc modules: pull in std::atomic bodies before this import mix
#include <doctest/doctest.h>
#include <filesystem>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.vfs.pak;
import draconic.content;
import draconic.resource;
import draconic.engine.project;
import draconic.geometry;
import draconic.geometry.editor;
import draconic.geometry.resource;
import draconic.engine.render;
import draconic.scene;
import draconic.scene.resource;
import draconic.scene.editor;
import draconic.editor;
import draconic.editor.core;
import draconic.settings;
import draconic.shaders; // CookedShaderPack (the web export test reads the staged pack)
import draconic.script.wren;        // the Wren backend (the cook compile-checks against it)
import draconic.script.wren.editor; // RegisterWrenScriptCook
import draconic.script.editor;      // ScriptClassAsset + ScriptClassAssetBuilder
import draconic.script.resource;    // RegisterScriptResource + ScriptClass + ScriptClassFactory

using namespace draconic::foundation;
namespace editor = draconic::editor;
namespace project = draconic::project;
namespace scene = draconic::scene;
namespace geometry = draconic::geometry;

namespace
{
    void NukeTree(StringView root)
    {
        std::filesystem::remove_all(
            std::filesystem::path(reinterpret_cast<const char*>(String(root).CStr())));
    }
}

TEST_CASE("export: a startup script asset cooks into the dist pak and binds like the player")
{
    namespace script = draconic::script;
    script::wren::RegisterWrenScriptBackend();
    script::RegisterWrenScriptCook();
    script::RegisterScriptResource();
    GlobalTypeRegistry().Register(script::ScriptClassAsset::StaticType());
    RegisterSerializable<script::ScriptClassAsset>();

    const StringView projectDir = u8"draconic_export_script_project";
    const StringView distDir = u8"draconic_export_script_dist";
    NukeTree(projectDir);
    NukeTree(distDir);

    Guid scriptId;
    {
        REQUIRE(editor::EditorProject::Create(projectDir, u8"S").IsOk());
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir);
        REQUIRE(static_cast<bool>(project));

        // The game script SOURCE in Sources/ (what New-Asset writes).
        String srcPath(project->SourcesRoot());
        srcPath.Append(u8"/game.wren");
        const StringView src = u8"class Game {\n  construct new() {}\n  launch() {}\n  update(dt) "
                               u8"{}\n  exit() {}\n}\n";
        REQUIRE(WriteFile(srcPath.AsView(),
                          Span<const byte>(reinterpret_cast<const byte*>(src.Data()), src.Size()))
                    .IsOk());

        // The ScriptClassAsset instance recording file + language (the picker's target).
        draconic::content::Instance* scriptAsset = project->SourceDb().RootGroup()->CreateInstance(
            u8"NetGame", script::ScriptClassAsset::StaticType());
        REQUIRE(scriptAsset != nullptr);
        script::ScriptClassAsset asset;
        asset.fileName = draconic::vfs::SourcePath(u8"game.wren");
        asset.language = String(u8"wren");
        REQUIRE(scriptAsset->WriteObject(asset).IsOk());
        scriptId = scriptAsset->Id();

        project->Settings().startupScriptId = scriptId;
        REQUIRE(project->SaveSettings().IsOk());
    }

    // Export (cooks the reachable closure - here the startup script) with the script builder.
    editor::BuilderRegistry registry;
    registry.Register(UniquePtr<editor::IAssetBuilder>(
        DefaultAllocator().New<script::ScriptClassAssetBuilder>(), DefaultAllocator()));
    editor::ExportStats stats;
    {
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir);
        REQUIRE(
            editor::ExportProject(*project, distDir, registry, /*rebuild=*/false, &stats).IsOk());
        CHECK(stats.cooked >= 1u); // the script cooked
    }

    // Consume the dist exactly like the player: manifest guid -> Bind<ScriptClass> from the pak.
    draconic::vfs::NativeFileSystem distRoot(distDir);
    project::ProjectSettings manifest;
    REQUIRE(project::LoadProjectSettings(distRoot, manifest, project::kDistManifestFile).IsOk());
    CHECK(manifest.startupScriptId == scriptId);

    draconic::vfs::PakFileSystem pak(PathJoin(distDir, project::kDistContentPak).AsView());
    REQUIRE(pak.IsValid());
    draconic::content::ContentDatabase db(pak, BinarySerializerFactory(),
                                          project::kCookedAssetExtension);
    draconic::resource::ResourceManager resources(db);
    script::ScriptClassFactory scriptFactory;
    resources.AddFactory(&scriptFactory);

    draconic::resource::Proxy<script::ScriptClass> proxy =
        resources.Bind<script::ScriptClass>(manifest.startupScriptId);
    REQUIRE(static_cast<bool>(proxy));
    CHECK(proxy->className == u8"Game"); // the cook harvested the class
    CHECK(proxy->source.Size() > 0u);    // the source rode into the pak

    NukeTree(projectDir);
    NukeTree(distDir);
}

TEST_CASE("export: project -> dist pak -> player-style load-back (versioned formats)")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();
    geometry::RegisterMeshAssets();
    GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
    RegisterSerializable<geometry::StaticMeshSource>();

    const StringView projectDir = u8"draconic_export_e2e_project";
    const StringView distDir = u8"draconic_export_e2e_dist";
    NukeTree(projectDir);
    NukeTree(distDir);

    Guid meshId;
    Guid sceneId;
    const Guid scriptId = Guid{0xABCD1234ull, 0x5678EF90ull}; // stand-in startup-script asset id
    // --- author the project ---
    {
        REQUIRE(editor::EditorProject::Create(projectDir, u8"E2E").IsOk());
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir);
        REQUIRE(static_cast<bool>(project));

        // A cooked-pipeline asset: a cube mesh (what the primitive creators produce).
        draconic::content::Group* meshes = project->SourceDb().RootGroup()->CreateGroup(u8"Meshes");
        draconic::content::Instance* meshAsset =
            meshes->CreateInstance(u8"Cube", geometry::StaticMeshAsset::StaticType());
        REQUIRE(meshAsset != nullptr);
        geometry::StaticMeshAsset asset;
        geometry::MeshImporter::Import(*geometry::Primitives::Cube(2.0f), asset);
        REQUIRE(meshAsset->WriteObject(asset).IsOk());
        meshId = meshAsset->Id();

        // A scene whose entity references the mesh by guid.
        draconic::content::Group* scenes = project->SourceDb().RootGroup()->CreateGroup(u8"Scenes");
        draconic::content::Instance* sceneInstance =
            scenes->CreateInstance(u8"Main", scene::SceneDocument::StaticType());
        REQUIRE(sceneInstance != nullptr);
        scene::SceneDocument doc;
        doc.name = String(u8"Main");
        REQUIRE(sceneInstance->WriteObject(doc).IsOk());
        sceneId = sceneInstance->Id();
        {
            scene::Scene scene(u8"Main");
            scene.AddSystem<draconic::render::MeshComponentManager>();
            const scene::EntityHandle e = scene.CreateEntity(u8"Box");
            draconic::render::MeshComponent& mc =
                scene.GetSystem<draconic::render::MeshComponentManager>()->Add(e);
            mc.mesh.SetId(meshId);
            REQUIRE(scene::SaveScene(scene, *sceneInstance).IsOk());
        }

        // The startup script is a guid-authoritative ScriptClass asset now (bound from the content DB
        // by the player). Here we just verify the manifest carries the guid; a full script-asset-in-dist
        // export/bind test belongs with the script-cook fixtures.
        project->Settings().defaultSceneId = sceneId;
        project->Settings().defaultScene = String(u8"Scenes/Main");
        project->Settings().startupScriptId = scriptId;
        REQUIRE(project->SaveSettings().IsOk());

        // --- export ---
        editor::BuilderRegistry registry;
        registry.Register(UniquePtr<editor::IAssetBuilder>(
            DefaultAllocator().New<geometry::StaticMeshAssetBuilder>(), DefaultAllocator()));
        // Pre-transcode the scene stream like the editor/CLI do: the staged pak carries
        // the BINARY wire even though the source (SaveScene) is XML now.
        HashMap<Guid, Array<byte>> sceneStreams;
        {
            UniquePtr<IStream> src = sceneInstance->ReadData(u8"scene");
            REQUIRE(src.Get() != nullptr);
            scene::Scene scratch(u8"scratch");
            scratch.AddSystem<draconic::render::MeshComponentManager>();
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*src, scratch, /*includeSettings=*/true);
            REQUIRE(bytes.HasValue());
            REQUIRE(bytes.Value().Size() > 0);
            CHECK(bytes.Value()[0] != static_cast<byte>(u8'<')); // binary, not the XML source
            sceneStreams.InsertOrAssign(sceneId, Move(bytes.Value()));
        }
        editor::ExportStats stats;
        REQUIRE(editor::ExportProject(*project, distDir, registry, false, &stats, {}, &sceneStreams)
                    .IsOk());
        CHECK(stats.cooked == 1u); // the cube
        CHECK(stats.scenesStaged == 1u);
        CHECK(stats.filesPacked >= 2u); // product + scene envelope + scene stream
    }

    // --- consume the dist exactly like Draconic.Engine.Player's dist mode ---
    draconic::vfs::NativeFileSystem distRoot(distDir);
    project::ProjectSettings manifest;
    REQUIRE(project::LoadProjectSettings(distRoot, manifest, project::kDistManifestFile).IsOk());
    CHECK(manifest.defaultScene == u8"Scenes/Main");
    CHECK(manifest.defaultSceneId == sceneId); // dist manifest carries the guid too
    CHECK(manifest.startupScriptId ==
          scriptId); // the startup script rides as a guid (bound from the DB)

    draconic::vfs::PakFileSystem pak(PathJoin(distDir, project::kDistContentPak).AsView());
    REQUIRE(pak.IsValid());
    draconic::content::ContentDatabase db(pak, BinarySerializerFactory(),
                                          project::kCookedAssetExtension);

    // The scene loads from the pak under its ORIGINAL guid/path, and its mesh ref resolves
    // against the pak-hosted product through the CPU mesh factory.
    // Resolve by guid (the player's primary path), then confirm the path mirror agrees.
    draconic::content::Instance* sceneInstance = db.GetInstance(manifest.defaultSceneId);
    REQUIRE(sceneInstance != nullptr);
    CHECK(sceneInstance->Id() == sceneId);
    CHECK(db.GetInstance(manifest.defaultScene.AsView()) == sceneInstance);

    // The PACKED stream is the binary wire (the transcode map was honored).
    {
        UniquePtr<IStream> packed = sceneInstance->ReadData(u8"scene");
        REQUIRE(packed.Get() != nullptr);
        byte first = static_cast<byte>(0);
        REQUIRE(packed->Read(&first, 1) == 1u);
        CHECK(first != static_cast<byte>(u8'<'));
    }
    scene::Scene scene;
    auto* meshes = scene.AddSystem<draconic::render::MeshComponentManager>();
    REQUIRE(scene::LoadScene(*sceneInstance, scene).IsOk());
    draconic::resource::ResourceManager resources(db);
    geometry::StaticMeshFactory meshFactory;
    resources.AddFactory(&meshFactory);
    scene::ResolveSceneResources(scene, resources);

    draconic::render::MeshComponent* mc = nullptr;
    meshes->ForEach([&](draconic::render::MeshComponent& c, scene::EntityHandle) { mc = &c; });
    REQUIRE(mc != nullptr);
    CHECK(mc->mesh.id == meshId);
    geometry::StaticMesh* mesh = mc->mesh.Get();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->bounds.max.x == doctest::Approx(1.0f)); // the 2.0 cube

    NukeTree(projectDir);
    NukeTree(distDir);
}

TEST_CASE("export: preset set round-trips through export_presets.xml")
{
    const String dir = PathJoin(StringView(reinterpret_cast<const utf8char*>(
                                    std::filesystem::temp_directory_path().string().c_str())),
                                u8"draconic_presets_test");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // Absent file => NotFound, so callers know to fall back to defaults.
    {
        editor::ExportPresetSet loaded;
        CHECK_FALSE(editor::LoadExportPresets(root, loaded).IsOk());
    }

    // The built-in default names the host platform and leaves templateId blank (resolve by platform).
    editor::ExportPresetSet defaults;
    editor::DefaultExportPresets(defaults);
    REQUIRE(defaults.presets.Size() == 1u);
    CHECK(defaults.presets[0].platform == GetHostPlatformName());
    CHECK(defaults.presets[0].templateId.IsEmpty());

    // Author a two-preset set (one blank-template, one explicit-template with extra files) and save it.
    editor::ExportPresetSet out;
    editor::ExportPreset a;
    a.name = String(u8"Linux Desktop");
    a.platform = String(u8"Linux64");
    a.outputSubdir = String(u8"Linux64");
    editor::ExportPreset b;
    b.name = String(u8"Windows Desktop");
    b.platform = String(u8"Win64");
    b.config = String(u8"RelWithDebInfo");
    b.stageSymbols = true;
    b.pruneToReachable = true;
    b.templateId = String(u8"draconic-win64-0.1.0");
    b.playerName = String(u8"MyGame.exe");
    b.outputSubdir = String(u8"Win64");
    b.additionalFiles.PushBack(String(u8"icon.ico"));
    b.additionalFiles.PushBack(String(u8"config.xml"));
    out.presets.PushBack(Move(a));
    out.presets.PushBack(Move(b));
    REQUIRE(editor::SaveExportPresets(*root.AsWritable(), out).IsOk());

    // Load back and check every field survived, including the additionalFiles array + lookup.
    editor::ExportPresetSet loaded;
    REQUIRE(editor::LoadExportPresets(root, loaded).IsOk());
    REQUIRE(loaded.presets.Size() == 2u);
    CHECK(loaded.presets[0].name == u8"Linux Desktop");
    CHECK(loaded.presets[0].templateId.IsEmpty());
    CHECK(loaded.presets[0].playerName.IsEmpty());

    CHECK_FALSE(loaded.presets[0].pruneToReachable); // default (unset) stays false

    const editor::ExportPreset* win = loaded.Find(u8"Windows Desktop");
    REQUIRE(win != nullptr);
    CHECK(win->platform == u8"Win64");
    CHECK(win->config == u8"RelWithDebInfo"); // config axis round-trips
    CHECK(win->stageSymbols);                 // symbols opt-in round-trips
    CHECK(win->pruneToReachable);             // pruning opt-in round-trips
    CHECK(win->templateId == u8"draconic-win64-0.1.0");
    CHECK(win->playerName == u8"MyGame.exe");
    REQUIRE(win->additionalFiles.Size() == 2u);
    CHECK(win->additionalFiles[0] == u8"icon.ico");
    CHECK(win->additionalFiles[1] == u8"config.xml");
    CHECK(loaded.Find(u8"nope") == nullptr);

    NukeTree(dir.AsView());
}

namespace
{
    String TempDir(StringView leaf)
    {
        return PathJoin(StringView(reinterpret_cast<const utf8char*>(
                            std::filesystem::temp_directory_path().string().c_str())),
                        leaf);
    }
    void SaveText(draconic::vfs::NativeFileSystem& fs, StringView name, StringView text)
    {
        (void)fs.AsWritable()->Save(
            name, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()));
    }

    // A scene/prefab reference scanner for the pruning tests (mirrors the CLI's MakeSceneScanner but
    // with just the manager these tests use). Loads the instance, resolves its Refs through a
    // factory-less ResourceManager so every bound id lands in CollectUnresolved, and reads back the
    // parked prefab instances (the scene->prefab->asset chain).
    editor::SceneReferenceScanner MakePruningScanner()
    {
        return [](draconic::content::Instance& instance, draconic::content::ContentDatabase& db,
                  editor::SceneReferences& out)
        {
            scene::Scene scene;
            scene.AddSystem<draconic::render::MeshComponentManager>();
            if (!scene::LoadScene(instance, scene).IsOk())
            {
                return;
            }
            draconic::resource::ResourceManager collector(db);
            scene::ResolveSceneResources(scene, collector);
            collector.CollectUnresolved(out.resources);
            scene.ForEachPendingPrefabInstance([&out](scene::Scene::PendingPrefabInstance& pending)
                                               { out.prefabs.PushBack(pending.prefabId); });
        };
    }

    // Author a cube StaticMeshAsset under `group` and return its guid.
    Guid AuthorMesh(draconic::content::Group& group, StringView name)
    {
        draconic::content::Instance* inst =
            group.CreateInstance(name, geometry::StaticMeshAsset::StaticType());
        REQUIRE(inst != nullptr);
        geometry::StaticMeshAsset asset;
        geometry::MeshImporter::Import(*geometry::Primitives::Cube(2.0f), asset);
        REQUIRE(inst->WriteObject(asset).IsOk());
        return inst->Id();
    }

    // A minimal host export template in `toolDir` (fake player + no sidecars). Returns a preset
    // targeting it (blank templateId => resolve by host platform).
    void SetupHostTemplate(StringView toolDir, editor::TemplateRegistry& registry)
    {
        REQUIRE(CreateDirectory(toolDir));
        draconic::vfs::NativeFileSystem toolFs(toolDir);
        SaveText(toolFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");
        registry.Refresh(StringView{}, nullptr, toolDir, &toolFs);
    }
}

TEST_CASE("export: template.xml round-trips + host synthesis reads its runtime-libs")
{
    const String dir = TempDir(u8"draconic_template_test");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // template.xml round-trip, including the v2 (platform, config) axis: config + compiler + symbols[].
    editor::ExportTemplate t;
    t.id = String(u8"draconic-win64-release-0.1.0");
    t.name = String(u8"Windows Desktop Release 0.1.0");
    t.platform = String(u8"Win64");
    t.config = String(u8"Release");
    t.compiler = String(u8"MSVC");
    t.engineVersion = String(u8"0.1.0");
    t.playerBinary = String(u8"Draconic.Engine.Player.exe");
    t.sidecars.PushBack(String(u8"SDL3.dll"));
    t.sidecars.PushBack(String(u8"dxcompiler.dll"));
    t.symbols.PushBack(String(u8"Draconic.Engine.Player.pdb"));
    REQUIRE(editor::SaveTemplateManifest(*root.AsWritable(), t).IsOk());

    editor::ExportTemplate loaded;
    REQUIRE(editor::LoadTemplateManifest(root, loaded).IsOk());
    CHECK(loaded.id == u8"draconic-win64-release-0.1.0");
    CHECK(loaded.platform == u8"Win64");
    CHECK(loaded.config == u8"Release");
    CHECK(loaded.compiler == u8"MSVC");
    CHECK(loaded.playerBinary == u8"Draconic.Engine.Player.exe");
    REQUIRE(loaded.sidecars.Size() == 2u);
    CHECK(loaded.sidecars[0] == u8"SDL3.dll");
    CHECK(loaded.sidecars[1] == u8"dxcompiler.dll");
    REQUIRE(loaded.symbols.Size() == 1u);
    CHECK(loaded.symbols[0] == u8"Draconic.Engine.Player.pdb");

    // Host synthesis: id/platform/config/player from the host; sidecars from "<player>.runtime-libs".
    SaveText(root, u8"Draconic.Engine.Player.runtime-libs", u8"SDL3.dll\r\n\n  dxil.dll  \n");
    editor::ExportTemplate host;
    editor::SynthesizeHostTemplate(dir.AsView(), &root, host);
    CHECK(host.isHost);
    CHECK(host.platform == GetHostPlatformName());
    CHECK(host.config == GetBuildConfigName()); // the config that built this test binary
    // Host id carries the config: "host-<platform>-<config>".
    String expectedId(u8"host-");
    expectedId += GetHostPlatformName();
    expectedId += u8"-";
    expectedId += GetBuildConfigName();
    CHECK(host.id == expectedId.AsView());
    CHECK(host.playerBinary == GetExecutableName(u8"Draconic.Engine.Player"));
    CHECK(host.directory == dir);
    REQUIRE(host.sidecars.Size() == 2u); // blank line skipped, CR + spaces trimmed
    CHECK(host.sidecars[0] == u8"SDL3.dll");
    CHECK(host.sidecars[1] == u8"dxil.dll");

    NukeTree(dir.AsView());
}

TEST_CASE("export: a v1 template.xml without a config field reads as Release (back-compat)")
{
    const String dir = TempDir(u8"draconic_template_v1_backcompat");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // A hand-written v1 manifest: the OLD schema (dataVersion 1, no config/compiler/symbols). This is
    // exactly what a pre-config-axis editor wrote; it must still load, defaulting config -> Release.
    const StringView v1 = u8"<root>"
                          u8"<array name=\"dataVersions\" count=\"1\">"
                          u8"<u64 name=\"type\">0</u64><u32 name=\"version\">1</u32>"
                          u8"</array>"
                          u8"<string name=\"id\">draconic-legacy</string>"
                          u8"<string name=\"name\">Legacy</string>"
                          u8"<string name=\"platform\">Win64</string>"
                          u8"<string name=\"engineVersion\">0.1.0</string>"
                          u8"<string name=\"playerBinary\">Draconic.Engine.Player.exe</string>"
                          u8"<array name=\"sidecars\" count=\"1\"><string>SDL3.dll</string></array>"
                          u8"<string name=\"notes\"></string>"
                          u8"</root>";
    SaveText(root, u8"template.xml", v1);

    editor::ExportTemplate loaded;
    REQUIRE(editor::LoadTemplateManifest(root, loaded).IsOk());
    CHECK(loaded.id == u8"draconic-legacy");
    CHECK(loaded.platform == u8"Win64");
    CHECK(loaded.config == u8"Release"); // absent config normalizes to Release
    CHECK(loaded.EffectiveConfig() == u8"Release");
    CHECK(loaded.compiler.IsEmpty());
    REQUIRE(loaded.sidecars.Size() == 1u); // the old required sidecars still map through
    CHECK(loaded.sidecars[0] == u8"SDL3.dll");
    CHECK(loaded.symbols.IsEmpty());

    NukeTree(dir.AsView());
}

TEST_CASE("export: template registry resolves by id, by platform, and host-falls-back")
{
    const String rootDir = TempDir(u8"draconic_templates_root");
    const String hostDir = TempDir(u8"draconic_host_tooldir");
    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(hostDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"foreign-template").AsView()));

    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    draconic::vfs::NativeFileSystem hostFs(hostDir.AsView());

    // One imported template for a NON-host platform, so the host-fallback check below is valid on
    // every host: if the import shared the host platform it would out-rank the synthesized host
    // (that branch has its own test). Pick whichever of Win64/Linux64 is not the current host.
    const StringView hostPlatform = GetHostPlatformName();
    const String foreignPlatform =
        (hostPlatform == StringView(u8"Win64")) ? String(u8"Linux64") : String(u8"Win64");

    editor::ExportTemplate foreign;
    foreign.id = String(u8"draconic-foreign-0.1.0");
    foreign.platform = foreignPlatform;
    foreign.playerBinary = String(u8"Draconic.Engine.Player");
    REQUIRE(editor::SaveTemplateManifest(*rootFs.AsWritable(), foreign,
                                         u8"foreign-template/template.xml")
                .IsOk());

    editor::TemplateRegistry reg;
    reg.Refresh(rootDir.AsView(), &rootFs, hostDir.AsView(), &hostFs);
    CHECK(reg.Count() == 2u); // imported foreign + synthesized host

    // Explicit id.
    const editor::ExportTemplate* byId = reg.FindById(u8"draconic-foreign-0.1.0");
    REQUIRE(byId != nullptr);
    CHECK(byId->directory == PathJoin(rootDir.AsView(), u8"foreign-template"));

    // By (platform, config): the imported (Release) template answers its own platform; the host
    // platform falls back to the synthesized host template (whatever config built this binary).
    CHECK(reg.FindBy(foreignPlatform.AsView(), u8"Release") == byId);
    CHECK(reg.FindBy(foreignPlatform.AsView(), u8"") == byId); // empty config => Release
    const editor::ExportTemplate* hostT = reg.FindBy(hostPlatform, GetBuildConfigName());
    REQUIRE(hostT != nullptr);
    CHECK(hostT->isHost);

    // Resolve a preset: blank-id-by-(platform,config), explicit id, and no-match => null.
    editor::ExportPreset p;
    p.platform = foreignPlatform; // blank config -> Release, which the imported foreign template is
    CHECK(reg.Resolve(p) == byId); // blank templateId -> by (platform, config)
    p.templateId = String(u8"draconic-foreign-0.1.0");
    CHECK(reg.Resolve(p) == byId); // explicit id
    editor::ExportPreset none;
    none.platform = String(u8"Nonexistent64");
    CHECK(reg.Resolve(none) == nullptr);

    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
}

TEST_CASE("export: an imported template out-ranks the synthesized host for the host platform")
{
    const String rootDir = TempDir(u8"draconic_templates_hostwin");
    const String hostDir = TempDir(u8"draconic_host_tooldir2");
    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(hostDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"host-template").AsView()));

    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    draconic::vfs::NativeFileSystem hostFs(hostDir.AsView());

    // An imported template for the SAME platform AND config as this host build - so the exact-match
    // pass returns both, and the imported (non-host) bundle must win the tiebreak.
    editor::ExportTemplate imported;
    imported.id = String(u8"draconic-host-import");
    imported.platform = String(GetHostPlatformName());
    imported.config = String(GetBuildConfigName());
    imported.playerBinary = String(u8"Draconic.Engine.Player");
    REQUIRE(
        editor::SaveTemplateManifest(*rootFs.AsWritable(), imported, u8"host-template/template.xml")
            .IsOk());

    editor::TemplateRegistry reg;
    reg.Refresh(rootDir.AsView(), &rootFs, hostDir.AsView(), &hostFs);
    CHECK(reg.Count() == 2u); // imported + synthesized host (same platform)

    // FindBy prefers the real imported bundle over the synthesized host template (platform-only
    // fallback: the imported template has no config => Release, and outranks the host).
    const editor::ExportTemplate* byPlatform =
        reg.FindBy(GetHostPlatformName(), GetBuildConfigName());
    REQUIRE(byPlatform != nullptr);
    CHECK_FALSE(byPlatform->isHost);
    CHECK(byPlatform->id == u8"draconic-host-import");

    // The synthesized host template is still present, reachable by its "host-<platform>-<config>" id.
    String hostId(u8"host-");
    hostId += GetHostPlatformName();
    hostId += u8"-";
    hostId += GetBuildConfigName();
    const editor::ExportTemplate* host = reg.FindById(hostId.AsView());
    REQUIRE(host != nullptr);
    CHECK(host->isHost);

    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
}

TEST_CASE("export: ExportOne stages the resolved template's player + sidecars alongside content")
{
    const String projectDir = TempDir(u8"draconic_exportone_proj");
    const String toolDir = TempDir(u8"draconic_exportone_tool");
    const String outRoot = TempDir(u8"draconic_exportone_out");
    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    // A minimal (asset-less) project - enough for the content pipeline; the driver test is about the
    // player/sidecar staging on top of it.
    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"ExportOneTest").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->SaveSettings().IsOk());

    // Fake host tool dir: a "player" + its runtime-libs listing one sidecar + the sidecar file.
    REQUIRE(CreateDirectory(toolDir.AsView()));
    draconic::vfs::NativeFileSystem toolFs(toolDir.AsView());
    SaveText(toolFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");
    SaveText(toolFs, u8"Draconic.Engine.Player.runtime-libs", u8"libfoo.so\n");
    SaveText(toolFs, u8"libfoo.so", u8"foo\n");

    editor::TemplateRegistry registry;
    registry.Refresh(StringView{}, nullptr, toolDir.AsView(), &toolFs); // host template only

    editor::ExportPreset preset;
    preset.name = String(u8"Host Build");
    preset.platform = String(GetHostPlatformName()); // -> the host template
    preset.outputSubdir = String(u8"host");

    editor::BuilderRegistry builders; // no assets -> no builders needed
    editor::ExportResult result;
    REQUIRE(
        editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false, &result)
            .IsOk());

    // The dist carries the player, the sidecar, and the content (Content.pak + player.xml).
    draconic::vfs::NativeFileSystem distFs(result.outputDir.AsView());
    CHECK(distFs.Exists(GetExecutableName(u8"Draconic.Engine.Player").AsView()));
    CHECK(distFs.Exists(u8"libfoo.so"));
    CHECK(distFs.Exists(u8"Content.pak"));
    CHECK(distFs.Exists(u8"player.xml"));
    CHECK(result.filesStaged == 3u); // player + shaders.dpak + one sidecar (no additionalFiles)
    // The host template is stamped with this build's engine version, so no soft-mismatch warning.
    CHECK(result.engineVersionWarning.IsEmpty());

    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE("export: a template built against a different engine version warns but still exports")
{
    const String projectDir = TempDir(u8"draconic_ev_proj");
    const String rootDir = TempDir(u8"draconic_ev_root");
    const String toolDir = TempDir(u8"draconic_ev_tool");
    const String outRoot = TempDir(u8"draconic_ev_out");
    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"EngineVersionTest").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->SaveSettings().IsOk());

    // An imported template for the host platform stamped with a DIFFERENT engine version.
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"old-template").AsView()));
    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    editor::ExportTemplate old;
    old.id = String(u8"draconic-old-engine");
    old.platform = String(GetHostPlatformName());
    old.engineVersion = String(u8"0.0.0-ancient");
    old.playerBinary = GetExecutableName(u8"Draconic.Engine.Player");
    REQUIRE(editor::SaveTemplateManifest(*rootFs.AsWritable(), old, u8"old-template/template.xml")
                .IsOk());
    // The player file the driver stages from the template dir.
    draconic::vfs::NativeFileSystem oldDirFs(PathJoin(rootDir.AsView(), u8"old-template").AsView());
    SaveText(oldDirFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");

    REQUIRE(CreateDirectory(toolDir.AsView()));
    draconic::vfs::NativeFileSystem toolFs(toolDir.AsView());
    editor::TemplateRegistry registry;
    registry.Refresh(rootDir.AsView(), &rootFs, toolDir.AsView(), &toolFs);

    editor::ExportPreset preset;
    preset.name = String(u8"Old Engine Build");
    preset.templateId = String(u8"draconic-old-engine"); // resolve to the mismatched template
    preset.outputSubdir = String(u8"old");

    editor::BuilderRegistry builders;
    editor::ExportResult result;
    // Export still SUCCEEDS (soft match) ...
    REQUIRE(
        editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false, &result)
            .IsOk());
    // ... but records the version mismatch for the caller to surface.
    CHECK_FALSE(result.engineVersionWarning.IsEmpty());

    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE("export: ImportTemplate installs a bundle the registry then resolves")
{
    const String src = TempDir(u8"draconic_tmpl_src");
    const String root = TempDir(u8"draconic_tmpl_root2");
    NukeTree(src.AsView());
    NukeTree(root.AsView());
    REQUIRE(CreateDirectory(src.AsView()));

    // A source bundle: template.xml + a fake player + a sidecar.
    draconic::vfs::NativeFileSystem srcFs(src.AsView());
    editor::ExportTemplate t;
    t.id = String(u8"draconic-win64-import");
    t.platform = String(u8"Win64");
    t.playerBinary = String(u8"Draconic.Engine.Player.exe");
    t.sidecars.PushBack(String(u8"SDL3.dll"));
    REQUIRE(editor::SaveTemplateManifest(*srcFs.AsWritable(), t).IsOk());
    SaveText(srcFs, u8"Draconic.Engine.Player.exe", u8"exe\n");
    SaveText(srcFs, u8"SDL3.dll", u8"dll\n");

    String importedId;
    REQUIRE(editor::ImportTemplate(src.AsView(), root.AsView(), &importedId).IsOk());
    CHECK(importedId == u8"draconic-win64-import");

    // The registry over the root now resolves it (alongside the synthesized host template).
    draconic::vfs::NativeFileSystem rootFs(root.AsView());
    draconic::vfs::NativeFileSystem toolFs(src.AsView()); // any dir for the host template
    editor::TemplateRegistry reg;
    reg.Refresh(root.AsView(), &rootFs, src.AsView(), &toolFs);
    const editor::ExportTemplate* found = reg.FindById(u8"draconic-win64-import");
    REQUIRE(found != nullptr);
    CHECK(found->platform == u8"Win64");
    REQUIRE(found->sidecars.Size() == 1u);
    CHECK(found->sidecars[0] == u8"SDL3.dll");
    CHECK(found->directory == PathJoin(root.AsView(), u8"draconic-win64-import"));

    // A source with no template.xml fails.
    const String empty = TempDir(u8"draconic_tmpl_empty");
    NukeTree(empty.AsView());
    REQUIRE(CreateDirectory(empty.AsView()));
    CHECK_FALSE(editor::ImportTemplate(empty.AsView(), root.AsView()).IsOk());

    NukeTree(src.AsView());
    NukeTree(root.AsView());
    NukeTree(empty.AsView());
}

TEST_CASE("export: CreateTemplate packages a Bin/<Config> dir and the registry then resolves it")
{
    const String base = TempDir(u8"draconic_createtmpl");
    const String root = TempDir(u8"draconic_createtmpl_root");
    NukeTree(base.AsView());
    NukeTree(root.AsView());

    // A fake build dir with the canonical layout "…/Bin/<Config>/<Platform>-<Compiler>", holding a
    // player + its runtime-libs manifest + the one sidecar it lists.
    String leaf(GetHostPlatformName());
    leaf += u8"-Clang";
    const String binDir = PathJoin(
        PathJoin(PathJoin(base.AsView(), u8"Bin").AsView(), u8"Release").AsView(), leaf.AsView());
    REQUIRE(CreateDirectories(binDir.AsView()));
    draconic::vfs::NativeFileSystem binFs(binDir.AsView());
    SaveText(binFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");
    SaveText(binFs, u8"Draconic.Engine.Player.runtime-libs", u8"libfoo.so\n");
    SaveText(binFs, u8"libfoo.so", u8"foo\n");

    // Install mode: writes into <root>/<id>. config/compiler come from the packaged dir path.
    String createdId, createdDir;
    REQUIRE(editor::CreateTemplate(binDir.AsView(), root.AsView(), editor::TemplateOutput::Install,
                                   &createdId, &createdDir)
                .IsOk());
    // id = draconic-<platform>-<config>-<engineVersion> (lowercased platform/config).
    String expectedId(u8"draconic-");
    expectedId += editor::AsciiLower(GetHostPlatformName());
    expectedId += u8"-release-";
    expectedId += draconic::project::kEngineVersionString;
    CHECK(createdId == expectedId.AsView());
    CHECK(createdDir == PathJoin(root.AsView(), createdId.AsView()));

    // The bundle exists on disk: manifest + player + the sidecar.
    draconic::vfs::NativeFileSystem bundleFs(createdDir.AsView());
    CHECK(bundleFs.Exists(u8"template.xml"));
    CHECK(bundleFs.Exists(GetExecutableName(u8"Draconic.Engine.Player").AsView()));
    CHECK(bundleFs.Exists(u8"libfoo.so"));

    // The manifest stamped config = Release (from the dir), compiler = Clang (from the leaf).
    editor::ExportTemplate manifest;
    REQUIRE(editor::LoadTemplateManifest(bundleFs, manifest).IsOk());
    CHECK(manifest.config == u8"Release");
    CHECK(manifest.compiler == u8"Clang");
    CHECK(manifest.platform == GetHostPlatformName());
    REQUIRE(manifest.sidecars.Size() == 1u);
    CHECK(manifest.sidecars[0] == u8"libfoo.so");

    // The registry over the root now finds the created template.
    draconic::vfs::NativeFileSystem rootFs(root.AsView());
    draconic::vfs::NativeFileSystem toolFs(base.AsView()); // any dir for the host template
    editor::TemplateRegistry reg;
    reg.Refresh(root.AsView(), &rootFs, base.AsView(), &toolFs);
    const editor::ExportTemplate* found = reg.FindById(createdId.AsView());
    REQUIRE(found != nullptr);
    CHECK_FALSE(found->isHost);
    CHECK(found->config == u8"Release");
    CHECK(reg.FindBy(GetHostPlatformName(), u8"Release") ==
          found); // resolves by (platform, config)

    NukeTree(base.AsView());
    NukeTree(root.AsView());
}

TEST_CASE("export: CreateTemplate --out mode writes a self-contained bundle to the folder")
{
    const String base = TempDir(u8"draconic_createtmpl_out");
    const String outFolder = TempDir(u8"draconic_createtmpl_bundle");
    NukeTree(base.AsView());
    NukeTree(outFolder.AsView());

    String leaf(GetHostPlatformName());
    leaf += u8"-GCC";
    const String binDir = PathJoin(
        PathJoin(PathJoin(base.AsView(), u8"Bin").AsView(), u8"Debug").AsView(), leaf.AsView());
    REQUIRE(CreateDirectories(binDir.AsView()));
    draconic::vfs::NativeFileSystem binFs(binDir.AsView());
    SaveText(binFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");
    // No runtime-libs manifest => no sidecars (an rpath-style build); the player alone still packages.

    String createdId, createdDir;
    REQUIRE(editor::CreateTemplate(binDir.AsView(), outFolder.AsView(),
                                   editor::TemplateOutput::ExportFolder, &createdId, &createdDir)
                .IsOk());
    // ExportFolder writes straight into the given folder (zip it to distribute).
    CHECK(createdDir == outFolder);
    draconic::vfs::NativeFileSystem bundleFs(outFolder.AsView());
    CHECK(bundleFs.Exists(u8"template.xml"));
    CHECK(bundleFs.Exists(GetExecutableName(u8"Draconic.Engine.Player").AsView()));

    editor::ExportTemplate manifest;
    REQUIRE(editor::LoadTemplateManifest(bundleFs, manifest).IsOk());
    CHECK(manifest.config == u8"Debug"); // from the Bin/Debug path
    CHECK(manifest.compiler == u8"GCC");

    // A missing player binary is a hard failure (nothing to package).
    const String emptyBin = PathJoin(base.AsView(), u8"empty");
    REQUIRE(CreateDirectories(emptyBin.AsView()));
    CHECK_FALSE(editor::CreateTemplate(emptyBin.AsView(), outFolder.AsView(),
                                       editor::TemplateOutput::ExportFolder)
                    .IsOk());

    NukeTree(base.AsView());
    NukeTree(outFolder.AsView());
}

TEST_CASE("export: FindBy resolves exact (platform,config) and falls back preferring Release")
{
    const String rootDir = TempDir(u8"draconic_findby_root");
    const String hostDir = TempDir(u8"draconic_findby_host");
    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(hostDir.AsView()));

    // Two imported templates for the SAME non-host platform: a Debug one and a Release one.
    const StringView hostPlatform = GetHostPlatformName();
    const String plat =
        (hostPlatform == StringView(u8"Win64")) ? String(u8"Linux64") : String(u8"Win64");

    const auto writeTemplate = [&](StringView id, StringView cfg, StringView subdir)
    {
        REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), subdir).AsView()));
        draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
        editor::ExportTemplate t;
        t.id = String(id);
        t.platform = plat;
        t.config = String(cfg);
        t.playerBinary = String(u8"Draconic.Engine.Player");
        const String manifestPath = PathJoin(subdir, u8"template.xml");
        REQUIRE(
            editor::SaveTemplateManifest(*rootFs.AsWritable(), t, manifestPath.AsView()).IsOk());
    };
    writeTemplate(u8"draconic-dbg", u8"Debug", u8"dbg");
    writeTemplate(u8"draconic-rel", u8"Release", u8"rel");

    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    draconic::vfs::NativeFileSystem hostFs(hostDir.AsView());
    editor::TemplateRegistry reg;
    reg.Refresh(rootDir.AsView(), &rootFs, hostDir.AsView(), &hostFs);

    // Exact match by config.
    const editor::ExportTemplate* dbg = reg.FindById(u8"draconic-dbg");
    const editor::ExportTemplate* rel = reg.FindById(u8"draconic-rel");
    REQUIRE(dbg != nullptr);
    REQUIRE(rel != nullptr);
    CHECK(reg.FindBy(plat.AsView(), u8"Debug") == dbg);
    CHECK(reg.FindBy(plat.AsView(), u8"Release") == rel);

    // No RelWithDebInfo template for this platform => platform-only fallback prefers the Release one.
    CHECK(reg.FindBy(plat.AsView(), u8"RelWithDebInfo") == rel);
    // Empty config defaults to Release (exact match).
    CHECK(reg.FindBy(plat.AsView(), u8"") == rel);

    // A preset selecting (platform, Debug) resolves to the Debug template.
    editor::ExportPreset p;
    p.platform = plat;
    p.config = String(u8"Debug");
    CHECK(reg.Resolve(p) == dbg);
    p.config = String(u8"Release");
    CHECK(reg.Resolve(p) == rel);
    p.config.Clear(); // blank => Release
    CHECK(reg.Resolve(p) == rel);

    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
}

TEST_CASE("export: ExportOne stages template symbols only when the preset opts in")
{
    const String projectDir = TempDir(u8"draconic_sym_proj");
    const String rootDir = TempDir(u8"draconic_sym_root");
    const String toolDir = TempDir(u8"draconic_sym_tool");
    const String outRoot = TempDir(u8"draconic_sym_out");
    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"SymbolsTest").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->SaveSettings().IsOk());

    // An imported template with a player, one required sidecar, and one SYMBOL file.
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"sym-template").AsView()));
    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    editor::ExportTemplate t;
    t.id = String(u8"draconic-sym");
    t.platform = String(GetHostPlatformName());
    t.playerBinary = GetExecutableName(u8"Draconic.Engine.Player");
    t.sidecars.PushBack(String(u8"libfoo.so"));
    t.symbols.PushBack(String(u8"Draconic.Engine.Player.debug"));
    REQUIRE(editor::SaveTemplateManifest(*rootFs.AsWritable(), t, u8"sym-template/template.xml")
                .IsOk());
    draconic::vfs::NativeFileSystem tmplDirFs(
        PathJoin(rootDir.AsView(), u8"sym-template").AsView());
    SaveText(tmplDirFs, GetExecutableName(u8"Draconic.Engine.Player").AsView(), u8"#!player\n");
    SaveText(tmplDirFs, u8"libfoo.so", u8"foo\n");
    SaveText(tmplDirFs, u8"Draconic.Engine.Player.debug", u8"dwarf\n");

    REQUIRE(CreateDirectory(toolDir.AsView()));
    draconic::vfs::NativeFileSystem toolFs(toolDir.AsView());
    editor::TemplateRegistry registry;
    registry.Refresh(rootDir.AsView(), &rootFs, toolDir.AsView(), &toolFs);

    editor::BuilderRegistry builders;

    // Default preset: symbols stripped from the dist (sidecar staged, symbol not).
    {
        editor::ExportPreset preset;
        preset.name = String(u8"Stripped");
        preset.templateId = String(u8"draconic-sym");
        preset.outputSubdir = String(u8"stripped");
        editor::ExportResult result;
        REQUIRE(editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false,
                                  &result)
                    .IsOk());
        draconic::vfs::NativeFileSystem distFs(result.outputDir.AsView());
        CHECK(distFs.Exists(GetExecutableName(u8"Draconic.Engine.Player").AsView()));
        CHECK(distFs.Exists(u8"libfoo.so"));                // required sidecar always staged
        CHECK_FALSE(distFs.Exists(u8"Draconic.Engine.Player.debug")); // symbols stripped by default
        CHECK(result.filesStaged == 3u);                    // player + shaders.dpak + sidecar
    }

    // Opt-in preset: symbols staged too.
    {
        editor::ExportPreset preset;
        preset.name = String(u8"WithSymbols");
        preset.templateId = String(u8"draconic-sym");
        preset.outputSubdir = String(u8"symbols");
        preset.stageSymbols = true;
        editor::ExportResult result;
        REQUIRE(editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false,
                                  &result)
                    .IsOk());
        draconic::vfs::NativeFileSystem distFs(result.outputDir.AsView());
        CHECK(distFs.Exists(u8"Draconic.Engine.Player.debug")); // opted in
        CHECK(result.filesStaged == 4u);              // player + shaders.dpak + sidecar + symbol
    }

    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE("export: a v1 export_presets.xml without config/stageSymbols reads as Release/false")
{
    const String dir = TempDir(u8"draconic_presets_v1");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // A hand-written v1 export_presets.xml (dataVersion 1, no config/stageSymbols on the preset).
    const StringView v1 = u8"<root>"
                          u8"<array name=\"dataVersions\" count=\"1\">"
                          u8"<u64 name=\"type\">0</u64><u32 name=\"version\">1</u32>"
                          u8"</array>"
                          u8"<array name=\"presets\" count=\"1\">"
                          u8"<object>"
                          u8"<string name=\"name\">Legacy</string>"
                          u8"<string name=\"platform\">Win64</string>"
                          u8"<string name=\"templateId\"></string>"
                          u8"<string name=\"playerName\"></string>"
                          u8"<string name=\"outputSubdir\">Win64</string>"
                          u8"<array name=\"additionalFiles\" count=\"0\"></array>"
                          u8"</object>"
                          u8"</array>"
                          u8"</root>";
    SaveText(root, u8"export_presets.xml", v1);

    editor::ExportPresetSet loaded;
    REQUIRE(editor::LoadExportPresets(root, loaded).IsOk());
    REQUIRE(loaded.presets.Size() == 1u);
    CHECK(loaded.presets[0].name == u8"Legacy");
    CHECK(loaded.presets[0].config.IsEmpty());       // absent => resolves as Release
    CHECK_FALSE(loaded.presets[0].stageSymbols);     // absent => stripped
    CHECK_FALSE(loaded.presets[0].pruneToReachable); // absent => pack everything (back-compat)

    NukeTree(dir.AsView());
}

TEST_CASE("export: ResolveTemplatesRoot prefers an explicit override")
{
    // An explicit override (the editor's EditorExportSettings::templatesRoot) wins verbatim.
    CHECK(editor::ResolveTemplatesRoot(u8"/shared/templates") == u8"/shared/templates");
    // An empty override falls through to env/default - a non-empty root, same as the no-arg form
    // the CLI uses (the two surfaces resolve identically when no setting is present).
    const String fallback = editor::ResolveTemplatesRoot(u8"");
    CHECK_FALSE(fallback.IsEmpty());
    CHECK(fallback == editor::ResolveTemplatesRoot());
}

TEST_CASE("export: EditorExportSettings round-trips through the editor settings store")
{
    namespace settings = draconic::settings;
    editor::RegisterEditorSettingsTypes(); // so Settings::Load can instantiate the section

    const String dir = TempDir(u8"draconic_editor_settings");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // First run: no file => NotFound, and the section is absent (reads as its defaults on access).
    {
        settings::Settings store;
        CHECK_FALSE(editor::LoadEditorSettings(root, store).IsOk());
        CHECK(store.Find<editor::EditorExportSettings>() == nullptr);
    }

    // Author a store with a custom templates root and persist it.
    {
        settings::Settings store;
        store.Section<editor::EditorExportSettings>().templatesRoot = String(u8"/shared/templates");
        REQUIRE(editor::SaveEditorSettings(*root.AsWritable(), store).IsOk());
    }

    // Load it back into a fresh store - the override survives the XML round-trip.
    {
        settings::Settings store;
        REQUIRE(editor::LoadEditorSettings(root, store).IsOk());
        const editor::EditorExportSettings* s = store.Find<editor::EditorExportSettings>();
        REQUIRE(s != nullptr);
        CHECK(s->templatesRoot == u8"/shared/templates");
    }

    NukeTree(dir.AsView());
}

TEST_CASE("export: pruned dist keeps the referenced closure, drops the rest, and reports both")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();
    geometry::RegisterMeshAssets();
    GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
    RegisterSerializable<geometry::StaticMeshSource>();

    const String projectDir = TempDir(u8"draconic_prune_proj");
    const String toolDir = TempDir(u8"draconic_prune_tool");
    const String outRoot = TempDir(u8"draconic_prune_out");
    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"Prune").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));

    // Two authored meshes; the scene references only the first.
    draconic::content::Group* meshes = project->SourceDb().RootGroup()->CreateGroup(u8"Meshes");
    const Guid meshRefId = AuthorMesh(*meshes, u8"Referenced");
    const Guid meshDeadId = AuthorMesh(*meshes, u8"Unreferenced");

    draconic::content::Group* scenes = project->SourceDb().RootGroup()->CreateGroup(u8"Scenes");
    draconic::content::Instance* sceneInst =
        scenes->CreateInstance(u8"Main", scene::SceneDocument::StaticType());
    REQUIRE(sceneInst != nullptr);
    {
        scene::SceneDocument doc;
        doc.name = String(u8"Main");
        REQUIRE(sceneInst->WriteObject(doc).IsOk());
    }
    const Guid sceneId = sceneInst->Id();
    {
        scene::Scene scene(u8"Main");
        scene.AddSystem<draconic::render::MeshComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Box");
        scene.GetSystem<draconic::render::MeshComponentManager>()->Add(e).mesh.SetId(meshRefId);
        REQUIRE(scene::SaveScene(scene, *sceneInst).IsOk());
    }
    project->Settings().defaultSceneId = sceneId;
    project->Settings().defaultScene = String(u8"Scenes/Main");
    REQUIRE(project->SaveSettings().IsOk());

    editor::BuilderRegistry builders;
    builders.Register(UniquePtr<editor::IAssetBuilder>(
        DefaultAllocator().New<geometry::StaticMeshAssetBuilder>(), DefaultAllocator()));
    editor::TemplateRegistry registry;
    SetupHostTemplate(toolDir.AsView(), registry);
    const editor::SceneReferenceScanner scanner = MakePruningScanner();

    // --- pruned export: only the reachable closure ships ---
    editor::ExportPreset preset;
    preset.name = String(u8"Pruned");
    preset.platform = String(GetHostPlatformName());
    preset.outputSubdir = String(u8"pruned");
    preset.pruneToReachable = true;
    editor::ExportResult result;
    REQUIRE(editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false,
                              &result, {}, true, nullptr, &scanner)
                .IsOk());

    draconic::vfs::PakFileSystem pak(
        PathJoin(result.outputDir.AsView(), project::kDistContentPak).AsView());
    REQUIRE(pak.IsValid());
    draconic::content::ContentDatabase db(pak, BinarySerializerFactory(),
                                          project::kCookedAssetExtension);
    CHECK(db.GetInstance(sceneId) != nullptr);    // scene staged
    CHECK(db.GetInstance(meshRefId) != nullptr);  // referenced mesh kept
    CHECK(db.GetInstance(meshDeadId) == nullptr); // unreferenced mesh pruned

    // The report is loud: pruned, one default-scene root, the unreferenced mesh named as dropped.
    CHECK(result.pruning.pruned);
    REQUIRE(result.pruning.roots.Size() == 1u);
    CHECK(result.pruning.roots[0].reason == editor::ExportRootReason::DefaultScene);
    CHECK(result.pruning.roots[0].id == sceneId);
    bool droppedDead = false;
    for (const String& d : result.pruning.dropped)
    {
        if (d == u8"Meshes/Unreferenced")
        {
            droppedDead = true;
        }
    }
    CHECK(droppedDead);
    draconic::vfs::NativeFileSystem distFs(result.outputDir.AsView());
    CHECK(distFs.Exists(u8"export-report.txt")); // report written beside the dist

    // --- non-pruned (default) export: EVERYTHING ships, no report (escape hatch, no regression) ---
    editor::ExportPreset full;
    full.name = String(u8"Full");
    full.platform = String(GetHostPlatformName());
    full.outputSubdir = String(u8"full"); // pruneToReachable defaults false
    editor::ExportResult fullResult;
    REQUIRE(editor::ExportOne(*project, full, registry, builders, outRoot.AsView(), false,
                              &fullResult, {}, true, nullptr, &scanner)
                .IsOk());
    draconic::vfs::PakFileSystem fullPak(
        PathJoin(fullResult.outputDir.AsView(), project::kDistContentPak).AsView());
    REQUIRE(fullPak.IsValid());
    draconic::content::ContentDatabase fullDb(fullPak, BinarySerializerFactory(),
                                              project::kCookedAssetExtension);
    CHECK(fullDb.GetInstance(meshRefId) != nullptr);
    CHECK(fullDb.GetInstance(meshDeadId) != nullptr); // unreferenced ships when not pruning
    CHECK_FALSE(fullResult.pruning.pruned);
    draconic::vfs::NativeFileSystem fullFs(fullResult.outputDir.AsView());
    CHECK_FALSE(fullFs.Exists(u8"export-report.txt"));

    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE(
    "export: precomputed reachable roots prune like an inline scanner (editor main-thread path)")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();
    geometry::RegisterMeshAssets();
    GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
    RegisterSerializable<geometry::StaticMeshSource>();

    const String projectDir = TempDir(u8"draconic_prune_pre_proj");
    const String toolDir = TempDir(u8"draconic_prune_pre_tool");
    const String outRoot = TempDir(u8"draconic_prune_pre_out");
    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"PrunePre").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));

    draconic::content::Group* meshes = project->SourceDb().RootGroup()->CreateGroup(u8"Meshes");
    const Guid meshRefId = AuthorMesh(*meshes, u8"Referenced");
    const Guid meshDeadId = AuthorMesh(*meshes, u8"Unreferenced");

    draconic::content::Group* scenes = project->SourceDb().RootGroup()->CreateGroup(u8"Scenes");
    draconic::content::Instance* sceneInst =
        scenes->CreateInstance(u8"Main", scene::SceneDocument::StaticType());
    REQUIRE(sceneInst != nullptr);
    {
        scene::SceneDocument doc;
        doc.name = String(u8"Main");
        REQUIRE(sceneInst->WriteObject(doc).IsOk());
    }
    const Guid sceneId = sceneInst->Id();
    {
        scene::Scene scene(u8"Main");
        scene.AddSystem<draconic::render::MeshComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Box");
        scene.GetSystem<draconic::render::MeshComponentManager>()->Add(e).mesh.SetId(meshRefId);
        REQUIRE(scene::SaveScene(scene, *sceneInst).IsOk());
    }
    project->Settings().defaultSceneId = sceneId;
    project->Settings().defaultScene = String(u8"Scenes/Main");
    REQUIRE(project->SaveSettings().IsOk());

    editor::BuilderRegistry builders;
    builders.Register(UniquePtr<editor::IAssetBuilder>(
        DefaultAllocator().New<geometry::StaticMeshAssetBuilder>(), DefaultAllocator()));
    editor::TemplateRegistry registry;
    SetupHostTemplate(toolDir.AsView(), registry);

    // MAIN-THREAD pre-scan: expand the reachable closure with a scanner (as the editor does before
    // submitting the background job), then export with scanner=null + the precomputed guid set - the
    // scene loading (unsafe off the main thread) has already happened.
    const editor::SceneReferenceScanner scanner = MakePruningScanner();
    const Array<editor::ExportRoot> seeds = editor::CollectExportRoots(*project);
    const Array<Guid> reachable = editor::ExpandReachableRoots(*project, seeds, scanner);
    CHECK(reachable.Size() >= 2u); // at least the scene + its referenced mesh

    editor::ExportPreset preset;
    preset.name = String(u8"Pruned");
    preset.platform = String(GetHostPlatformName());
    preset.outputSubdir = String(u8"pruned");
    preset.pruneToReachable = true;
    editor::ExportResult result;
    REQUIRE(editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false,
                              &result, {}, true, nullptr, /*scanner*/ nullptr, &reachable)
                .IsOk());

    draconic::vfs::PakFileSystem pak(
        PathJoin(result.outputDir.AsView(), project::kDistContentPak).AsView());
    REQUIRE(pak.IsValid());
    draconic::content::ContentDatabase db(pak, BinarySerializerFactory(),
                                          project::kCookedAssetExtension);
    CHECK(db.GetInstance(sceneId) != nullptr);
    CHECK(db.GetInstance(meshRefId) != nullptr);
    CHECK(db.GetInstance(meshDeadId) == nullptr); // pruned via the precomputed set, no live scanner
    CHECK(result.pruning.pruned);

    // Guard: pruning requested but NEITHER a scanner NOR a precomputed set -> safe fallback (pack
    // everything, no prune) rather than a silently-broken dist.
    editor::ExportPreset noHelp;
    noHelp.name = String(u8"NoHelp");
    noHelp.platform = String(GetHostPlatformName());
    noHelp.outputSubdir = String(u8"nohelp");
    noHelp.pruneToReachable = true;
    editor::ExportResult noHelpResult;
    REQUIRE(editor::ExportOne(*project, noHelp, registry, builders, outRoot.AsView(), false,
                              &noHelpResult)
                .IsOk());
    draconic::vfs::PakFileSystem noHelpPak(
        PathJoin(noHelpResult.outputDir.AsView(), project::kDistContentPak).AsView());
    REQUIRE(noHelpPak.IsValid());
    draconic::content::ContentDatabase noHelpDb(noHelpPak, BinarySerializerFactory(),
                                                project::kCookedAssetExtension);
    CHECK(noHelpDb.GetInstance(meshDeadId) != nullptr); // fell back to pack-everything
    CHECK_FALSE(noHelpResult.pruning.pruned);

    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE("export: pruning keeps a scene -> prefab -> asset chain")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();
    GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType());
    RegisterSerializable<scene::PrefabDocument>();
    geometry::RegisterMeshAssets();
    GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
    RegisterSerializable<geometry::StaticMeshSource>();

    const String projectDir = TempDir(u8"draconic_prunepf_proj");
    const String toolDir = TempDir(u8"draconic_prunepf_tool");
    const String outRoot = TempDir(u8"draconic_prunepf_out");
    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"PrunePrefab").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));

    draconic::content::Group* meshes = project->SourceDb().RootGroup()->CreateGroup(u8"Meshes");
    const Guid meshInPrefab = AuthorMesh(*meshes, u8"PrefabMesh");
    const Guid meshDead = AuthorMesh(*meshes, u8"Unreferenced");

    // The prefab body: an entity with a MeshComponent -> meshInPrefab, captured into the prefab
    // instance's "scene" stream. meshInPrefab is reachable ONLY through this prefab.
    draconic::content::Group* prefabsGroup =
        project->SourceDb().RootGroup()->CreateGroup(u8"Prefabs");
    draconic::content::Instance* prefabInst =
        prefabsGroup->CreateInstance(u8"Barrel", scene::PrefabDocument::StaticType());
    REQUIRE(prefabInst != nullptr);
    {
        scene::PrefabDocument doc;
        doc.name = String(u8"Barrel");
        REQUIRE(prefabInst->WriteObject(doc).IsOk());
    }
    const Guid prefabId = prefabInst->Id();
    {
        scene::Scene author(u8"Barrel");
        author.AddSystem<draconic::render::MeshComponentManager>();
        const scene::EntityHandle e = author.CreateEntity(u8"Body");
        author.GetSystem<draconic::render::MeshComponentManager>()->Add(e).mesh.SetId(meshInPrefab);
        MemoryStream payload;
        REQUIRE(scene::CapturePrefab(author, e, payload).IsOk());
        const Span<const byte> bytes = payload.Bytes();
        REQUIRE(prefabInst->WriteData(u8"scene", bytes).IsOk());
    }

    // The main scene spawns the prefab, then saves it (persists as ref + deltas -> a
    // PendingPrefabInstance on load, NOT a flattened mesh).
    draconic::content::Group* scenes = project->SourceDb().RootGroup()->CreateGroup(u8"Scenes");
    draconic::content::Instance* sceneInst =
        scenes->CreateInstance(u8"Main", scene::SceneDocument::StaticType());
    REQUIRE(sceneInst != nullptr);
    {
        scene::SceneDocument doc;
        doc.name = String(u8"Main");
        REQUIRE(sceneInst->WriteObject(doc).IsOk());
    }
    const Guid sceneId = sceneInst->Id();
    {
        scene::Scene scene(u8"Main");
        scene.AddSystem<draconic::render::MeshComponentManager>();
        UniquePtr<IStream> payloadStream = prefabInst->ReadData(u8"scene");
        REQUIRE(payloadStream.Get() != nullptr);
        const scene::EntityHandle spawned = scene::SpawnPrefab(scene, *payloadStream, prefabId);
        REQUIRE(spawned.IsAssigned());
        REQUIRE(scene.PrefabInstanceCount() == 1u);
        REQUIRE(scene::SaveScene(scene, *sceneInst).IsOk());
    }
    project->Settings().defaultSceneId = sceneId;
    project->Settings().defaultScene = String(u8"Scenes/Main");
    REQUIRE(project->SaveSettings().IsOk());

    editor::BuilderRegistry builders;
    builders.Register(UniquePtr<editor::IAssetBuilder>(
        DefaultAllocator().New<geometry::StaticMeshAssetBuilder>(), DefaultAllocator()));
    editor::TemplateRegistry registry;
    SetupHostTemplate(toolDir.AsView(), registry);
    const editor::SceneReferenceScanner scanner = MakePruningScanner();

    editor::ExportPreset preset;
    preset.name = String(u8"Pruned");
    preset.platform = String(GetHostPlatformName());
    preset.outputSubdir = String(u8"pruned");
    preset.pruneToReachable = true;
    editor::ExportResult result;
    REQUIRE(editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false,
                              &result, {}, true, nullptr, &scanner)
                .IsOk());

    // The whole chain is kept: scene staged, prefab staged (scene->prefab), the prefab's mesh cooked
    // in (prefab->asset); the unreferenced mesh is gone.
    draconic::vfs::PakFileSystem pak(
        PathJoin(result.outputDir.AsView(), project::kDistContentPak).AsView());
    REQUIRE(pak.IsValid());
    draconic::content::ContentDatabase db(pak, BinarySerializerFactory(),
                                          project::kCookedAssetExtension);
    CHECK(db.GetInstance(sceneId) != nullptr);
    CHECK(db.GetInstance(prefabId) != nullptr);     // prefab staged
    CHECK(db.GetInstance(meshInPrefab) != nullptr); // reachable only through the prefab
    CHECK(db.GetInstance(meshDead) == nullptr);     // unreferenced dropped

    NukeTree(projectDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

TEST_CASE(
    "export: ExportPresetsController round-trips add/edit/duplicate/delete through save->load")
{
    const String dir = TempDir(u8"draconic_presets_controller");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    // First load, no file yet => seeded with the built-in default (one host-platform preset).
    editor::ExportPresetsController ctl;
    ctl.Load(root);
    REQUIRE(ctl.Count() == 1u);
    CHECK(ctl.At(0).platform == GetHostPlatformName());

    // Add two presets; the second collides on name and is auto-uniqued to "Windows Copy".
    editor::ExportPreset a;
    a.name = String(u8"Windows");
    a.platform = String(u8"Win64");
    a.config = String(u8"Release");
    a.stageSymbols = true;
    const usize ia = ctl.Add(a);
    CHECK(ctl.At(ia).name == u8"Windows");
    editor::ExportPreset dup;
    dup.name = String(u8"Windows");
    dup.platform = String(u8"Win64");
    const usize idup = ctl.Add(dup);
    CHECK(ctl.At(idup).name == u8"Windows Copy"); // name collision resolved on Add
    REQUIRE(ctl.Count() == 3u);

    // Edit: rename + change fields on the "Windows" preset.
    editor::ExportPreset edited = ctl.At(ia);
    edited.name = String(u8"Windows Ship");
    edited.playerName = String(u8"MyGame.exe");
    edited.additionalFiles.PushBack(String(u8"icon.ico"));
    ctl.Update(ia, edited);
    CHECK(ctl.At(ia).name == u8"Windows Ship");
    CHECK(ctl.At(ia).playerName == u8"MyGame.exe");

    // Duplicate: a distinct " Copy" name off the source, appended at the end.
    const usize icopy = ctl.Duplicate(ia);
    CHECK(icopy == ctl.Count() - 1u);
    CHECK(ctl.At(icopy).name == u8"Windows Ship Copy");
    CHECK(ctl.At(icopy).playerName == u8"MyGame.exe"); // fields carried over

    // Persist and reload into a fresh controller: every mutation survives the XML round-trip.
    REQUIRE(ctl.Save(*root.AsWritable()).IsOk());
    editor::ExportPresetsController reloaded;
    reloaded.Load(root);
    REQUIRE(reloaded.Count() == ctl.Count());
    const editor::ExportPreset* ship = reloaded.Set().Find(u8"Windows Ship");
    REQUIRE(ship != nullptr);
    CHECK(ship->playerName == u8"MyGame.exe");
    CHECK(ship->stageSymbols);
    REQUIRE(ship->additionalFiles.Size() == 1u);
    CHECK(ship->additionalFiles[0] == u8"icon.ico");
    CHECK(reloaded.Set().Find(u8"Windows Ship Copy") != nullptr);

    // Delete: drop the duplicate, save, reload - it's gone; the rest stays.
    for (usize i = 0; i < reloaded.Count(); ++i)
    {
        if (reloaded.At(i).name == u8"Windows Ship Copy")
        {
            reloaded.Remove(i);
            break;
        }
    }
    REQUIRE(reloaded.Save(*root.AsWritable()).IsOk());
    editor::ExportPresetsController afterDelete;
    afterDelete.Load(root);
    CHECK(afterDelete.Set().Find(u8"Windows Ship Copy") == nullptr);
    CHECK(afterDelete.Set().Find(u8"Windows Ship") != nullptr);

    NukeTree(dir.AsView());
}

TEST_CASE("export: RemoveTemplate deletes an installed bundle the registry then drops")
{
    const String rootDir = TempDir(u8"draconic_removetmpl_root");
    const String hostDir = TempDir(u8"draconic_removetmpl_host");
    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(hostDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"draconic-remove-me").AsView()));

    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    draconic::vfs::NativeFileSystem hostFs(hostDir.AsView());
    editor::ExportTemplate t;
    t.id = String(u8"draconic-remove-me");
    t.platform = String(u8"Win64");
    t.playerBinary = String(u8"Draconic.Engine.Player.exe");
    REQUIRE(editor::SaveTemplateManifest(*rootFs.AsWritable(), t, u8"draconic-remove-me/template.xml")
                .IsOk());

    // Present before removal.
    {
        editor::TemplateRegistry reg;
        reg.Refresh(rootDir.AsView(), &rootFs, hostDir.AsView(), &hostFs);
        REQUIRE(reg.FindById(u8"draconic-remove-me") != nullptr);
    }

    // Remove the bundle dir; a fresh registry no longer sees it (host template remains).
    REQUIRE(editor::RemoveTemplate(rootDir.AsView(), u8"draconic-remove-me").IsOk());
    {
        draconic::vfs::NativeFileSystem rootFs2(rootDir.AsView());
        editor::TemplateRegistry reg;
        reg.Refresh(rootDir.AsView(), &rootFs2, hostDir.AsView(), &hostFs);
        CHECK(reg.FindById(u8"draconic-remove-me") == nullptr);
        CHECK(reg.Count() == 1u); // just the synthesized host template
    }

    // Removing a non-existent id fails softly (NotFound), never crashes; empty id is rejected.
    CHECK_FALSE(editor::RemoveTemplate(rootDir.AsView(), u8"draconic-remove-me").IsOk());
    CHECK_FALSE(editor::RemoveTemplate(rootDir.AsView(), u8"").IsOk());

    NukeTree(rootDir.AsView());
    NukeTree(hostDir.AsView());
}

TEST_CASE("export: TemplateEngineMatches flags a version mismatch, passes host + unstamped")
{
    // The synthesized host template carries this build's engine version => matches.
    editor::ExportTemplate host;
    host.engineVersion = String(draconic::project::kEngineVersionString);
    CHECK(editor::TemplateEngineMatches(host));

    // A stamped, differing version => mismatch (the "!" note in the templates manager).
    editor::ExportTemplate old;
    old.engineVersion = String(u8"0.0.0-ancient");
    CHECK_FALSE(editor::TemplateEngineMatches(old));

    // An unstamped (hand-written) manifest is treated as a match (driver only soft-warns on a real
    // differing stamp).
    editor::ExportTemplate blank;
    CHECK(editor::TemplateEngineMatches(blank));
}

// === Reachability Phase 2: the "Always Export" roots set (docs/design/export-reachability.md) ===

TEST_CASE("export: ExportRootsSet membership toggle is idempotent and round-trips through XML")
{
    const String dir = TempDir(u8"draconic_export_roots_set");
    NukeTree(dir.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    editor::ExportRootsSet set;
    CHECK(set.IsEmpty());

    const Guid a{0x1111111111111111ull, 0x2222222222222222ull};
    const Guid b{0x3333333333333333ull, 0x4444444444444444ull};

    CHECK(set.ToggleInstance(a) == true); // a is now a root
    CHECK(set.HasInstance(a));
    CHECK_FALSE(set.HasInstance(b));
    CHECK(set.SetInstance(a, true) == true);   // adding a present guid is a no-op
    REQUIRE(set.instances.Size() == 1u);       // not duplicated
    CHECK(set.SetInstance(b, false) == false); // removing an absent guid is a no-op
    REQUIRE(set.instances.Size() == 1u);

    CHECK(set.ToggleGroup(u8"Weapons/Runtime") == true);
    CHECK(set.HasGroup(u8"Weapons/Runtime"));
    CHECK_FALSE(set.IsEmpty());

    REQUIRE(editor::SaveExportRoots(*root.AsWritable(), set).IsOk());

    editor::ExportRootsSet loaded;
    REQUIRE(editor::LoadExportRoots(root, loaded).IsOk());
    REQUIRE(loaded.instances.Size() == 1u);
    REQUIRE(loaded.groups.Size() == 1u);
    CHECK(loaded.HasInstance(a));
    CHECK(loaded.HasGroup(u8"Weapons/Runtime"));

    // Toggling off drops membership (and the reverse of the earlier toggle).
    CHECK(loaded.ToggleInstance(a) == false);
    CHECK_FALSE(loaded.HasInstance(a));
    CHECK(loaded.ToggleGroup(u8"Weapons/Runtime") == false);
    CHECK(loaded.IsEmpty());

    // A project with no export_roots.xml => NotFound (caller treats as the empty set).
    editor::ExportRootsSet none;
    CHECK(editor::LoadExportRoots(root, none, u8"does_not_exist.xml").Code() ==
          ErrorCode::NotFound);

    NukeTree(dir.AsView());
}

TEST_CASE("export: CollectGroupInstances enumerates a group subtree, not its siblings")
{
    const String projectDir = TempDir(u8"draconic_export_roots_group");
    NukeTree(projectDir.AsView());
    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"Roots").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));

    draconic::content::ContentDatabase& db = project->SourceDb();
    const TypeInfo& ty = scene::SceneDocument::StaticType(); // any type; enumeration ignores it

    draconic::content::Group* weapons = db.RootGroup()->CreateGroup(u8"Weapons");
    const Guid sword = weapons->CreateInstance(u8"Sword", ty)->Id();
    const Guid axe = weapons->CreateInstance(u8"Axe", ty)->Id();
    draconic::content::Group* rare = weapons->CreateGroup(u8"Rare"); // nested subtree
    const Guid excalibur = rare->CreateInstance(u8"Excalibur", ty)->Id();

    draconic::content::Group* props = db.RootGroup()->CreateGroup(u8"Props");
    const Guid crate = props->CreateInstance(u8"Crate", ty)->Id();

    // "Weapons" pulls its own instances AND the nested Rare subtree, but not the Props sibling.
    Array<Guid> got;
    editor::CollectGroupInstances(db, u8"Weapons", got);
    const auto has = [&](const Guid& id)
    {
        for (const Guid& g : got)
        {
            if (g == id)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(got.Size() == 3u);
    CHECK(has(sword));
    CHECK(has(axe));
    CHECK(has(excalibur));
    CHECK_FALSE(has(crate));

    // Empty path = the whole DB (root subtree); a missing group contributes nothing.
    Array<Guid> all;
    editor::CollectGroupInstances(db, u8"", all);
    CHECK(all.Size() == 4u);
    Array<Guid> missing;
    editor::CollectGroupInstances(db, u8"NoSuchGroup", missing);
    CHECK(missing.IsEmpty());

    NukeTree(projectDir.AsView());
}

TEST_CASE("export: CollectExportRoots seeds Always-Export flags + group members, deduped by guid")
{
    const String projectDir = TempDir(u8"draconic_export_roots_seed");
    NukeTree(projectDir.AsView());
    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"Roots").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));

    draconic::content::ContentDatabase& db = project->SourceDb();
    const TypeInfo& ty = scene::SceneDocument::StaticType();

    draconic::content::Group* scenes = db.RootGroup()->CreateGroup(u8"Scenes");
    const Guid mainScene = scenes->CreateInstance(u8"Main", ty)->Id();
    const Guid weaponMesh =
        db.RootGroup()->CreateGroup(u8"Meshes")->CreateInstance(u8"Sword", ty)->Id();
    draconic::content::Group* runtime = db.RootGroup()->CreateGroup(u8"RuntimeLoaded");
    const Guid table = runtime->CreateInstance(u8"LootTable", ty)->Id();

    project->Settings().defaultSceneId = mainScene;
    project->ExportRoots().SetInstance(weaponMesh, true);     // an explicit instance flag
    project->ExportRoots().SetGroup(u8"RuntimeLoaded", true); // a group subtree flag
    // The default scene ALSO flagged directly => must dedupe to one root, keeping DefaultScene.
    project->ExportRoots().SetInstance(mainScene, true);

    const Array<editor::ExportRoot> roots = editor::CollectExportRoots(*project);

    const auto reasonOf = [&](const Guid& id) -> const editor::ExportRoot*
    {
        for (const editor::ExportRoot& r : roots)
        {
            if (r.id == id)
            {
                return &r;
            }
        }
        return nullptr;
    };
    // Exactly three distinct roots (default scene deduped despite the redundant flag).
    CHECK(roots.Size() == 3u);
    REQUIRE(reasonOf(mainScene) != nullptr);
    CHECK(reasonOf(mainScene)->reason == editor::ExportRootReason::DefaultScene); // wins over Flag
    REQUIRE(reasonOf(weaponMesh) != nullptr);
    CHECK(reasonOf(weaponMesh)->reason == editor::ExportRootReason::Flag);
    REQUIRE(reasonOf(table) != nullptr);
    CHECK(reasonOf(table)->reason == editor::ExportRootReason::Group);

    // The set persists: save it, reopen the project, the flags survive (Open reads export_roots.xml).
    REQUIRE(project->SaveExportRoots().IsOk());
    project.Reset();
    UniquePtr<editor::EditorProject> reopened = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(reopened));
    CHECK(reopened->ExportRoots().HasInstance(weaponMesh));
    CHECK(reopened->ExportRoots().HasGroup(u8"RuntimeLoaded"));

    NukeTree(projectDir.AsView());
}

TEST_CASE("export: template create recognizes a WEB build dir (player page + web sidecars)")
{
    // A wasm build dir holds the browser player PAGE (html) + js/wasm sidecars, not a host
    // executable: CreateTemplate must synthesize a platform "Web" template whose player is the
    // page - the reusable web export template (the player fetches the dist from the serving
    // folder, so nothing project-specific is inside).
    const String dir = TempDir(u8"draconic_web_template_src");
    const String dest = TempDir(u8"draconic_web_template_out");
    NukeTree(dir.AsView());
    NukeTree(dest.AsView());
    REQUIRE(CreateDirectory(dir.AsView()));
    draconic::vfs::NativeFileSystem root(dir.AsView());

    SaveText(root, u8"Draconic.Engine.Player.html", u8"<html>player page</html>");
    SaveText(root, u8"Draconic.Engine.Player.js", u8"// glue");
    SaveText(root, u8"Draconic.Engine.Player.wasm", u8"\0asm");
    SaveText(root, u8"serve.py", u8"# server");
    SaveText(root, u8"Draconic.Engine.Player.runtime-libs",
             u8"Draconic.Engine.Player.js\nDraconic.Engine.Player.wasm\nserve.py\n");

    String id, outDir;
    REQUIRE(editor::CreateTemplate(dir.AsView(), dest.AsView(),
                                   editor::TemplateOutput::ExportFolder, &id, &outDir)
                .IsOk());

    draconic::vfs::NativeFileSystem out(outDir.AsView());
    editor::ExportTemplate created;
    REQUIRE(editor::LoadTemplateManifest(out, created).IsOk());
    CHECK(created.platform == u8"Web");
    CHECK(created.compiler == u8"Emscripten");
    CHECK(created.playerBinary == u8"Draconic.Engine.Player.html");
    REQUIRE(created.sidecars.Size() == 3u);
    CHECK(created.sidecars[0] == u8"Draconic.Engine.Player.js");
    CHECK(created.sidecars[1] == u8"Draconic.Engine.Player.wasm");
    CHECK(created.sidecars[2] == u8"serve.py");
    // The bundle materialized the page + every sidecar.
    CHECK(out.Exists(u8"Draconic.Engine.Player.html"));
    CHECK(out.Exists(u8"Draconic.Engine.Player.js"));
    CHECK(out.Exists(u8"Draconic.Engine.Player.wasm"));
    CHECK(out.Exists(u8"serve.py"));

    NukeTree(dir.AsView());
    NukeTree(dest.AsView());
}

TEST_CASE("export: a Web preset stages the browser player + a WGSL shader pack")
{
    // The full web export pipeline: preset (platform Web) -> the imported Web template ->
    // a served-folder dist (player page + js/wasm/serve.py + Content.pak + player.xml +
    // a WGSL-format shaders.dpak). This is the export-from-editor flow; the output folder
    // is directly servable (serve.py) and the player fetches the three dist files.
    const String projectDir = TempDir(u8"draconic_webexport_proj");
    const String rootDir = TempDir(u8"draconic_webexport_root");
    const String toolDir = TempDir(u8"draconic_webexport_tool");
    const String outRoot = TempDir(u8"draconic_webexport_out");
    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());

    REQUIRE(editor::EditorProject::Create(projectDir.AsView(), u8"WebExportTest").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(projectDir.AsView());
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->SaveSettings().IsOk());

    // An installed Web template (the shape `--template create <wasm build dir>` produces).
    REQUIRE(CreateDirectory(rootDir.AsView()));
    REQUIRE(CreateDirectory(PathJoin(rootDir.AsView(), u8"web-template").AsView()));
    draconic::vfs::NativeFileSystem rootFs(rootDir.AsView());
    editor::ExportTemplate web;
    web.id = String(u8"draconic-web-debug-test");
    web.platform = String(u8"Web");
    web.compiler = String(u8"Emscripten");
    web.playerBinary = String(u8"Draconic.Engine.Player.html");
    web.sidecars.PushBack(String(u8"Draconic.Engine.Player.js"));
    web.sidecars.PushBack(String(u8"Draconic.Engine.Player.wasm"));
    web.sidecars.PushBack(String(u8"serve.py"));
    REQUIRE(editor::SaveTemplateManifest(*rootFs.AsWritable(), web, u8"web-template/template.xml")
                .IsOk());
    draconic::vfs::NativeFileSystem webDirFs(PathJoin(rootDir.AsView(), u8"web-template").AsView());
    SaveText(webDirFs, u8"Draconic.Engine.Player.html", u8"<html>player</html>");
    SaveText(webDirFs, u8"Draconic.Engine.Player.js", u8"// glue");
    SaveText(webDirFs, u8"Draconic.Engine.Player.wasm", u8"\0asm");
    SaveText(webDirFs, u8"serve.py", u8"# server");

    REQUIRE(CreateDirectory(toolDir.AsView()));
    draconic::vfs::NativeFileSystem toolFs(toolDir.AsView());
    editor::TemplateRegistry registry;
    registry.Refresh(rootDir.AsView(), &rootFs, toolDir.AsView(), &toolFs);

    editor::ExportPreset preset;
    preset.name = String(u8"Web Build");
    preset.platform = String(u8"Web"); // -> the Web template by platform
    preset.outputSubdir = String(u8"web");

    editor::BuilderRegistry builders;
    editor::ExportResult result;
    REQUIRE(
        editor::ExportOne(*project, preset, registry, builders, outRoot.AsView(), false, &result)
            .IsOk());

    // The served folder: page + sidecars + content + the ENGINE shader pack.
    draconic::vfs::NativeFileSystem distFs(result.outputDir.AsView());
    CHECK(distFs.Exists(u8"Draconic.Engine.Player.html"));
    CHECK(distFs.Exists(u8"Draconic.Engine.Player.js"));
    CHECK(distFs.Exists(u8"Draconic.Engine.Player.wasm"));
    CHECK(distFs.Exists(u8"serve.py"));
    CHECK(distFs.Exists(u8"Content.pak"));
    CHECK(distFs.Exists(u8"player.xml"));
    CHECK(distFs.Exists(u8"shaders.dpak"));

    // The pack is WGSL-format (the Web platform's runtime format), not SPIR-V/DXIL.
    {
        UniquePtr<IStream> packStream = distFs.Open(u8"shaders.dpak", FileMode::Read);
        REQUIRE(static_cast<bool>(packStream));
        draconic::shaders::CookedShaderPack pack;
        REQUIRE(pack.Read(*packStream).IsOk());
        CHECK(pack.Find(u8"tonemap", draconic::shaders::ShaderStage::Fragment,
                        draconic::shaders::ShaderFlags::None,
                        draconic::shaders::CookedShaderFormat::Wgsl) != nullptr);
        CHECK(pack.Find(u8"tonemap", draconic::shaders::ShaderStage::Fragment,
                        draconic::shaders::ShaderFlags::None,
                        draconic::shaders::CookedShaderFormat::SpirV) == nullptr);
    }

    NukeTree(projectDir.AsView());
    NukeTree(rootDir.AsView());
    NukeTree(toolDir.AsView());
    NukeTree(outRoot.AsView());
}

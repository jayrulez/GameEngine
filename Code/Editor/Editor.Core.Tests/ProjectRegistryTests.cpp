// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Project-registry tests: the recent-projects Settings section (touch/dedupe/order/cap/remove +
// store round-trip), manifest probing, the engine-version relation, and the manifest backup -
// the headless core of the built-in project manager.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.xml.serialization;
import engine.project;
import editor.core;

using namespace foundation::core;
using namespace editor;
namespace vfs = foundation::vfs;
namespace settings = foundation::settings;
namespace project = engine::project;

namespace
{
    bool Contains(StringView hay, StringView needle)
    {
        if (needle.Size() > hay.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (StringView(hay.Data() + i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    }

    void RemoveProjectTree(StringView root)
    {
        FileDelete(PathJoin(root, u8"Project.xml"));
        FileDelete(PathJoin(root, u8"Project.xml.0.1.0.bak"));
        RemoveDirectory(PathJoin(root, u8"Content"));
        RemoveDirectory(PathJoin(root, u8"Sources"));
        RemoveDirectory(PathJoin(root, u8"Cooked"));
        RemoveDirectory(PathJoin(root, u8"Editor"));
        RemoveDirectory(PathJoin(root, u8".cache"));
        RemoveDirectory(root);
    }
}

TEST_CASE("project-registry: touch inserts most-recent-first, dedupes, and caps")
{
    RegisterProjectRegistryTypes();
    settings::Settings store(foundation::core::DefaultAllocator());

    TouchRecentProject(store, u8"/projects/a", u8"A", u8"0.1.0");
    TouchRecentProject(store, u8"/projects/b", u8"B", u8"0.1.0");
    const RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
    REQUIRE(reg.entries.Size() == 2u);
    CHECK(reg.entries[0].path.AsView() == u8"/projects/b"); // most recent first

    // Re-touching A moves it to the front and refreshes its snapshot (no duplicate row).
    TouchRecentProject(store, u8"/projects/a", u8"A renamed", u8"0.2.0");
    REQUIRE(reg.entries.Size() == 2u);
    CHECK(reg.entries[0].path.AsView() == u8"/projects/a");
    CHECK(reg.entries[0].name.AsView() == u8"A renamed");
    CHECK(reg.entries[0].engineVersion.AsView() == u8"0.2.0");
    CHECK(reg.entries[1].path.AsView() == u8"/projects/b");

    // The cap drops the OLDEST entries.
    for (u32 i = 0; i < RecentProjectsSettings::kMaxEntries + 5; ++i)
    {
        const String p = Format(u8"/projects/p{}", i);
        TouchRecentProject(store, p.AsView(), u8"P", u8"0.1.0");
    }
    CHECK(reg.entries.Size() == RecentProjectsSettings::kMaxEntries);
    CHECK(reg.entries[0].path.AsView() ==
          Format(u8"/projects/p{}", RecentProjectsSettings::kMaxEntries + 4).AsView());
    CHECK(reg.Find(u8"/projects/a") == nullptr); // evicted
}

TEST_CASE("project-registry: remove deletes a row; the section round-trips the settings store")
{
    RegisterProjectRegistryTypes();
    settings::Settings store(foundation::core::DefaultAllocator());
    TouchRecentProject(store, u8"/projects/keep", u8"Keep", u8"0.1.0");
    TouchRecentProject(store, u8"/projects/drop", u8"Drop", u8"0.1.0");

    CHECK(RemoveRecentProject(store, u8"/projects/drop"));
    CHECK_FALSE(RemoveRecentProject(store, u8"/projects/drop")); // already gone
    CHECK(store.Section<RecentProjectsSettings>().entries.Size() == 1u);

    // Round-trip through the fs-explicit editor-settings helpers (the store's real backend).
    const StringView dir = u8"scratch_registry_roundtrip_test";
    (void)CreateDirectory(dir);
    vfs::NativeFileSystem fs(dir, DefaultAllocator());
    REQUIRE(SaveEditorSettings(*fs.AsWritable(), store).IsOk());

    settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(LoadEditorSettings(fs, loaded).IsOk());
    const RecentProjectsSettings& reg = loaded.Section<RecentProjectsSettings>();
    REQUIRE(reg.entries.Size() == 1u);
    CHECK(reg.entries[0].path.AsView() == u8"/projects/keep");
    CHECK(reg.entries[0].name.AsView() == u8"Keep");

    FileDelete(PathJoin(dir, kEditorSettingsFile));
    RemoveDirectory(dir);
}

TEST_CASE("project-registry: probe reads the manifest without opening; NotFound for non-projects")
{
    const StringView dir = u8"scratch_registry_probe_test";
    RemoveProjectTree(dir);

    engine::project::ProjectSettings probed;
    CHECK(ProbeProject(dir, probed).Code() == ErrorCode::NotFound); // no dir at all

    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Probe Me").IsOk());
    REQUIRE(ProbeProject(dir, probed).IsOk());
    CHECK(probed.name.AsView() == u8"Probe Me");
    CHECK(probed.engineVersion.AsView() == project::kEngineVersionString);

    RemoveProjectTree(dir);
}

TEST_CASE("project-registry: engine-version relation")
{
    // This engine is kEngineVersionString; build relative stamps from the constants so the
    // test stays true when the engine version bumps.
    const String same = String(project::kEngineVersionString);
    const String older = Format(u8"{}.{}.{}", project::kEngineVersionMajor,
                                project::kEngineVersionMinor, project::kEngineVersionPatch);
    CHECK(CompareProjectEngineVersion(same.AsView()) == EngineVersionRelation::Same);
    CHECK(CompareProjectEngineVersion(older.AsView()) == EngineVersionRelation::Same);

    const String newer = Format(u8"{}.{}.{}", project::kEngineVersionMajor + 1, 0, 0);
    CHECK(CompareProjectEngineVersion(newer.AsView()) == EngineVersionRelation::ProjectNewer);
    const String newerPatch =
        Format(u8"{}.{}.{}", project::kEngineVersionMajor, project::kEngineVersionMinor,
               project::kEngineVersionPatch + 1);
    CHECK(CompareProjectEngineVersion(newerPatch.AsView()) ==
          EngineVersionRelation::ProjectNewer);

    // Anything below the current version is older; 0.0.x is always below (version starts 0.1.0).
    CHECK(CompareProjectEngineVersion(u8"0.0.9") == EngineVersionRelation::ProjectOlder);

    CHECK(CompareProjectEngineVersion(u8"") == EngineVersionRelation::Unstamped);
    CHECK(CompareProjectEngineVersion(u8"abc") == EngineVersionRelation::Unstamped);
    CHECK(CompareProjectEngineVersion(u8"1.2") == EngineVersionRelation::Unstamped);
    CHECK(CompareProjectEngineVersion(u8"1..2") == EngineVersionRelation::Unstamped);
    CHECK(CompareProjectEngineVersion(u8"1.2.3.4") == EngineVersionRelation::Unstamped);
}

TEST_CASE("project-registry: manifest backup copies Project.xml beside itself")
{
    const StringView dir = u8"scratch_registry_backup_test";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Backup Me").IsOk());

    Result<String> backup = BackupProjectManifest(dir);
    REQUIRE(backup.HasValue());
    CHECK(FileExists(backup.Value().AsView()));
    // The original manifest is untouched and still probes.
    engine::project::ProjectSettings probed;
    CHECK(ProbeProject(dir, probed).IsOk());

    FileDelete(backup.Value().AsView());
    RemoveProjectTree(dir);

    CHECK_FALSE(BackupProjectManifest(u8"scratch_registry_no_such_dir").HasValue());
}

TEST_CASE("project-manager controller: open gates + prompt copy + registry pass-through")
{
    RegisterProjectRegistryTypes();
    settings::Settings store(foundation::core::DefaultAllocator());
    ProjectManagerController controller(store);

    // Not a project.
    ProjectManagerController::OpenDecision decision;
    controller.DecideOpen(u8"scratch_manager_no_such_dir", decision);
    CHECK(decision.gate == ProjectOpenGate::NotAProject);

    // A freshly scaffolded project is stamped with THIS engine: opens directly.
    const StringView dir = u8"scratch_manager_gate_test";
    RemoveProjectTree(dir);
    REQUIRE(controller.Create(dir, u8"Gate Test").IsOk());
    controller.DecideOpen(dir, decision);
    CHECK(decision.gate == ProjectOpenGate::OpenDirectly);
    CHECK(decision.probed.name.AsView() == u8"Gate Test");

    // Rewrite the manifest with a NEWER stamp (raw text edit: SaveProjectSettings re-stamps
    // to the current engine by design, so it cannot write a foreign version).
    {
        const String manifest = PathJoin(dir, u8"Project.xml");
        Result<Array<byte>> bytes = ReadFile(manifest.AsView());
        REQUIRE(bytes.HasValue());
        String text(StringView(reinterpret_cast<const char8_t*>(bytes.Value().Data()),
                               bytes.Value().Size()));
        const String current(project::kEngineVersionString);
        usize at = text.Size();
        for (usize i = 0; i + current.Size() <= text.Size(); ++i)
        {
            if (StringView(text.Data() + i, current.Size()) == current.AsView())
            {
                at = i;
                break;
            }
        }
        REQUIRE(at != text.Size());
        String patched(StringView(text.Data(), at));
        patched += u8"99.0.0";
        patched += StringView(text.Data() + at + current.Size(), text.Size() - at - current.Size());
        REQUIRE(WriteFile(manifest.AsView(),
                          Span<const byte>(reinterpret_cast<const byte*>(patched.Data()),
                                           patched.Size()))
                    .IsOk());
    }
    controller.DecideOpen(dir, decision);
    CHECK(decision.gate == ProjectOpenGate::PromptNewerEngine);
    // Opened anyway, a newer engine's project keeps its stamp; only an older one's is upgraded.
    CHECK_FALSE(ProjectManagerController::RestampsOnOpen(decision.gate));
    CHECK(ProjectManagerController::RestampsOnOpen(ProjectOpenGate::PromptOlderBackup));
    CHECK_FALSE(ProjectManagerController::RestampsOnOpen(ProjectOpenGate::OpenDirectly));
    CHECK(Contains(decision.promptBody.AsView(), u8"99.0.0"));
    CHECK(Contains(decision.promptBody.AsView(), project::kEngineVersionString));

    // Registry pass-through.
    controller.NoteOpened(dir, u8"Gate Test", u8"0.1.0");
    CHECK(controller.Entries().entries.Size() == 1u);
    CHECK(controller.Remove(dir));
    CHECK(controller.Entries().entries.IsEmpty());

    RemoveProjectTree(dir);
}

TEST_CASE("project manifest v7: defaultUiFontId (and the once-dropped defaults) round-trip Open")
{
    const StringView dir = u8"scratch_manifest_v7_test";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"V7 Test").IsOk());

    Guid fontId;
    Guid busId;
    Guid loadingId;
    REQUIRE(Guid::TryParse(u8"6ba7b810-9dad-11d1-80b4-00c04fd430c8", fontId));
    REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8", busId));
    REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8", loadingId));
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        project->Settings().defaultUiFontId = fontId;
        project->Settings().defaultBusLayoutId = busId; // the per-field move must not DROP this
        project->Settings().guiLoadingDocumentId = loadingId; // nor these two, which it did drop
        project->Settings().renderMsaaSamples = 4;
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(project);
        CHECK(project->Settings().defaultUiFontId == fontId);
        CHECK(project->Settings().defaultBusLayoutId == busId);
        CHECK(project->Settings().guiLoadingDocumentId == loadingId);
        CHECK(project->Settings().renderMsaaSamples == 4u);
    }
    RemoveProjectTree(dir);
}

TEST_CASE("editor.settings: every registered section type is INSTANTIABLE (factory too)")
{
    // A section type registered without RegisterSerializable is a time bomb: the first
    // save that includes it makes every later Load abort mid-file (Settings phase-1
    // semantics), silently dropping the sections after it - the empty-project-list
    // incident (EditorUiSettings had a type registration but no factory).
    RegisterEditorSettingsTypes();
    RegisterProjectRegistryTypes();
    // Every ISerializable registered in the Editor domain, asked of the registry - not a list
    // kept by hand here, which the shortcut section proved nobody extends (it was missing).
    usize checked = 0;
    for (const TypeInfo* type : GlobalTypeRegistry().All())
    {
        if (GlobalTypeRegistry().DomainOf(type->id) != TypeDomain(u8"Editor") ||
            !IsDerivedFrom(type, &ISerializable::StaticType()))
        {
            continue;
        }
        INFO("section type: " << reinterpret_cast<const char*>(type->name));
        CHECK(GlobalSerializableRegistry().Create(type->id).Get() != nullptr);
        ++checked;
    }
    CHECK(checked >= 6u); // export, font, ui, mcp, shortcuts, recent projects - at least
}

TEST_CASE("editor.settings: the MCP section round-trips, and a minted token is a fresh Guid")
{
    RegisterEditorSettingsTypes();
    // Defaults: off, the documented port, no token yet.
    {
        foundation::settings::Settings store(foundation::core::DefaultAllocator());
        const EditorMcpSettings& mcp = store.Section<EditorMcpSettings>();
        CHECK_FALSE(mcp.enabled);
        CHECK(mcp.port == kEditorMcpDefaultPort);
        CHECK(mcp.token.IsEmpty());
    }
    foundation::settings::Settings store(foundation::core::DefaultAllocator());
    EditorMcpSettings& mcp = store.Section<EditorMcpSettings>();
    mcp.enabled = true;
    mcp.port = 7500;
    mcp.token = GenerateMcpToken();
    REQUIRE(mcp.token.Size() == 36u);
    CHECK(GenerateMcpToken() != mcp.token); // every mint is a new secret

    MemoryStream buffer;
    REQUIRE(store.Save(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(loaded.Load(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    const EditorMcpSettings* back = loaded.Find<EditorMcpSettings>();
    REQUIRE(back != nullptr);
    CHECK(back->enabled);
    CHECK(back->port == 7500u);
    CHECK(back->token == mcp.token);
}

TEST_CASE("editor.settings: the MCP section takes the Preferences fields, refusing a bad port")
{
    EditorMcpSettings mcp;
    mcp.token = String(u8"old");
    CHECK(mcp.ApplyFromPreferences(true, u8"7500", u8"new"));
    CHECK(mcp.enabled);
    CHECK(mcp.port == 7500u);
    CHECK(mcp.token == u8"new");
    // A port outside 1024..65535, or not a number, is refused and the port stands.
    CHECK_FALSE(mcp.ApplyFromPreferences(true, u8"80", u8"new"));
    CHECK_FALSE(mcp.ApplyFromPreferences(true, u8"70000", u8"new"));
    CHECK_FALSE(mcp.ApplyFromPreferences(true, u8"lots", u8"new"));
    CHECK(mcp.port == 7500u);
    // Disabling with an empty token: off, and the next enable mints a fresh secret.
    CHECK(mcp.ApplyFromPreferences(false, u8"7500", u8""));
    CHECK_FALSE(mcp.enabled);
    CHECK(mcp.token.IsEmpty());
}

TEST_CASE("editor.settings: a store with EVERY section round-trips (registry survives)")
{
    RegisterEditorSettingsTypes();
    RegisterProjectRegistryTypes();
    foundation::settings::Settings store(foundation::core::DefaultAllocator());
    store.Section<EditorUiSettings>().uiScale = 1.2f;
    store.Section<EditorUiSettings>().scheme = String(u8"light");
    store.Section<EditorUiSettings>().accent = String(u8"#0f8a7e");
    store.Section<EditorUiSettings>().density = String(u8"comfortable");
    store.Section<EditorUiSettings>().reduceMotion = true;
    TouchRecentProject(store, u8"/proj/a", u8"A", u8"0.1.0");

    MemoryStream buffer;
    REQUIRE(store.Save(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);

    foundation::settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(loaded.Load(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    const RecentProjectsSettings* reg = loaded.Find<RecentProjectsSettings>();
    REQUIRE(reg != nullptr);
    REQUIRE(reg->entries.Size() == 1u);
    CHECK(reg->entries[0].name.AsView() == u8"A");
    const EditorUiSettings* ui = loaded.Find<EditorUiSettings>();
    REQUIRE(ui != nullptr);
    CHECK(ui->uiScale == doctest::Approx(1.2f));
    CHECK(ui->scheme == String(u8"light")); // the look's inputs (appended 2026-10-24)
    CHECK(ui->accent == String(u8"#0f8a7e"));
    CHECK(ui->density == String(u8"comfortable"));
    CHECK(ui->reduceMotion);
}

// Sedulous c5100e94: the Game tab's preview resolutions seed the common sizes once, a user's
// removal stays removed, and the list round-trips through the store; the per-project choice
// rides the project store.
TEST_CASE("editor.settings: the preview resolutions seed once and round-trip")
{
    RegisterEditorSettingsTypes();
    CHECK(GamePreviewSettings::From(nullptr) == nullptr); // no store, nothing
    foundation::settings::Settings store(foundation::core::DefaultAllocator());
    GamePreviewSettings* previews = GamePreviewSettings::From(&store);
    REQUIRE(previews != nullptr);
    CHECK(previews->seeded);
    REQUIRE(previews->presets.Size() == 6u);
    CHECK(previews->presets[3].name == StringView(u8"Steam Deck"));
    CHECK(previews->presets[3].width == 1280u);
    CHECK(previews->presets[3].height == 800u);

    previews->presets.Clear();
    previews->presets.PushBack(GamePreviewResolution{String(u8"Handheld"), 960, 544});
    CHECK(GamePreviewSettings::From(&store)->presets.Size() == 1u); // not seeded again
    store.Section<GamePageSettings>().resolutionKey = String(u8"preset:Handheld");

    MemoryStream buffer;
    REQUIRE(store.Save(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(loaded.Load(buffer, foundation::xml::XmlSerializerFactory()).IsOk());
    const GamePreviewSettings* back = GamePreviewSettings::From(&loaded);
    REQUIRE(back->presets.Size() == 1u);
    CHECK(back->presets[0].name == StringView(u8"Handheld"));
    CHECK(back->presets[0].width == 960u);
    CHECK(back->presets[0].height == 544u);
    REQUIRE(loaded.Find<GamePageSettings>() != nullptr);
    CHECK(loaded.Find<GamePageSettings>()->resolutionKey == StringView(u8"preset:Handheld"));
}

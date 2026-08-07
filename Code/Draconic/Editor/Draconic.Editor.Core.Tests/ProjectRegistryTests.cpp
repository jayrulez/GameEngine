// Project-registry tests: the recent-projects Settings section (touch/dedupe/order/cap/remove +
// store round-trip), manifest probing, the engine-version relation, and the manifest backup -
// the headless core of the built-in project manager.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.vfs;
import draconic.settings;
import draconic.xml.serialization;
import draconic.engine.project;
import draconic.editor.core;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace settings = draconic::settings;
namespace project = draconic::project;

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
    settings::Settings store;

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
    settings::Settings store;
    TouchRecentProject(store, u8"/projects/keep", u8"Keep", u8"0.1.0");
    TouchRecentProject(store, u8"/projects/drop", u8"Drop", u8"0.1.0");

    CHECK(RemoveRecentProject(store, u8"/projects/drop"));
    CHECK_FALSE(RemoveRecentProject(store, u8"/projects/drop")); // already gone
    CHECK(store.Section<RecentProjectsSettings>().entries.Size() == 1u);

    // Round-trip through the fs-explicit editor-settings helpers (the store's real backend).
    const StringView dir = u8"draconic_registry_roundtrip_test";
    (void)CreateDirectory(dir);
    vfs::NativeFileSystem fs(dir);
    REQUIRE(SaveEditorSettings(*fs.AsWritable(), store).IsOk());

    settings::Settings loaded;
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
    const StringView dir = u8"draconic_registry_probe_test";
    RemoveProjectTree(dir);

    project::ProjectSettings probed;
    CHECK(ProbeProject(dir, probed).Code() == ErrorCode::NotFound); // no dir at all

    REQUIRE(EditorProject::Create(dir, u8"Probe Me").IsOk());
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
    const StringView dir = u8"draconic_registry_backup_test";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"Backup Me").IsOk());

    Result<String> backup = BackupProjectManifest(dir);
    REQUIRE(backup.HasValue());
    CHECK(FileExists(backup.Value().AsView()));
    // The original manifest is untouched and still probes.
    project::ProjectSettings probed;
    CHECK(ProbeProject(dir, probed).IsOk());

    FileDelete(backup.Value().AsView());
    RemoveProjectTree(dir);

    CHECK_FALSE(BackupProjectManifest(u8"draconic_registry_no_such_dir").HasValue());
}

TEST_CASE("project-manager controller: open gates + prompt copy + registry pass-through")
{
    RegisterProjectRegistryTypes();
    settings::Settings store;
    ProjectManagerController controller(store);

    // Not a project.
    ProjectManagerController::OpenDecision decision;
    controller.DecideOpen(u8"draconic_manager_no_such_dir", decision);
    CHECK(decision.gate == ProjectOpenGate::NotAProject);

    // A freshly scaffolded project is stamped with THIS engine: opens directly.
    const StringView dir = u8"draconic_manager_gate_test";
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
    const StringView dir = u8"draconic_manifest_v7_test";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"V7 Test").IsOk());

    Guid fontId;
    Guid busId;
    REQUIRE(Guid::TryParse(u8"6ba7b810-9dad-11d1-80b4-00c04fd430c8", fontId));
    REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8", busId));
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(project);
        project->Settings().defaultUiFontId = fontId;
        project->Settings().defaultBusLayoutId = busId; // the per-field move used to DROP this
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(project);
        CHECK(project->Settings().defaultUiFontId == fontId);
        CHECK(project->Settings().defaultBusLayoutId == busId);
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
    const TypeInfo* sectionTypes[] = {
        &EditorExportSettings::StaticType(),
        &EditorFontSettings::StaticType(),
        &EditorUiSettings::StaticType(),
        &RecentProjectsSettings::StaticType(),
    };
    for (const TypeInfo* type : sectionTypes)
    {
        CHECK(GlobalSerializableRegistry().Create(type->id).Get() != nullptr);
    }
}

TEST_CASE("editor.settings: a store with EVERY section round-trips (registry survives)")
{
    RegisterEditorSettingsTypes();
    RegisterProjectRegistryTypes();
    draconic::settings::Settings store;
    store.Section<EditorUiSettings>().uiScale = 1.2f;
    TouchRecentProject(store, u8"/proj/a", u8"A", u8"0.1.0");

    MemoryStream buffer;
    REQUIRE(store.Save(buffer, draconic::xml::XmlSerializerFactory()).IsOk());
    (void)buffer.Seek(0, SeekOrigin::Begin);

    draconic::settings::Settings loaded;
    REQUIRE(loaded.Load(buffer, draconic::xml::XmlSerializerFactory()).IsOk());
    const RecentProjectsSettings* reg = loaded.Find<RecentProjectsSettings>();
    REQUIRE(reg != nullptr);
    REQUIRE(reg->entries.Size() == 1u);
    CHECK(reg->entries[0].name.AsView() == u8"A");
    const EditorUiSettings* ui = loaded.Find<EditorUiSettings>();
    REQUIRE(ui != nullptr);
    CHECK(ui->uiScale == doctest::Approx(1.2f));
}

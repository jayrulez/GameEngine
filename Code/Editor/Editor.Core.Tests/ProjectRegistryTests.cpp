// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Project-registry tests, the headless core of the built-in project manager: the recent-projects
// Settings section (touch, dedupe, order, cap, remove and the store round-trip), manifest probing,
// the engine-version relation, and the manifest backup.

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

    // The cap drops the oldest entries.
    for (u32 i = 0; i < kRecentProjectsSettingsMaxEntries + 5; ++i)
    {
        const String p = Format(u8"/projects/p{}", i);
        TouchRecentProject(store, p.AsView(), u8"P", u8"0.1.0");
    }
    CHECK(reg.entries.Size() == kRecentProjectsSettingsMaxEntries);
    CHECK(reg.entries[0].path.AsView() ==
          Format(u8"/projects/p{}", kRecentProjectsSettingsMaxEntries + 4).AsView());
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

TEST_CASE("project-registry: an entry keeps when it was opened and its pin; the list keeps its order and last location")
{
    RegisterProjectRegistryTypes();
    settings::Settings store(foundation::core::DefaultAllocator());
    TouchRecentProject(store, u8"/projects/a", u8"A", u8"0.1.0", 1000);
    CHECK(SetRecentProjectFavourite(store, u8"/projects/a", true));
    CHECK_FALSE(SetRecentProjectFavourite(store, u8"/projects/none", true));
    TouchRecentProject(store, u8"/projects/a", u8"A", u8"0.1.0", 2000); // opened again: the pin stays
    RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
    REQUIRE(reg.entries.Size() == 1u);
    CHECK(reg.entries[0].lastOpened == 2000);
    CHECK(reg.entries[0].favourite);
    reg.sortByName = true;
    reg.lastLocation = String(u8"/home/me/Games");

    // Opened without a time: now, by the wall clock.
    const i64 before = GetUnixTime();
    TouchRecentProject(store, u8"/projects/b", u8"B", u8"0.1.0");
    CHECK(reg.Find(u8"/projects/b")->lastOpened >= before);

    // They round-trip the store.
    const StringView dir = u8"scratch_registry_fields_test";
    (void)CreateDirectory(dir);
    vfs::NativeFileSystem fs(dir, DefaultAllocator());
    REQUIRE(SaveEditorSettings(*fs.AsWritable(), store).IsOk());
    settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(LoadEditorSettings(fs, loaded).IsOk());
    const RecentProjectsSettings& back = loaded.Section<RecentProjectsSettings>();
    REQUIRE(back.Find(u8"/projects/a") != nullptr);
    CHECK(back.Find(u8"/projects/a")->lastOpened == 2000);
    CHECK(back.Find(u8"/projects/a")->favourite);
    CHECK_FALSE(back.Find(u8"/projects/b")->favourite);
    CHECK(back.sortByName);
    CHECK(back.lastLocation.AsView() == u8"/home/me/Games");

    // A store written before these fields existed still loads, each at its default.
    const String file = PathJoin(dir, kEditorSettingsFile);
    Result<Array<byte>> bytes = ReadFile(file.AsView());
    REQUIRE(bytes.HasValue());
    const String text(StringView(reinterpret_cast<const char8_t*>(bytes.Value().Data()), bytes.Value().Size()));
    String old;
    usize start = 0;
    while (start < text.Size())
    {
        usize end = start;
        while (end < text.Size() && text[end] != u8'\n')
            ++end;
        const StringView line = text.AsView().SubStr(start, end - start);
        const bool added = line.ContainsIgnoreCase(u8"\"lastOpened\"") || line.ContainsIgnoreCase(u8"\"favourite\"") ||
                           line.ContainsIgnoreCase(u8"\"sortByName\"") || line.ContainsIgnoreCase(u8"\"lastLocation\"");
        if (!added)
        {
            old += line;
            old += u8"\n";
        }
        start = end + 1;
    }
    CHECK(old.Size() < text.Size());
    REQUIRE(WriteFile(file.AsView(), Span<const byte>(reinterpret_cast<const byte*>(old.Data()), old.Size())).IsOk());
    settings::Settings older(foundation::core::DefaultAllocator());
    REQUIRE(LoadEditorSettings(fs, older).IsOk());
    const RecentProjectsSettings& olderReg = older.Section<RecentProjectsSettings>();
    REQUIRE(olderReg.entries.Size() == 2u);
    CHECK(olderReg.Find(u8"/projects/a")->lastOpened == 0);
    CHECK_FALSE(olderReg.Find(u8"/projects/a")->favourite);
    CHECK_FALSE(olderReg.sortByName);
    CHECK(olderReg.lastLocation.IsEmpty());

    FileDelete(file);
    RemoveDirectory(dir);
}

TEST_CASE("project-registry: a missing project is relocated in place; arranging pins, sorts and searches")
{
    RegisterProjectRegistryTypes();
    settings::Settings store(foundation::core::DefaultAllocator());
    TouchRecentProject(store, u8"/games/zeta", u8"Zeta", u8"0.1.0", 10);
    TouchRecentProject(store, u8"/old/Alpha", u8"alpha", u8"0.1.0", 20);
    TouchRecentProject(store, u8"/games/Mid", u8"Mid", u8"0.1.0", 30);
    RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();

    // Locate...: the entry takes the new folder and keeps its place; a folder already listed is refused.
    CHECK(RelocateRecentProject(store, u8"/old/Alpha", u8"/games/Alpha"));
    CHECK(reg.entries[1].path.AsView() == u8"/games/Alpha");
    CHECK(reg.entries[1].lastOpened == 20);
    CHECK_FALSE(RelocateRecentProject(store, u8"/games/Alpha", u8"/games/zeta"));
    CHECK_FALSE(RelocateRecentProject(store, u8"/nowhere", u8"/games/new"));

    // By recency (the list's order), by name in any case, and a pinned one first in both.
    auto names = [](const Array<const RecentProjectEntry*>& list)
    {
        String out;
        for (const RecentProjectEntry* e : list)
            out += Format(u8"{} ", e->name);
        return out;
    };
    CHECK(names(RecentProjects::Arrange(reg, {}, false)).AsView() == u8"Mid alpha Zeta ");
    CHECK(names(RecentProjects::Arrange(reg, {}, true)).AsView() == u8"alpha Mid Zeta ");
    REQUIRE(SetRecentProjectFavourite(store, u8"/games/zeta", true));
    CHECK(names(RecentProjects::Arrange(reg, {}, false)).AsView() == u8"Zeta Mid alpha ");
    CHECK(names(RecentProjects::Arrange(reg, {}, true)).AsView() == u8"Zeta alpha Mid ");

    // Search looks at the name and the folder, in any case.
    CHECK(names(RecentProjects::Arrange(reg, u8"ALP", false)).AsView() == u8"alpha ");
    CHECK(names(RecentProjects::Arrange(reg, u8"games/m", false)).AsView() == u8"Mid ");
    CHECK(RecentProjects::Arrange(reg, u8"nothing", false).IsEmpty());
}

TEST_CASE("project-registry: when a project was opened is said for a person, by the local calendar")
{
    const i64 now = 1793539320; // 2026-11-01 13:22 UTC
    CHECK(RecentProjects::DescribeLastOpened(0, now, 0).IsEmpty()); // never kept one
    CHECK(RecentProjects::DescribeLastOpened(now - 20, now, 0).AsView() == u8"Just now");
    CHECK(RecentProjects::DescribeLastOpened(now - 60, now, 0).AsView() == u8"1 minute ago");
    CHECK(RecentProjects::DescribeLastOpened(now - 45 * 60, now, 0).AsView() == u8"45 minutes ago");
    CHECK(RecentProjects::DescribeLastOpened(now - 3 * 3600, now, 0).AsView() == u8"3 hours ago");
    CHECK(RecentProjects::DescribeLastOpened(now - 14 * 3600, now, 0).AsView() == u8"Yesterday"); // 23:22 the day before
    CHECK(RecentProjects::DescribeLastOpened(now - 3 * 86400, now, 0).AsView() == u8"3 days ago");
    CHECK(RecentProjects::DescribeLastOpened(now - 20 * 86400, now, 0).AsView() == u8"2 weeks ago");
    CHECK(RecentProjects::DescribeLastOpened(now - 60 * 86400, now, 0).AsView() == u8"2 Sep 2026");
    // The day turns at local midnight: 14 hours ago is today where it is 13 hours later in the day.
    CHECK(RecentProjects::DescribeLastOpened(now - 14 * 3600, now, 10 * 3600).AsView() == u8"14 hours ago");

    CHECK(RecentProjects::DescribeTime(0, 0).IsEmpty());
    CHECK(RecentProjects::DescribeTime(now, 0).AsView() == u8"1 Nov 2026, 13:22");
    CHECK(RecentProjects::DescribeTime(now - 13 * 3600 - 20 * 60, 0).AsView() == u8"1 Nov 2026, 00:02");
    CHECK(RecentProjects::DescribeTime(now, -14 * 3600).AsView() == u8"31 Oct 2026, 23:22");
}

TEST_CASE("project-registry: a folder under the home shows as ~, a whole folder name only")
{
    CHECK(RecentProjects::ShortenHomePath(u8"/home/rob/Games/Snowline", u8"/home/rob").AsView() == u8"~/Games/Snowline");
    CHECK(RecentProjects::ShortenHomePath(u8"/home/rob/Games", u8"/home/rob/").AsView() == u8"~/Games");
    CHECK(RecentProjects::ShortenHomePath(u8"/home/rob", u8"/home/rob").AsView() == u8"~");
    CHECK(RecentProjects::ShortenHomePath(u8"/home/robert/Games", u8"/home/rob").AsView() == u8"/home/robert/Games");
    CHECK(RecentProjects::ShortenHomePath(u8"/media/usb/Jam", u8"/home/rob").AsView() == u8"/media/usb/Jam");
    CHECK(RecentProjects::ShortenHomePath(u8"C:\\Users\\rob\\Games", u8"C:\\Users\\rob").AsView() == u8"~\\Games");
    CHECK(RecentProjects::ShortenHomePath(u8"/home/rob/Games", u8"").AsView() == u8"/home/rob/Games");
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

    // A freshly scaffolded project is stamped with this engine, so it opens directly.
    const StringView dir = u8"scratch_manager_gate_test";
    RemoveProjectTree(dir);
    REQUIRE(controller.Create(dir, u8"Gate Test").IsOk());
    controller.DecideOpen(dir, decision);
    CHECK(decision.gate == ProjectOpenGate::OpenDirectly);
    CHECK(decision.probed.name.AsView() == u8"Gate Test");

    // Rewrite the manifest with a newer stamp as a raw text edit: SaveProjectSettings always
    // stamps the current engine, so it cannot write a foreign version.
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
    CHECK(controller.SetFavourite(dir, true));
    CHECK(controller.Entries().entries[0].favourite);
    controller.SetSortByName(true);
    CHECK(controller.SortByName());
    controller.SetLastLocation(u8"/home/me/Games");
    CHECK(controller.LastLocation() == StringView(u8"/home/me/Games"));
    CHECK(controller.Remove(dir));
    CHECK(controller.Entries().entries.IsEmpty());

    // Locate... takes only a folder holding a project, and refreshes the entry from it; Remove
    // missing takes off the list every entry whose folder holds none, and touches no files.
    controller.NoteOpened(u8"scratch_manager_gone", u8"Old name", u8"0.0.1");
    controller.NoteOpened(u8"scratch_manager_gone_too", u8"Gone", u8"0.0.1");
    controller.NoteOpened(dir, u8"Gate Test", u8"0.1.0");
    CHECK_FALSE(controller.Relocate(u8"scratch_manager_gone", u8"scratch_manager_no_such_dir"));
    CHECK_FALSE(controller.Relocate(u8"scratch_manager_gone", dir)); // listed already
    CHECK(controller.Remove(dir));
    CHECK(controller.Relocate(u8"scratch_manager_gone", dir));
    const RecentProjectEntry* moved = controller.Entries().Find(dir);
    REQUIRE(moved != nullptr);
    CHECK(moved->name.AsView() == u8"Gate Test");
    CHECK(controller.RemoveMissing() == 1u);
    REQUIRE(controller.Entries().entries.Size() == 1u);
    CHECK(controller.Entries().entries[0].path.AsView() == dir);
    CHECK(DirectoryExists(dir));

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
        project->Settings().defaultBusLayoutId = busId; // the per-field move must not drop this
        project->Settings().guiLoadingDocumentId = loadingId; // nor these two
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
    // A section type registered without RegisterSerializable breaks the store: the first
    // save that includes it makes every later Load abort mid-file, silently dropping the
    // sections after it (an empty recent-projects list, for example).
    RegisterEditorSettingsTypes();
    RegisterProjectRegistryTypes();
    // Every ISerializable registered in the Editor domain, asked of the registry rather than a
    // list kept by hand here, so a new section is covered without editing this test.
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
    CHECK(checked >= 6u); // at least export, font, ui, mcp, shortcuts and recent projects
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
    CHECK(ui->scheme == String(u8"light")); // the look's inputs (appended fields)
    CHECK(ui->accent == String(u8"#0f8a7e"));
    CHECK(ui->density == String(u8"comfortable"));
    CHECK(ui->reduceMotion);
}

// The Game tab's preview resolutions seed the common sizes once, a size the user removes stays
// removed, and the list round-trips through the store; the per-project choice is kept in the
// project store.
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

TEST_CASE("project-registry: New Project's check names where it goes, and refuses what cannot be made")
{
    using Level = NewProjectCheckLevel;
    const String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_new_project_check");
    (void)RemoveDirectoryRecursive(root.AsView());
    REQUIRE(CreateDirectories(PathJoin(root.AsView(), u8"Taken").AsView()));
    REQUIRE(WriteFile(PathJoin(root.AsView(), u8"Taken/notes.txt").AsView(), Span<const byte>()).IsOk());
    REQUIRE(CreateDirectories(PathJoin(root.AsView(), u8"Empty").AsView()));
    REQUIRE(EditorProject::Create(DefaultAllocator(), PathJoin(root.AsView(), u8"Made").AsView(), u8"Made").IsOk());
    // `in` is under the scratch root (empty: the root itself), which shows as ~.
    const auto check = [&root](StringView name, StringView in, bool ownFolder)
    {
        const String location = in.IsEmpty() ? root : PathJoin(root.AsView(), in);
        return ProjectManagerController::CheckNewProject(name, location.AsView(), ownFolder, root.AsView());
    };

    // Green: a new folder of its name in a folder that exists.
    NewProjectCheck c = ProjectManagerController::CheckNewProject(u8"Harbour", root.AsView(), true, root.AsView());
    CHECK(c.level == Level::Ok);
    CHECK(c.target == PathJoin(root.AsView(), u8"Harbour"));
    CHECK(c.message.AsView() == u8"Creates ~/Harbour, and opens it.");
    // Without its own folder, an empty folder takes the project.
    c = check(u8"Harbour", u8"Empty", false);
    CHECK(c.level == Level::Ok);
    CHECK(c.target == PathJoin(root.AsView(), u8"Empty"));
    CHECK(c.message.AsView() == u8"Creates Harbour in ~/Empty, and opens it.");
    // A sample's download says what it does instead.
    c = ProjectManagerController::CheckNewProject(u8"Snowline", root.AsView(), true, root.AsView(), NewProjectAction::Download);
    CHECK(c.level == Level::Ok);
    CHECK(c.message.AsView() == u8"Downloads into ~/Snowline.");
    CHECK(check(u8"Snowline", u8"Empty", false).level == Level::Ok);
    CHECK(ProjectManagerController::CheckNewProject(u8"Snowline", PathJoin(root.AsView(), u8"Empty").AsView(), false, root.AsView(), NewProjectAction::Download)
              .message.AsView() == u8"Downloads Snowline into ~/Empty.");

    // Red: what cannot be made, and Create waits.
    CHECK(ProjectManagerController::CheckNewProject(u8"  ", root.AsView(), true, {}).message.AsView() == u8"Give the project a name.");
    c = ProjectManagerController::CheckNewProject(u8"A/B", root.AsView(), true, {});
    CHECK(c.level == Level::Error);
    CHECK(c.message.AsView() == u8"A name cannot hold / \\ : * ? \" < > or |.");
    CHECK(ProjectManagerController::CheckNewProject(u8"Why?", root.AsView(), true, {}).level == Level::Error);
    CHECK(ProjectManagerController::CheckNewProject(u8"Harbour", u8" ", true, {}).message.AsView() == u8"Choose the folder it is made in.");
    c = check(u8"Taken", u8"", true);
    CHECK(c.level == Level::Error);
    CHECK(c.message.AsView() == u8"~/Taken already exists and has files in it. Choose another name or folder.");
    CHECK(check(u8"Harbour", u8"Taken", false).message.AsView() ==
          u8"~/Taken already has files in it. A project needs an empty folder: turn on the folder option, or choose an empty one.");
    CHECK(check(u8"Made", u8"", true).message.AsView() == u8"~/Made is a project already: Open Folder opens it.");
    CHECK(check(u8"Harbour", u8"Taken/notes.txt", true).message.AsView() == u8"~/Taken/notes.txt is a file, not a folder.");

    // Amber: it goes ahead, but says what it will do.
    c = check(u8"Harbour", u8"Later/Games", true);
    CHECK(c.level == Level::Warning);
    CHECK(c.message.AsView() == u8"~/Later/Games does not exist yet; it will be made, then Harbour inside it.");
    CHECK(check(u8"Harbour", u8"Later", false).message.AsView() == u8"~/Later does not exist yet; it will be made, and the project made in it.");
    c = check(u8" Harbour ", u8"", true);
    CHECK(c.level == Level::Warning);
    CHECK(c.name.AsView() == u8"Harbour");
    CHECK(c.message.AsView() == u8"The name starts or ends with a space; the folder will be ~/Harbour.");
    CHECK_FALSE(DirectoryExists(PathJoin(root.AsView(), u8"Later").AsView())); // checking makes nothing
    (void)RemoveDirectoryRecursive(root.AsView());
}

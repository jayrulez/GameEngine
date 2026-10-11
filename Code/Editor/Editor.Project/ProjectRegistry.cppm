// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The per-user project registry - the headless core of the built-in project manager,
// the RecentProjects section.
//
// The registry is a Settings SECTION in the user-level editor settings store
// (<user-data>/editor.settings.xml), deliberately OUTSIDE any engine install or project
// directory: every editor version on the machine lists the same projects, and a
// separate launcher/hub can read the same file. Ordering IS recency (most recent first) -
// no timestamps to go stale. Display data (name, engine version) is a cached snapshot,
// refreshed every time the project is touched; ProbeProject reads the live manifest when
// the list needs truth (missing dir, out-of-date stamp).
//
// Also here: the engine-version relation used by the open-time upgrade prompt, and the
// manifest backup that prompt offers ("back up Project.xml, then open with this engine").

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:project_registry;

import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.xml.serialization;
import engine.project;

using namespace foundation::core;

export namespace editor
{
    namespace vfs = foundation::vfs;
    namespace settings = foundation::settings;
    namespace project = engine::project;

    // One known project. `path` is the project DIRECTORY as the user opened it (the identity
    // key - compared verbatim); name/engineVersion are the last-seen manifest snapshot.
    struct RecentProjectEntry
    {
        String path;
        String name;
        String engineVersion;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "path", path);
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "engineVersion", engineVersion);
        }
    };

    inline void Serialize(ISerializer& ar, RecentProjectEntry& e)
    {
        ar.BeginObject();
        e.Serialize(ar);
        ar.EndObject();
    }

    // The registry section (user-level settings store). Most recent FIRST.
    class RecentProjectsSettings final : public ISerializable
    {
        RTTI_OBJECT(RecentProjectsSettings, ISerializable)
    public:
        // ENGINE_EXPORT_DATA: odr-used from another library (shared-libraries.md P5/W1).
        static constexpr ENGINE_EXPORT_DATA usize kMaxEntries = 20;

        Array<RecentProjectEntry> entries;
        // The launcher shows the projects as a grid of their pictures, else as a list.
        bool gridView = false;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "entries", entries);
            SerializeAppended(ar, "gridView", gridView); // after the rest: older stores lack it
        }

        [[nodiscard]] const RecentProjectEntry* Find(StringView path) const
        {
            for (const RecentProjectEntry& e : entries)
            {
                if (e.path.AsView() == path)
                {
                    return &e;
                }
            }
            return nullptr;
        }
    };

    // Register the registry section type so a Settings store can instantiate it on Load. Call
    // once at editor startup alongside RegisterEditorSettingsTypes.
    inline void RegisterProjectRegistryTypes()
    {
        GlobalTypeRegistry().Register(RecentProjectsSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<RecentProjectsSettings>();
    }

    // Record `path` as the most recently opened project (insert or move-to-front), refreshing
    // its display snapshot. Caps the list at kMaxEntries. Marks the section changed; the caller
    // persists the store (SaveEditorSettingsToUserData).
    inline void TouchRecentProject(settings::Settings& store, StringView path, StringView name,
                                   StringView engineVersion)
    {
        RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
        RecentProjectEntry entry;
        entry.path = String(path);
        entry.name = String(name);
        entry.engineVersion = String(engineVersion);
        for (usize i = 0; i < reg.entries.Size(); ++i)
        {
            if (reg.entries[i].path.AsView() == path)
            {
                reg.entries.RemoveAt(i);
                break;
            }
        }
        reg.entries.Insert(0, Move(entry));
        while (reg.entries.Size() > RecentProjectsSettings::kMaxEntries)
        {
            reg.entries.RemoveAt(reg.entries.Size() - 1);
        }
        store.MarkChanged<RecentProjectsSettings>();
    }

    // Remove `path` from the registry (a dead/unwanted row in the manager). True if removed.
    inline bool RemoveRecentProject(settings::Settings& store, StringView path)
    {
        RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
        for (usize i = 0; i < reg.entries.Size(); ++i)
        {
            if (reg.entries[i].path.AsView() == path)
            {
                reg.entries.RemoveAt(i);
                store.MarkChanged<RecentProjectsSettings>();
                return true;
            }
        }
        return false;
    }

    // Read a project's manifest WITHOUT opening the project (no content DBs, no dir scaffolding).
    // NotFound: no manifest at `directory` (not a project / moved / deleted).
    [[nodiscard]] inline Status ProbeProject(StringView directory, engine::project::ProjectSettings& out)
    {
        if (!DirectoryExists(directory))
        {
            return Status{ErrorCode::NotFound};
        }
        vfs::NativeFileSystem fs(directory, foundation::core::DefaultAllocator());
        return project::LoadProjectSettings(fs, out);
    }

    // How a project's stamped engine version relates to THIS engine. Drives the manager's
    // open-time prompt: Same/Unstamped open silently; ProjectOlder offers backup-then-upgrade;
    // ProjectNewer warns hard (this editor may not read newer data).
    enum class EngineVersionRelation
    {
        Same,
        ProjectOlder,
        ProjectNewer,
        Unstamped, // empty or unparseable stamp (pre-v2 manifest)
    };

    [[nodiscard]] inline EngineVersionRelation
    CompareProjectEngineVersion(StringView projectVersion)
    {
        if (projectVersion.IsEmpty())
        {
            return EngineVersionRelation::Unstamped;
        }
        // Parse "major.minor.patch" (all three numeric, dot-separated).
        u32 parts[3] = {0, 0, 0};
        usize part = 0;
        bool anyDigit = false;
        for (usize i = 0; i < projectVersion.Size(); ++i)
        {
            const char8_t c = projectVersion[i];
            if (c >= u8'0' && c <= u8'9')
            {
                parts[part] = parts[part] * 10u + static_cast<u32>(c - u8'0');
                anyDigit = true;
            }
            else if (c == u8'.' && part < 2 && anyDigit)
            {
                ++part;
                anyDigit = false;
            }
            else
            {
                return EngineVersionRelation::Unstamped;
            }
        }
        if (part != 2 || !anyDigit)
        {
            return EngineVersionRelation::Unstamped;
        }
        const u32 engine[3] = {project::kEngineVersionMajor, project::kEngineVersionMinor,
                               project::kEngineVersionPatch};
        for (usize i = 0; i < 3; ++i)
        {
            if (parts[i] < engine[i])
            {
                return EngineVersionRelation::ProjectOlder;
            }
            if (parts[i] > engine[i])
            {
                return EngineVersionRelation::ProjectNewer;
            }
        }
        return EngineVersionRelation::Same;
    }

    // Back up the manifest before an engine upgrade rewrites it: copies Project.xml to
    // "Project.xml.<stampedVersion>.bak" ("unstamped" when the manifest predates the stamp)
    // beside it, overwriting an older backup for the SAME version. Returns the backup path.
    // Content assets are NOT copied - they migrate through their own versioned serializers,
    // and whole-tree safety is version control's job (the prompt says so).
    [[nodiscard]] inline Result<String> BackupProjectManifest(StringView directory)
    {
        engine::project::ProjectSettings probed;
        if (Status s = ProbeProject(directory, probed); !s.IsOk())
        {
            return Err(s.Code());
        }
        const String manifest = PathJoin(directory, project::kProjectManifestFile);
        String backup = manifest;
        backup += u8".";
        backup += probed.engineVersion.IsEmpty() ? String(u8"unstamped") : probed.engineVersion;
        backup += u8".bak";
        if (!FileCopyPreserving(manifest.AsView(), backup.AsView()))
        {
            return Err(ErrorCode::Unknown);
        }
        return backup;
    }

    RTTI_DEFINE_OBJECT_VERSIONED(RecentProjectsSettings, "rtti::editor::editor", 1)
}

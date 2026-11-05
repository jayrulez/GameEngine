// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The per-user project registry, the headless core of the built-in project manager: the
// RecentProjects section.
//
// The registry is a Settings section in the user-level editor settings store
// (<user-data>/editor.settings.xml), outside any engine install or project directory, so every
// editor version on the machine lists the same projects and a separate launcher can read the same
// file. The order is the recency (most recent first), and each entry keeps when it was last opened
// (Unix seconds, which the manager shows as "2 hours ago") and whether the user pinned it. Display
// data (name, engine version) is a cached snapshot, refreshed every time the project is touched;
// ProbeProject reads the live manifest when the list needs the current state (a missing directory,
// an out-of-date stamp).
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

    // One known project. `path` is the project directory as the user opened it (the identity key,
    // compared exactly); name and engineVersion are the last-seen manifest snapshot.
    struct RecentProjectEntry
    {
        String path;
        String name;
        String engineVersion;
        i64 lastOpened = 0;     // Unix seconds; 0 for an entry from before the editor kept it
        bool favourite = false; // pinned: listed before the rest, whatever the order

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "path", path);
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "engineVersion", engineVersion);
            SerializeAppended(ar, "lastOpened", lastOpened); // after the rest, since older stores lack them
            SerializeAppended(ar, "favourite", favourite);
        }
    };

    inline void Serialize(ISerializer& ar, RecentProjectEntry& e)
    {
        ar.BeginObject();
        e.Serialize(ar);
        ar.EndObject();
    }

    /// RecentProjectsSettings::kMaxEntries, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr usize kRecentProjectsSettingsMaxEntries = 20;

    // The registry section (user-level settings store), most recent first.
    class RecentProjectsSettings final : public ISerializable
    {
        RTTI_OBJECT(RecentProjectsSettings, ISerializable)
    public:
        // ENGINE_EXPORT_DATA: odr-used from another library.
        static constexpr ENGINE_EXPORT_DATA usize kMaxEntries = kRecentProjectsSettingsMaxEntries;

        Array<RecentProjectEntry> entries;
        // The launcher shows the projects as a grid of their pictures, else as a list.
        bool gridView = false;
        // The list's order: by name, else by when each was last opened.
        bool sortByName = false;
        // The folder the last project was made or downloaded in, where the next one starts.
        String lastLocation;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "entries", entries);
            SerializeAppended(ar, "gridView", gridView); // after the rest, since older stores lack them
            SerializeAppended(ar, "sortByName", sortByName);
            SerializeAppended(ar, "lastLocation", lastLocation);
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

    // Registers the registry section type so a Settings store can instantiate it on Load. Call
    // once at editor startup alongside RegisterEditorSettingsTypes.
    inline void RegisterProjectRegistryTypes()
    {
        GlobalTypeRegistry().Register(RecentProjectsSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<RecentProjectsSettings>();
    }

    // Records `path` as the most recently opened project, opened at `openedAt` (inserting it or
    // moving it to the front, its pin kept), and refreshes its display snapshot. Caps the list at
    // kMaxEntries. Marks the section changed; the caller saves the store
    // (SaveEditorSettingsToUserData).
    inline void TouchRecentProject(settings::Settings& store, StringView path, StringView name,
                                   StringView engineVersion, i64 openedAt = GetUnixTime())
    {
        RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
        RecentProjectEntry entry;
        entry.path = String(path);
        entry.name = String(name);
        entry.engineVersion = String(engineVersion);
        entry.lastOpened = openedAt;
        for (usize i = 0; i < reg.entries.Size(); ++i)
        {
            if (reg.entries[i].path.AsView() == path)
            {
                entry.favourite = reg.entries[i].favourite;
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

    // Removes `path` from the registry (a dead or unwanted row in the manager). True if removed.
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

    // Pins or unpins `path`; false when it is not in the list. Marks the section changed.
    inline bool SetRecentProjectFavourite(settings::Settings& store, StringView path, bool favourite)
    {
        RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
        for (RecentProjectEntry& e : reg.entries)
        {
            if (e.path.AsView() == path)
            {
                e.favourite = favourite;
                store.MarkChanged<RecentProjectsSettings>();
                return true;
            }
        }
        return false;
    }

    // Points the entry at `from` at the folder the project is in now (Locate... on a missing
    // project), keeping its place, pin and time. False when `from` is not listed, or `to` already
    // is (that entry stays; the caller removes this one if the user wants).
    inline bool RelocateRecentProject(settings::Settings& store, StringView from, StringView to)
    {
        RecentProjectsSettings& reg = store.Section<RecentProjectsSettings>();
        if (reg.Find(to) != nullptr)
            return false;
        for (RecentProjectEntry& e : reg.entries)
        {
            if (e.path.AsView() == from)
            {
                e.path = String(to);
                store.MarkChanged<RecentProjectsSettings>();
                return true;
            }
        }
        return false;
    }

    // The manager's view of the list: its order, and its words for when and where. Static members
    // of a struct, not functions at namespace scope: gcc 15 wrote module files it could not read
    // back (a "Bad file data" for every unit importing editor.core) for the latter here.
    struct RecentProjects
    {
        // The entries the manager lists, in its order: those matching `query` (in the name or the
        // folder, any case; empty matches all), pinned ones first, then by name or by recency (the
        // list's own order). Pointers into `reg`, valid until it changes.
        [[nodiscard]] static Array<const RecentProjectEntry*> Arrange(const RecentProjectsSettings& reg, StringView query, bool byName)
        {
            Array<const RecentProjectEntry*> out;
            for (const RecentProjectEntry& e : reg.entries)
                if (query.IsEmpty() || e.name.AsView().ContainsIgnoreCase(query) || e.path.AsView().ContainsIgnoreCase(query))
                    out.PushBack(&e);
            const auto lower = [](utf8char c) { return c >= u8'A' && c <= u8'Z' ? static_cast<utf8char>(c - u8'A' + u8'a') : c; };
            const auto nameLess = [lower](StringView a, StringView b)
            {
                for (usize i = 0; i < a.Size() && i < b.Size(); ++i)
                    if (lower(a[i]) != lower(b[i]))
                        return lower(a[i]) < lower(b[i]);
                return a.Size() < b.Size();
            };
            out.Sort([byName, nameLess](const RecentProjectEntry* a, const RecentProjectEntry* b)
                     {
                         if (a->favourite != b->favourite)
                             return a->favourite;
                         return byName && nameLess(a->name.AsView(), b->name.AsView());
                     });
            return out;
        }

        // When a project was last opened, said for a person at `now`: "Just now", "5 minutes ago",
        // "3 hours ago", "Yesterday", "4 days ago", "2 weeks ago", then the date ("19 Oct 2026").
        // Empty for an entry that never kept one (0). `offset` is the local time zone's
        // (GetLocalTimeOffset), so a date is the user's own.
        [[nodiscard]] static String DescribeLastOpened(i64 when, i64 now, i64 offset)
        {
            if (when <= 0)
                return String();
            const i64 ago = now > when ? now - when : 0;
            const auto count = [](i64 n, StringView unit) { return Format(u8"{} {}{} ago", n, unit, n == 1 ? u8"" : u8"s"); };
            if (ago < 60)
                return String(u8"Just now");
            if (ago < 3600)
                return count(ago / 60, u8"minute");
            // From here by calendar days, so "Yesterday" is the day before today, not 24 hours ago.
            const i64 days = (now + offset) / 86400 - (when + offset) / 86400;
            if (days == 0)
                return count(ago / 3600, u8"hour");
            if (days == 1)
                return String(u8"Yesterday");
            if (days < 14)
                return count(days, u8"day");
            if (days < 35)
                return count(days / 7, u8"week");
            static constexpr StringView kMonths[] = {u8"Jan", u8"Feb", u8"Mar", u8"Apr", u8"May", u8"Jun",
                                                     u8"Jul", u8"Aug", u8"Sep", u8"Oct", u8"Nov", u8"Dec"};
            const CivilTime date = CivilTimeOf(when + offset);
            return Format(u8"{} {} {}", date.day, kMonths[date.month - 1], date.year);
        }

        // The exact local time `when` names, for a tooltip: "31 Oct 2026, 18:42". Empty for 0.
        [[nodiscard]] static String DescribeTime(i64 when, i64 offset)
        {
            if (when <= 0)
                return String();
            static constexpr StringView kMonths[] = {u8"Jan", u8"Feb", u8"Mar", u8"Apr", u8"May", u8"Jun",
                                                     u8"Jul", u8"Aug", u8"Sep", u8"Oct", u8"Nov", u8"Dec"};
            const CivilTime t = CivilTimeOf(when + offset);
            return Format(u8"{} {} {}, {}{}:{}{}", t.day, kMonths[t.month - 1], t.year, t.hour < 10 ? u8"0" : u8"", t.hour,
                          t.minute < 10 ? u8"0" : u8"", t.minute);
        }

        // A folder as the manager shows it: under `home`, the home written as "~" ("~/Games/Snowline");
        // anywhere else, or with no home, as it is. Only a whole folder name matches, so "/home/rob"
        // does not shorten "/home/robert".
        [[nodiscard]] static String ShortenHomePath(StringView path, StringView home)
        {
            while (home.Size() > 1 && (home[home.Size() - 1] == u8'/' || home[home.Size() - 1] == u8'\\'))
                home = home.SubStr(0, home.Size() - 1);
            if (home.IsEmpty() || !path.StartsWith(home))
                return String(path);
            if (path.Size() == home.Size())
                return String(u8"~");
            const utf8char next = path[home.Size()];
            if (next != u8'/' && next != u8'\\')
                return String(path);
            String out(u8"~");
            out += path.SubStr(home.Size(), path.Size() - home.Size());
            return out;
        }
    };

    // Reads a project's manifest without opening the project (no content DBs, no directory
    // scaffolding). NotFound when there is no manifest at `directory` (not a project, moved or
    // deleted).
    [[nodiscard]] inline Status ProbeProject(StringView directory, engine::project::ProjectSettings& out)
    {
        if (!DirectoryExists(directory))
        {
            return Status{ErrorCode::NotFound};
        }
        vfs::NativeFileSystem fs(directory, foundation::core::DefaultAllocator());
        return project::LoadProjectSettings(fs, out);
    }

    // How a project's stamped engine version relates to this engine. It drives the manager's
    // open-time prompt: Same opens silently; ProjectOlder and Unstamped offer a backup, then the
    // upgrade; ProjectNewer warns strongly (this editor may not read newer data).
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
        // Parses "major.minor.patch" (all three numeric, separated by dots).
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

    // Backs up the manifest before an engine upgrade rewrites it: copies Project.xml to
    // "Project.xml.<stampedVersion>.bak" beside it ("unstamped" when the manifest has no stamp),
    // overwriting an older backup for the same version. Returns the backup path. Content assets
    // are not copied: they migrate through their own versioned serializers, and the safety of the
    // whole tree is version control's job (the prompt says so).
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

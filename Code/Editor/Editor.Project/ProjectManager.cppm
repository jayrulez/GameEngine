// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The project manager's controller: the headless decision layer between the manager UI and the
// registry and manifest primitives (:project_registry). The UI (editor.app's ProjectManagerView
// and the application's dialogs) renders what this class decides; nothing here touches a view, so
// the open-gate logic and the prompt text are unit-testable.

module;
#include "Core/Prelude.h"

export module editor.project:project_manager;

import foundation.core;
import foundation.settings;
import engine.project;
import :project;
import :project_registry;

using namespace foundation::core;

export namespace editor
{
    namespace settings = foundation::settings;
    namespace project = engine::project;

    // What Open must do for a directory, decided from the manifest probe and the version relation.
    enum class ProjectOpenGate
    {
        NotAProject,       // no readable manifest: refuse (creating is the explicit path)
        OpenDirectly,      // same engine version: open without prompting
        PromptOlderBackup, // older or unstamped: offer a backup, then upgrade (re-stamp on open)
        PromptNewerEngine, // saved by a newer engine: warn strongly before opening
    };

    // How New Project's check line reads: green, amber (it goes ahead), or red (Create waits).
    enum class NewProjectCheckLevel
    {
        Ok,
        Warning,
        Error,
    };
    // What the folder is for: a new project made there, or a sample downloaded into it.
    enum class NewProjectAction
    {
        Create,
        Download,
    };
    // New Project's check, kept current as the fields change: where the project goes, and what to
    // say about it.
    struct NewProjectCheck
    {
        NewProjectCheckLevel level = NewProjectCheckLevel::Error;
        String message;
        String target; // the project's folder (empty when there is none yet)
        String name;   // the project's name, its spaces at either end trimmed
    };

    class ProjectManagerController
    {
    public:
        /// The folder a project named `name` gets: spaces become '-'; letters, digits, '-', '_', '.'
        /// and anything past ASCII (accented letters, other scripts) stay; other characters, those
        /// a file system refuses and those awkward in a path ("'", '!', '&', '#'), go. Runs of '-'
        /// become one, '-' and '.' at either end go, and a name Windows reserves (CON, NUL, COM1)
        /// gets "-Project". Empty when nothing usable is left ("???").
        [[nodiscard]] static String FolderNameFor(StringView name)
        {
            String folder;
            for (const utf8char c : name)
            {
                const bool keep = (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_' ||
                                  c == u8'.' || static_cast<u8>(c) >= 0x80;
                if (keep)
                    folder.Append(c);
                else if ((c == u8' ' || c == u8'-' || c == u8'\t') && !folder.IsEmpty() && folder[folder.Size() - 1] != u8'-')
                    folder.Append(u8'-');
            }
            usize start = 0, end = folder.Size();
            while (start < end && (folder[start] == u8'-' || folder[start] == u8'.'))
                ++start;
            while (end > start && (folder[end - 1] == u8'-' || folder[end - 1] == u8'.'))
                --end;
            folder = String(folder.AsView().SubStr(start, end - start));
            // CON, PRN, AUX, NUL, COM1 to COM9 and LPT1 to LPT9, before any extension, any case.
            StringView stem = folder.AsView();
            for (usize i = 0; i < stem.Size(); ++i)
                if (stem[i] == u8'.')
                {
                    stem = stem.SubStr(0, i);
                    break;
                }
            const auto is = [stem](StringView reserved) { return stem.Size() == reserved.Size() && stem.ContainsIgnoreCase(reserved); };
            const bool numbered = stem.Size() == 4 && (stem.SubStr(0, 3).ContainsIgnoreCase(u8"COM") || stem.SubStr(0, 3).ContainsIgnoreCase(u8"LPT")) &&
                                  stem[3] >= u8'1' && stem[3] <= u8'9';
            if (is(u8"CON") || is(u8"PRN") || is(u8"AUX") || is(u8"NUL") || numbered)
                folder += u8"-Project";
            return folder;
        }

        /// New Project's check for a project named `name` made in `location`, in a folder named
        /// after it there (FolderNameFor) when `ownFolder` is set, else in `location` itself
        /// (Specs/project-manager.md). Red, Create waiting: no name, a name with nothing a folder
        /// can be named by, no location, a location that is a file, a target that is a project
        /// already or holds files. Amber: the location does not exist yet (it is made). Else green.
        /// Folders in the message show under `home` as ~. A sample's download dialog asks the same
        /// with `action` Download, and the green line says so.
        [[nodiscard]] static NewProjectCheck CheckNewProject(StringView name, StringView location, bool ownFolder, StringView home,
                                                             NewProjectAction action = NewProjectAction::Create)
        {
            NewProjectCheck check;
            const StringView trimmed = TrimSpaces(name);
            check.name = String(trimmed);
            const auto fail = [&check](String message)
            {
                check.level = NewProjectCheckLevel::Error;
                check.message = Move(message);
                return check;
            };
            if (trimmed.IsEmpty())
                return fail(String(u8"Give the project a name."));
            const String folder = FolderNameFor(trimmed);
            if (ownFolder && folder.IsEmpty())
                return fail(String(u8"The name needs a letter or a digit for its folder."));
            const StringView where = TrimSpaces(location);
            if (where.IsEmpty())
                return fail(String(u8"Choose the folder it is made in."));
            const String shownWhere = RecentProjects::ShortenHomePath(where, home);
            if (!DirectoryExists(where) && FileExists(where))
                return fail(Format(u8"{} is a file, not a folder.", shownWhere));
            check.target = ownFolder ? PathJoin(where, folder.AsView()) : String(where);
            const String shownTarget = RecentProjects::ShortenHomePath(check.target.AsView(), home);
            if (FileExists(PathJoin(check.target.AsView(), u8"Project.xml").AsView()))
                return fail(Format(u8"{} is a project already: Open Folder opens it.", shownTarget));
            if (!DirectoryExists(check.target.AsView()) && FileExists(check.target.AsView()))
                return fail(Format(u8"{} is a file, not a folder.", shownTarget));
            if (HoldsAnything(check.target.AsView()))
                return fail(ownFolder ? Format(u8"{} already exists and has files in it. Choose another name or folder.", shownTarget)
                                      : Format(u8"{} already has files in it. A project needs an empty folder: turn on the folder "
                                               u8"option, or choose an empty one.",
                                               shownTarget));
            check.level = NewProjectCheckLevel::Warning;
            if (!DirectoryExists(where))
            {
                check.message = ownFolder ? Format(u8"{} does not exist yet; it will be made, then {} inside it.", shownWhere, folder)
                                          : Format(u8"{} does not exist yet; it will be made, and the project made in it.", shownWhere);
                return check;
            }
            check.level = NewProjectCheckLevel::Ok;
            if (action == NewProjectAction::Download)
                check.message = ownFolder ? Format(u8"Downloads into {}.", shownTarget) : Format(u8"Downloads {} into {}.", trimmed, shownTarget);
            else
                check.message = ownFolder ? Format(u8"Creates {}, and opens it.", shownTarget) : Format(u8"Creates {} in {}, and opens it.", trimmed, shownTarget);
            return check;
        }

        explicit ProjectManagerController(settings::Settings& store) : m_store(&store) {}

        /// Whether opening through `gate`, once the user agrees, stamps the project with this
        /// engine: an older engine's project is upgraded (backed up first or not, as asked); a
        /// newer engine's is opened as it is, keeping its stamp, so the newer engine still knows
        /// it as its own; one of this engine's needs nothing.
        [[nodiscard]] static constexpr bool RestampsOnOpen(ProjectOpenGate gate) noexcept { return gate == ProjectOpenGate::PromptOlderBackup; }

        struct OpenDecision
        {
            ProjectOpenGate gate = ProjectOpenGate::NotAProject;
            engine::project::ProjectSettings probed; // valid unless NotAProject
            String promptTitle;              // set for the two prompt gates
            String promptBody;
        };

        // Probes and classifies `directory` and writes the prompt text into `decision` (an out
        // parameter, since ProjectSettings is neither copyable nor movable). It only decides: the
        // caller shows dialogs for the prompt gates and calls the open path it owns.
        void DecideOpen(StringView directory, OpenDecision& decision) const
        {
            decision.gate = ProjectOpenGate::NotAProject;
            decision.promptTitle.Clear();
            decision.promptBody.Clear();
            if (!ProbeProject(directory, decision.probed).IsOk())
            {
                return;
            }
            const EngineVersionRelation relation =
                CompareProjectEngineVersion(decision.probed.engineVersion.AsView());
            if (relation == EngineVersionRelation::Same)
            {
                decision.gate = ProjectOpenGate::OpenDirectly;
                return;
            }
            const String stamped = decision.probed.engineVersion.IsEmpty()
                                       ? String(u8"an unknown version")
                                       : decision.probed.engineVersion;
            if (relation == EngineVersionRelation::ProjectNewer)
            {
                decision.gate = ProjectOpenGate::PromptNewerEngine;
                decision.promptTitle = String(u8"Project from a newer engine");
                decision.promptBody = String(u8"This project was last saved by engine ");
                decision.promptBody += stamped;
                decision.promptBody += u8" - NEWER than this editor (";
                decision.promptBody += project::kEngineVersionString;
                decision.promptBody +=
                    u8"). Opening may lose or misread data. Open it with the newer engine "
                    u8"instead if you can.";
                return;
            }
            decision.gate = ProjectOpenGate::PromptOlderBackup;
            decision.promptTitle = String(u8"Open with this engine version?");
            decision.promptBody = String(u8"This project was last saved by engine ");
            decision.promptBody += stamped;
            decision.promptBody += u8"; this editor is ";
            decision.promptBody += project::kEngineVersionString;
            decision.promptBody +=
                u8". Back up Project.xml before opening? (Content migrates as assets "
                u8"re-save; use version control for whole-project safety.)";
        }

        // Scaffolds a new project at `directory` without opening it; the caller opens it on
        // success.
        [[nodiscard]] Status Create(StringView directory, StringView name) const
        {
            return EditorProject::Create(foundation::core::DefaultAllocator(), directory, name);
        }

        // The pre-upgrade manifest backup (see :project_registry). Returns the backup path.
        [[nodiscard]] Result<String> BackupManifest(StringView directory) const
        {
            return BackupProjectManifest(directory);
        }

        // Registry changes. NoteOpened records a successful open (most recent first, with a
        // fresh display snapshot); the caller persists the store afterward.
        void NoteOpened(StringView directory, StringView name, StringView engineVersion)
        {
            TouchRecentProject(*m_store, directory, name, engineVersion);
        }
        bool Remove(StringView directory) { return RemoveRecentProject(*m_store, directory); }

        [[nodiscard]] const RecentProjectsSettings& Entries()
        {
            return m_store->Section<RecentProjectsSettings>(); // creates the section lazily, so not const
        }
        [[nodiscard]] settings::Settings& Store() noexcept { return *m_store; }

        /// Whether the launcher shows a grid of the projects' pictures (else a list); kept in the
        /// store with the recent projects (the caller saves it, as after Remove).
        [[nodiscard]] bool GridView() { return m_store->Section<RecentProjectsSettings>().gridView; }
        void SetGridView(bool grid)
        {
            m_store->Section<RecentProjectsSettings>().gridView = grid;
            m_store->MarkChanged<RecentProjectsSettings>();
        }

        /// Pins or unpins a project (pinned ones are listed first); false when it is not listed.
        bool SetFavourite(StringView directory, bool favourite) { return SetRecentProjectFavourite(*m_store, directory, favourite); }
        /// Points a missing project's entry at the folder it is in now; false when `to` holds no
        /// project, or is listed already.
        bool Relocate(StringView from, StringView to)
        {
            engine::project::ProjectSettings probed;
            if (!ProbeProject(to, probed).IsOk() || !RelocateRecentProject(*m_store, from, to))
                return false;
            TouchSnapshot(to, probed);
            return true;
        }
        /// Removes every project whose folder no longer holds it; how many went. Files are never
        /// touched: only the list changes.
        usize RemoveMissing()
        {
            RecentProjectsSettings& reg = m_store->Section<RecentProjectsSettings>();
            usize removed = 0;
            for (usize i = reg.entries.Size(); i-- > 0;)
            {
                engine::project::ProjectSettings probed;
                if (!ProbeProject(reg.entries[i].path.AsView(), probed).IsOk())
                {
                    reg.entries.RemoveAt(i);
                    ++removed;
                }
            }
            if (removed > 0)
                m_store->MarkChanged<RecentProjectsSettings>();
            return removed;
        }

        /// The list's order, by name or by when each was last opened.
        [[nodiscard]] bool SortByName() { return m_store->Section<RecentProjectsSettings>().sortByName; }
        void SetSortByName(bool byName)
        {
            m_store->Section<RecentProjectsSettings>().sortByName = byName;
            m_store->MarkChanged<RecentProjectsSettings>();
        }
        /// Where the last project was made or downloaded: where New Project's location starts.
        [[nodiscard]] StringView LastLocation() { return m_store->Section<RecentProjectsSettings>().lastLocation.AsView(); }
        void SetLastLocation(StringView folder)
        {
            m_store->Section<RecentProjectsSettings>().lastLocation = String(folder);
            m_store->MarkChanged<RecentProjectsSettings>();
        }

    private:
        [[nodiscard]] static StringView TrimSpaces(StringView text)
        {
            usize start = 0, end = text.Size();
            while (start < end && text[start] == u8' ')
                ++start;
            while (end > start && text[end - 1] == u8' ')
                --end;
            return text.SubStr(start, end - start);
        }
        // Whether a folder holds any file or folder (false when it does not exist).
        [[nodiscard]] static bool HoldsAnything(StringView directory)
        {
            bool any = false;
            (void)ListDirectory(directory, [](void* found, StringView, bool) { *static_cast<bool*>(found) = true; }, &any);
            return any;
        }

        // Refreshes an entry's name and version from its manifest, keeping its place and time.
        void TouchSnapshot(StringView directory, const engine::project::ProjectSettings& probed)
        {
            RecentProjectsSettings& reg = m_store->Section<RecentProjectsSettings>();
            for (RecentProjectEntry& e : reg.entries)
                if (e.path.AsView() == directory)
                {
                    e.name = probed.name;
                    e.engineVersion = probed.engineVersion;
                }
        }

        settings::Settings* m_store;
    };
}

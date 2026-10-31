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

    class ProjectManagerController
    {
    public:
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

    private:
        settings::Settings* m_store;
    };
}

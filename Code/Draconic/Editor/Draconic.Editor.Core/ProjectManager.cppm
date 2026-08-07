// The project-manager CONTROLLER - the headless decision layer between the manager UI and
// the registry/manifest primitives (:project_registry). The UI (draconic.editor.app's
// ProjectManagerView + the application's dialogs) renders what this class decides; nothing
// here touches a view, so the open-gate logic and prompt copy are unit-testable.
//
// Future home of project TEMPLATES (create-from-template picks a scaffold here, not in UI).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.core:project_manager;

import draconic.foundation;
import draconic.settings;
import draconic.engine.project;
import :project;
import :project_registry;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace settings = draconic::settings;
    namespace project = draconic::project;

    // What Open must do for a directory, decided from the manifest probe + version relation.
    enum class ProjectOpenGate
    {
        NotAProject,       // no readable manifest - refuse (create is the explicit path)
        OpenDirectly,      // same engine version - no ceremony
        PromptOlderBackup, // older/unstamped - offer backup-then-upgrade (re-stamp on open)
        PromptNewerEngine, // saved by a NEWER engine - warn hard before opening
    };

    class ProjectManagerController
    {
    public:
        explicit ProjectManagerController(settings::Settings& store) : m_store(&store) {}

        struct OpenDecision
        {
            ProjectOpenGate gate = ProjectOpenGate::NotAProject;
            project::ProjectSettings probed; // valid unless NotAProject
            String promptTitle;              // set for the two prompt gates
            String promptBody;
        };

        // Probe + classify + compose the prompt copy for `directory` into `decision` (an
        // out-param: ProjectSettings is neither copyable nor movable). Pure decision - the
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

        // Scaffold a new project at `directory` (no open - the caller opens on success).
        // Later: a template parameter selects the scaffold instead of the bare layout.
        [[nodiscard]] Status Create(StringView directory, StringView name) const
        {
            return EditorProject::Create(directory, name);
        }

        // The pre-upgrade manifest backup (see :project_registry). Returns the backup path.
        [[nodiscard]] Result<String> BackupManifest(StringView directory) const
        {
            return BackupProjectManifest(directory);
        }

        // Registry mutations. NoteOpened records a successful open (most-recent-first with a
        // fresh display snapshot); the caller persists the store afterward.
        void NoteOpened(StringView directory, StringView name, StringView engineVersion)
        {
            TouchRecentProject(*m_store, directory, name, engineVersion);
        }
        bool Remove(StringView directory) { return RemoveRecentProject(*m_store, directory); }

        [[nodiscard]] const RecentProjectsSettings& Entries()
        {
            return m_store->Section<RecentProjectsSettings>(); // lazily creates: non-const
        }
        [[nodiscard]] settings::Settings& Store() noexcept { return *m_store; }

    private:
        settings::Settings* m_store;
    };
}

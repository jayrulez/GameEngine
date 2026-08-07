// Draconic::EditorApp - :project_manager_view partition.
//
// The PROJECT MANAGER screen (built into the single editor exe): shown at startup when no
// project was given on the command line, and returned to by File > Close Project.
//
// Design: no selection state at all. The standalone entry points (New Project... /
// Open Folder...) sit at the top and are always available; below them, every recent
// project is a CARD carrying its own [Open] and [Remove] buttons - the action is always
// one click, and an empty list shows a friendly empty state instead of dangling
// selection-dependent buttons. Rows are live-probed on every Rebuild (missing manifests
// render dim with no Open; newer-engine stamps render amber) - the list never lies about
// what Open will find.
//
// Everything with project consequences goes through callbacks the application binds (and
// defers through the UI mutation queue - opening detaches THIS view mid-dispatch):
//   OnOpenProject(dir)         - open an existing project (the app runs the version gate)
//   OnCreateProject(dir, name) - scaffold + open a new project at dir
//   OnStoreChanged()           - the registry changed; persist the settings store
// Remove is view-internal (registry mutation + list rebuild) but still queues its rebuild:
// the button lives in the very row the rebuild destroys.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:project_manager_view;

import draconic.foundation;
import draconic.settings;
import draconic.shell;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.engine.project;
import draconic.editor.core;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;
    namespace project = draconic::project;

    class ProjectManagerView final
    {
    public:
        Function<void(StringView)> OnOpenProject;
        Function<void(StringView, StringView)> OnCreateProject;
        Function<void()> OnStoreChanged;

        ProjectManagerView() = default;
        ProjectManagerView(const ProjectManagerView&) = delete;
        ProjectManagerView& operator=(const ProjectManagerView&) = delete;

        void Build(draconic::editor::ProjectManagerController& controller,
                   shell::IDialogService* dialogs, ui::UIContext* uiContext, u32 width,
                   u32 height)
        {
            m_controller = &controller;
            m_dialogs = dialogs;
            m_uiContext = uiContext;
            m_root = MakeRef<ui::RootView>(DefaultAllocator());
            m_root->ViewportSize = Float2{static_cast<f32>(width), static_cast<f32>(height)};
            m_root->DpiScale = 1.0f;

            // Center a fixed-width column: [spacer][column 720][spacer].
            auto outer = MakeRef<ui::FlexLayout>(DefaultAllocator());
            outer->Direction = ui::Orientation::Horizontal;
            auto leftSpacer = MakeRef<ui::FlexLayout>(DefaultAllocator());
            auto rightSpacer = MakeRef<ui::FlexLayout>(DefaultAllocator());
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 12;
            column->Padding = ui::Thickness{0, 28};
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                outer->AddView(leftSpacer.Get(), grow);
            }
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(720.0f));
                lp->Height = ui::SizeSpec::Match();
                outer->AddView(column.Get(), lp);
            }
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                outer->AddView(rightSpacer.Get(), grow);
            }

            // Header: product name + engine version (the manager IS the version disambiguator).
            {
                auto header = MakeRef<ui::FlexLayout>(DefaultAllocator());
                header->Direction = ui::Orientation::Horizontal;
                header->Spacing = 12;
                auto title = MakeRef<ui::Label>(DefaultAllocator());
                title->SetText(u8"Draconic Editor");
                title->FontSize.SetValue(24.0f);
                header->AddView(title.Get());
                auto version = MakeRef<ui::Label>(DefaultAllocator());
                String v(u8"engine ");
                v += project::kEngineVersionString;
                version->SetText(v.AsView());
                version->FontSize.SetValue(12.0f);
                version->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
                header->AddView(version.Get());
                column->AddView(header.Get());
            }

            // Standalone entry points - always available, independent of the list below.
            {
                auto actions = MakeRef<ui::FlexLayout>(DefaultAllocator());
                actions->Direction = ui::Orientation::Horizontal;
                actions->Spacing = 8;
                ProjectManagerView* self = this;
                auto create =
                    MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"New Project..."));
                create->OnClick.Add([self](ui::ButtonBase*) { self->ShowCreateDialog(); });
                actions->AddView(create.Get());
                auto browse =
                    MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Open Folder..."));
                browse->OnClick.Add([self](ui::ButtonBase*) { self->BrowseAndOpen(); });
                actions->AddView(browse.Get());
                column->AddView(actions.Get());
            }

            {
                auto caption = MakeRef<ui::Label>(DefaultAllocator());
                caption->SetText(u8"Recent projects");
                caption->FontSize.SetValue(13.0f);
                caption->TextColor.SetValue(Optional<Color>(Color{0.72f, 0.72f, 0.72f, 1.0f}));
                column->AddView(caption.Get());
            }

            // The card list: a vertical column inside a scroll view; rows are rebuilt whole.
            m_listColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
            m_listColumn->Direction = ui::Orientation::Vertical;
            m_listColumn->Spacing = 6;
            auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
            scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            {
                auto lp = MakeRef<ui::LayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                scroll->AddView(m_listColumn.Get(), lp);
            }
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                column->AddView(scroll.Get(), lp);
            }

            m_status = MakeRef<ui::Label>(DefaultAllocator());
            m_status->FontSize.SetValue(12.0f);
            m_status->TextColor.SetValue(Optional<Color>(Color{0.7f, 0.7f, 0.7f, 1.0f}));
            column->AddView(m_status.Get());

            m_root->AddView(outer.Get());
            Rebuild();
        }

        [[nodiscard]] ui::RootView* Root() const noexcept { return m_root.Get(); }

        void SetStatus(StringView text) { m_status->SetText(text); }

        // Re-read the registry + live-probe every entry, then rebuild the card list. Call on
        // every return to the manager; registry mutations queue a rebuild themselves.
        void Rebuild()
        {
            m_listColumn->RemoveAllViews();

            const RecentProjectsSettings& reg = m_controller->Entries();
            if (reg.entries.IsEmpty())
            {
                auto empty = MakeRef<ui::Label>(DefaultAllocator());
                empty->SetText(u8"No projects yet. Create a new project, or open an existing "
                               u8"project folder.");
                empty->FontSize.SetValue(13.0f);
                empty->TextColor.SetValue(Optional<Color>(Color{0.5f, 0.5f, 0.5f, 1.0f}));
                empty->WordWrap.SetValue(true);
                auto pad = MakeRef<ui::FlexLayout>(DefaultAllocator());
                pad->Padding = ui::Thickness{4, 16};
                pad->AddView(empty.Get());
                m_listColumn->AddView(pad.Get());
                return;
            }

            for (const RecentProjectEntry& entry : reg.entries)
            {
                String name = entry.name;
                String engineVersion = entry.engineVersion;
                bool missing = false;
                EngineVersionRelation relation = EngineVersionRelation::Same;
                project::ProjectSettings probed;
                if (ProbeProject(entry.path.AsView(), probed).IsOk())
                {
                    name = probed.name;
                    engineVersion = probed.engineVersion;
                    relation = CompareProjectEngineVersion(probed.engineVersion.AsView());
                }
                else
                {
                    missing = true;
                }
                AddRow(entry.path, name, engineVersion, relation, missing);
            }
        }

    private:
        void AddRow(const String& path, const String& name, const String& engineVersion,
                    EngineVersionRelation relation, bool missing)
        {
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 10;
            row->Padding = ui::Thickness{10, 8};

            // Left block: name line (+ state suffix) over the dim path line.
            auto text = MakeRef<ui::FlexLayout>(DefaultAllocator());
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 2;
            {
                String title = name.IsEmpty() ? String(u8"(unnamed)") : name;
                if (!engineVersion.IsEmpty())
                {
                    title += u8"   ";
                    title += engineVersion;
                }
                if (missing)
                {
                    title += u8"   (missing)";
                }
                else if (relation == EngineVersionRelation::ProjectNewer)
                {
                    title += u8"   (newer engine)";
                }
                auto nameLabel = MakeRef<ui::Label>(DefaultAllocator());
                nameLabel->SetText(title.AsView());
                nameLabel->FontSize.SetValue(14.0f);
                nameLabel->TextColor.SetValue(Optional<Color>(
                    missing ? Color{0.5f, 0.5f, 0.5f, 1.0f}
                    : (relation == EngineVersionRelation::ProjectNewer)
                        ? Color{0.95f, 0.75f, 0.3f, 1.0f}
                        : Color{0.9f, 0.9f, 0.9f, 1.0f}));
                text->AddView(nameLabel.Get());

                auto pathLabel = MakeRef<ui::Label>(DefaultAllocator());
                pathLabel->SetText(path.AsView());
                pathLabel->FontSize.SetValue(11.0f);
                pathLabel->TextColor.SetValue(Optional<Color>(Color{0.5f, 0.5f, 0.5f, 1.0f}));
                text->AddView(pathLabel.Get());
            }
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                row->AddView(text.Get(), grow);
            }

            ProjectManagerView* self = this;
            const String rowPath = path; // captured by value in the button handlers
            if (!missing)
            {
                auto open = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Open"));
                open->OnClick.Add(
                    [self, rowPath](ui::ButtonBase*)
                    {
                        if (self->OnOpenProject)
                        {
                            self->OnOpenProject(rowPath.AsView());
                        }
                    });
                row->AddView(open.Get());
            }
            auto remove = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Remove"));
            remove->OnClick.Add([self, rowPath](ui::ButtonBase*)
                                { self->RemoveEntry(rowPath); });
            row->AddView(remove.Get());

            // The card surface: a themed Panel (the stylesheet's "panel" class = the
            // palette's Surface color) so rows read as cards against the window background.
            auto card = MakeRef<ui::Panel>(DefaultAllocator());
            card->AddClass(u8"panel");
            {
                auto rowLp = MakeRef<ui::LayoutParams>(DefaultAllocator());
                rowLp->Width = ui::SizeSpec::Match();
                card->AddView(row.Get(), rowLp);
            }
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            m_listColumn->AddView(card.Get(), lp);
        }

        void RemoveEntry(const String& path)
        {
            if (!m_controller->Remove(path.AsView()))
            {
                return;
            }
            if (OnStoreChanged)
            {
                OnStoreChanged();
            }
            // The Remove button lives in the row the rebuild destroys - defer (the
            // mutation-queue rule: never mutate the tree mid-event-dispatch).
            ProjectManagerView* self = this;
            m_uiContext->MutationQueueRef().QueueAction(
                Function<void()>{[self]() { self->Rebuild(); }});
        }

        void BrowseAndOpen()
        {
            if (m_dialogs == nullptr)
            {
                return;
            }
            ProjectManagerView* self = this;
            m_dialogs->ShowOpenFolder(draconic::shell::DialogResultCallback{
                [self](Span<const String> paths)
                {
                    if (paths.Size() == 0)
                    {
                        return; // cancelled
                    }
                    project::ProjectSettings probed;
                    if (!ProbeProject(paths[0].AsView(), probed).IsOk())
                    {
                        self->SetStatus(
                            u8"Not a project (no Project.xml there). Use New Project to start "
                            u8"one.");
                        return;
                    }
                    if (self->OnOpenProject)
                    {
                        self->OnOpenProject(paths[0].AsView());
                    }
                }});
        }

        void ShowCreateDialog()
        {
            RefPtr<ui::Dialog> dialog =
                MakeRef<ui::Dialog>(DefaultAllocator(), StringView(u8"New Project"));
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            auto nameEdit = MakeRef<ui::EditText>(DefaultAllocator());
            nameEdit->SetPlaceholder(u8"Project name");
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(nameEdit.Get(), lp);
            }

            auto dirRow = MakeRef<ui::FlexLayout>(DefaultAllocator());
            dirRow->Direction = ui::Orientation::Horizontal;
            dirRow->Spacing = 6;
            auto dirEdit = MakeRef<ui::EditText>(DefaultAllocator());
            dirEdit->SetPlaceholder(u8"Parent directory (the project is created inside it)");
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                dirRow->AddView(dirEdit.Get(), lp);
            }
            auto browse = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Browse..."));
            {
                ProjectManagerView* self = this;
                ui::EditText* dirRaw = dirEdit.Get();
                browse->OnClick.Add(
                    [self, dirRaw](ui::ButtonBase*)
                    {
                        if (self->m_dialogs == nullptr)
                        {
                            return;
                        }
                        self->m_dialogs->ShowOpenFolder(draconic::shell::DialogResultCallback{
                            [dirRaw](Span<const String> paths)
                            {
                                if (paths.Size() > 0)
                                {
                                    dirRaw->SetText(paths[0].AsView());
                                }
                            }});
                    });
            }
            dirRow->AddView(browse.Get());
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(dirRow.Get(), lp);
            }
            dialog->SetContent(column.Get());
            dialog->MinWidth.SetValue(460.0f);

            ui::Dialog* rawDialog = dialog.Get();
            ui::EditText* nameRaw = nameEdit.Get();
            ui::EditText* dirRaw = dirEdit.Get();
            ProjectManagerView* self = this;
            ui::Button* create = dialog->AddButton(u8"Create", ui::DialogResult::None);
            create->OnClick.Add(
                [self, rawDialog, nameRaw, dirRaw](ui::ButtonBase*)
                {
                    const StringView name = nameRaw->Text();
                    const StringView parent = dirRaw->Text();
                    if (name.IsEmpty() || parent.IsEmpty())
                    {
                        return; // both fields required; dialog stays up
                    }
                    rawDialog->Close(ui::DialogResult::OK);
                    if (self->OnCreateProject)
                    {
                        self->OnCreateProject(PathJoin(parent, name).AsView(), name);
                    }
                });
            dialog->AddButton(u8"Cancel", ui::DialogResult::Cancel);
            dialog->Show(m_uiContext);
        }

        draconic::editor::ProjectManagerController* m_controller = nullptr;
        shell::IDialogService* m_dialogs = nullptr;
        ui::UIContext* m_uiContext = nullptr;
        RefPtr<ui::RootView> m_root;
        RefPtr<ui::FlexLayout> m_listColumn;
        RefPtr<ui::Label> m_status;
    };
}

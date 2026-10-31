// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_manager_view partition.
//
// The project manager (Documentation/Specs/project-manager.md): the window's whole screen at startup
// when no project is given, and after File > Close Project. Its frame is markup
// (Data/Editor/ProjectManager.xml): a header with the Projects and Samples tabs; on Projects a
// toolbar (New Project, Open Folder, a search, the sort, list or grid) over the projects as rows or
// as tiles, and under them the missing ones, each with Locate... and Remove from list; a footer with
// the status and the keys. With no projects at all, a welcome stands in for the lists.
//
// There is no selection: an action is one click on the project it is for. A focus ring that Up and
// Down move marks the project Enter opens and Del takes off the list. Each project is probed when the
// list is rebuilt; a search or a sort only arranges what was probed.
//
// Everything with consequences for the project goes through the services the application binds:
// opening (the application runs the version check), creating, and saving the list after a change.
// After a change the list is rebuilt at the GUI's next drain, never inside the click that asked for
// it, since the clicked button is among what the rebuild removes.
module;
#include "Core/Prelude.h"

export module editor.app:project_manager_view;

import foundation.core;
import foundation.shell;
import foundation.gui;
import foundation.image;
import foundation.image.io;
import engine.project;
import editor.core;
import :project_picture;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// ProjectManagerView::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kProjectManagerViewPath = u8"Editor/ProjectManager.xml";

    class ProjectManagerView
    {
    public:
        /// Its frames' markup path, under the data root.
        static constexpr const char8_t* kPath = kProjectManagerViewPath;
        static constexpr const char8_t* kNewProjectPath = u8"Editor/NewProject.xml";
        /// Where the header's Documentation link goes.
        static constexpr const char8_t* kDocumentationUrl = u8"https://assiduousengine.github.io/docs-site/";

        struct Services
        {
            editor::ProjectManagerController* controller = nullptr;
            foundation::shell::IDialogService* dialogs = nullptr; // null: no folder or file dialogs
            /// Opens the project in this folder (the application checks its engine first).
            Function<void(StringView)> open;
            /// Makes a project named `name` in `directory`, then opens it.
            Function<void(StringView directory, StringView name)> create;
            /// Called when the recent list or its layout changed, so the store is saved.
            Function<void()> storeChanged;
            /// Loads a dialog's markup by its path (New Project's).
            Function<RefPtr<gui::Node>(StringView)> loadMarkup;
        };

        ProjectManagerView(IAllocator& allocator, Services services, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_services(Move(services)), m_content(Move(content)), m_pictures(allocator), m_probes(allocator),
              m_shown(allocator), m_rows(allocator)
        {
            if (const Optional<String> home = GetEnvironmentVariable(u8"HOME"); home.HasValue() && !home.Value().IsEmpty())
                m_home = home.Value();
            else if (const Optional<String> profile = GetEnvironmentVariable(u8"USERPROFILE"); profile.HasValue())
                m_home = profile.Value();
            if (!m_content)
                return;
            ProjectManagerView* self = this;
            static constexpr const char8_t* kNew[] = {u8"manager-new", u8"manager-welcome-new"};
            static constexpr const char8_t* kOpen[] = {u8"manager-open", u8"manager-welcome-open"};
            for (const char8_t* id : kNew)
                if (auto* create = Find<gui::Button>(id))
                    create->SetOnClick([self]() { self->ShowCreateDialog(); });
            for (const char8_t* id : kOpen)
                if (auto* open = Find<gui::Button>(id))
                    open->SetOnClick([self]() { self->BrowseAndOpen(); });
            if (auto* list = Find<gui::Button>(u8"manager-list"))
                list->SetOnClick([self]() { self->SetGridView(false); });
            if (auto* grid = Find<gui::Button>(u8"manager-grid"))
                grid->SetOnClick([self]() { self->SetGridView(true); });
            if (auto* projects = Find<gui::Button>(u8"manager-tab-projects"))
                projects->SetOnClick([self]() { self->ShowSamples(false); });
            if (auto* samples = Find<gui::Button>(u8"manager-tab-samples"))
                samples->SetOnClick([self]() { self->ShowSamples(true); });
            if (auto* docs = Find<gui::Button>(u8"manager-docs"))
                docs->SetOnClick([self]()
                                 {
                                     if (self->m_services.dialogs != nullptr)
                                         self->m_services.dialogs->OpenPath(kDocumentationUrl);
                                 });
            if (auto* removeMissing = Find<gui::Button>(u8"manager-remove-missing"))
                removeMissing->SetOnClick([self]() { self->RemoveMissing(); });
            if (auto* search = Find<gui::TextField>(u8"manager-search"))
                search->SetOnTextChanging([self](StringView text) { self->SetQuery(text); });
            if (auto* sort = Find<gui::ComboBox>(u8"manager-sort"))
            {
                if (m_services.controller != nullptr)
                    sort->SetSelectedIndex(m_services.controller->SortByName() ? 1 : 0);
                sort->SetOnSelectionChanged([self](i32 index) { self->SetSortByName(index == 1); });
            }
            if (auto* engine = Find<gui::Label>(u8"manager-engine"))
                engine->SetText(Format(u8"Editor {}", engine::project::kEngineVersionString).AsView());
            // The scroller takes focus when the page is clicked, so the keys reach the page.
            if (auto* scroll = Find<gui::Node>(u8"manager-scroll"))
                scroll->SetTabFocusable(true);
            // Capture: the page's keys come before the search field's (Up, Down and Enter move and
            // open, as in a palette).
            (void)m_content->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                              {
                                                  const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                  if (self->HandleKey(key.KeyCode, key.Modifiers))
                                                      e.Handled = true;
                                              },
                                              true);
            Rebuild();
        }
        ProjectManagerView(const ProjectManagerView&) = delete;
        ProjectManagerView& operator=(const ProjectManagerView&) = delete;
        ~ProjectManagerView() { CloseDialog(); }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// Called once the page is on screen: the list takes the keys.
        void Shown()
        {
            if (auto* scroll = Find<gui::Node>(u8"manager-scroll"); scroll != nullptr && scroll->IsTreeVisible())
                scroll->RequestFocus();
        }

        /// Sets the status line to a problem, in the warning colour (empty shows the idle hint).
        void SetStatus(StringView text) { ShowStatus(text, true); }
        /// Sets the status line to what just happened, in the text colour.
        void SetNote(StringView text) { ShowStatus(text, false); }
        [[nodiscard]] StringView Status() const { return m_status.AsView(); }

        /// Probes every listed project, then lays the list out again.
        void Rebuild()
        {
            m_probes.Clear();
            if (m_services.controller != nullptr)
                for (const editor::RecentProjectEntry& entry : m_services.controller->Entries().entries)
                {
                    Probe probe;
                    probe.path = entry.path;
                    engine::project::ProjectSettings probed;
                    if (editor::ProbeProject(entry.path.AsView(), probed).IsOk())
                    {
                        probe.found = true;
                        probe.name = probed.name;
                        probe.engineVersion = probed.engineVersion;
                    }
                    m_probes.PushBack(Move(probe));
                }
            Populate();
        }

        // The list's layout: a grid of the pictures, else rows (kept in the store).
        [[nodiscard]] bool IsGridView() const { return m_services.controller != nullptr && m_services.controller->GridView(); }
        void SetGridView(bool grid)
        {
            if (m_services.controller == nullptr || grid == m_services.controller->GridView())
                return;
            m_services.controller->SetGridView(grid);
            if (m_services.storeChanged)
                m_services.storeChanged();
            Later([](ProjectManagerView& self) { self.Populate(); });
        }
        /// The order: by name, else by when each was last opened (kept in the store).
        void SetSortByName(bool byName)
        {
            if (m_services.controller == nullptr || byName == m_services.controller->SortByName())
                return;
            m_services.controller->SetSortByName(byName);
            if (m_services.storeChanged)
                m_services.storeChanged();
            Later([](ProjectManagerView& self) { self.Populate(); });
        }
        /// Shows only the projects whose name or folder holds `query` (any case).
        void SetQuery(StringView query)
        {
            m_query = String(query);
            Populate();
        }
        /// Pins a project to the top, or unpins it.
        void SetFavourite(StringView path, bool favourite)
        {
            if (m_services.controller == nullptr || !m_services.controller->SetFavourite(path, favourite))
                return;
            if (m_services.storeChanged)
                m_services.storeChanged();
            m_focusPath = String(path); // the ring follows the project to its new place
            Later([](ProjectManagerView& self) { self.Populate(); });
        }

        /// The Samples tab, else the Projects tab.
        void ShowSamples(bool samples)
        {
            m_samples = samples;
            Populate();
        }
        [[nodiscard]] bool IsShowingSamples() const noexcept { return m_samples; }
        /// Whether the first run's welcome stands in for the lists (no projects at all).
        [[nodiscard]] bool IsWelcome() const { return !m_samples && m_probes.IsEmpty(); }

        /// The projects shown (missing ones too), and how many show their thumbnail (for tests).
        [[nodiscard]] usize ShownCount() const noexcept { return m_shown.Size(); }
        [[nodiscard]] usize ThumbnailCount() const noexcept { return m_pictures.Size(); }
        /// The folder of the project the focus ring is on (empty with none).
        [[nodiscard]] StringView FocusedPath() const
        {
            return m_focus < m_shown.Size() ? m_shown[m_focus].path.AsView() : StringView{};
        }

        /// The page's keys: Ctrl+F searches, Ctrl+N is New Project, Ctrl+O is Open Folder; Up and
        /// Down move the focus ring, Enter opens its project (Locate... for a missing one), Del takes
        /// it off the list. A key the focused widget needs for itself (Enter on a button, Del in the
        /// search, the arrows in an open list) stays with it. True when it took the key.
        bool HandleKey(u32 keyCode, u32 modifiers)
        {
            const auto code = static_cast<gui::KeyCode>(keyCode);
            if (modifiers == gui::KeyModCtrl)
            {
                if (code == gui::KeyCode::F && !m_samples)
                {
                    if (auto* search = Find<gui::TextField>(u8"manager-search"); search != nullptr && search->IsTreeVisible())
                        search->RequestFocus();
                    return true;
                }
                if (code == gui::KeyCode::N)
                {
                    ShowCreateDialog();
                    return true;
                }
                if (code == gui::KeyCode::O)
                {
                    BrowseAndOpen();
                    return true;
                }
                return false;
            }
            if (modifiers != 0 || m_samples || m_shown.IsEmpty())
                return false;
            gui::EventDispatcher* dispatcher = m_content ? m_content->GetEventDispatcher() : nullptr;
            gui::Node* focus = dispatcher != nullptr ? dispatcher->GetFocusNode() : nullptr;
            if (code == gui::KeyCode::Down || code == gui::KeyCode::Up)
            {
                if (Cast<gui::ComboBox>(focus) != nullptr)
                    return false;
                MoveFocus(code == gui::KeyCode::Down ? 1 : -1);
                return true;
            }
            if (code == gui::KeyCode::Return || code == gui::KeyCode::KeypadEnter)
            {
                if (Cast<gui::ButtonBase>(focus) != nullptr || Cast<gui::ComboBox>(focus) != nullptr)
                    return false;
                const Listed& shown = m_shown[Min(m_focus, m_shown.Size() - 1)];
                if (shown.missing)
                    BrowseAndLocate(shown.path);
                else
                    Open(shown.path.AsView());
                return true;
            }
            if (code == gui::KeyCode::Delete)
            {
                if (Cast<gui::TextField>(focus) != nullptr)
                    return false;
                Run(m_shown[Min(m_focus, m_shown.Size() - 1)].path.AsView(), u8"Remove from List");
                return true;
            }
            return false;
        }
        /// Moves the focus ring by `step` projects, stopping at the ends.
        void MoveFocus(isize step)
        {
            if (m_shown.IsEmpty())
                return;
            const isize last = static_cast<isize>(m_shown.Size()) - 1;
            const isize at = Clamp(static_cast<isize>(Min(m_focus, m_shown.Size() - 1)) + step, isize(0), last);
            SetFocusIndex(static_cast<usize>(at));
        }

        /// A folder dropped on the window: opens the project in it, or says it holds none. False
        /// when nothing was dropped.
        bool OpenDropped(Span<const String> paths)
        {
            if (paths.Size() == 0)
                return false;
            engine::project::ProjectSettings probed;
            if (!editor::ProbeProject(paths[0].AsView(), probed).IsOk())
            {
                SetStatus(Format(u8"{} holds no Project.xml, so it is not a project. New Project makes one.", PathFilename(paths[0].AsView())).AsView());
                return true;
            }
            Open(paths[0].AsView());
            return true;
        }

        /// Points a missing project at the folder it is in now (Locate...'s second half), and says
        /// so; a folder that holds no project is refused.
        bool Locate(StringView from, StringView to)
        {
            if (m_services.controller == nullptr)
                return false;
            if (!m_services.controller->Relocate(from, to))
            {
                SetStatus(u8"That folder holds no Project.xml, or is listed already.");
                return false;
            }
            if (m_services.storeChanged)
                m_services.storeChanged();
            m_focusPath = String(to);
            SetNote(Format(u8"Found it: the project is listed at {} now.", ShortenHomePath(to, m_home.AsView())).AsView());
            Later([](ProjectManagerView& self) { self.Rebuild(); });
            return true;
        }
        /// Takes every missing project off the list. No files are touched.
        void RemoveMissing()
        {
            if (m_services.controller == nullptr)
                return;
            const usize removed = m_services.controller->RemoveMissing();
            if (removed == 0)
                return;
            if (m_services.storeChanged)
                m_services.storeChanged();
            SetNote(removed == 1 ? StringView(u8"The missing project was taken off the list; no files were touched.")
                                 : Format(u8"{} missing projects were taken off the list; no files were touched.", removed).AsView());
            Later([](ProjectManagerView& self) { self.Rebuild(); });
        }

        /// What a project's menu offers.
        [[nodiscard]] static Array<String> MenuItemsFor(StringView path)
        {
            Array<String> items;
            const bool missing = !FileExists(PathJoin(path, u8"Project.xml").AsView());
            if (!missing)
                items.PushBack(String(u8"Open"));
            items.PushBack(String(u8"Show in Folder"));
            items.PushBack(String(u8"Copy Path"));
            if (!missing)
            {
                items.PushBack(String(u8"Choose Thumbnail Image..."));
                if (FileExists(editor::ProjectThumbnailPath(path).AsView()))
                    items.PushBack(String(u8"Clear Thumbnail"));
            }
            items.PushBack(String(u8"Remove from List"));
            return items;
        }
        /// Runs one of a project's menu items.
        void Run(StringView path, StringView item)
        {
            const String where(path);
            if (item == StringView(u8"Open"))
                Open(where.AsView());
            else if (item == StringView(u8"Show in Folder"))
            {
                if (m_services.dialogs != nullptr)
                    m_services.dialogs->OpenPath(where.AsView());
            }
            else if (item == StringView(u8"Copy Path"))
            {
                gui::EventDispatcher* dispatcher = m_content ? m_content->GetEventDispatcher() : nullptr;
                if (dispatcher != nullptr && dispatcher->GetClipboard() != nullptr)
                {
                    dispatcher->GetClipboard()->SetText(where.AsView());
                    SetNote(u8"Copied the project's path.");
                }
            }
            else if (item == StringView(u8"Choose Thumbnail Image..."))
            {
                if (m_services.dialogs == nullptr)
                    return;
                static constexpr foundation::shell::FileFilter kImages[] = {{u8"Images", u8"png;jpg;jpeg;bmp;tga"}};
                ProjectManagerView* self = this;
                m_services.dialogs->ShowOpenFile(foundation::shell::DialogResultCallback{[self, where](Span<const String> paths)
                                                                                         {
                                                                                             if (paths.Size() > 0) // none: cancelled
                                                                                                 (void)self->SetThumbnailFromImage(where.AsView(), paths[0].AsView());
                                                                                         }},
                                                 Span<const foundation::shell::FileFilter>(kImages, 1));
            }
            else if (item == StringView(u8"Clear Thumbnail"))
            {
                (void)FileDelete(editor::ProjectThumbnailPath(where.AsView()).AsView());
                Later([](ProjectManagerView& self) { self.Populate(); });
            }
            else if (item == StringView(u8"Remove from List"))
            {
                const String name = NameOf(where.AsView());
                if (m_services.controller == nullptr || !m_services.controller->Remove(where.AsView()))
                    return;
                if (m_services.storeChanged)
                    m_services.storeChanged();
                SetNote(Format(u8"{} was taken off the list; its files are untouched.", name).AsView());
                Later([](ProjectManagerView& self) { self.Rebuild(); });
            }
        }
        /// Sets `image` (any picture the editor reads) as the project's thumbnail, saved as a PNG in
        /// its editor folder. False, reported in the status line, when it fails to read or save.
        bool SetThumbnailFromImage(StringView projectPath, StringView image)
        {
            foundation::image::Image picture;
            if (!foundation::image::io::LoadImage(image, picture).IsOk())
            {
                SetStatus(u8"That image could not be read.");
                return false;
            }
            const String target = editor::ProjectThumbnailPath(projectPath);
            (void)CreateDirectories(PathParent(target.AsView()));
            if (!foundation::image::io::SaveImage(picture, target.AsView(), foundation::image::io::ImageFileFormat::PNG).IsOk())
            {
                SetStatus(u8"The thumbnail could not be saved (an 8-bit image is needed).");
                return false;
            }
            Later([](ProjectManagerView& self) { self.Populate(); });
            return true;
        }

        /// New Project: a name and the folder it is made in; Create needs both.
        void ShowCreateDialog()
        {
            gui::SceneRoot* root = m_content ? Cast<gui::SceneRoot>(m_content->GetRootNode()) : nullptr;
            RefPtr<gui::Node> body = m_services.loadMarkup ? m_services.loadMarkup(kNewProjectPath) : RefPtr<gui::Node>();
            if (root == nullptr || !body || (m_dialog && m_dialog->IsOpen()))
                return;
            m_dialog = MakeRef<gui::Window>(m_allocator);
            m_dialog->SetTitle(u8"New Project");
            m_dialog->SetSize(Float2{kDialogWidth, kDialogHeight});
            m_dialog->GetContent()->AddChild(body.Get());
            auto* name = Cast<gui::TextField>(FindIn(*body, u8"new-project-name"));
            auto* parent = Cast<gui::TextField>(FindIn(*body, u8"new-project-parent"));
            auto* where = Cast<gui::Label>(FindIn(*body, u8"new-project-where"));
            auto* create = Cast<gui::Button>(FindIn(*body, u8"new-project-create"));
            auto* browse = Cast<gui::Button>(FindIn(*body, u8"new-project-browse"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"new-project-cancel"));
            if (name == nullptr || parent == nullptr || create == nullptr)
                return;
            ProjectManagerView* self = this;
            gui::Window* dialog = m_dialog.Get();
            // Create needs both fields; the line under them says where the project will go.
            const auto refresh = [name, parent, where, create]()
            {
                const bool ready = !name->GetText().IsEmpty() && !parent->GetText().IsEmpty();
                create->SetEnabled(ready);
                if (where != nullptr)
                    where->SetText(ready ? PathJoin(parent->GetText(), name->GetText()).AsView() : StringView{});
            };
            name->SetOnTextChanging([refresh](StringView) { refresh(); });
            parent->SetOnTextChanging([refresh](StringView) { refresh(); });
            create->SetOnClick([self, dialog, name, parent]()
                               {
                                   const String projectName(name->GetText());
                                   const String directory = PathJoin(parent->GetText(), projectName.AsView());
                                   if (projectName.IsEmpty() || parent->GetText().IsEmpty())
                                       return;
                                   dialog->Close();
                                   if (self->m_services.create)
                                       self->m_services.create(directory.AsView(), projectName.AsView());
                               });
            if (browse != nullptr)
                browse->SetOnClick([self, parent, refresh]()
                                   {
                                       if (self->m_services.dialogs == nullptr)
                                           return;
                                       self->m_services.dialogs->ShowOpenFolder(foundation::shell::DialogResultCallback{[parent, refresh](Span<const String> paths)
                                                                                                                         {
                                                                                                                             if (paths.Size() > 0)
                                                                                                                             {
                                                                                                                                 parent->SetText(paths[0].AsView());
                                                                                                                                 refresh();
                                                                                                                             }
                                                                                                                         }});
                                   });
            if (cancel != nullptr)
                cancel->SetOnClick([dialog]() { dialog->Close(); });
            refresh();
            m_dialog->OpenModal(*root);
            name->RequestFocus();
        }
        /// New Project's window while it is open (for tests).
        [[nodiscard]] gui::Window* CreateDialog() const noexcept { return m_dialog && m_dialog->IsOpen() ? m_dialog.Get() : nullptr; }

    private:
        static constexpr f32 kDialogWidth = 480.0f;
        static constexpr f32 kDialogHeight = 220.0f;

        // A listed project as the last probe found it.
        struct Probe
        {
            String path;
            String name;          // from its manifest, when found
            String engineVersion; // likewise
            bool found = false;
        };
        // A project on the page, in the order the focus ring walks: the projects, then the missing.
        struct Listed
        {
            String path;
            bool missing = false;
        };
        // What a row or a tile shows.
        struct Card
        {
            String path;
            String name;
            String engineVersion;
            String opened;   // "2 hours ago"
            String openedAt; // its exact time, for the tooltip
            editor::EngineVersionRelation relation = editor::EngineVersionRelation::Same;
            bool favourite = false;
        };

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? Cast<T>(FindIn(*m_content, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }
        static void SetClass(gui::Node* node, StringView name, bool on)
        {
            if (auto* widget = Cast<gui::UIWidget>(node))
            {
                if (on)
                    widget->AddClass(name);
                else
                    widget->RemoveClass(name);
            }
        }
        static void Clear(gui::Node* node)
        {
            if (node != nullptr)
                while (node->ChildCount() > 0)
                    node->RemoveChild(node->GetChildAt(node->ChildCount() - 1));
        }

        // Runs `work` at the GUI's next drain (the clicked button is among what a rebuild removes);
        // with no GUI it runs at once.
        void Later(void (*work)(ProjectManagerView&))
        {
            gui::MutationQueue* queue = m_content ? m_content->GetMutationQueue() : nullptr;
            if (queue == nullptr)
            {
                work(*this);
                return;
            }
            ProjectManagerView* self = this;
            queue->Enqueue([self, work]() { work(*self); });
        }

        void ShowStatus(StringView text, bool problem)
        {
            m_status = String(text);
            m_problem = problem;
            auto* status = Find<gui::Label>(u8"manager-status");
            if (status == nullptr)
                return;
            // Nothing to say: how to open a project, while there are some.
            const bool idle = text.IsEmpty();
            status->SetText(idle && !m_shown.IsEmpty() && !m_samples ? StringView(u8"Double-click a project or press Enter to open it.") : text);
            SetClass(status, u8"warning", problem && !idle);
        }

        [[nodiscard]] const Probe* ProbeOf(StringView path) const
        {
            for (const Probe& probe : m_probes)
                if (probe.path.AsView() == path)
                    return &probe;
            return nullptr;
        }
        [[nodiscard]] String NameOf(StringView path) const
        {
            if (m_services.controller != nullptr)
                for (const editor::RecentProjectEntry& entry : m_services.controller->Entries().entries)
                    if (entry.path.AsView() == path)
                        return entry.name;
            return String(PathFilename(path));
        }

        // Lays the page out from the last probe: the tab, the welcome or the lists in their order,
        // the counts, the focus ring.
        void Populate()
        {
            if (!m_content || m_services.controller == nullptr)
                return;
            if (auto* projects = Find<gui::Button>(u8"manager-tab-projects"))
                projects->SetChecked(!m_samples);
            if (auto* samples = Find<gui::Button>(u8"manager-tab-samples"))
                samples->SetChecked(m_samples);
            const bool welcome = IsWelcome();
            if (auto* page = Find<gui::UIWidget>(u8"manager-projects-page"))
                page->SetDisplayed(!m_samples && !welcome);
            if (auto* page = Find<gui::UIWidget>(u8"manager-samples-page"))
                page->SetDisplayed(m_samples);
            if (auto* page = Find<gui::UIWidget>(u8"manager-welcome"))
                page->SetDisplayed(welcome);
            if (auto* logo = Find<gui::UIWidget>(u8"manager-logo"))
                logo->SetDisplayed(!welcome); // the welcome shows it large
            const bool grid = m_services.controller->GridView();
            if (auto* asList = Find<gui::Button>(u8"manager-list"))
                asList->SetChecked(!grid);
            if (auto* asGrid = Find<gui::Button>(u8"manager-grid"))
                asGrid->SetChecked(grid);

            gui::Node* list = Find<gui::Node>(u8"manager-projects");
            gui::Node* missingList = Find<gui::Node>(u8"manager-missing-list");
            Clear(list);
            Clear(missingList);
            m_pictures.Clear();
            m_shown.Clear();
            m_rows.Clear();
            SetClass(list, u8"grid", grid);

            const i64 now = GetUnixTime();
            const i64 offset = GetLocalTimeOffset(now);
            const editor::RecentProjectsSettings& recent = m_services.controller->Entries();
            const Array<const editor::RecentProjectEntry*> arranged =
                editor::ArrangeRecentProjects(recent, m_query.AsView(), m_services.controller->SortByName());
            usize present = 0;
            for (const Probe& probe : m_probes)
                present += probe.found ? 1u : 0u;
            Array<const editor::RecentProjectEntry*> missing;
            for (const editor::RecentProjectEntry* entry : arranged)
            {
                const Probe* probe = ProbeOf(entry->path.AsView());
                if (probe == nullptr || !probe->found)
                {
                    missing.PushBack(entry);
                    continue;
                }
                Card card;
                card.path = entry->path;
                card.name = probe->name.IsEmpty() ? entry->name : probe->name;
                card.engineVersion = probe->engineVersion;
                card.relation = editor::CompareProjectEngineVersion(probe->engineVersion.AsView());
                card.opened = editor::DescribeLastOpened(entry->lastOpened, now, offset);
                card.openedAt = editor::DescribeTime(entry->lastOpened, offset);
                card.favourite = entry->favourite;
                if (list != nullptr)
                    list->AddChild((grid ? Tile(card) : Row(card)).Get());
                m_shown.PushBack(Listed{card.path, false});
            }
            const usize matched = m_shown.Size();
            for (const editor::RecentProjectEntry* entry : missing)
            {
                if (missingList != nullptr)
                    missingList->AddChild(MissingRow(*entry).Get());
                m_shown.PushBack(Listed{entry->path, true});
            }

            if (auto* count = Find<gui::Label>(u8"manager-count"))
                count->SetText(matched == present ? Format(u8"{} project{}", present, present == 1 ? u8"" : u8"s").AsView()
                                                  : Format(u8"{} of {}", matched, present).AsView());
            if (auto* noMatch = Find<gui::Label>(u8"manager-no-match"))
            {
                noMatch->SetDisplayed(!m_query.IsEmpty() && matched == 0);
                noMatch->SetText(Format(u8"No project matches \"{}\". Search looks at names and folders.", m_query).AsView());
            }
            if (auto* section = Find<gui::UIWidget>(u8"manager-missing"))
                section->SetDisplayed(!missing.IsEmpty());
            if (auto* missingCount = Find<gui::Label>(u8"manager-missing-count"))
                missingCount->SetText(Format(u8"{} project{}, their folders are not where they were", missing.Size(), missing.Size() == 1 ? u8"" : u8"s").AsView());
            if (auto* keys = Find<gui::UIWidget>(u8"manager-keys"))
                keys->SetDisplayed(!m_samples && !welcome);

            // The ring stays on its project when that is still shown, else on the same place.
            usize focus = Min(m_focus, m_shown.IsEmpty() ? usize(0) : m_shown.Size() - 1);
            for (usize i = 0; i < m_shown.Size(); ++i)
                if (m_shown[i].path == m_focusPath)
                    focus = i;
            SetFocusIndex(focus);
            ShowStatus(m_status.AsView(), m_problem);
            // A rebuild removed the focused button: the list takes the keys again.
            gui::EventDispatcher* dispatcher = m_content->GetEventDispatcher();
            if (dispatcher != nullptr && dispatcher->GetFocusNode() == nullptr)
                Shown();
        }

        void SetFocusIndex(usize index)
        {
            m_focus = index;
            m_focusPath = index < m_shown.Size() ? m_shown[index].path : String();
            for (usize i = 0; i < m_rows.Size(); ++i)
                SetClass(m_rows[i], u8"focused", i == index);
            if (index < m_rows.Size())
                if (auto* scroll = Find<gui::Node>(u8"manager-scroll"); scroll != nullptr && IsInTree(*m_rows[index], *scroll))
                    scroll->ScrollIntoView(*m_rows[index]);
        }
        [[nodiscard]] static bool IsInTree(const gui::Node& node, const gui::Node& ancestor)
        {
            for (const gui::Node* n = &node; n != nullptr; n = n->GetParent())
                if (n == &ancestor)
                    return true;
            return false;
        }

        void Open(StringView path)
        {
            if (m_services.open)
                m_services.open(path);
        }
        void BrowseAndOpen()
        {
            if (m_services.dialogs == nullptr)
                return;
            ProjectManagerView* self = this;
            m_services.dialogs->ShowOpenFolder(foundation::shell::DialogResultCallback{[self](Span<const String> paths)
                                                                                       {
                                                                                           if (paths.Size() == 0)
                                                                                               return; // cancelled
                                                                                           engine::project::ProjectSettings probed;
                                                                                           if (!editor::ProbeProject(paths[0].AsView(), probed).IsOk())
                                                                                           {
                                                                                               self->SetStatus(u8"Not a project (no Project.xml there). New Project makes one.");
                                                                                               return;
                                                                                           }
                                                                                           self->Open(paths[0].AsView());
                                                                                       }});
        }
        // Locate...: asks for the folder a missing project is in now.
        void BrowseAndLocate(const String& from)
        {
            if (m_services.dialogs == nullptr)
                return;
            ProjectManagerView* self = this;
            m_services.dialogs->ShowOpenFolder(foundation::shell::DialogResultCallback{[self, from](Span<const String> paths)
                                                                                       {
                                                                                           if (paths.Size() > 0) // none: cancelled
                                                                                               (void)self->Locate(from.AsView(), paths[0].AsView());
                                                                                       }});
        }

        // Shows a project's menu under `anchor`.
        void ShowMenu(const String& path, gui::Node& anchor)
        {
            m_menu = MakeRef<gui::Menu>(m_allocator);
            ProjectManagerView* self = this;
            for (const String& item : MenuItemsFor(path.AsView()))
            {
                if (item.AsView() == StringView(u8"Remove from List"))
                    m_menu->AddSeparator();
                (void)m_menu->AddItem(item.AsView(), [self, path, item]() { self->Run(path.AsView(), item.AsView()); });
            }
            gui::Node* root = anchor.GetRootNode();
            if (root != nullptr)
                m_menu->OpenBelow(*root, anchor);
        }

        [[nodiscard]] RefPtr<gui::Label> Text(StringView text, StringView classes)
        {
            RefPtr<gui::Label> label = MakeRef<gui::Label>(m_allocator);
            label->AddClasses(classes);
            label->SetText(text);
            return label;
        }
        [[nodiscard]] RefPtr<gui::Button> MoreButton(const String& path)
        {
            RefPtr<gui::Button> more = MakeRef<gui::Button>(m_allocator);
            more->AddClass(u8"slot-button");
            more->SetIconSource(String(u8"Editor/Icons/more.svg"));
            more->SetTooltip(u8"More");
            ProjectManagerView* self = this;
            gui::Button* raw = more.Get();
            more->SetOnClick([self, path, raw]() { self->ShowMenu(path, *raw); });
            return more;
        }
        [[nodiscard]] RefPtr<gui::Button> StarButton(const Card& card)
        {
            RefPtr<gui::Button> star = MakeRef<gui::Button>(m_allocator);
            star->AddClass(u8"manager-star");
            if (card.favourite)
                star->AddClass(u8"on");
            star->SetIconSource(String(card.favourite ? u8"Editor/Icons/star-filled.svg" : u8"Editor/Icons/star.svg"));
            star->SetTooltip(card.favourite ? StringView(u8"Unpin") : StringView(u8"Pin to the top"));
            ProjectManagerView* self = this;
            const String where = card.path;
            const bool pin = !card.favourite;
            star->SetOnClick([self, where, pin]() { self->SetFavourite(where.AsView(), pin); });
            return star;
        }
        // The version's classes: muted when it is this editor's, else newer or older.
        [[nodiscard]] static StringView VersionClass(editor::EngineVersionRelation relation)
        {
            switch (relation)
            {
            case editor::EngineVersionRelation::ProjectNewer:
                return u8"newer";
            case editor::EngineVersionRelation::ProjectOlder:
            case editor::EngineVersionRelation::Unstamped:
                return u8"older";
            default:
                return u8"";
            }
        }
        [[nodiscard]] static String VersionText(const Card& card) { return card.engineVersion.IsEmpty() ? String(u8"unknown") : card.engineVersion; }

        // A project's row or tile: a press puts the ring on it, a double-click opens it.
        void Listen(gui::Node& node, usize index, const String& path)
        {
            ProjectManagerView* self = this;
            (void)node.AddEventListener(gui::EventType::MouseClick, [self, index, path](const gui::Event& e)
                                        {
                                            const auto& click = static_cast<const gui::MouseEvent&>(e);
                                            if (index < self->m_rows.Size())
                                                self->SetFocusIndex(index);
                                            if (click.ClickCount == 2 && !e.Handled)
                                                self->Open(path.AsView());
                                        });
            (void)node.AddEventListener(gui::EventType::ContextMenu, [self, path, raw = &node](const gui::Event& e)
                                        {
                                            self->ShowMenu(path, *raw);
                                            e.Handled = true;
                                        });
        }

        [[nodiscard]] RefPtr<gui::Node> Row(const Card& card)
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"manager-row");
            const StringView version = VersionClass(card.relation);
            if (!version.IsEmpty())
                row->AddClass(version);
            row->AddChild(ProjectPicture::Make(m_allocator, card.path.AsView(), card.name.AsView(), u8"manager-row-picture", m_pictures).Get());
            RefPtr<gui::Panel> text = MakeRef<gui::Panel>(m_allocator);
            text->AddClass(u8"manager-text");
            RefPtr<gui::Panel> title = MakeRef<gui::Panel>(m_allocator);
            title->AddClass(u8"manager-title");
            title->AddChild(Text(card.name.AsView(), u8"manager-name").Get());
            if (card.relation == editor::EngineVersionRelation::ProjectNewer)
                title->AddChild(Text(u8"Made with a newer engine", u8"badge warning").Get());
            else if (!version.IsEmpty())
                title->AddChild(Text(u8"Upgrades when opened", u8"badge info").Get());
            text->AddChild(title.Get());
            RefPtr<gui::Label> path = Text(ShortenHomePath(card.path.AsView(), m_home.AsView()).AsView(), u8"manager-path");
            path->SetTooltip(card.path.AsView());
            text->AddChild(path.Get());
            row->AddChild(text.Get());
            RefPtr<gui::Label> opened = Text(card.opened.AsView(), u8"manager-opened");
            opened->SetTooltip(card.openedAt.AsView());
            row->AddChild(opened.Get());
            RefPtr<gui::Label> stamp = Text(VersionText(card).AsView(), u8"manager-version");
            if (!version.IsEmpty())
                stamp->AddClass(version);
            row->AddChild(stamp.Get());
            row->AddChild(StarButton(card).Get());
            RefPtr<gui::Button> open = MakeRef<gui::Button>(m_allocator);
            open->SetText(u8"Open");
            ProjectManagerView* self = this;
            const String where = card.path;
            open->SetOnClick([self, where]() { self->Open(where.AsView()); });
            row->AddChild(open.Get());
            row->AddChild(MoreButton(card.path).Get());
            Listen(*row, m_rows.Size(), card.path);
            m_rows.PushBack(row.Get());
            return RefPtr<gui::Node>(row);
        }

        [[nodiscard]] RefPtr<gui::Node> Tile(const Card& card)
        {
            RefPtr<gui::Panel> tile = MakeRef<gui::Panel>(m_allocator);
            tile->AddClass(u8"manager-tile");
            const StringView version = VersionClass(card.relation);
            // Clicking the picture opens the project: a flat button holds it.
            RefPtr<gui::Button> picture = MakeRef<gui::Button>(m_allocator);
            picture->AddClass(u8"manager-tile-open");
            picture->AddChild(ProjectPicture::Make(m_allocator, card.path.AsView(), card.name.AsView(), u8"manager-tile-picture", m_pictures).Get());
            picture->SetTooltip(card.path.AsView());
            ProjectManagerView* self = this;
            const String where = card.path;
            picture->SetOnClick([self, where]() { self->Open(where.AsView()); });
            tile->AddChild(picture.Get());
            RefPtr<gui::Panel> foot = MakeRef<gui::Panel>(m_allocator);
            foot->AddClass(u8"manager-tile-foot");
            RefPtr<gui::Panel> text = MakeRef<gui::Panel>(m_allocator);
            text->AddClass(u8"manager-text");
            text->AddChild(Text(card.name.AsView(), u8"manager-name").Get());
            RefPtr<gui::Panel> meta = MakeRef<gui::Panel>(m_allocator);
            meta->AddClass(u8"manager-tile-meta");
            if (!card.opened.IsEmpty())
            {
                RefPtr<gui::Label> opened = Text(card.opened.AsView(), u8"manager-tile-opened");
                opened->SetTooltip(card.openedAt.AsView());
                meta->AddChild(opened.Get());
                meta->AddChild(Text(u8"·", u8"manager-tile-dot").Get());
            }
            RefPtr<gui::Label> stamp = Text(VersionText(card).AsView(), u8"manager-tile-version");
            if (!version.IsEmpty())
            {
                stamp->AddClass(version);
                stamp->SetTooltip(card.relation == editor::EngineVersionRelation::ProjectNewer ? StringView(u8"Made with a newer engine")
                                                                                              : StringView(u8"Upgrades when opened"));
            }
            meta->AddChild(stamp.Get());
            text->AddChild(meta.Get());
            foot->AddChild(text.Get());
            foot->AddChild(StarButton(card).Get());
            foot->AddChild(MoreButton(card.path).Get());
            tile->AddChild(foot.Get());
            Listen(*tile, m_rows.Size(), card.path);
            m_rows.PushBack(tile.Get());
            return RefPtr<gui::Node>(tile);
        }

        // A missing project: its folder marked broken, its name and old folder, Locate... and
        // Remove from list.
        [[nodiscard]] RefPtr<gui::Node> MissingRow(const editor::RecentProjectEntry& entry)
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"manager-row");
            row->AddClass(u8"missing");
            RefPtr<gui::Panel> icon = MakeRef<gui::Panel>(m_allocator);
            icon->AddClass(u8"manager-missing-icon");
            RefPtr<gui::Image> broken = MakeRef<gui::Image>(m_allocator);
            broken->SetSource(String(u8"Editor/Icons/folder-missing.svg"));
            icon->AddChild(broken.Get());
            row->AddChild(icon.Get());
            RefPtr<gui::Panel> text = MakeRef<gui::Panel>(m_allocator);
            text->AddClass(u8"manager-text");
            text->AddChild(Text(entry.name.AsView(), u8"manager-name").Get());
            RefPtr<gui::Label> path = Text(ShortenHomePath(entry.path.AsView(), m_home.AsView()).AsView(), u8"manager-path");
            path->SetTooltip(entry.path.AsView());
            text->AddChild(path.Get());
            row->AddChild(text.Get());
            ProjectManagerView* self = this;
            const String where = entry.path;
            RefPtr<gui::Button> locate = MakeRef<gui::Button>(m_allocator);
            locate->SetText(u8"Locate...");
            locate->SetTooltip(u8"Choose the folder the project is in now");
            locate->SetOnClick([self, where]() { self->BrowseAndLocate(where); });
            row->AddChild(locate.Get());
            RefPtr<gui::Button> remove = MakeRef<gui::Button>(m_allocator);
            remove->AddClass(u8"ghost");
            remove->SetText(u8"Remove from list");
            remove->SetOnClick([self, where]() { self->Run(where.AsView(), u8"Remove from List"); });
            row->AddChild(remove.Get());
            const usize index = m_shown.Size(); // its place in the ring's walk
            (void)row->AddEventListener(gui::EventType::MouseClick, [self, index](const gui::Event&) { self->SetFocusIndex(index); });
            m_rows.PushBack(row.Get());
            return RefPtr<gui::Node>(row);
        }

        void CloseDialog()
        {
            if (m_dialog && m_dialog->IsOpen())
                m_dialog->Close();
            if (m_menu && m_menu->IsOpen())
                m_menu->Close();
        }

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_content;
        RefPtr<gui::Window> m_dialog; // New Project
        RefPtr<gui::Menu> m_menu;     // a project's context menu
        Array<UniquePtr<foundation::image::Image>> m_pictures; // what the pictures show
        Array<Probe> m_probes;   // every listed project, as the last rebuild found it
        Array<Listed> m_shown;    // the projects on the page, in the ring's order
        Array<gui::Node*> m_rows; // their rows or tiles, in the same order
        String m_home;           // shown as ~ in a folder
        String m_query;
        String m_status;
        String m_focusPath;
        usize m_focus = 0;
        bool m_problem = false;
        bool m_samples = false;
    };
}

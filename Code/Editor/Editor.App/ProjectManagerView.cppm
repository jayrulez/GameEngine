// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_manager_view partition.
//
// The project manager (Documentation/Specs/editor-gui.md, 5.1, layer 13): the window's whole
// screen at startup with no project given, and after File > Close Project. Its frame is markup
// (Data/Editor/ProjectManager.gml), New Project and Open Folder first, then every recent project as
// a row (its picture, name, folder and engine, Open, a menu) or a tile of its picture. There is no
// selection: an action is one click on the project it is for. Each project is probed as it is
// shown: one whose folder no longer holds it is dimmed with no Open; one saved by a newer engine
// is named in the warning colour. The status line under the head says what went wrong.
//
// Everything with consequences for the project goes through the services the application binds:
// opening (the application runs the version check), creating, and saving the list after a change.
// A change to the list it shows is made again at the GUI's next drain, never inside the click that
// asked (the clicked button is in what goes).
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

    class ProjectManagerView
    {
    public:
        /// Its frames, under the data root.
        static constexpr const char8_t* kPath = u8"Editor/ProjectManager.gml";
        static constexpr const char8_t* kNewProjectPath = u8"Editor/NewProject.gml";

        struct Services
        {
            editor::ProjectManagerController* controller = nullptr;
            foundation::shell::IDialogService* dialogs = nullptr; // null: no folder or file dialogs
            /// Open the project in this folder (the application checks its engine first).
            Function<void(StringView)> open;
            /// Make a project named `name` in `directory`, then open it.
            Function<void(StringView directory, StringView name)> create;
            /// The recent list or its layout changed: the store is saved.
            Function<void()> storeChanged;
            /// A dialog's markup by its path (New Project's).
            Function<RefPtr<gui::Node>(StringView)> loadMarkup;
        };

        ProjectManagerView(IAllocator& allocator, Services services, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_services(Move(services)), m_content(Move(content)), m_pictures(allocator), m_shown(allocator)
        {
            if (!m_content)
                return;
            ProjectManagerView* self = this;
            if (auto* create = Find<gui::Button>(u8"manager-new"))
                create->SetOnClick([self]() { self->ShowCreateDialog(); });
            if (auto* open = Find<gui::Button>(u8"manager-open"))
                open->SetOnClick([self]() { self->BrowseAndOpen(); });
            if (auto* list = Find<gui::Button>(u8"manager-list"))
                list->SetOnClick([self]() { self->SetGridView(false); });
            if (auto* grid = Find<gui::Button>(u8"manager-grid"))
                grid->SetOnClick([self]() { self->SetGridView(true); });
            if (auto* engine = Find<gui::Label>(u8"manager-engine"))
                engine->SetText(Format(u8"engine {}", engine::project::kEngineVersionString).AsView());
            Rebuild();
        }
        ProjectManagerView(const ProjectManagerView&) = delete;
        ProjectManagerView& operator=(const ProjectManagerView&) = delete;
        ~ProjectManagerView() { CloseDialog(); }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }

        /// The status line (empty hides it).
        void SetStatus(StringView text)
        {
            if (auto* status = Find<gui::Label>(u8"manager-status"))
            {
                status->SetText(text);
                status->SetDisplayed(!text.IsEmpty());
            }
        }
        [[nodiscard]] StringView Status() const
        {
            const auto* status = Find<gui::Label>(u8"manager-status");
            return status != nullptr ? status->GetText() : StringView{};
        }

        /// The projects again, each probed now.
        void Rebuild()
        {
            gui::Node* list = Find<gui::Node>(u8"manager-projects");
            if (list == nullptr || m_services.controller == nullptr)
                return;
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            m_pictures.Clear();
            m_shown.Clear();
            const bool grid = m_services.controller->GridView();
            if (auto* asList = Find<gui::Button>(u8"manager-list"))
                asList->SetChecked(!grid);
            if (auto* asGrid = Find<gui::Button>(u8"manager-grid"))
                asGrid->SetChecked(grid);
            if (auto* view = Cast<gui::UIWidget>(list))
            {
                if (grid)
                    view->AddClass(u8"grid");
                else
                    view->RemoveClass(u8"grid");
            }
            const editor::RecentProjectsSettings& recent = m_services.controller->Entries();
            if (recent.entries.IsEmpty())
            {
                RefPtr<gui::Label> empty = MakeRef<gui::Label>(m_allocator);
                empty->AddClass(u8"manager-empty");
                empty->SetText(u8"No projects yet. Make a new project, or open a project's folder.");
                list->AddChild(empty.Get());
                return;
            }
            for (const editor::RecentProjectEntry& entry : recent.entries)
            {
                Card card;
                card.path = entry.path;
                card.name = entry.name;
                card.engineVersion = entry.engineVersion;
                engine::project::ProjectSettings probed;
                if (editor::ProbeProject(entry.path.AsView(), probed).IsOk())
                {
                    card.name = probed.name;
                    card.engineVersion = probed.engineVersion;
                    card.newer = editor::CompareProjectEngineVersion(probed.engineVersion.AsView()) == editor::EngineVersionRelation::ProjectNewer;
                }
                else
                    card.missing = true;
                list->AddChild((grid ? Tile(card) : Row(card)).Get());
                m_shown.PushBack(Move(card));
            }
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
            Later([](ProjectManagerView& self) { self.Rebuild(); });
        }

        /// The projects shown, and how many show their thumbnail (a test's).
        [[nodiscard]] usize ShownCount() const noexcept { return m_shown.Size(); }
        [[nodiscard]] usize ThumbnailCount() const noexcept { return m_pictures.Size(); }
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
                    SetStatus(u8"Copied the project's path.");
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
                Later([](ProjectManagerView& self) { self.Rebuild(); });
            }
            else if (item == StringView(u8"Remove from List"))
            {
                if (m_services.controller == nullptr || !m_services.controller->Remove(where.AsView()))
                    return;
                if (m_services.storeChanged)
                    m_services.storeChanged();
                Later([](ProjectManagerView& self) { self.Rebuild(); });
            }
        }
        /// `image` (any picture the editor reads) as the project's thumbnail, saved as a PNG in its
        /// editor folder. False, said in the status line, when it does not read or save.
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
            Later([](ProjectManagerView& self) { self.Rebuild(); });
            return true;
        }

        /// New Project: a name and the folder it is made in; Create waits for both.
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
            // Create waits for both fields; the line under them says where the project goes.
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
        /// New Project's window while it is open (a test's).
        [[nodiscard]] gui::Window* CreateDialog() const noexcept { return m_dialog && m_dialog->IsOpen() ? m_dialog.Get() : nullptr; }

    private:
        static constexpr f32 kDialogWidth = 480.0f;
        static constexpr f32 kDialogHeight = 220.0f;

        struct Card
        {
            String path;
            String name;
            String engineVersion;
            bool missing = false;
            bool newer = false;
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

        // `work` at the GUI's next drain (the clicked button is in what a rebuild takes away); at
        // once in no GUI.
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

        // The menu of a project, under `anchor`.
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
        [[nodiscard]] String Title(const Card& card) const
        {
            if (card.missing)
                return Format(u8"{} (missing)", card.name);
            if (card.newer)
                return Format(u8"{} (newer engine)", card.name);
            return card.name;
        }

        [[nodiscard]] RefPtr<gui::Node> Row(const Card& card)
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"manager-row");
            if (card.missing)
                row->AddClass(u8"missing");
            if (card.newer)
                row->AddClass(u8"newer");
            row->AddChild(ProjectPicture::Make(m_allocator, card.missing ? StringView{} : card.path.AsView(), card.name.AsView(), u8"manager-row-picture",
                                               m_pictures)
                              .Get());
            RefPtr<gui::Panel> text = MakeRef<gui::Panel>(m_allocator);
            text->AddClass(u8"manager-text");
            text->AddChild(Text(Title(card).AsView(), u8"manager-name").Get());
            RefPtr<gui::Label> path = Text(card.path.AsView(), u8"manager-path");
            path->SetTooltip(card.path.AsView());
            text->AddChild(path.Get());
            row->AddChild(text.Get());
            row->AddChild(Text(card.engineVersion.AsView(), u8"manager-version").Get());
            // Open in a slot of its own, empty for a missing project: every row's columns line up.
            RefPtr<gui::Panel> slot = MakeRef<gui::Panel>(m_allocator);
            slot->AddClass(u8"manager-open-slot");
            if (!card.missing)
            {
                RefPtr<gui::Button> open = MakeRef<gui::Button>(m_allocator);
                open->SetText(u8"Open");
                ProjectManagerView* self = this;
                const String where = card.path;
                open->SetOnClick([self, where]() { self->Open(where.AsView()); });
                slot->AddChild(open.Get());
            }
            row->AddChild(slot.Get());
            row->AddChild(MoreButton(card.path).Get());
            return RefPtr<gui::Node>(row);
        }

        [[nodiscard]] RefPtr<gui::Node> Tile(const Card& card)
        {
            RefPtr<gui::Panel> tile = MakeRef<gui::Panel>(m_allocator);
            tile->AddClass(u8"manager-tile");
            if (card.missing)
                tile->AddClass(u8"missing");
            if (card.newer)
                tile->AddClass(u8"newer");
            // The picture opens it: a flat button holding it.
            RefPtr<gui::Button> picture = MakeRef<gui::Button>(m_allocator);
            picture->AddClass(u8"manager-tile-open"); // its hover rings the picture evenly
            picture->AddChild(ProjectPicture::Make(m_allocator, card.missing ? StringView{} : card.path.AsView(), card.name.AsView(), u8"manager-tile-picture",
                                                   m_pictures)
                                  .Get());
            picture->SetTooltip(card.path.AsView());
            picture->SetEnabled(!card.missing);
            ProjectManagerView* self = this;
            const String where = card.path;
            picture->SetOnClick([self, where]() { self->Open(where.AsView()); });
            tile->AddChild(picture.Get());
            RefPtr<gui::Panel> foot = MakeRef<gui::Panel>(m_allocator);
            foot->AddClass(u8"manager-tile-foot");
            RefPtr<gui::Panel> text = MakeRef<gui::Panel>(m_allocator);
            text->AddClass(u8"manager-text");
            text->AddChild(Text(card.name.AsView(), u8"manager-name").Get());
            const String subtitle = card.missing ? Format(u8"{}, missing", card.engineVersion)
                                    : card.newer ? Format(u8"{}, newer engine", card.engineVersion)
                                                 : card.engineVersion;
            text->AddChild(Text(subtitle.AsView(), u8"manager-path").Get());
            foot->AddChild(text.Get());
            foot->AddChild(MoreButton(card.path).Get());
            tile->AddChild(foot.Get());
            return RefPtr<gui::Node>(tile);
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
        RefPtr<gui::Menu> m_menu;     // a project's
        Array<UniquePtr<foundation::image::Image>> m_pictures; // what the pictures show
        Array<Card> m_shown;
    };
}

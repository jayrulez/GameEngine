// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :import_dialog partition
//
// The import window, over its markup (Data/Editor/Import.xml): one window for one file or many, in
// three panes (Specs/import-window.md). The files, grouped by the importer each resolves to and
// headed by that importer's Label(), each kept or dropped, with its asset type's icon, its size,
// its state while it needs attention (reading, failed), and one selected. The selected file's
// preview, with a line of facts under it. Its settings: its importer (a choice when several
// accept it), its options, and what it would create by kind, each kind collapsed to its count and
// opening to its items, each kept or dropped and named, a kind's box keeping or dropping all of it.
// Under the panes, where the files go, in a field that opens the group picker. A file still being
// read says so and shows its settings when it is ready. Nothing here names a file type: each part
// comes from the file's importer. The line under
// it says what Import will do, or why it waits, and Import is disabled while it waits. A name
// another asset holds is reported on the file (AssetImport::ClashOf): an asset of another type
// waits for a rename; one of the same type from another file offers Replace or Keep both, and
// Import waits for the choice. It is the view of an AssetImport session, which does the work.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:import_dialog;

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;    // PropertyGrid, ReflectedPropertySource (the importer's settings)
import foundation.runtime.client; // IApplicationHost (a preview's frame hooks)
import foundation.graphics;       // FrameContext
import pipeline.importer;
import editor.core; // EditorIcons
import :asset_import;
import :group_picker_dialog;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace content = foundation::content;

    /// ImportWindow::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kImportWindowPath = u8"Editor/Import.xml";

    /// What a preview that renders (a model's live view) publishes: the editor's frame hooks, which
    /// the window passes on while it is open, as the editor drives a page's.
    class IImportPreviewFrames : public editor::IImportPreviewService
    {
    public:
        /// Each frame, after the GUI laid out.
        virtual void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) = 0;
        /// Each frame, inside the scene renderer's bracket, before the GUI draws.
        virtual void OnRenderWindow(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) = 0;

    protected:
        ~IImportPreviewFrames() = default;
    };

    class ImportWindow
    {
    public:
        /// Its markup's path, under the data root.
        static constexpr const char8_t* kPath = kImportWindowPath;

        struct Services
        {
            AssetImport* import = nullptr;
            /// Opens the group picker with `current` checked; `picked` gets the choice.
            Function<void(content::Group& current, Function<void(content::Group&)> picked)> pickGroup;
            /// The asset types' icons, as the Assets panel shows them (null: the generic one).
            editor::EditorIcons* icons = nullptr;
            /// The domains' previews by asset type, and the pool they read their files on (either
            /// null: none, each file shows its icon).
            const editor::ImportPreviewRegistry* previews = nullptr;
            TaskPool* tasks = nullptr;
        };

        ImportWindow(IAllocator& allocator, Services services, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_services(Move(services)), m_body(Move(body))
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Import");
            m_window->AddClass(u8"import-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            ImportWindow* self = this;
            if (auto* group = Find<gui::Button>(u8"import-group"))
                group->SetOnClick([self]()
                                  {
                                      if (!self->m_session || !self->m_services.pickGroup)
                                          return;
                                      self->m_services.pickGroup(*self->m_session->group, [self](content::Group& picked)
                                                                 {
                                                                     if (!self->m_session)
                                                                         return;
                                                                     self->m_services.import->SetGroup(*self->m_session, picked);
                                                                     self->ShowAll();
                                                                 });
                                  });
            if (auto* run = Find<gui::Button>(u8"import-run"))
                run->SetOnClick([self]() { (void)self->Import(); });
            if (auto* cancel = Find<gui::Button>(u8"import-cancel"))
                cancel->SetOnClick([self]() { self->Close(); });
            // However it closes, release the session: a read that arrives afterwards is dropped.
            m_window->SetOnClose([self]() { self->Release(); });
        }
        ImportWindow(const ImportWindow&) = delete;
        ImportWindow& operator=(const ImportWindow&) = delete;
        ~ImportWindow() { Close(); }

        /// Opens the window over `root` for `session` with its first file selected; false with no
        /// markup, no session, or when one is already open.
        bool Open(gui::Node& root, RefPtr<AssetImport::Session> session)
        {
            if (!m_window || !session || m_window->IsOpen())
                return false;
            m_session = Move(session);
            m_selected = 0;
            ImportWindow* self = this;
            m_session->onChanged = [self](usize file)
            {
                self->ShowFiles();
                if (file == self->m_selected)
                {
                    self->ShowPreview(); // one waiting for the importer's read is made now
                    self->ShowDetail();
                }
                self->ShowWhy();
            };
            ShowAll();
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }
        void Close()
        {
            if (IsOpen())
                m_window->Close();
        }

        /// The editor's frame, passed to the selected file's preview while the window is open.
        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
        {
            if (IsOpen() && m_preview)
                if (IImportPreviewFrames* frames = m_preview->Service<IImportPreviewFrames>())
                    frames->OnUpdate(host, dt);
        }
        void OnRenderWindow(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame)
        {
            if (IsOpen() && m_preview)
                if (IImportPreviewFrames* frames = m_preview->Service<IImportPreviewFrames>())
                    frames->OnRenderWindow(host, frame);
        }

        /// Imports what is kept and closes; false (the window stays, saying why) when it cannot yet.
        bool Import()
        {
            if (!m_session)
                return false;
            if (m_preview)
                m_preview->StopReading(); // before the import changes what the importer read
            if (!m_services.import->Commit(*m_session))
            {
                // The window stays, saying why: its preview is made again rather than stay stopped.
                m_previewPath = {};
                ShowPreview();
                return false;
            }
            Close();
            return true;
        }
        /// Setting `name` of file `index`'s options was changed (in the grid, or by a kind row's box):
        /// the settings that follow from it (its importer's to say: a texture's colour space with its
        /// usage), what the file makes described again, shown in the tree and the details, and in the
        /// preview at once (a sound's trim shaded on its waveform).
        void SettingChanged(usize index, StringView name)
        {
            if (!m_session || index >= m_session->files.Size())
                return;
            AssetImport::File& changed = m_session->files[index];
            const String path(name);
            const PropertyInfo* setting = changed.options ? FindProperty(*changed.options->GetType(), reinterpret_cast<const char*>(path.CStr())) : nullptr;
            if (setting != nullptr && changed.Importer() != nullptr)
            {
                changed.Importer()->SettingChanged(*changed.options, *setting);
                // Not inside the grid's own edit.
                ImportWindow* self = this;
                Later([self, index]()
                      {
                          if (!self->m_session || index >= self->m_session->files.Size())
                              return;
                          // The part selected, found again in the new plan by what it is (its place
                          // may move, or it may be gone: the file is selected then).
                          AssetImport::File& file = self->m_session->files[index];
                          pipeline::ImportPlanEntry selected;
                          const bool hadPart = index == self->m_selected && self->m_part >= 0 && static_cast<usize>(self->m_part) < file.plan.entries.Size();
                          if (hadPart)
                              selected = file.plan.entries[static_cast<usize>(self->m_part)];
                          self->m_services.import->RefreshPlan(*self->m_session, index);
                          self->PlanChanged(index);
                          if (hadPart)
                          {
                              self->m_part = -1;
                              for (usize e = 0; e < file.plan.entries.Size(); ++e)
                                  if (file.plan.entries[e].kind == selected.kind && file.plan.entries[e].sourceName == selected.sourceName)
                                      self->m_part = static_cast<i64>(e);
                          }
                          self->ShowFiles(); // the tree: what it makes now
                          if (index == self->m_selected)
                              self->ShowDetail();
                          self->ShowWhy();
                      });
            }
            if (m_preview)
                m_preview->OnSettingsChanged();
        }

        /// A kind row's box: keeps or drops every part of `kind` in file `index`. Where its importer
        /// has a setting for the kind, the box is that setting: unticked leaves them out as the
        /// importer does (a model's materials then name no textures), ticked imports them again;
        /// ticked while the setting is on but every part was dropped, it keeps them all again.
        void SetKindKept(usize index, pipeline::ImportResourceKind kind, bool on)
        {
            if (!m_session || index >= m_session->files.Size())
                return;
            AssetImport::File& file = m_session->files[index];
            if (bool* setting = KindFlag(file, kind); setting != nullptr && *setting != on)
            {
                *setting = on;
                SettingChanged(index, file.Importer()->KindSetting(kind));
                return;
            }
            for (pipeline::ImportPlanEntry& entry : file.plan.entries)
                if (entry.kind == kind)
                    entry.enabled = on;
            PlanChanged(index);
            ImportWindow* self = this;
            Later([self]() { self->ShowFiles(); self->ShowDetail(); self->ShowWhy(); });
        }

        /// Gives the other parts of part `e`'s kind in file `index` its own settings (the "Apply to all"
        /// in a part's details); returns how many it gave them to.
        usize SharePartSettings(usize index, usize e)
        {
            if (!m_session || index >= m_session->files.Size() || e >= m_session->files[index].plan.entries.Size())
                return 0;
            AssetImport::File& file = m_session->files[index];
            const pipeline::ImportPlanEntry& from = file.plan.entries[e];
            const pipeline::IFileImporter* importer = file.Importer();
            if (!from.settings || importer == nullptr)
                return 0;
            usize given = 0;
            for (usize i = 0; i < file.plan.entries.Size(); ++i)
            {
                pipeline::ImportPlanEntry& to = file.plan.entries[i];
                if (i == e || to.kind != from.kind)
                    continue;
                if (!to.settings)
                    to.settings = importer->PartSettings(file.path.AsView(), file.options.Get(), to, m_session->group, m_allocator);
                if (to.settings && to.settings->GetType() == from.settings->GetType())
                {
                    CopyScalars(*from.settings, *to.settings);
                    ++given;
                }
            }
            return given;
        }

        /// File `index`'s plan changed (a part kept or dropped, the file described again): its settings
        /// carry the plan as the review stands, what the import will make part by part, and its
        /// preview draws that (a material dropped draws the default).
        void PlanChanged(usize index)
        {
            if (!m_session || index >= m_session->files.Size())
                return;
            AssetImport::File& file = m_session->files[index];
            if (file.options)
                file.options->selection = file.plan;
            if (index == m_selected && m_preview)
                m_preview->OnSettingsChanged();
        }

        /// Shows file `index`'s preview and settings.
        void Select(usize index)
        {
            if (!m_session || index >= m_session->files.Size())
                return;
            m_selected = index;
            m_part = -1;
            ShowFiles();
            ShowPreview();
            ShowDetail();
        }
        /// Shows part `entry` of file `index` (an entry of its plan): its preview and its details.
        void SelectPart(usize index, usize entry)
        {
            if (!m_session || index >= m_session->files.Size() || entry >= m_session->files[index].plan.entries.Size())
                return;
            m_selected = index;
            m_part = static_cast<i64>(entry);
            ShowFiles();
            ShowPreview();
            ShowDetail();
        }
        /// The selected part's index in the selected file's plan; -1 when the file itself is selected.
        [[nodiscard]] i64 SelectedPart() const noexcept { return m_part; }
        /// Gives file `index`'s settings to the session's other files of its importer (its "Apply to
        /// all ... in this import"); returns how many took them.
        usize ShareSettings(usize index)
        {
            if (!m_session || index >= m_session->files.Size() || !m_session->files[index].options)
                return 0;
            const AssetImport::File& from = m_session->files[index];
            usize shared = 0;
            for (usize i = 0; i < m_session->files.Size(); ++i)
            {
                AssetImport::File& to = m_session->files[i];
                if (i != index && to.Importer() == from.Importer() && to.options && to.options->CopySettingsFrom(*from.options))
                    ++shared;
            }
            return shared;
        }
        /// Opens or closes a kind of what file `index` would create, in the tree under it (its
        /// items listed or not).
        void SetKindOpen(usize index, pipeline::ImportResourceKind kind, bool open)
        {
            bool already = false;
            for (usize i = 0; i < m_openKinds.Size(); ++i)
                if (m_openKinds[i].file == index && m_openKinds[i].kind == kind)
                {
                    already = true;
                    if (!open)
                        m_openKinds.RemoveAt(i);
                    break;
                }
            if (open && !already)
                m_openKinds.PushBack(OpenKind{index, kind});
            ShowFiles();
        }
        /// SetKindOpen for the selected file.
        void SetKindOpen(pipeline::ImportResourceKind kind, bool open) { SetKindOpen(m_selected, kind, open); }
        [[nodiscard]] bool IsKindOpen(usize index, pipeline::ImportResourceKind kind) const
        {
            for (const OpenKind& open : m_openKinds)
                if (open.file == index && open.kind == kind)
                    return true;
            return false;
        }
        [[nodiscard]] bool IsKindOpen(pipeline::ImportResourceKind kind) const { return IsKindOpen(m_selected, kind); }

        // For tests: the session, the window, the selected file, the line, the Import button.
        [[nodiscard]] AssetImport::Session* Session() const noexcept { return m_session.Get(); }
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }
        [[nodiscard]] usize Selected() const noexcept { return m_selected; }
        [[nodiscard]] StringView Why() const
        {
            const auto* why = Find<gui::Label>(u8"import-why");
            return why != nullptr ? why->GetText() : StringView{};
        }
        [[nodiscard]] gui::Button* ImportButton() const { return Find<gui::Button>(u8"import-run"); }
        [[nodiscard]] gui::Node* Detail() const { return Find<gui::Node>(u8"import-detail"); }
        [[nodiscard]] gui::Node* Files() const { return Find<gui::Node>(u8"import-files"); }
        [[nodiscard]] gui::Node* Facts() const { return Find<gui::Node>(u8"import-facts"); }
        /// The selected file's preview; null when its type has none.
        [[nodiscard]] editor::IImportPreview* Preview() const noexcept { return m_preview.Get(); }

    private:
        static constexpr f32 kWidth = 1100.0f;
        static constexpr f32 kHeight = 680.0f;

        void Release()
        {
            if (!m_session)
                return;
            m_preview = {}; // first: it may read the session's files' settings
            m_session->onChanged = {};
            m_session->closed = true;
            m_session = {};
            m_previewPath = {};
            m_previewType = {};
        }
        void ShowAll()
        {
            if (auto* group = Find<gui::Button>(u8"import-group"); group != nullptr && m_session)
                group->SetText(GroupPicker::ShownName(*m_session->group).AsView());
            ShowFiles();
            ShowPreview();
            ShowDetail();
            ShowWhy();
        }

        [[nodiscard]] static StringView LabelOf(const AssetImport::File& file)
        {
            const pipeline::IFileImporter* importer = file.Importer();
            return importer != nullptr ? importer->Label() : StringView(u8"Other");
        }
        // The icon of the asset type the file's importer makes, as the Assets panel shows that type.
        [[nodiscard]] String IconOf(const AssetImport::File& file) const
        {
            const pipeline::IFileImporter* importer = file.Importer();
            const StringView type = importer != nullptr ? importer->ListedTypeName(file.path.AsView()) : StringView{};
            if (m_services.icons == nullptr)
                return String(u8"Editor/Icons/Types/Asset.svg");
            return m_services.icons->ForTypeName(type, u8"Asset").Source;
        }
        // The file's size on disk; empty when it cannot be read.
        [[nodiscard]] static String SizeOf(const AssetImport::File& file)
        {
            u64 size = 0;
            i64 time = 0;
            return FileStat(file.path.AsView(), size, time) ? ByteSizeText(size) : String{};
        }

        // Shows the files, grouped by the importer each resolves to, in the order those first come:
        // each kept or not, its icon, its size, its state while it needs attention, the selected
        // one marked.
        void ShowFiles()
        {
            auto* list = Find<gui::UIWidget>(u8"import-files");
            if (list == nullptr || !m_session)
                return;
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            Array<String> labels;
            for (const AssetImport::File& file : m_session->files)
            {
                bool seen = false;
                for (const String& label : labels)
                    seen = seen || label.AsView() == LabelOf(file);
                if (!seen)
                    labels.PushBack(String(LabelOf(file)));
            }
            for (const String& label : labels)
            {
                usize count = 0;
                for (const AssetImport::File& file : m_session->files)
                    count += LabelOf(file) == label.AsView() ? 1u : 0u;
                list->AddChild(Text(Format(u8"{} · {}", label, count).AsView(), u8"import-section").Get());
                for (usize i = 0; i < m_session->files.Size(); ++i)
                    if (LabelOf(m_session->files[i]) == label.AsView())
                    {
                        list->AddChild(FileRow(i).Get());
                        AddPartRows(*list, i);
                    }
            }
        }

        [[nodiscard]] RefPtr<gui::Node> FileRow(usize i)
        {
            ImportWindow* self = this;
            {
                const AssetImport::File& file = m_session->files[i];
                RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
                row->AddClass(u8"import-file");
                if (i == m_selected && m_part < 0)
                    row->AddClass(u8"selected");
                RefPtr<gui::CheckBox> kept = MakeRef<gui::CheckBox>(m_allocator);
                kept->SetChecked(file.kept);
                kept->SetEnabled(file.state != AssetImport::File::State::Failed);
                kept->SetTooltip(u8"Import this file");
                kept->SetOnToggled([self, i](bool on)
                                   {
                                       if (self->m_session && i < self->m_session->files.Size())
                                           self->m_session->files[i].kept = on;
                                       self->ShowWhy();
                                   });
                RefPtr<gui::Image> icon = MakeRef<gui::Image>(m_allocator);
                icon->AddClass(u8"import-file-icon");
                icon->SetSource(IconOf(file));
                icon->SetHitTestVisible(false);
                RefPtr<gui::Button> name = MakeRef<gui::Button>(m_allocator);
                name->AddClass(u8"import-file-name");
                name->SetText(file.Name());
                name->SetTooltip(file.path.AsView());
                name->SetOnClick([self, i]() { self->Later([self, i]() { self->Select(i); }); }); // the rows are rebuilt, so not inside the click
                row->AddChild(kept.Get());
                row->AddChild(icon.Get());
                row->AddChild(name.Get());
                // Its state only while it needs attention; a ready file shows its size.
                if (file.state == AssetImport::File::State::Ready)
                    row->AddChild(Text(SizeOf(file).AsView(), u8"import-file-size").Get());
                else
                {
                    const bool failed = file.state == AssetImport::File::State::Failed;
                    row->AddChild(Text(failed ? StringView(u8"Failed") : StringView(u8"Reading"), failed ? u8"badge danger" : u8"badge warning").Get());
                }
                return RefPtr<gui::Node>(row);
            }
        }

        // Asks the preview to show the part selected in the tree, or the whole file when none is (or
        // when it cannot show that part): a preview that publishes IImportPartPreview.
        void ShowSelectedPart()
        {
            if (!m_preview || !m_session || m_selected >= m_session->files.Size())
                return;
            editor::IImportPartPreview* parts = m_preview->Service<editor::IImportPartPreview>();
            if (parts == nullptr)
                return;
            const pipeline::ImportPlan& plan = m_session->files[m_selected].plan;
            if (m_part >= 0 && static_cast<usize>(m_part) < plan.entries.Size() && parts->ShowPart(plan.entries[static_cast<usize>(m_part)]))
                return;
            parts->ShowWhole();
        }

        // Shows the selected file's preview, and its facts under it: the preview a domain registers
        // for the type its importer lists it by, made again only when the file or that type
        // changes; with none, its type's icon and its name. A preview reads its file off the main
        // thread, and its facts show again when the read comes back.
        void ShowPreview()
        {
            auto* view = Find<gui::UIWidget>(u8"import-preview-view");
            if (view == nullptr || !m_session || m_selected >= m_session->files.Size())
                return;
            const AssetImport::File& file = m_session->files[m_selected];
            const pipeline::IFileImporter* importer = file.Importer();
            const String type(importer != nullptr ? importer->ListedTypeName(file.path.AsView()) : StringView{});
            if (file.path != m_previewPath || type != m_previewType)
            {
                while (view->ChildCount() > 0)
                    view->RemoveChild(view->GetChildAt(view->ChildCount() - 1));
                m_preview = {};
                editor::IImportPreviewFactory* factory = m_services.previews != nullptr && m_services.tasks != nullptr
                                                             ? m_services.previews->FindByName(type.AsView())
                                                             : nullptr;
                // While the importer reads the file, its preview waits to show that read rather
                // than read it again (made when the read arrives, see Open).
                const bool waits = factory != nullptr && file.state == AssetImport::File::State::Reading;
                if (!waits)
                {
                    m_previewPath = file.path;
                    m_previewType = type;
                }
                else
                    m_previewPath = {};
                if (factory != nullptr && !waits)
                {
                    if (file.options)
                        m_session->files[m_selected].options->selection = file.plan; // the preview draws the plan as it stands
                    const editor::ImportPreviewSource source{file.path.AsView(), file.options.Get(), file.prepared.Get()};
                    m_preview = factory->Create(source, *m_services.tasks, m_allocator);
                }
                if (m_preview)
                {
                    ImportWindow* self = this;
                    m_preview->onChanged = [self]() { self->ShowFacts(); };
                    view->AddChild(&m_preview->View());
                }
                else
                {
                    RefPtr<gui::Image> icon = MakeRef<gui::Image>(m_allocator);
                    icon->AddClass(u8"import-preview-icon");
                    icon->SetSource(IconOf(file));
                    view->AddChild(icon.Get());
                    view->AddChild(Text(waits ? Format(u8"Reading {}...", file.Name()).AsView() : file.Name(), u8"import-preview-name").Get());
                }
            }
            ShowFacts();
            ShowSelectedPart(); // the whole file, or the part selected in the tree
        }

        // The selected file's facts: its preview's (its importer's label without one), then its size.
        void ShowFacts()
        {
            auto* facts = Find<gui::UIWidget>(u8"import-facts");
            if (facts == nullptr || !m_session || m_selected >= m_session->files.Size())
                return;
            const AssetImport::File& file = m_session->files[m_selected];
            while (facts->ChildCount() > 0)
                facts->RemoveChild(facts->GetChildAt(facts->ChildCount() - 1));
            if (m_preview)
                for (const String& fact : m_preview->Facts())
                    facts->AddChild(Text(fact.AsView(), u8"import-fact").Get());
            else
                facts->AddChild(Text(LabelOf(file), u8"import-fact").Get());
            if (const String size = SizeOf(file); !size.IsEmpty())
                facts->AddChild(Text(size.AsView(), u8"import-fact").Get());
        }

        // Shows the selected file: its importer, its options, what it would create by kind.
        void ShowDetail()
        {
            auto* detail = Find<gui::UIWidget>(u8"import-detail");
            if (detail == nullptr || !m_session || m_selected >= m_session->files.Size())
                return;
            while (detail->ChildCount() > 0)
                detail->RemoveChild(detail->GetChildAt(detail->ChildCount() - 1));
            AssetImport::File& file = m_session->files[m_selected];
            ImportWindow* self = this;
            const usize index = m_selected;
            if (m_part >= 0 && static_cast<usize>(m_part) < file.plan.entries.Size())
            {
                ShowPart(*detail, index, static_cast<usize>(m_part));
                return;
            }
            detail->AddChild(Text(file.Name(), u8"import-title").Get());
            if (file.importers.Size() > 1)
            {
                RefPtr<gui::Panel> line = Row(u8"import-importer");
                line->AddChild(Text(u8"Import as", u8"form-label").Get());
                RefPtr<gui::ComboBox> choice = MakeRef<gui::ComboBox>(m_allocator);
                for (const pipeline::IFileImporter* importer : file.importers)
                    choice->AddItem(importer->Label());
                choice->SetSelectedIndex(static_cast<i32>(file.importer));
                choice->SetOnSelectionChanged([self, index](i32 picked)
                                              {
                                                  if (!self->m_session || picked < 0)
                                                      return;
                                                  self->m_services.import->SetImporter(*self->m_session, index, static_cast<usize>(picked));
                                                  self->Later([self]() { self->ShowAll(); }); // not inside the combo's own event
                                              });
                line->AddChild(choice.Get());
                detail->AddChild(line.Get());
            }
            if (file.state == AssetImport::File::State::Reading)
            {
                detail->AddChild(Text(Format(u8"Reading {}...", file.Name()).AsView(), u8"form-help").Get());
                RefPtr<gui::ProgressBar> bar = MakeRef<gui::ProgressBar>(m_allocator);
                bar->SetIndeterminate(true);
                detail->AddChild(bar.Get());
                return;
            }
            if (file.state == AssetImport::File::State::Failed)
            {
                detail->AddChild(Text(u8"It could not be read; the Console says why.", u8"form-help refused").Get());
                return;
            }
            // Its settings, as its importer declares them, drawn by the inspector's property editors.
            // The last grid lets go of its source before the source goes.
            if (m_settingsGrid)
                m_settingsGrid->SetSource(nullptr);
            m_settingsGrid = {};
            m_settings.Reset();
            if (file.options && !file.options->Settings().IsEmpty())
            {
                m_settings = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
                // Each setting beside what its importer says it does to this file, or disabled
                // with why it cannot apply.
                m_settings->SetOnDescribe([self, index](StringView path, gui::PropertyInfo& info)
                                          {
                                              if (!self->m_session || index >= self->m_session->files.Size())
                                                  return;
                                              const AssetImport::File& described = self->m_session->files[index];
                                              const pipeline::IFileImporter* importer = described.Importer();
                                              if (importer == nullptr || !described.options)
                                                  return;
                                              // Only each part's default (each part carries its own): shown on the parts.
                                              if (importer->SettingIsPerPart(path))
                                              {
                                                  info.Visible = false;
                                                  return;
                                              }
                                              // A setting a kind row in the tree stands for is that row, not a second switch here.
                                              for (const pipeline::ImportResourceKind kind : kKinds)
                                                  if (!importer->KindSetting(kind).IsEmpty() && importer->KindSetting(kind) == path)
                                                  {
                                                      info.Visible = false;
                                                      return;
                                                  }
                                              const String name(path);
                                              const PropertyInfo* setting = FindProperty(*described.options->GetType(), reinterpret_cast<const char*>(name.CStr()));
                                              if (setting == nullptr)
                                                  return;
                                              const pipeline::ImportSettingNote note =
                                                  importer->DescribeSetting(described.path.AsView(), *described.options, described.prepared.Get(), *setting);
                                              if (!note.reason.IsEmpty())
                                              {
                                                  info.ReadOnly = true;
                                                  info.Note = note.reason;
                                              }
                                              else
                                                  info.Note = note.effect;
                                          });
                // A change brings the settings that follow from it (its importer's to say: a
                // texture's colour space with its usage), and shows in the preview at once (a
                // sound's trim shaded on its waveform).
                m_settings->SetOnChange([self, index](const gui::ReflectedChange& change) { self->SettingChanged(index, change.Path); });
                const Instance target(file.options.Get(), file.options->GetType());
                m_settings->SetObjects(Span<const Instance>(&target, 1u));
                RefPtr<gui::PropertyGrid> grid = MakeRef<gui::PropertyGrid>(m_allocator);
                grid->AddClass(u8"import-settings");
                grid->SetFitsRows(true); // as tall as its settings: what the file makes follows right under them
                grid->SetSource(m_settings.Get());
                detail->AddChild(grid.Get());
                m_settingsGrid = grid;
                // The same settings for the drop's other files of this importer, in one click.
                usize others = 0;
                for (usize i = 0; i < m_session->files.Size(); ++i)
                    others += i != index && m_session->files[i].Importer() == file.Importer() && m_session->files[i].options ? 1u : 0u;
                if (others > 0)
                {
                    RefPtr<gui::Button> share = MakeRef<gui::Button>(m_allocator);
                    share->AddClass(u8"import-share");
                    share->SetText(Format(u8"Apply to all {} in this import", LabelOf(file)).AsView());
                    share->SetTooltip(Format(u8"Gives the {} other {} files these settings", others, LabelOf(file)).AsView());
                    share->SetOnClick([self, index]() { self->ShareSettings(index); });
                    detail->AddChild(share.Get());
                }
            }
            ShowClash(*detail, file, index);
            if (file.plan.IsEmpty())
            {
                detail->AddChild(Text(u8"It makes one asset, named after the file.", u8"form-help").Get());
                return;
            }
            if (HasTree(file))
                detail->AddChild(Text(u8"What it makes is in the tree under it: open a kind, and select an item to see it.", u8"form-help").Get());
        }

        // The kinds a plan holds, in the order the tree shows them.
        static constexpr pipeline::ImportResourceKind kKinds[] = {
            pipeline::ImportResourceKind::Mesh,          pipeline::ImportResourceKind::Material,      pipeline::ImportResourceKind::Texture,
            pipeline::ImportResourceKind::Skeleton,      pipeline::ImportResourceKind::AnimationClip, pipeline::ImportResourceKind::Collision,
            pipeline::ImportResourceKind::Asset};

        // Whether a file's plan makes a tree under it: more than the one asset a single-file
        // importer makes.
        [[nodiscard]] static bool HasTree(const AssetImport::File& file)
        {
            if (file.plan.entries.Size() > 1 || (file.plan.entries.Size() == 1 && file.plan.entries[0].kind != pipeline::ImportResourceKind::Asset))
                return true;
            for (const pipeline::ImportResourceKind kind : kKinds) // a kind left out still has its row, to tick it back
                if (const bool* setting = KindFlag(file, kind); setting != nullptr && !*setting)
                    return true;
            return false;
        }

        // The tree under file `index`: each kind it makes, its box keeping or dropping all of it
        // (mixed while some are kept), its chevron and its count; an open kind lists its items,
        // each with its box and its name, selected to show it.
        void AddPartRows(gui::UIWidget& list, usize index)
        {
            const AssetImport::File& file = m_session->files[index];
            if (file.state != AssetImport::File::State::Ready || !HasTree(file))
                return;
            ImportWindow* self = this;
            for (const pipeline::ImportResourceKind kind : kKinds)
            {
                usize count = 0, kept = 0;
                for (const pipeline::ImportPlanEntry& entry : file.plan.entries)
                    if (entry.kind == kind)
                    {
                        ++count;
                        kept += entry.enabled ? 1u : 0u;
                    }
                // A kind its importer's setting has left out keeps its row, unticked, so it is turned
                // back on where it was turned off.
                const bool* setting = KindFlag(file, kind);
                const bool off = setting != nullptr && !*setting;
                if (count == 0 && !off)
                    continue;
                const bool open = IsKindOpen(index, kind);
                RefPtr<gui::Panel> head = Row(u8"import-kind");
                RefPtr<gui::CheckBox> all = MakeRef<gui::CheckBox>(m_allocator);
                all->SetTooltip(setting != nullptr ? StringView(u8"Import them, or leave them all out") : StringView(u8"Keep or drop all of them"));
                all->SetChecked(!off && kept == count);
                all->SetIndeterminate(!off && kept > 0 && kept < count);
                all->SetOnToggled([self, index, kind](bool on) { self->SetKindKept(index, kind, on); });
                RefPtr<gui::Button> toggle = MakeRef<gui::Button>(m_allocator);
                toggle->AddClass(u8"import-kind-toggle");
                toggle->AddClass(u8"with-icon");
                toggle->SetIconSource(String(open ? u8"Editor/Icons/chevron-down.svg" : u8"Editor/Icons/chevron-right.svg"));
                toggle->SetText(pipeline::ImportResourceKindLabel(kind));
                toggle->SetTooltip(off ? StringView(u8"Left out: tick to import them") : open ? StringView(u8"Hide them") : StringView(u8"Show them"));
                toggle->SetEnabled(!off);
                toggle->SetOnClick([self, index, kind, open]() { self->Later([self, index, kind, open]() { self->SetKindOpen(index, kind, !open); }); });
                head->AddChild(all.Get());
                head->AddChild(toggle.Get());
                head->AddChild(Text((off ? String(u8"Left out") : Format(u8"{} of {}", kept, count)).AsView(), u8"import-kind-count").Get());
                list.AddChild(head.Get());
                if (!open || off)
                    continue;
                for (usize e = 0; e < file.plan.entries.Size(); ++e)
                {
                    const pipeline::ImportPlanEntry& entry = file.plan.entries[e];
                    if (entry.kind != kind)
                        continue;
                    RefPtr<gui::Panel> line = Row(u8"import-part");
                    if (index == m_selected && m_part == static_cast<i64>(e))
                        line->AddClass(u8"selected");
                    RefPtr<gui::CheckBox> on = MakeRef<gui::CheckBox>(m_allocator);
                    on->SetChecked(entry.enabled);
                    on->SetTooltip(u8"Import it");
                    on->SetOnToggled([self, index, e](bool checked)
                                     {
                                         if (self->m_session && index < self->m_session->files.Size() && e < self->m_session->files[index].plan.entries.Size())
                                             self->m_session->files[index].plan.entries[e].enabled = checked;
                                         self->PlanChanged(index);
                                         self->Later([self]() { self->ShowFiles(); self->ShowDetail(); self->ShowWhy(); });
                                     });
                    RefPtr<gui::Button> name = MakeRef<gui::Button>(m_allocator);
                    name->AddClass(u8"import-part-name");
                    name->SetText(entry.targetName.IsEmpty() ? entry.sourceName.AsView() : entry.targetName.AsView());
                    name->SetOnClick([self, index, e]() { self->Later([self, index, e]() { self->SelectPart(index, e); }); });
                    line->AddChild(on.Get());
                    line->AddChild(name.Get());
                    list.AddChild(line.Get());
                }
            }
        }

        // Copies `from`'s reflected settings of the kinds a setting is (a flag, a number, a choice)
        // into `to`, an object of the same type.
        static void CopyScalars(Object& from, Object& to)
        {
            const Instance source(&from, from.GetType());
            const Instance target(&to, to.GetType());
            for (const TypeInfo* type = from.GetType(); type != nullptr; type = type->base)
                for (u32 i = 0; i < type->propertyCount; ++i)
                {
                    const PropertyInfo& property = type->properties[i];
                    if (property.type == nullptr || (property.type != &TypeOf<bool>() && property.type != &TypeOf<f32>() && property.type != &TypeOf<i32>() &&
                                                     !IsEnum(*property.type)))
                        continue;
                    MemCopy(property.address(target), property.address(source), property.type->size);
                }
        }

        // The importer's setting that leaves `kind` out of file's import, or null when it has none.
        [[nodiscard]] static bool* KindFlag(const AssetImport::File& file, pipeline::ImportResourceKind kind)
        {
            const pipeline::IFileImporter* importer = file.Importer();
            if (importer == nullptr || !file.options)
                return nullptr;
            const StringView name = importer->KindSetting(kind);
            return name.IsEmpty() ? nullptr : file.options->FindBoolSetting(name);
        }

        // The selected part's details: what it is, its name to give it (with the file's own name
        // when it differs), and its box.
        void ShowPart(gui::UIWidget& detail, usize index, usize e)
        {
            ImportWindow* self = this;
            const pipeline::ImportPlanEntry& entry = m_session->files[index].plan.entries[e];
            detail.AddChild(Text(pipeline::ImportResourceKindName(entry.kind), u8"import-part-kind").Get());
            RefPtr<gui::Panel> line = Row(u8"import-part-name-line");
            line->AddChild(Text(u8"Name", u8"form-label").Get());
            RefPtr<gui::TextField> name = MakeRef<gui::TextField>(m_allocator);
            name->SetText(entry.targetName.AsView());
            name->SetPlaceholder(entry.sourceName.AsView()); // when emptied, it keeps the importer's name
            name->SetOnTextChanging([self, index, e](StringView text)
                                    {
                                        if (self->m_session && index < self->m_session->files.Size() && e < self->m_session->files[index].plan.entries.Size())
                                            self->m_session->files[index].plan.entries[e].targetName = String(text);
                                        self->ShowWhy(); // a rename may free a name, or take one
                                    });
            // Committed (Enter, or leaving the field): the tree shows the new name.
            (void)name->AddEventListener(gui::EventType::Changed, [self](const gui::Event&) { self->Later([self]() { self->ShowFiles(); }); });
            line->AddChild(name.Get());
            detail.AddChild(line.Get());
            if (!entry.targetName.IsEmpty() && entry.targetName != entry.sourceName)
                detail.AddChild(Text(Format(u8"In the file it is {}.", entry.sourceName).AsView(), u8"form-help").Get());
            RefPtr<gui::CheckBox> on = MakeRef<gui::CheckBox>(m_allocator);
            on->SetText(u8"Import it");
            on->SetChecked(entry.enabled);
            on->SetOnToggled([self, index, e](bool checked)
                             {
                                 if (self->m_session && index < self->m_session->files.Size() && e < self->m_session->files[index].plan.entries.Size())
                                     self->m_session->files[index].plan.entries[e].enabled = checked;
                                 self->PlanChanged(index);
                                 self->Later([self]() { self->ShowFiles(); self->ShowWhy(); });
                             });
            detail.AddChild(on.Get());
            // What the part holds, as the file has it: read-only, from the importer's read.
            const AssetImport::File& file = m_session->files[index];
            const pipeline::IFileImporter* importer = file.Importer();
            if (importer == nullptr)
                return;
            const Array<pipeline::ImportPartFact> facts = importer->DescribePart(file.path.AsView(), file.options.Get(), file.prepared.Get(), entry);
            if (!facts.IsEmpty())
            {
                detail.AddChild(Text(u8"As the file has it", u8"import-part-kind").Get());
                for (const pipeline::ImportPartFact& fact : facts)
                {
                    RefPtr<gui::Panel> row = Row(u8"import-part-fact");
                    row->AddChild(Text(fact.label.AsView(), u8"form-label").Get());
                    row->AddChild(Text(fact.value.AsView(), u8"import-part-fact-value").Get());
                    detail.AddChild(row.Get());
                }
            }
            ShowPartSettings(detail, index, e);
        }

        // The part's own settings (a clip's root motion), as its importer makes them, in a grid; the
        // plan keeps them for the import. With others of its kind in the file, a button gives them
        // all the same.
        void ShowPartSettings(gui::UIWidget& detail, usize index, usize e)
        {
            AssetImport::File& file = m_session->files[index];
            pipeline::ImportPlanEntry& entry = file.plan.entries[e];
            const pipeline::IFileImporter* importer = file.Importer();
            if (!entry.settings && importer != nullptr)
                entry.settings = importer->PartSettings(file.path.AsView(), file.options.Get(), entry, m_session->group, m_allocator);
            if (!entry.settings)
                return;
            detail.AddChild(Text(u8"Its own settings", u8"import-part-kind").Get());
            m_partSettings = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
            const Instance target(entry.settings.Get(), entry.settings->GetType());
            m_partSettings->SetObjects(Span<const Instance>(&target, 1u));
            RefPtr<gui::PropertyGrid> grid = MakeRef<gui::PropertyGrid>(m_allocator);
            grid->AddClass(u8"import-settings");
            grid->SetFitsRows(true);
            grid->SetSource(m_partSettings.Get());
            detail.AddChild(grid.Get());
            usize others = 0;
            for (usize i = 0; i < file.plan.entries.Size(); ++i)
                others += i != e && file.plan.entries[i].kind == entry.kind ? 1u : 0u;
            if (others == 0)
                return;
            ImportWindow* self = this;
            RefPtr<gui::Button> share = MakeRef<gui::Button>(m_allocator);
            share->SetText(Format(u8"Apply to all {} in this file", pipeline::ImportResourceKindLabel(entry.kind)).AsView());
            share->SetTooltip(u8"Gives every part of this kind in the file these settings");
            share->SetOnClick([self, index, e]() { (void)self->SharePartSettings(index, e); });
            detail.AddChild(share.Get());
        }

        // Shows what the file's name clashes with (AssetImport::ClashOf): nothing when the name is
        // free or held by the file's own earlier import; an asset of another type, which must be
        // renamed; or another file's asset of the same type, with Replace and Keep both.
        void ShowClash(gui::UIWidget& detail, const AssetImport::File& file, usize index)
        {
            const pipeline::ImportClash clash = AssetImport::ClashOf(*m_session, file);
            if (clash.kind == pipeline::ImportClashKind::None || clash.kind == pipeline::ImportClashKind::Reimport)
                return;
            const bool chosen = pipeline::ImportClashAllows(clash, file.onConflict);
            detail.AddChild(Text(pipeline::DescribeImportClash(clash).AsView(), chosen ? u8"form-help" : u8"form-help refused").Get());
            if (clash.kind != pipeline::ImportClashKind::SameType)
                return;
            ImportWindow* self = this;
            RefPtr<gui::Panel> choices = Row(u8"segmented");
            auto option = [&](StringView text, StringView tip, pipeline::ImportConflictChoice choice)
            {
                RefPtr<gui::Button> segment = MakeRef<gui::Button>(m_allocator);
                segment->AddClass(u8"segment");
                segment->SetText(text);
                segment->SetTooltip(tip);
                segment->SetToggle(true);
                segment->SetChecked(file.onConflict == choice);
                segment->SetOnClick([self, index, choice]()
                                    {
                                        if (self->m_session && index < self->m_session->files.Size())
                                            self->m_session->files[index].onConflict = choice;
                                        self->Later([self]()
                                                    {
                                                        self->ShowDetail();
                                                        self->ShowWhy();
                                                    });
                                    });
                choices->AddChild(segment.Get());
            };
            option(u8"Replace", u8"The asset there takes this file; what refers to it keeps working", pipeline::ImportConflictChoice::Replace);
            option(u8"Keep both", u8"This one under the next free name", pipeline::ImportConflictChoice::KeepBoth);
            detail.AddChild(choices.Get());
        }

        // Shows what Import will do, or why it waits, and enables Import to match.
        void ShowWhy()
        {
            if (!m_session)
                return;
            String why;
            const bool can = AssetImport::CanImport(*m_session, why);
            if (auto* line = Find<gui::Label>(u8"import-why"))
            {
                line->SetText(why.AsView());
                if (can)
                    line->RemoveClass(u8"refused");
                else
                    line->AddClass(u8"refused");
            }
            if (gui::Button* run = ImportButton())
            {
                run->SetEnabled(can);
                usize kept = 0;
                for (const AssetImport::File& file : m_session->files)
                    kept += file.kept ? 1u : 0u;
                run->SetText(kept > 1 ? Format(u8"Import {} Files", kept).AsView() : StringView(u8"Import"));
            }
        }

        // Runs `work` after the event that asked for it (a control's own callback rebuilding the
        // controls).
        void Later(Function<void()> work)
        {
            gui::MutationQueue* queue = m_body ? m_body->GetMutationQueue() : nullptr;
            if (queue == nullptr)
                work();
            else
                queue->Enqueue(Move(work));
        }
        [[nodiscard]] RefPtr<gui::Label> Text(StringView text, StringView classes)
        {
            RefPtr<gui::Label> label = MakeRef<gui::Label>(m_allocator);
            label->AddClasses(classes);
            label->SetText(text);
            return label;
        }
        [[nodiscard]] RefPtr<gui::Panel> Row(StringView cls)
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(cls);
            return row;
        }
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_body ? Cast<T>(FindIn(*m_body, id)) : nullptr;
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

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        RefPtr<AssetImport::Session> m_session;
        usize m_selected = 0;
        struct OpenKind
        {
            usize file = 0;
            pipeline::ImportResourceKind kind = pipeline::ImportResourceKind::Asset;
        };
        Array<OpenKind> m_openKinds; // the kinds open in the tree, by file
        i64 m_part = -1;             // the selected part, an entry of the selected file's plan; -1: the file
        UniquePtr<gui::ReflectedPropertySource> m_settings; // the selected file's settings, which its grid shows
        UniquePtr<gui::ReflectedPropertySource> m_partSettings; // the selected part's own settings
        RefPtr<gui::PropertyGrid> m_settingsGrid;
        UniquePtr<editor::IImportPreview> m_preview; // the selected file's, while its path and type stay
        String m_previewPath;
        String m_previewType;
    };
}

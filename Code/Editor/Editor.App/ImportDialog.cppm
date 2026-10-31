// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :import_dialog partition
//
// The import window, over its markup (Data/Editor/Import.xml): one window for one file or many.
// It shows where the files go, in a field that opens the group picker; the files, when there are
// several, each kept or dropped, with its state (reading, ready, failed) and one selected; and the
// selected file's importer (a choice when several accept it), its options, and what it would
// create by kind, each item kept or dropped and named, with a kind's box keeping or dropping all of
// that kind. A file still being read says so and shows its detail when it is ready. The line under
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
import pipeline.importer;
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
                    self->ShowDetail();
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

        /// Imports what is kept and closes; false (the window stays, saying why) when it cannot yet.
        bool Import()
        {
            if (!m_session || !m_services.import->Commit(*m_session))
                return false;
            Close();
            return true;
        }
        /// Shows file `index`'s detail.
        void Select(usize index)
        {
            if (!m_session || index >= m_session->files.Size())
                return;
            m_selected = index;
            ShowFiles();
            ShowDetail();
        }

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

    private:
        static constexpr f32 kWidth = 640.0f;
        static constexpr f32 kHeight = 520.0f;

        void Release()
        {
            if (!m_session)
                return;
            m_session->onChanged = {};
            m_session->closed = true;
            m_session = {};
        }
        void ShowAll()
        {
            if (auto* group = Find<gui::Button>(u8"import-group"); group != nullptr && m_session)
                group->SetText(GroupPicker::ShownName(*m_session->group).AsView());
            ShowFiles();
            ShowDetail();
            ShowWhy();
        }

        // Shows the files when there are several: each kept or not, its state, the selected one marked.
        void ShowFiles()
        {
            auto* scroll = Find<gui::UIWidget>(u8"import-files-scroll");
            auto* list = Find<gui::UIWidget>(u8"import-files");
            if (scroll == nullptr || list == nullptr || !m_session)
                return;
            const bool several = m_session->files.Size() > 1;
            scroll->SetDisplayed(several);
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            if (!several)
                return;
            ImportWindow* self = this;
            for (usize i = 0; i < m_session->files.Size(); ++i)
            {
                const AssetImport::File& file = m_session->files[i];
                RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
                row->AddClass(u8"import-file");
                if (i == m_selected)
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
                RefPtr<gui::Button> name = MakeRef<gui::Button>(m_allocator);
                name->AddClass(u8"import-file-name");
                name->SetText(file.Name());
                name->SetTooltip(file.path.AsView());
                name->SetOnClick([self, i]() { self->Later([self, i]() { self->Select(i); }); }); // the rows are rebuilt, so not inside the click
                RefPtr<gui::Label> state = MakeRef<gui::Label>(m_allocator);
                state->AddClass(u8"badge");
                state->AddClass(file.state == AssetImport::File::State::Ready    ? StringView(u8"success")
                                : file.state == AssetImport::File::State::Failed ? StringView(u8"danger")
                                                                                  : StringView(u8"warning"));
                state->SetText(file.state == AssetImport::File::State::Ready    ? StringView(u8"Ready")
                               : file.state == AssetImport::File::State::Failed ? StringView(u8"Failed")
                                                                                 : StringView(u8"Reading"));
                row->AddChild(kept.Get());
                row->AddChild(name.Get());
                row->AddChild(state.Get());
                list->AddChild(row.Get());
            }
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
            // Its options.
            if (file.options)
                for (const pipeline::ImportOptions::Toggle& toggle : file.options->Toggles())
                {
                    RefPtr<gui::CheckBox> box = MakeRef<gui::CheckBox>(m_allocator);
                    box->SetText(toggle.label);
                    box->SetTooltip(toggle.description);
                    box->SetChecked(toggle.value != nullptr && *toggle.value);
                    bool* value = toggle.value;
                    box->SetOnToggled([value](bool on)
                                      {
                                          if (value != nullptr)
                                              *value = on;
                                      });
                    detail->AddChild(box.Get());
                }
            ShowClash(*detail, file, index);
            if (file.plan.IsEmpty())
            {
                detail->AddChild(Text(u8"It makes one asset, named after the file.", u8"form-help").Get());
                return;
            }
            // What it would create, by kind: a kind's box keeps or drops all of that kind.
            static constexpr pipeline::ImportResourceKind kKinds[] = {
                pipeline::ImportResourceKind::Mesh,          pipeline::ImportResourceKind::Material,  pipeline::ImportResourceKind::Texture,
                pipeline::ImportResourceKind::Skeleton,      pipeline::ImportResourceKind::AnimationClip, pipeline::ImportResourceKind::Collision,
                pipeline::ImportResourceKind::Asset};
            for (const pipeline::ImportResourceKind kind : kKinds)
            {
                usize count = 0, kept = 0;
                for (const pipeline::ImportPlanEntry& entry : file.plan.entries)
                    if (entry.kind == kind)
                    {
                        ++count;
                        kept += entry.enabled ? 1u : 0u;
                    }
                if (count == 0)
                    continue;
                RefPtr<gui::CheckBox> all = MakeRef<gui::CheckBox>(m_allocator);
                all->AddClass(u8"import-kind");
                all->SetText(Format(u8"{} ({})", pipeline::ImportResourceKindLabel(kind), count).AsView());
                all->SetChecked(kept == count);
                all->SetIndeterminate(kept > 0 && kept < count);
                all->SetOnToggled([self, index, kind](bool on)
                                  {
                                      if (!self->m_session || index >= self->m_session->files.Size())
                                          return;
                                      for (pipeline::ImportPlanEntry& entry : self->m_session->files[index].plan.entries)
                                          if (entry.kind == kind)
                                              entry.enabled = on;
                                      self->Later([self]() { self->ShowDetail(); });
                                  });
                detail->AddChild(all.Get());
                for (usize e = 0; e < file.plan.entries.Size(); ++e)
                {
                    const pipeline::ImportPlanEntry& entry = file.plan.entries[e];
                    if (entry.kind != kind)
                        continue;
                    RefPtr<gui::Panel> line = Row(u8"import-entry");
                    RefPtr<gui::CheckBox> on = MakeRef<gui::CheckBox>(m_allocator);
                    on->SetChecked(entry.enabled);
                    on->SetOnToggled([self, index, e](bool checked)
                                     {
                                         if (self->m_session && index < self->m_session->files.Size() && e < self->m_session->files[index].plan.entries.Size())
                                             self->m_session->files[index].plan.entries[e].enabled = checked;
                                         self->Later([self]() { self->ShowDetail(); });
                                     });
                    RefPtr<gui::TextField> name = MakeRef<gui::TextField>(m_allocator);
                    name->SetText(entry.targetName.AsView());
                    name->SetPlaceholder(entry.sourceName.AsView()); // when emptied, it keeps the importer's name
                    name->SetOnTextChanging([self, index, e](StringView text)
                                            {
                                                if (self->m_session && index < self->m_session->files.Size() && e < self->m_session->files[index].plan.entries.Size())
                                                    self->m_session->files[index].plan.entries[e].targetName = String(text);
                                                self->ShowWhy(); // a rename may free a name, or take one
                                            });
                    // When the name is committed (Enter, or leaving the field), the clash line shows
                    // what the name clashes with now.
                    (void)name->AddEventListener(gui::EventType::Changed, [self](const gui::Event&) { self->Later([self]() { self->ShowDetail(); }); });
                    line->AddChild(on.Get());
                    line->AddChild(name.Get());
                    detail->AddChild(line.Get());
                }
            }
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
                run->SetEnabled(can);
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
    };
}

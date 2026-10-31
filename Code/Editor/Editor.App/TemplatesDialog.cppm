// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :templates_dialog partition
//
// Export Templates: the player builds this editor exports with, in a window over its markup
// (Data/Editor/Templates.xml) opened by Edit > Export Templates (and by Export's own button, over
// that dialog). The templates belong to the editor, not to a project: every project shares them.
// They are listed on the left, each with the icon its bundle carries, its name, platform, config
// and engine version; the host build is marked as this editor's own, and one built for another
// engine version gets a warning. The selected template's details are on the right (its id,
// compiler, engine and whether it is this editor's, where it lives, its player, runtime files,
// symbols and notes; empty details are left out), with Reveal and Remove (also Delete on the list).
// Remove asks first and cannot remove the host build. Install from Folder copies a template bundle
// in; Create from Build makes one from a Bin/<Config> build folder; with no templates, it says how
// to get one. What it needs from outside (the registry, the OS folder picker, opening a folder)
// comes through TemplatesDialogSeams, so a test drives it without a window system.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:templates_dialog;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// What the templates dialog asks of the application.
    struct TemplatesDialogSeams
    {
        /// The templates root installs and removals go to (Preferences > Export, or the default).
        Function<String()> templatesRoot;
        /// Fills `registry` with the templates this host sees (the root's, then the host build).
        Function<void(editor::TemplateRegistry&)> refresh;
        /// Asks for a folder; `chosen` gets its path, or nothing when the user cancels.
        Function<void(Function<void(String)> chosen)> pickFolder;
        /// Shows a folder in the OS file manager.
        Function<void(StringView)> revealFolder;
    };

    /// A template's row (and an export preset's): its icon, its name over its summary, and a warning
    /// saying what is wrong when something is (an engine version other than this editor's, no
    /// template here).
    class TemplateRow final : public gui::UIWidget
    {
        RTTI_OBJECT(TemplateRow, gui::UIWidget)
    public:
        TemplateRow()
        {
            AddClass(u8"template-row");
            m_icon = Part<gui::Image>(*this, u8"template-row-icon");
            RefPtr<gui::Panel> text = Part<gui::Panel>(*this, u8"template-row-text");
            m_name = Part<gui::Label>(*text, u8"row-name");
            m_summary = Part<gui::Label>(*text, u8"template-summary");
            m_warning = Part<gui::Image>(*this, u8"badge-icon");
            m_warning->AddClass(u8"warning");
            m_warning->SetSource(String(u8"Editor/Icons/warning.svg"));
        }
        /// An empty `warning` shows no warning.
        void Show(RefPtr<gui::Drawable> icon, StringView name, StringView summary, StringView warning)
        {
            m_icon->SetDrawable(foundation::core::Move(icon));
            m_name->SetText(name);
            m_summary->SetText(summary);
            m_warning->SetTooltip(warning);
            m_warning->SetVisible(!warning.IsEmpty());
        }
        [[nodiscard]] bool ShowsWarning() const { return m_warning->IsVisible(); }
        [[nodiscard]] StringView Name() const { return m_name->GetText(); }

    private:
        template <typename T>
        RefPtr<T> Part(gui::Node& into, StringView cls)
        {
            RefPtr<T> part = MakeRef<T>(MemoryAllocator());
            part->AddClass(cls);
            into.AddChild(part.Get());
            return part;
        }

        RefPtr<gui::Image> m_icon;
        RefPtr<gui::Label> m_name;
        RefPtr<gui::Label> m_summary;
        RefPtr<gui::Image> m_warning;
    };

    /// TemplatesDialog::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kTemplatesDialogPath = u8"Editor/Templates.xml";

    class TemplatesDialog
    {
    public:
        /// Its markup's path, under the data root.
        static constexpr const char8_t* kPath = kTemplatesDialogPath;

        TemplatesDialog(IAllocator& allocator, editor::EditorContext& context, TemplatesDialogSeams seams, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_context(&context), m_seams(Move(seams)), m_body(Move(body)), m_icons(allocator),
              m_details(allocator), m_model(*this)
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Export Templates");
            m_window->AddClass(u8"templates-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            TemplatesDialog* self = this;
            m_list = Find<gui::ListView>(u8"templates-list");
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.Create = [self](u32) -> RefPtr<gui::Node> { return MakeRef<TemplateRow>(self->m_allocator); };
                rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index.Row); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                m_list->SetOnSelectionChanged([self](gui::ModelIndex) { self->ShowDetails(); });
                (void)m_list->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                               {
                                                   const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                   if (static_cast<gui::KeyCode>(key.KeyCode) == gui::KeyCode::Delete && key.Modifiers == 0)
                                                   {
                                                       self->RemoveSelected();
                                                       e.Handled = true;
                                                   }
                                               });
            }
            BuildDetails();
            m_reveal = Find<gui::Button>(u8"templates-reveal");
            m_remove = Find<gui::Button>(u8"templates-remove");
            if (m_reveal != nullptr)
                m_reveal->SetOnClick([self]()
                                     {
                                         if (const editor::ExportTemplate* tmpl = self->Selected(); tmpl != nullptr && self->m_seams.revealFolder)
                                             self->m_seams.revealFolder(tmpl->directory.AsView());
                                     });
            if (m_remove != nullptr)
                m_remove->SetOnClick([self]() { self->RemoveSelected(); });
            if (auto* install = Find<gui::Button>(u8"templates-install"))
            {
                install->SetTooltip(u8"Copy in a template bundle (a folder holding a template.xml)");
                install->SetOnClick([self]() { self->InstallFromFolder(); });
            }
            if (auto* create = Find<gui::Button>(u8"templates-create"))
            {
                create->SetTooltip(u8"Make a template from a player build folder (Bin/<Config>/<Platform>-<Compiler>)");
                create->SetOnClick([self]() { self->CreateFromBuild(); });
            }
            if (auto* close = Find<gui::Button>(u8"templates-close"))
                close->SetOnClick([self]() { self->Close(); });
            // However it closes (Close, its close button, Escape), its owner is told.
            m_window->SetOnClose([self]()
                                 {
                                     Function<void()> closed = Move(self->m_closed);
                                     if (closed)
                                         closed();
                                 });
        }
        TemplatesDialog(const TemplatesDialog&) = delete;
        TemplatesDialog& operator=(const TemplatesDialog&) = delete;
        ~TemplatesDialog()
        {
            Close();
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
        }

        /// Opens the dialog over `root`, reading the templates again; `closed` is called when it
        /// closes (Export then reads them again). False with no markup or when one is already open.
        bool Open(gui::Node& root, Function<void()> closed = {})
        {
            if (!m_window || m_window->IsOpen())
                return false;
            m_root = &root;
            m_closed = Move(closed);
            Refresh();
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }
        void Close()
        {
            if (IsOpen())
                m_window->Close();
        }

        /// Re-reads the templates, keeping the selected one selected while it is there.
        void Refresh()
        {
            String selectedId;
            if (const editor::ExportTemplate* tmpl = Selected())
                selectedId = tmpl->id;
            if (m_seams.refresh)
                m_seams.refresh(m_registry);
            m_icons.Clear(); // a reinstall may carry a new icon
            m_model.Reset();
            usize select = 0;
            for (usize i = 0; i < m_registry.Count(); ++i)
                if (m_registry.At(i)->id == selectedId)
                    select = i;
            Select(select);
            ShowDetails();
        }

        /// The templates listed, in order (the registry's).
        [[nodiscard]] usize TemplateCount() const noexcept { return m_registry.Count(); }
        [[nodiscard]] const editor::ExportTemplate* TemplateAt(usize index) const { return index < m_registry.Count() ? m_registry.At(index) : nullptr; }
        /// The selected template, or null.
        [[nodiscard]] const editor::ExportTemplate* Selected() const
        {
            const i32 index = m_list != nullptr ? m_list->GetSelectedItem() : -1;
            return index >= 0 ? TemplateAt(static_cast<usize>(index)) : nullptr;
        }
        void Select(usize index)
        {
            if (m_list == nullptr)
                return;
            if (index < m_registry.Count())
                m_list->SetSelectedItem(static_cast<i32>(index));
            else
                m_list->ClearSelection();
            ShowDetails();
        }

        /// The detail actions and a detail's value by its label ("Engine", "Folder"; empty when
        /// it is left out), for tests.
        [[nodiscard]] gui::Button* RemoveButton() const noexcept { return m_remove; }
        [[nodiscard]] gui::Button* RevealButton() const noexcept { return m_reveal; }
        [[nodiscard]] StringView DetailText(StringView label) const
        {
            for (const Detail& detail : m_details)
                if (detail.label == label && detail.value->IsDisplayed())
                    return detail.value->GetText();
            return {};
        }
        /// A detail's value label by its caption; null with none.
        [[nodiscard]] gui::Label* DetailValue(StringView label) const
        {
            for (const Detail& detail : m_details)
                if (detail.label == label)
                    return detail.value;
            return nullptr;
        }
        [[nodiscard]] bool ShowsEmpty() const
        {
            const auto* empty = Find<gui::UIWidget>(u8"templates-empty");
            return empty != nullptr && empty->IsDisplayed();
        }
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }

        void InstallFromFolder()
        {
            if (!m_seams.pickFolder)
                return;
            TemplatesDialog* self = this;
            m_seams.pickFolder([self](String folder)
                               {
                                   const String root = self->m_seams.templatesRoot ? self->m_seams.templatesRoot() : String();
                                   String id;
                                   if (editor::ImportTemplate(folder.AsView(), root.AsView(), &id).IsOk())
                                   {
                                       self->m_context->Notify(editor::NoticeKind::Success, Format(u8"Installed template '{}'.", id).AsView());
                                       self->RefreshTo(id.AsView());
                                   }
                                   else
                                       self->m_context->Notify(editor::NoticeKind::Error, u8"That folder holds no template (no valid template.xml).");
                               });
        }
        void CreateFromBuild()
        {
            if (!m_seams.pickFolder)
                return;
            TemplatesDialog* self = this;
            m_seams.pickFolder([self](String folder)
                               {
                                   const String root = self->m_seams.templatesRoot ? self->m_seams.templatesRoot() : String();
                                   String id, dir;
                                   if (editor::CreateTemplate(folder.AsView(), root.AsView(), editor::TemplateOutput::Install, &id, &dir).IsOk())
                                   {
                                       self->m_context->Notify(editor::NoticeKind::Success, Format(u8"Created template '{}'.", id).AsView());
                                       self->RefreshTo(id.AsView());
                                   }
                                   else
                                       self->m_context->Notify(editor::NoticeKind::Error,
                                                               u8"No player build there: pick a Bin/<Config>/<Platform>-<Compiler> folder holding Engine.Player.");
                               });
        }
        /// Removes the selected template once the user confirms (`confirmed` skips the question);
        /// the host build is never removed.
        void RemoveSelected(bool confirmed = false)
        {
            const editor::ExportTemplate* tmpl = Selected();
            if (tmpl == nullptr || tmpl->isHost)
                return;
            const String id(tmpl->id);
            if (!confirmed)
            {
                if (m_root == nullptr)
                    return;
                auto box = MakeRef<gui::MessageBox>(m_allocator);
                box->Configure(u8"Remove Template",
                               Format(u8"Remove '{}'? Its folder is deleted; presets that pick it fall back to another template for their platform.", tmpl->name)
                                   .AsView(),
                               gui::MessageBox::Buttons::Ok);
                box->ClearButtons();
                (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
                (void)box->AddButton(u8"Remove", u8"remove", gui::DialogRole::Destructive);
                TemplatesDialog* self = this;
                gui::MessageBox* raw = box.Get();
                (void)box->AddEventListener(gui::EventType::Changed, [self, raw, id](const gui::Event&)
                                            {
                                                const editor::ExportTemplate* now = self->Selected();
                                                if (raw->GetResultName().AsView() == StringView(u8"remove") && now != nullptr && now->id == id)
                                                    self->RemoveSelected(true);
                                            });
                box->OpenModal(*m_root);
                return;
            }
            const String root = m_seams.templatesRoot ? m_seams.templatesRoot() : String();
            if (editor::RemoveTemplate(root.AsView(), id.AsView()).IsOk())
                m_context->Notify(editor::NoticeKind::Success, Format(u8"Removed template '{}'.", id).AsView());
            else
                m_context->Notify(editor::NoticeKind::Error, u8"Removing the template failed (see the Console).");
            Refresh();
        }

    private:
        static constexpr f32 kWidth = 760.0f;
        static constexpr f32 kHeight = 460.0f;

        struct Detail
        {
            String label;
            gui::Label* caption = nullptr;
            gui::Label* value = nullptr;
        };

        // The templates as the list's model; a template's id identifies its row.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(TemplatesDialog& dialog) : m_dialog(dialog) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_dialog.m_registry.Count(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                const editor::ExportTemplate* tmpl = index.Row >= 0 ? m_dialog.TemplateAt(static_cast<usize>(index.Row)) : nullptr;
                return role == gui::ModelRole::Display && tmpl != nullptr ? gui::Variant(tmpl->name.AsView()) : gui::Variant{};
            }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                const editor::ExportTemplate* tmpl = index.Row >= 0 ? m_dialog.TemplateAt(static_cast<usize>(index.Row)) : nullptr;
                return tmpl != nullptr ? Hash<StringView>{}(tmpl->id.AsView()) : 0;
            }
            void Reset() { NotifyReset(); }

        private:
            TemplatesDialog& m_dialog;
        };

        /// "Linux64 · Release · 0.1.0", the line under a template's name.
        [[nodiscard]] static String SummaryOf(const editor::ExportTemplate& tmpl)
        {
            String text(tmpl.platform.AsView());
            text += u8" · ";
            text += tmpl.EffectiveConfig();
            if (!tmpl.engineVersion.IsEmpty())
            {
                text += u8" · ";
                text += tmpl.engineVersion.AsView();
            }
            if (tmpl.isHost)
                text += u8" · this editor's own";
            return text;
        }
        // The icon a template is shown with, made once per template and folder (so a reinstall
        // gets a fresh one).
        [[nodiscard]] RefPtr<gui::Drawable> IconFor(const editor::ExportTemplate& tmpl)
        {
            String key(tmpl.id.AsView());
            key += u8"|";
            key += tmpl.directory.AsView();
            if (const RefPtr<gui::Drawable>* known = m_icons.Find(key))
                return *known;
            const String svg = editor::TemplateIconSvg(tmpl, m_allocator);
            RefPtr<gui::Drawable> icon(gui::SvgDrawable::FromText(m_allocator, svg.AsView()).Get());
            m_icons.InsertOrAssign(Move(key), icon);
            return icon;
        }
        void BindRow(gui::Node& node, i32 row)
        {
            auto* shown = Cast<TemplateRow>(&node);
            const editor::ExportTemplate* tmpl = row >= 0 ? TemplateAt(static_cast<usize>(row)) : nullptr;
            if (shown != nullptr && tmpl != nullptr)
                shown->Show(IconFor(*tmpl), tmpl->name.AsView(), SummaryOf(*tmpl).AsView(),
                            editor::TemplateEngineMatches(*tmpl) ? StringView{} : StringView(u8"Built for another engine version"));
        }
        void RefreshTo(StringView id)
        {
            Refresh();
            for (usize i = 0; i < m_registry.Count(); ++i)
                if (m_registry.At(i)->id.AsView() == id)
                    Select(i);
        }

        // Builds a caption and a value per detail, in the form's two columns.
        void BuildDetails()
        {
            auto* fields = Find<gui::UIWidget>(u8"templates-fields");
            if (fields == nullptr)
                return;
            static constexpr StringView kLabels[] = {u8"Platform", u8"Config", u8"Compiler", u8"Engine", u8"Source", u8"Folder",
                                                     u8"Player", u8"Runtime files", u8"Symbols", u8"Notes"};
            for (const StringView label : kLabels)
            {
                RefPtr<gui::Label> caption = MakeRef<gui::Label>(m_allocator);
                caption->AddClass(u8"form-label");
                caption->SetText(label);
                RefPtr<gui::Label> value = MakeRef<gui::Label>(m_allocator);
                value->AddClass(u8"template-value");
                fields->AddChild(caption.Get());
                fields->AddChild(value.Get());
                m_details.PushBack(Detail{String(label), caption.Get(), value.Get()});
            }
        }
        // Sets a detail's value; an empty one leaves its row out.
        void SetDetail(StringView label, StringView text)
        {
            for (Detail& detail : m_details)
                if (detail.label == label)
                {
                    detail.value->SetText(text);
                    detail.caption->SetDisplayed(!text.IsEmpty());
                    detail.value->SetDisplayed(!text.IsEmpty());
                }
        }
        void ShowDetails()
        {
            const editor::ExportTemplate* tmpl = Selected();
            const bool none = m_registry.Count() == 0;
            if (auto* empty = Find<gui::UIWidget>(u8"templates-empty"))
                empty->SetDisplayed(none);
            if (auto* detail = Find<gui::UIWidget>(u8"templates-detail"))
                detail->SetDisplayed(tmpl != nullptr);
            if (tmpl == nullptr)
                return;
            if (auto* icon = Find<gui::Image>(u8"templates-icon"))
                icon->SetDrawable(IconFor(*tmpl));
            if (auto* name = Find<gui::Label>(u8"templates-name"))
                name->SetText(tmpl->name.AsView());
            if (auto* id = Find<gui::Label>(u8"templates-id"))
                id->SetText(tmpl->id.AsView());
            SetDetail(u8"Platform", tmpl->platform.AsView());
            SetDetail(u8"Config", tmpl->EffectiveConfig());
            SetDetail(u8"Compiler", tmpl->compiler.AsView());
            const bool matches = editor::TemplateEngineMatches(*tmpl);
            String engine(tmpl->engineVersion.IsEmpty() ? StringView(u8"unstamped") : tmpl->engineVersion.AsView());
            if (!matches)
                engine += Format(u8" (this editor is {}; an export still runs)", editor::kEngineVersionString).AsView();
            SetDetail(u8"Engine", engine.AsView());
            for (Detail& detail : m_details)
                if (detail.label == StringView(u8"Engine"))
                {
                    if (matches)
                        detail.value->RemoveClass(u8"warning");
                    else
                        detail.value->AddClass(u8"warning");
                }
            SetDetail(u8"Source", tmpl->isHost ? StringView(u8"This editor's own player build (always available)") : StringView(u8"Installed in the templates folder"));
            SetDetail(u8"Folder", tmpl->directory.AsView());
            SetDetail(u8"Player", tmpl->playerBinary.AsView());
            String files;
            for (const String& sidecar : tmpl->sidecars)
            {
                if (!files.IsEmpty())
                    files += u8", ";
                files += sidecar.AsView();
            }
            SetDetail(u8"Runtime files", files.IsEmpty() ? StringView(u8"none") : files.AsView());
            SetDetail(u8"Symbols", tmpl->symbols.IsEmpty() ? String(u8"none").AsView() : Format(u8"{} file(s), staged when a preset asks", tmpl->symbols.Size()).AsView());
            SetDetail(u8"Notes", tmpl->notes.AsView());
            // The host build is this editor's own, never on disk as a bundle: it cannot be removed.
            if (m_remove != nullptr)
            {
                m_remove->SetEnabled(!tmpl->isHost);
                m_remove->SetTooltip(tmpl->isHost ? StringView(u8"This editor's own build cannot be removed") : StringView(u8"Delete this template's folder"));
            }
            if (m_reveal != nullptr)
                m_reveal->SetEnabled(!tmpl->directory.IsEmpty());
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
        editor::EditorContext* m_context;
        TemplatesDialogSeams m_seams;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        gui::Node* m_root = nullptr; // what it opened over (its confirms open there too)
        Function<void()> m_closed;
        editor::TemplateRegistry m_registry;
        HashMap<String, RefPtr<gui::Drawable>> m_icons; // by template id and folder
        Array<Detail> m_details;
        gui::ListView* m_list = nullptr;
        gui::Button* m_reveal = nullptr;
        gui::Button* m_remove = nullptr;
        Model m_model;
    };

    RTTI_DEFINE_OBJECT(TemplateRow, "rtti::editor::editor::app")
}

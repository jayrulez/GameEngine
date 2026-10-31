// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :preferences_dialog partition.
//
// EditorPreferencesDialog: the user's own editor settings (the foundation.settings store at
// <user-data>/editor.settings.xml), distinct from Project Settings, which edits the project. It is
// a modal window from the editor's markup (Data/Editor/Preferences.xml), with a tab per category.
// Appearance holds the look's inputs: the scheme, the accent (one of the editor's, or any
// #rrggbb), the density, reduced motion and the UI scale, plus whether axis views are
// orthographic; each change to the look shows at once. Export holds the templates' folder, Agent
// access the MCP host (enabled, its port, its token), and Game preview the sizes the Game tab
// offers. Shortcuts lists every action with its chord: a click captures the next keys, and Reset
// restores the declared chord. Then comes a tab per category that other parts of the editor
// contribute, whose checks apply as they change. Save writes everything into the store and
// persists it; a chord another action holds is refused and a port out of range is kept as it was,
// each with a warning. Cancel, or closing the window, restores the look and discards the rest.
module;
#include "Core/Prelude.h"

export module editor.app:preferences_dialog;

import foundation.core;
import foundation.gui;
import foundation.settings;
import editor.core;
import :editor_window; // EditorLook, LookFrom, StoreLook, the accents and densities
import :shortcut_capture;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// EditorPreferencesDialog::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kEditorPreferencesDialogPath = u8"Editor/Preferences.xml";

    class EditorPreferencesDialog
    {
    public:
        /// Its markup's path, under the data root.
        static constexpr const char8_t* kPath = kEditorPreferencesDialogPath;

        /// The services it works with.
        struct Services
        {
            foundation::settings::Settings* user = nullptr; // the user's store
            editor::EditorActionRegistry* actions = nullptr; // what the Shortcuts tab lists and edits
            /// Shows the user a warning (a chord another action holds).
            Function<void(StringView)> warn;
            /// The settings other parts of the editor contribute, a tab per category.
            const Array<editor::EditorContext::EditorSettingsContribution>* contributions = nullptr;
            /// Called when the MCP section was saved, so the host follows it for the open project.
            Function<void()> mcpChanged;
            /// Shows a look (the window's SetLook).
            Function<void(const EditorLook&)> showLook;
            /// Persists the user's store; false when it could not be written.
            Function<bool()> persist;
        };

        EditorPreferencesDialog(IAllocator& allocator, Services services, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_services(Move(services)), m_body(Move(body))
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Preferences");
            m_window->AddClass(u8"preferences-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            Bind();
        }
        EditorPreferencesDialog(const EditorPreferencesDialog&) = delete;
        EditorPreferencesDialog& operator=(const EditorPreferencesDialog&) = delete;
        ~EditorPreferencesDialog() { Close(); }

        /// Opens the dialog over `root` with its controls as the store has them; false with no markup.
        bool Open(gui::Node& root)
        {
            if (!m_window || m_window->IsOpen())
                return false;
            m_original = LookFrom(m_services.user != nullptr ? m_services.user->Find<editor::EditorUiSettings>() : nullptr);
            m_look = m_original;
            m_saved = false;
            m_shortcutEdits = editor::ShortcutEdits{};
            ShowControls();
            ShowSettings();
            ShowShortcuts();
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }

        /// Keeps the look: writes it into the store, persists it and closes the window.
        bool Save()
        {
            if (m_services.user != nullptr)
            {
                StoreLook(m_services.user->Section<editor::EditorUiSettings>(), m_look);
                m_services.user->MarkChanged<editor::EditorUiSettings>();
                SaveSettings();
                if (!m_shortcutEdits.IsEmpty() && m_services.actions != nullptr)
                {
                    Array<String> collisions;
                    (void)m_shortcutEdits.Apply(*m_services.actions, m_services.user->Section<editor::EditorShortcutSettings>(), &collisions);
                    m_services.user->MarkChanged<editor::EditorShortcutSettings>();
                    for (const String& collision : collisions)
                        if (m_services.warn)
                            m_services.warn(Format(u8"Shortcut kept: {}", collision).AsView());
                }
            }
            const bool persisted = !m_services.persist || m_services.persist();
            m_saved = true;
            if (m_window)
                m_window->Close();
            return persisted;
        }
        /// Puts the look back as it was and closes.
        void Cancel()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close(); // closing it restores the look
        }

        /// The look as the controls have it now.
        [[nodiscard]] const EditorLook& Look() const noexcept { return m_look; }
        /// The window (its controls by id, for a test).
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }

    private:
        static constexpr f32 kWidth = 560.0f;
        static constexpr f32 kHeight = 440.0f;

        void Close()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close();
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

        // Binds the controls once: each change the user makes shows the look at once.
        void Bind()
        {
            EditorPreferencesDialog* self = this;
            const auto onChange = [self](gui::Node* node, Function<void()> apply)
            {
                if (node != nullptr)
                    (void)node->AddEventListener(gui::EventType::Changed, [self, applyChange = Move(apply)](const gui::Event&)
                                                 {
                                                     if (!self->m_showing)
                                                         applyChange();
                                                 });
            };
            onChange(Find<gui::Button>(u8"pref-dark"), [self]() { self->Change([](EditorLook& look) { look.dark = true; }); });
            onChange(Find<gui::Button>(u8"pref-light"), [self]() { self->Change([](EditorLook& look) { look.dark = false; }); });
            if (gui::Node* accents = Find<gui::Node>(u8"pref-accents"))
                for (const EditorAccent& accent : kEditorAccents)
                {
                    RefPtr<gui::Button> segment = MakeRef<gui::Button>(m_allocator);
                    segment->AddClass(u8"segment");
                    segment->SetText(accent.name);
                    segment->SetToggle(true);
                    m_accentGroup = m_accentGroup ? m_accentGroup : MakeRef<gui::ButtonGroup>(m_allocator);
                    segment->SetGroup(m_accentGroup);
                    const String fill(accent.fill);
                    onChange(segment.Get(), [self, fill]() { self->Change([&fill](EditorLook& look) { look.accent = fill; }); });
                    accents->AddChild(segment.Get());
                    m_accents.PushBack(segment);
                }
            if (auto* field = Find<gui::TextField>(u8"pref-accent"))
                field->SetOnTextChanging([self](StringView text)
                                         {
                                             if (!self->m_showing && IsHexColor(text))
                                                 self->Change([&text](EditorLook& look) { look.accent = String(text); });
                                         });
            for (StringView density : kEditorDensities)
            {
                const String name(density);
                onChange(Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()),
                         [self, name]() { self->Change([&name](EditorLook& look) { look.density = name; }); });
            }
            onChange(Find<gui::CheckBox>(u8"pref-motion"), [self]()
                     {
                         const auto* motion = self->Find<gui::CheckBox>(u8"pref-motion");
                         self->Change([motion](EditorLook& look) { look.reduceMotion = motion->IsChecked(); });
                     });
            onChange(Find<gui::Slider>(u8"pref-scale"), [self]()
                     {
                         const auto* scale = self->Find<gui::Slider>(u8"pref-scale");
                         self->Change([scale](EditorLook& look) { look.uiScale = static_cast<f32>(scale->GetValue()); });
                     });
            if (auto* add = Find<gui::Button>(u8"pref-preview-add"))
                add->SetOnClick([self]() { self->AddPreview(u8"Custom", 1920, 1080); });
            AddContributions();
            if (auto* save = Find<gui::Button>(u8"pref-save"))
                save->SetOnClick([self]() { (void)self->Save(); });
            if (auto* cancel = Find<gui::Button>(u8"pref-cancel"))
                cancel->SetOnClick([self]() { self->Cancel(); });
            // However it closes unsaved (Cancel, its close button, Escape), the look is restored.
            m_window->SetOnClose([self]()
                                 {
                                     if (!self->m_saved && self->m_services.showLook)
                                         self->m_services.showLook(self->m_original);
                                 });
        }

        void Change(const Function<void(EditorLook&)>& edit)
        {
            edit(m_look);
            m_look.uiScale = Clamp(m_look.uiScale, editor::kUiScaleMin, editor::kUiScaleMax);
            ShowControls();
            if (m_services.showLook)
                m_services.showLook(m_look);
        }

        // Fills the tabs other than the look and the keys, as the store has them.
        void ShowSettings()
        {
            foundation::settings::Settings* user = m_services.user;
            if (user == nullptr)
                return;
            const editor::EditorUiSettings* ui = user->Find<editor::EditorUiSettings>();
            if (auto* ortho = Find<gui::CheckBox>(u8"pref-auto-ortho"))
                ortho->SetChecked(ui == nullptr || ui->autoOrthographic);
            if (auto* root = Find<gui::TextField>(u8"pref-templates-root"))
            {
                const editor::EditorExportSettings* exportSettings = user->Find<editor::EditorExportSettings>();
                root->SetText(exportSettings != nullptr ? exportSettings->templatesRoot.AsView() : StringView{});
                root->SetPlaceholder(editor::DefaultTemplatesRoot().AsView());
            }
            const editor::EditorMcpSettings* mcp = user->Find<editor::EditorMcpSettings>();
            if (auto* enabled = Find<gui::CheckBox>(u8"pref-mcp-enabled"))
                enabled->SetChecked(mcp != nullptr && mcp->enabled);
            if (auto* port = Find<gui::TextField>(u8"pref-mcp-port"))
                port->SetText(Format(u8"{}", mcp != nullptr ? mcp->port : editor::kEditorMcpDefaultPort).AsView());
            if (auto* token = Find<gui::TextField>(u8"pref-mcp-token"))
                token->SetText(mcp != nullptr ? mcp->token.AsView() : StringView{});
            ShowPreviews();
            for (const Contributed& check : m_contributed)
                check.box->SetChecked(check.field->get ? check.field->get() : false);
        }
        void SaveSettings()
        {
            foundation::settings::Settings& user = *m_services.user;
            if (auto* ortho = Find<gui::CheckBox>(u8"pref-auto-ortho"))
            {
                user.Section<editor::EditorUiSettings>().autoOrthographic = ortho->IsChecked();
                user.MarkChanged<editor::EditorUiSettings>();
            }
            if (auto* root = Find<gui::TextField>(u8"pref-templates-root"))
            {
                user.Section<editor::EditorExportSettings>().templatesRoot = String(root->GetText());
                user.MarkChanged<editor::EditorExportSettings>();
            }
            auto* enabled = Find<gui::CheckBox>(u8"pref-mcp-enabled");
            auto* port = Find<gui::TextField>(u8"pref-mcp-port");
            auto* token = Find<gui::TextField>(u8"pref-mcp-token");
            if (enabled != nullptr && port != nullptr && token != nullptr)
            {
                if (!user.Section<editor::EditorMcpSettings>().ApplyFromPreferences(enabled->IsChecked(), port->GetText(), token->GetText()) &&
                    m_services.warn)
                    m_services.warn(u8"The MCP port must be a number from 1024 to 65535; the old one is kept.");
                user.MarkChanged<editor::EditorMcpSettings>();
                if (m_services.mcpChanged)
                    m_services.mcpChanged();
            }
            if (editor::GamePreviewSettings* previews = editor::GamePreviewSettings::From(&user))
            {
                previews->presets.Clear();
                for (const PreviewRow& row : m_previews)
                    if (!row.removed && !row.name->GetText().IsEmpty())
                        previews->presets.PushBack(editor::GamePreviewResolution{String(row.name->GetText()), static_cast<u32>(row.width->GetValue()),
                                                                                 static_cast<u32>(row.height->GetValue())});
                user.MarkChanged<editor::GamePreviewSettings>();
            }
        }

        // Game preview: a row per resolution (its name, width and height, Remove), then Add.
        void ShowPreviews()
        {
            gui::Node* list = Find<gui::Node>(u8"pref-previews");
            if (list == nullptr)
                return;
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            m_previews.Clear();
            if (const editor::GamePreviewSettings* previews = editor::GamePreviewSettings::From(m_services.user))
                for (const editor::GamePreviewResolution& preview : previews->presets)
                    AddPreview(preview.name.AsView(), preview.width, preview.height);
        }
        void AddPreview(StringView name, u32 width, u32 height)
        {
            gui::Node* list = Find<gui::Node>(u8"pref-previews");
            if (list == nullptr)
                return;
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"pref-preview");
            RefPtr<gui::TextField> nameField = MakeRef<gui::TextField>(m_allocator);
            nameField->SetText(name);
            const auto size = [this](u32 value)
            {
                RefPtr<gui::NumberField> field = MakeRef<gui::NumberField>(m_allocator);
                field->SetInteger(true);
                field->SetMin(1.0);
                field->SetMax(16384.0);
                field->SetStep(1.0);
                field->SetValue(static_cast<f64>(value));
                return field;
            };
            RefPtr<gui::NumberField> widthField = size(width);
            RefPtr<gui::NumberField> heightField = size(height);
            RefPtr<gui::Label> by = MakeRef<gui::Label>(m_allocator);
            by->SetText(u8"x");
            RefPtr<gui::Button> remove = MakeRef<gui::Button>(m_allocator);
            remove->AddClass(u8"ghost");
            remove->SetText(u8"Remove");
            const usize index = m_previews.Size();
            EditorPreferencesDialog* self = this;
            // Hidden, and skipped on Save, rather than removed from under its own click.
            remove->SetOnClick([self, index]()
                               {
                                   self->m_previews[index].removed = true;
                                   self->m_previews[index].row->SetDisplayed(false);
                               });
            row->AddChild(nameField.Get());
            row->AddChild(widthField.Get());
            row->AddChild(by.Get());
            row->AddChild(heightField.Get());
            row->AddChild(remove.Get());
            list->AddChild(row.Get());
            m_previews.PushBack(PreviewRow{row, nameField.Get(), widthField.Get(), heightField.Get(), false});
        }

        // Adds a tab per category other parts of the editor contribute (contributions naming the
        // same category share it), its checks applied as they change.
        void AddContributions()
        {
            auto* tabs = Find<gui::TabView>(u8"pref-tabs");
            if (tabs == nullptr || m_services.contributions == nullptr)
                return;
            Array<String> categories;
            Array<gui::Node*> columns;
            for (const editor::EditorContext::EditorSettingsContribution& contribution : *m_services.contributions)
            {
                gui::Node* column = nullptr;
                for (usize i = 0; i < categories.Size(); ++i)
                    if (categories[i] == contribution.category)
                        column = columns[i];
                if (column == nullptr)
                {
                    gui::TabPage* page = tabs->AddPage(contribution.category.AsView());
                    RefPtr<gui::Panel> body = MakeRef<gui::Panel>(m_allocator);
                    body->AddClass(u8"dialog-body");
                    body->AddClass(u8"pref-contributed");
                    page->AddChild(body.Get());
                    column = body.Get();
                    categories.PushBack(contribution.category);
                    columns.PushBack(column);
                }
                for (const editor::EditorContext::EditorSettingsBoolField& field : contribution.bools)
                {
                    RefPtr<gui::CheckBox> box = MakeRef<gui::CheckBox>(m_allocator);
                    box->SetText(field.label.AsView());
                    if (!field.description.IsEmpty())
                        box->SetTooltip(field.description.AsView());
                    // The field belongs to the context and lives from startup: registrations come
                    // before any dialog opens.
                    const editor::EditorContext::EditorSettingsBoolField* fieldPtr = &field;
                    gui::CheckBox* raw = box.Get();
                    (void)box->AddEventListener(gui::EventType::Changed, [fieldPtr, raw](const gui::Event&)
                                                {
                                                    if (fieldPtr->set)
                                                        fieldPtr->set(raw->IsChecked());
                                                });
                    column->AddChild(box.Get());
                    m_contributed.PushBack(Contributed{fieldPtr, raw});
                }
            }
        }

        // The Shortcuts tab: a row per action, its chord as the registry has it now.
        void ShowShortcuts()
        {
            gui::Node* list = Find<gui::Node>(u8"pref-shortcuts");
            if (list == nullptr || m_services.actions == nullptr)
                return;
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            editor::EditorActionRegistry& actions = *m_services.actions;
            EditorPreferencesDialog* self = this;
            for (const editor::EditorActionDeclaration& action : actions.Actions())
            {
                RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
                row->AddClass(u8"pref-shortcut");
                RefPtr<gui::Label> name = MakeRef<gui::Label>(m_allocator);
                name->AddClass(u8"row-name");
                name->SetText(action.label.AsView());
                RefPtr<gui::Label> where = MakeRef<gui::Label>(m_allocator);
                where->AddClass(u8"pref-where");
                where->SetText(action.menuPath.AsView());
                RefPtr<ShortcutCaptureButton> capture = MakeRef<ShortcutCaptureButton>(m_allocator);
                capture->SetChord(actions.Shortcut(action.id.AsView()));
                RefPtr<gui::Button> reset = MakeRef<gui::Button>(m_allocator);
                reset->AddClass(u8"ghost");
                reset->SetText(u8"Reset");
                reset->SetEnabled(actions.HasOverride(action.id.AsView()));
                const String id = action.id;
                const EditorShortcut declared = action.shortcut;
                gui::Button* resetRaw = reset.Get();
                ShortcutCaptureButton* captureRaw = capture.Get();
                capture->SetOnChordChosen([self, id, resetRaw](EditorShortcut chord)
                                          {
                                              self->m_shortcutEdits.Set(id.AsView(), chord);
                                              resetRaw->SetEnabled(true);
                                          });
                reset->SetOnClick([self, id, captureRaw, resetRaw, declared]()
                                  {
                                      self->m_shortcutEdits.Reset(id.AsView());
                                      captureRaw->SetChord(declared); // show the default at once
                                      resetRaw->SetEnabled(false);
                                  });
                row->AddChild(name.Get());
                row->AddChild(where.Get());
                row->AddChild(capture.Get());
                row->AddChild(reset.Get());
                list->AddChild(row.Get());
            }
        }

        // Sets the controls from m_look quietly, so none of their change callbacks fire.
        void ShowControls()
        {
            m_showing = true;
            if (auto* dark = Find<gui::Button>(u8"pref-dark"))
                dark->SetChecked(m_look.dark);
            if (auto* light = Find<gui::Button>(u8"pref-light"))
                light->SetChecked(!m_look.dark);
            for (usize i = 0; i < m_accents.Size(); ++i)
                m_accents[i]->SetChecked(m_look.accent.AsView() == kEditorAccents[i].fill);
            if (auto* field = Find<gui::TextField>(u8"pref-accent"); field != nullptr && field->GetText() != m_look.accent.AsView())
                field->SetText(m_look.accent.AsView());
            for (StringView density : kEditorDensities)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()))
                    segment->SetChecked(m_look.density.AsView() == density);
            if (auto* motion = Find<gui::CheckBox>(u8"pref-motion"))
                motion->SetChecked(m_look.reduceMotion);
            if (auto* scale = Find<gui::Slider>(u8"pref-scale"))
                scale->SetValue(m_look.uiScale);
            if (auto* value = Find<gui::Label>(u8"pref-scale-value"))
                value->SetText(Format(u8"{}%", static_cast<i32>(m_look.uiScale * 100.0f + 0.5f)).AsView());
            m_showing = false;
        }

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        RefPtr<gui::ButtonGroup> m_accentGroup;
        Array<RefPtr<gui::Button>> m_accents;
        EditorLook m_original; // the look as it was when the dialog opened
        EditorLook m_look;     // the look as the controls have it
        struct PreviewRow
        {
            RefPtr<gui::Panel> row;
            gui::TextField* name = nullptr;
            gui::NumberField* width = nullptr;
            gui::NumberField* height = nullptr;
            bool removed = false;
        };
        struct Contributed
        {
            const editor::EditorContext::EditorSettingsBoolField* field = nullptr;
            gui::CheckBox* box = nullptr;
        };

        editor::ShortcutEdits m_shortcutEdits; // staged; applied and persisted on Save
        Array<PreviewRow> m_previews;
        Array<Contributed> m_contributed;
        bool m_saved = false;
        bool m_showing = false; // the controls being set from code
    };
}

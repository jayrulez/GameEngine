// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :preferences_dialog partition.
//
// EditorPreferencesDialog: a modal editor for PER-USER editor preferences (the foundation.settings
// store persisted at <user-data>/editor.settings.xml) - distinct from ProjectSettingsDialog, which
// edits the project manifest. A tab per category: Appearance (the editor FONT paths, blank = the
// built-in chain, and the UI scale), Export (the templates root, blank = "$ENV_TEMPLATES_DIR, else
// <user-data>/templates"), Agent access (the MCP host), Game preview (the Game tab's resolutions),
// Shortcuts, then one per domain-contributed category. [Save] writes every tab's sections back into
// the store and persists it; font changes apply on the next editor start. [Cancel] discards.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:preferences_dialog;

import foundation.core;
import foundation.ui;
import editor.core;
import foundation.settings;
import :shortcut_capture;
import :category_tabs;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;
    namespace settings = foundation::settings;

    class EditorPreferencesDialog final : public ui::Dialog
    {
        RTTI_OBJECT(EditorPreferencesDialog, ui::Dialog)
    public:
        EditorPreferencesDialog(editor::EditorContext& context, settings::Settings& store)
            : ui::Dialog(u8"Preferences"), m_context(&context), m_settings(&store)
        {
            // A fixed size: a tab view measures only the page it shows, so a free height would
            // jump as the tabs change. Each page scrolls inside it.
            MinWidth.SetValue(620.0f);
            MaxWidth.SetValue(760.0f);
            MinHeight.SetValue(480.0f);
            MaxHeight.SetValue(480.0f);

            m_tabs = MakeUnique<CategoryTabs>(MemoryAllocator(), MemoryAllocator());

            // --- Appearance: the editor's fonts and scale ---
            {
                ui::FlexLayout& column = m_tabs->Column(u8"Appearance");
                StringView fontPath;
                StringView monoPath;
                if (const editor::EditorFontSettings* f =
                        store.Find<editor::EditorFontSettings>())
                {
                    fontPath = f->fontPath.AsView();
                    monoPath = f->monoFontPath.AsView();
                }
                m_fontEdit = AddTextRow(column, u8"UI font (.ttf)", fontPath);
                m_fontEdit->SetPlaceholder(u8"built-in (embedded fallback)");
                m_monoFontEdit = AddTextRow(column, u8"Mono font (.ttf)", monoPath);
                m_monoFontEdit->SetPlaceholder(u8"built-in");
                f32 uiScale = 1.0f;
                if (const editor::EditorUiSettings* u =
                        store.Find<editor::EditorUiSettings>())
                {
                    uiScale = Clamp(u->uiScale, editor::kUiScaleMin, editor::kUiScaleMax);
                }
                ui::FlexLayout* row = AddRow(column, u8"UI scale");
                auto slider = MakeRef<ui::Slider>(MemoryAllocator());
                slider->Min.SetValue(editor::kUiScaleMin);
                slider->Max.SetValue(editor::kUiScaleMax);
                slider->Step.SetValue(0.05f);
                slider->Value.SetValue(uiScale);
                m_uiScaleSlider = slider.Get();
                {
                    ui::LayoutStyle lp;
                    lp.FlexGrow = 1.0f;
                    lp.AlignSelf = ui::Align::Center;
                    row->AddView(slider.Get(), lp);
                }
                auto valueLabel = MakeRef<ui::Label>(MemoryAllocator(), StringView(u8"1.00x"));
                valueLabel->FontSize.SetValue(11.0f);
                m_uiScaleLabel = valueLabel.Get();
                {
                    ui::LayoutStyle lp;
                    lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(44));
                    lp.AlignSelf = ui::Align::Center;
                    row->AddView(valueLabel.Get(), lp);
                }
                UpdateScaleLabel(uiScale);
                EditorPreferencesDialog* self = this;
                slider->OnValueChanged.Add(
                    ui::Event<void(ui::Slider*, f32)>::Handler{
                        [self](ui::Slider*, f32 v) { self->UpdateScaleLabel(v); }});
                CategoryTabs::AddNote(column, u8"Font changes apply on restart.");
            }

            // --- Export: where the export templates live ---
            {
                ui::FlexLayout& column = m_tabs->Column(u8"Export");
                StringView current;
                if (const editor::EditorExportSettings* s =
                        store.Find<editor::EditorExportSettings>())
                {
                    current = s->templatesRoot.AsView();
                }
                m_rootEdit = AddTextRow(column, u8"Templates root", current);
                m_rootEdit->SetPlaceholder(editor::DefaultTemplatesRoot().AsView());
                CategoryTabs::AddNote(column, u8"Blank uses $ENV_TEMPLATES_DIR, else the templates folder "
                                u8"under the editor's user data.");
            }

            // --- Agent access: the MCP host over the open project (EditorMcpSettings) ---
            {
                ui::FlexLayout& column = m_tabs->Column(u8"Agent access");
                const editor::EditorMcpSettings* mcp = store.Find<editor::EditorMcpSettings>();
                auto check = MakeRef<ui::CheckBox>(
                    MemoryAllocator(), StringView(u8"Serve the open project to agents (MCP)"),
                    mcp != nullptr && mcp->enabled);
                check->FontSize.SetValue(12.0f);
                check->TooltipText =
                    String(u8"An MCP host on 127.0.0.1 for the project this editor has open; "
                           u8"the token below is the secret an agent presents");
                m_mcpEnabled = check.Get();
                {
                    ui::LayoutStyle lp;
                    lp.Width = ui::SizeSpec::Match();
                    lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(22.0f));
                    column.AddView(check.Get(), lp);
                }
                const u32 port = mcp != nullptr ? mcp->port : editor::kEditorMcpDefaultPort;
                m_mcpPortEdit = AddTextRow(column, u8"MCP port", Format(u8"{}", port).AsView());
                m_mcpTokenEdit = AddTextRow(column, u8"MCP token",
                                            mcp != nullptr ? mcp->token.AsView() : StringView());
                m_mcpTokenEdit->SetPlaceholder(u8"minted on first enable");
                CategoryTabs::AddNote(column, u8"Applies to the open project on Save; the token is also "
                                u8"written to <user-data>/mcp-token for a local agent.");
            }

            // --- Game preview: the Game tab's resolutions (GamePreviewSettings), sizes to test
            // at on this machine after the project's own and its export presets'. Staged,
            // applied on Save. ---
            {
                ui::FlexLayout& column = m_tabs->Column(u8"Game preview");
                CategoryTabs::AddNote(column, u8"Sizes the Game tab offers on this machine, after the "
                                u8"project's own and its export presets'.");
                auto previews = MakeRef<ui::FlexLayout>(MemoryAllocator());
                previews->Direction = ui::Orientation::Vertical;
                previews->Spacing = 4;
                ui::LayoutStyle match;
                match.Width = ui::SizeSpec::Match();
                column.AddView(previews.Get(), match);
                m_previewColumn = previews.Get();
                if (const editor::GamePreviewSettings* seeded = editor::GamePreviewSettings::From(&store))
                {
                    for (const editor::GamePreviewResolution& preview : seeded->presets)
                    {
                        AddPreviewRow(preview.name.AsView(), preview.width, preview.height);
                    }
                }
                auto add = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"Add resolution"));
                EditorPreferencesDialog* self = this;
                add->OnClick.Add([self](ui::ButtonBase*) { self->AddPreviewRow(u8"Custom", 1920, 1080); });
                ui::LayoutStyle left;
                left.AlignSelf = ui::Align::Start;
                column.AddView(add.Get(), left);
            }

            // --- Shortcuts: every action with its effective chord; a click on the chord captures
            // the next key, Reset forgets the override. Staged in m_shortcutEdits, applied on
            // Save. ---
            {
                ui::FlexLayout& column = m_tabs->Column(u8"Shortcuts");
                CategoryTabs::AddNote(column, u8"Click a chord and press the new keys (Esc cancels, Del clears). "
                                u8"A chord another action holds is refused on Save, naming it.");
                EditorActionRegistry& actions = context.Actions();
                for (const editor::EditorActionDeclaration& action : actions.Actions())
                {
                    AddShortcutRow(column, actions, action);
                }
            }

            // Domain-contributed categories (EditorContext::RegisterEditorSettingsContribution),
            // a tab each (contributions naming one category share it): the app hardcodes
            // nothing - each domain's fields render generically here and write through their
            // own closures (usually into the domain's user-store section).
            for (const editor::EditorContext::EditorSettingsContribution& contribution :
                 context.EditorSettingsContributions())
            {
                ui::FlexLayout& column = m_tabs->Column(contribution.category.AsView());
                for (const editor::EditorContext::EditorSettingsBoolField& field :
                     contribution.bools)
                {
                    const bool checked = field.get ? field.get() : false;
                    auto check =
                        MakeRef<ui::CheckBox>(MemoryAllocator(), field.label.AsView(), checked);
                    check->FontSize.SetValue(12.0f);
                    if (!field.description.IsEmpty())
                    {
                        check->TooltipText = String(field.description);
                    }
                    // Function is move-only: capture the FIELD (context-owned, stable -
                    // registrations happen at boot, before any dialog opens).
                    const editor::EditorContext::EditorSettingsBoolField* fieldPtr = &field;
                    check->OnCheckedChanged.Add(
                        [fieldPtr](ui::CheckBox*, bool checked)
                        {
                            if (fieldPtr->set)
                            {
                                fieldPtr->set(checked);
                            }
                        });
                    ui::LayoutStyle lp;
                    lp.Width = ui::SizeSpec::Match();
                    lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(22.0f));
                    column.AddView(check.Get(), lp);
                }
            }

            SetContent(&m_tabs->View());

            {
                EditorPreferencesDialog* self = this;
                ui::Button* save = AddButton(u8"Save", ui::DialogResult::None);
                save->OnClick.Add([self](ui::ButtonBase*) { self->Apply(); });
            }
            AddButton(u8"Cancel", ui::DialogResult::Cancel);
        }

    public:
        /// Fired on Apply with the new UI scale so the app can apply it LIVE (set the
        /// host's scale + re-bake icons); the saved setting covers the next launch.
        Function<void(f32)> OnUiScaleApplied;
        /// Fired on Apply after the MCP section changed, so the app restarts (or stops) the
        /// host for the open project without a reopen.
        Function<void()> OnMcpSettingsApplied;

        /// The category tabs: Appearance, Export, Agent access, Game preview, Shortcuts, then one
        /// per domain-contributed category.
        [[nodiscard]] ui::TabView& Tabs() const noexcept { return m_tabs->View(); }
        /// The tab for a category, or -1.
        [[nodiscard]] i32 TabIndexOf(StringView category) const { return m_tabs->IndexOf(category); }

        /// The preview resolutions staged in the dialog (rows not removed), in order.
        [[nodiscard]] usize PreviewRowCount() const noexcept
        {
            usize count = 0;
            for (const PreviewRow& row : m_previewRows)
            {
                count += row.removed ? 0u : 1u;
            }
            return count;
        }

    private:
        /// One staged preview resolution; its views are the rows container's.
        struct PreviewRow
        {
            ui::FlexLayout* row = nullptr;
            ui::EditText* name = nullptr;
            ui::NumericField* width = nullptr;
            ui::NumericField* height = nullptr;
            bool removed = false;
        };

        void AddPreviewRow(StringView name, u32 width, u32 height)
        {
            auto row = MakeRef<ui::FlexLayout>(MemoryAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 6;
            auto nameEdit = MakeRef<ui::EditText>(MemoryAllocator());
            nameEdit->SetText(name);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.AlignSelf = ui::Align::Center;
            row->AddView(nameEdit.Get(), grow);
            const auto sizeField = [this, &row](u32 value)
            {
                auto field = MakeRef<ui::NumericField>(MemoryAllocator());
                field->SetDecimalPlaces(0);
                field->SetMin(1.0);
                field->SetMax(16384.0);
                field->SetStep(1.0);
                field->SetValue(static_cast<f64>(value));
                ui::LayoutStyle style;
                style.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(80));
                style.AlignSelf = ui::Align::Center;
                row->AddView(field.Get(), style);
                return field.Get();
            };
            ui::NumericField* widthField = sizeField(width);
            ui::LayoutStyle centred;
            centred.AlignSelf = ui::Align::Center;
            row->AddView(MakeRef<ui::Label>(MemoryAllocator(), StringView(u8"x")).Get(), centred);
            ui::NumericField* heightField = sizeField(height);
            auto remove = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"Remove"));
            const usize index = m_previewRows.Size();
            EditorPreferencesDialog* self = this;
            // Hidden and skipped on Save rather than torn out from under its own click.
            remove->OnClick.Add(
                [self, index](ui::ButtonBase*)
                {
                    self->m_previewRows[index].removed = true;
                    self->m_previewRows[index].row->Visibility = ui::Visibility::Gone;
                    self->m_previewRows[index].row->Invalidate();
                });
            row->AddView(remove.Get(), centred);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            m_previewColumn->AddView(row.Get(), match);
            m_previewRows.PushBack(PreviewRow{row.Get(), nameEdit.Get(), widthField, heightField, false});
        }

        void UpdateScaleLabel(f32 value)
        {
            if (m_uiScaleLabel != nullptr)
            {
                const i32 percent = static_cast<i32>(value * 100.0f + 0.5f);
                m_uiScaleLabel->SetText(Format(u8"{}%", percent).AsView());
            }
        }

        // A labeled horizontal row (fixed-width label, callers append the field views).
        ui::FlexLayout* AddRow(ui::FlexLayout& column, StringView label)
        {
            auto row = MakeRef<ui::FlexLayout>(MemoryAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8;
            {
                auto text = MakeRef<ui::Label>(MemoryAllocator(), label);
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(110));
                lp.AlignSelf = ui::Align::Center;
                row->AddView(text.Get(), lp);
            }
            ui::FlexLayout* raw = row.Get();
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column.AddView(row.Get(), lp);
            }
            return raw;
        }

        // One action: its label, its menu path, the chord (a capture button) and Reset.
        void AddShortcutRow(ui::FlexLayout& column, EditorActionRegistry& actions,
                            const editor::EditorActionDeclaration& action)
        {
            auto row = MakeRef<ui::FlexLayout>(MemoryAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8;
            {
                auto text = MakeRef<ui::Label>(MemoryAllocator(), action.label.AsView());
                text->FontSize.SetValue(12.0f);
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(170));
                lp.AlignSelf = ui::Align::Center;
                row->AddView(text.Get(), lp);
            }
            {
                auto where = MakeRef<ui::Label>(MemoryAllocator(), action.menuPath.AsView());
                where->FontSize.SetValue(11.0f);
                where->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.AlignSelf = ui::Align::Center;
                row->AddView(where.Get(), lp);
            }
            const StringView id = action.id.AsView();
            auto capture = MakeRef<ShortcutCaptureButton>(MemoryAllocator(), actions.Shortcut(id));
            capture->FontSize.SetValue(Optional<f32>(11.0f));
            ShortcutCaptureButton* captureRaw = capture.Get();
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(150));
                lp.AlignSelf = ui::Align::Center;
                row->AddView(capture.Get(), lp);
            }
            auto reset = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"Reset"));
            reset->FontSize.SetValue(Optional<f32>(11.0f));
            reset->IsEnabled = actions.HasOverride(id);
            ui::Button* resetRaw = reset.Get();
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(60));
                lp.AlignSelf = ui::Align::Center;
                row->AddView(reset.Get(), lp);
            }
            EditorPreferencesDialog* self = this;
            const String idText(id);
            capture->OnChordChosen = [self, idText, resetRaw](EditorShortcut chord)
            {
                self->m_shortcutEdits.Set(idText.AsView(), chord);
                resetRaw->IsEnabled = true;
            };
            const editor::EditorActionDeclaration* declaration = &action;
            reset->OnClick.Add(
                [self, idText, captureRaw, resetRaw, declaration](ui::ButtonBase*)
                {
                    self->m_shortcutEdits.Reset(idText.AsView());
                    captureRaw->SetChord(declaration->shortcut); // the default, shown at once
                    resetRaw->IsEnabled = false;
                });
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column.AddView(row.Get(), lp);
            }
        }

        ui::EditText* AddTextRow(ui::FlexLayout& column, StringView label, StringView value)
        {
            ui::FlexLayout* row = AddRow(column, label);
            auto edit = MakeRef<ui::EditText>(MemoryAllocator());
            edit->SetText(value);
            ui::EditText* raw = edit.Get();
            ui::LayoutStyle lp;
            lp.FlexGrow = 1.0f;
            row->AddView(edit.Get(), lp);
            return raw;
        }

        void Apply()
        {
            m_settings->Section<editor::EditorExportSettings>().templatesRoot =
                String(m_rootEdit->Text());
            m_settings->MarkChanged<editor::EditorExportSettings>();
            editor::EditorFontSettings& fontPrefs =
                m_settings->Section<editor::EditorFontSettings>();
            fontPrefs.fontPath = String(m_fontEdit->Text());
            fontPrefs.monoFontPath = String(m_monoFontEdit->Text());
            m_settings->MarkChanged<editor::EditorFontSettings>();
            const f32 uiScale = Clamp(m_uiScaleSlider->Value.Value(), editor::kUiScaleMin, editor::kUiScaleMax);
            m_settings->Section<editor::EditorUiSettings>().uiScale = uiScale;
            m_settings->MarkChanged<editor::EditorUiSettings>();
            if (OnUiScaleApplied)
            {
                OnUiScaleApplied(uiScale); // live: host scale + icon re-bake
            }
            if (!m_settings->Section<editor::EditorMcpSettings>().ApplyFromPreferences(
                    m_mcpEnabled->IsChecked.Value(), m_mcpPortEdit->Text(),
                    m_mcpTokenEdit->Text()))
            {
                m_context->Notify(editor::NoticeKind::Warning,
                                  u8"MCP port must be a number in 1024..65535 - kept the old one.");
            }
            m_settings->MarkChanged<editor::EditorMcpSettings>();
            if (OnMcpSettingsApplied)
            {
                OnMcpSettingsApplied(); // live: the host follows the new enabled/port/token
            }
            if (editor::GamePreviewSettings* previews = editor::GamePreviewSettings::From(m_settings))
            {
                previews->presets.Clear();
                for (const PreviewRow& row : m_previewRows)
                {
                    if (row.removed || row.name->Text().IsEmpty())
                    {
                        continue;
                    }
                    previews->presets.PushBack(editor::GamePreviewResolution{
                        String(row.name->Text()), static_cast<u32>(row.width->Value()),
                        static_cast<u32>(row.height->Value())});
                }
                m_settings->MarkChanged<editor::GamePreviewSettings>();
            }
            if (!m_shortcutEdits.IsEmpty())
            {
                Array<String> collisions;
                (void)m_shortcutEdits.Apply(m_context->Actions(),
                                            m_settings->Section<editor::EditorShortcutSettings>(),
                                            &collisions);
                m_settings->MarkChanged<editor::EditorShortcutSettings>();
                for (const String& collision : collisions)
                {
                    m_context->Notify(editor::NoticeKind::Warning,
                                      Format(u8"Shortcut kept: {}", collision.AsView()).AsView());
                }
            }
            if (editor::SaveEditorSettingsToUserData(*m_settings).IsOk())
            {
                m_context->SetStatus(u8"Preferences saved.");
            }
            else
            {
                m_context->Notify(editor::NoticeKind::Error,
                                  u8"Preferences save FAILED (see console).");
            }
            Close(ui::DialogResult::OK);
        }

        editor::EditorContext* m_context;
        settings::Settings* m_settings;
        UniquePtr<CategoryTabs> m_tabs;
        ui::EditText* m_rootEdit = nullptr;
        ui::EditText* m_fontEdit = nullptr;
        ui::EditText* m_monoFontEdit = nullptr;
        ui::Slider* m_uiScaleSlider = nullptr;
        ui::Label* m_uiScaleLabel = nullptr;
        ui::CheckBox* m_mcpEnabled = nullptr;
        ui::EditText* m_mcpPortEdit = nullptr;
        ui::EditText* m_mcpTokenEdit = nullptr;
        editor::ShortcutEdits m_shortcutEdits; // staged; applied and persisted on Save
        ui::FlexLayout* m_previewColumn = nullptr; // the preview rows sit here
        Array<PreviewRow> m_previewRows;
    };

    RTTI_DEFINE_OBJECT(EditorPreferencesDialog, "rtti::editor::editor::app")
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :export_dialog partition.
//
// ExportDialog: Project > Export. The project's export presets on the left (add, duplicate,
// remove), each by the icon of the template it resolves to here, with a warning when none does;
// the selected one's settings on the right, a tab per category (General: its name, template,
// platform, config, player and output folder; Content: extra files, symbols, pruning; Display: its
// own render size and window), under a card naming the template it exports with, or saying none
// is installed. Edits apply as they are made and are saved when the selection moves, an export
// starts or the dialog closes. Export runs the selected preset, Export All every one; Export
// Templates opens the templates dialog over this one, and the cards follow what it changed. What
// it needs from outside (the registry, saving, running an export, the file pickers) comes through
// ExportDialogSeams, so a test drives it without a window.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:export_dialog;

import foundation.core;
import foundation.ui;
import editor.core;
import :category_tabs;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    /// What the export dialog asks of the application.
    struct ExportDialogSeams
    {
        /// Fill `registry` with the templates this host sees.
        Function<void(editor::TemplateRegistry&)> refresh;
        /// Persist the presets (the controller the dialog edits) to the project.
        Function<void()> save;
        /// Run the export of one preset by name, or of every preset (`all`).
        Function<void(StringView preset, bool all)> runExport;
        /// Open the templates dialog over this one; null when it cannot.
        Function<ui::Dialog*()> openTemplates;
        /// Ask for files; `chosen` hears their paths, or nothing when the user cancels.
        Function<void(Function<void(Array<String>)> chosen)> pickFiles;
        /// The host's platform, a new preset's.
        String hostPlatform;
        /// Where exports are written, said under the presets.
        String outputRoot;
    };

    class ExportDialog final : public ui::Dialog
    {
        RTTI_OBJECT(ExportDialog, ui::Dialog)
    public:
        ExportDialog(editor::EditorContext& context, editor::ExportPresetsController& presets,
                     ExportDialogSeams seams);
        ~ExportDialog() override;

        /// The presets' count and the selected one's index (-1 when there are none).
        [[nodiscard]] usize PresetCount() const noexcept { return m_presets->Count(); }
        [[nodiscard]] i32 SelectedIndex() const;
        void Select(usize index);

        /// The template the selected preset resolves to here, or null.
        [[nodiscard]] const editor::ExportTemplate* Resolved() const;
        /// The resolve card's text: the template's name, or why there is none.
        [[nodiscard]] StringView ResolveText() const;

        void AddPreset();
        void DuplicateSelected();
        void RemoveSelected();
        void ExportSelected();
        void ExportAll();
        /// Re-read the templates (after the templates dialog changed them).
        void RefreshTemplates();

        /// The tabs, and the General tab's fields, for tests.
        [[nodiscard]] ui::TabView& Tabs() const noexcept { return m_tabs->View(); }
        [[nodiscard]] i32 TabIndexOf(StringView category) const { return m_tabs->IndexOf(category); }
        [[nodiscard]] ui::EditText& NameField() const noexcept { return *m_name; }
        [[nodiscard]] ui::ComboBox& TemplateField() const noexcept { return *m_template; }
        [[nodiscard]] ui::CheckBox& PruneField() const noexcept { return *m_prune; }

    private:
        class Adapter;

        void BuildGeneral(ui::FlexLayout& column);
        void BuildContent(ui::FlexLayout& column);
        void BuildDisplay(ui::FlexLayout& column);
        void BuildResolveCard(ui::FlexLayout& column);
        ui::FlexLayout* AddRow(ui::FlexLayout& column, StringView label, ui::View* field);

        /// Show the selected preset in the fields (without writing it back).
        void LoadFields();
        /// Write the fields into the selected preset, then refresh what shows it.
        void StoreFields();
        void FillTemplateChoices();
        void FillChoices(ui::ComboBox& combo, StringView current, StringView fallback, bool platforms);
        void UpdateResolve();
        void Save();
        [[nodiscard]] ui::DrawablePtr IconFor(const editor::ExportTemplate& tmpl);

        editor::EditorContext* m_context;
        editor::ExportPresetsController* m_presets;
        ExportDialogSeams m_seams;
        editor::TemplateRegistry m_registry;
        UniquePtr<Adapter> m_adapter;
        RefPtr<ui::ListView> m_list;
        UniquePtr<CategoryTabs> m_tabs;
        HashMap<String, ui::DrawablePtr> m_icons; // by template id and directory
        bool m_loading = false;                   // the fields are being filled, not edited
        bool m_dirty = false;                     // edited since the last save

        // The resolve card.
        ui::DrawableView* m_resolveIcon = nullptr;
        ui::Label* m_resolveTitle = nullptr;
        ui::Label* m_resolveDetail = nullptr;
        // General.
        ui::EditText* m_name = nullptr;
        ui::ComboBox* m_template = nullptr;
        Array<String> m_templateIds; // the combo's items after "any": template ids
        ui::ComboBox* m_platform = nullptr;
        ui::ComboBox* m_config = nullptr;
        ui::EditText* m_player = nullptr;
        ui::EditText* m_subdir = nullptr;
        // Content.
        ui::EditText* m_files = nullptr;
        ui::CheckBox* m_symbols = nullptr;
        ui::CheckBox* m_prune = nullptr;
        // Display.
        ui::CheckBox* m_overridesRender = nullptr;
        ui::NumericField* m_renderWidth = nullptr;
        ui::NumericField* m_renderHeight = nullptr;
        ui::ComboBox* m_renderFit = nullptr;
        ui::CheckBox* m_overridesWindow = nullptr;
        ui::NumericField* m_windowWidth = nullptr;
        ui::NumericField* m_windowHeight = nullptr;
        ui::ComboBox* m_windowMode = nullptr;
        ui::CheckBox* m_windowResizable = nullptr;
        // The left side's buttons.
        ui::View* m_duplicate = nullptr;
        ui::View* m_remove = nullptr;
        ui::Button* m_export = nullptr;
    };

    RTTI_DEFINE_OBJECT(ExportDialog, "rtti::editor::editor::app")
}

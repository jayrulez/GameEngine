// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :settings_dialog partition.
//
// ProjectSettingsDialog: a modal editor for the project manifest (Project.xml) - the settings
// ProjectSettings' reflection describes (its name, native module and asset settings, each asset
// slot filtered to the type the setting names) and the MSAA level, a tab per category the settings
// name (their `category`; Preferences reads the same way). The engine version stamp is shown
// read-only under General (every save re-stamps it to the running engine; the
// launcher/project-manager owns migration).
//
// [Save] writes the fields back into EditorProject::Settings() and persists the manifest;
// [Cancel]/Escape discards. Every asset setting is a ResourceRefEditor row: pick, drop and clear
// set the instance GUID (rename/move-proof); Save keeps the path alongside as a mirror. An asset
// list setting (the other UI fonts) is a ContainerListEditor row of slots.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:settings_dialog;

import foundation.core;
import foundation.content;
import foundation.ui;
import engine.render; // the canonical MSAA level table (kMsaaLevels + index<->samples helpers)
import engine.project; // ProjectSettings' reflected settings
import editor.core;
import :resource_ref_editor;
import :container_list_editor;
import :asset_picker_dialog;
import :category_tabs;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;
    namespace content = foundation::content;

    class ProjectSettingsDialog final : public ui::Dialog
    {
        RTTI_OBJECT(ProjectSettingsDialog, ui::Dialog)
    public:
        explicit ProjectSettingsDialog(editor::EditorContext& context)
            : ui::Dialog(u8"Project Settings"), m_context(&context)
        {
            // A fixed size, as Preferences: a tab view measures only the page it shows, so a
            // free width or height would change as the tabs do. Each page scrolls inside it.
            MinWidth.SetValue(620.0f);
            MaxWidth.SetValue(620.0f);
            MinHeight.SetValue(420.0f);
            MaxHeight.SetValue(420.0f);

            editor::EditorProject* project = context.Project();
            m_tabs = MakeUnique<CategoryTabs>(MemoryAllocator(), MemoryAllocator());

            // The settings as the type describes them (ProjectSettings' reflection): a text row
            // per string setting, per asset setting a slot that picks, takes a dropped asset of
            // its type and clears (editor-lists-and-asset-slots.md P1), a list of slots per asset
            // list, and a field, check box or combo per number, flag or choice, seeded from the
            // manifest, each in its category's tab. MSAA, a choice from the render subsystem's
            // levels, follows in its own.
            BuildSettingRows(project);

            // Scene-pass MSAA: Off / 2x / 4x maps to renderMsaaSamples 1 / 2 / 4. The
            // player and play-in-editor apply it; the render subsystem capability-clamps at runtime
            // (2x degrades to 1x on WebGPU).
            {
                const PropertyInfo* msaa =
                    FindProperty(engine::project::ProjectSettings::StaticType(), "renderMsaaSamples");
                ui::FlexLayout* row = AddRow(
                    m_tabs->Column(msaa != nullptr ? engine::project::SettingCategory(*msaa)
                                                   : engine::project::kSettingDefaultCategory),
                    u8"MSAA");
                m_msaaCombo = MakeRef<ui::ComboBox>(MemoryAllocator());
                for (u32 i = 0; i < engine::render::MsaaLevelCount(); ++i)
                {
                    (void)m_msaaCombo->AddItem(engine::render::kMsaaLevels[i].label);
                }
                const u32 samples = (project != nullptr) ? project->Settings().renderMsaaSamples : 1u;
                m_msaaCombo->SetSelectedIndex(engine::render::MsaaIndexForSamples(samples));
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                row->AddView(m_msaaCombo.Get(), lp);
            }

            // Engine stamp - informational; re-stamped by every save.
            {
                ui::FlexLayout* row =
                    AddRow(m_tabs->Column(engine::project::kSettingDefaultCategory), u8"Engine version");
                auto value =
                    MakeRef<ui::Label>(MemoryAllocator(), editor::kEngineVersionString);
                ui::LayoutStyle lp;
                lp.AlignSelf = ui::Align::Center;
                row->AddView(value.Get(), lp);
            }

            SetContent(&m_tabs->View());

            {
                ProjectSettingsDialog* self = this;
                ui::Button* save = AddButton(u8"Save", ui::DialogResult::None);
                save->OnClick.Add([self](ui::ButtonBase*) { self->Apply(); });
            }
            AddButton(u8"Cancel", ui::DialogResult::Cancel);
        }

        /// The category tabs, in the order the settings first name them.
        [[nodiscard]] ui::TabView& Tabs() const noexcept { return m_tabs->View(); }
        /// The tab for a category, or -1.
        [[nodiscard]] i32 TabIndexOf(StringView category) const { return m_tabs->IndexOf(category); }

    private:
        // A labeled horizontal row (fixed-width label, callers append the field views).
        ui::FlexLayout* AddRow(ui::FlexLayout& column, StringView label);

        ui::EditText* AddTextRow(ui::FlexLayout& column, StringView label, StringView value);

        /// One row per reflected setting a row edits, in its category's tab: strings as text,
        /// asset settings as slots.
        void BuildSettingRows(editor::EditorProject* project);

        /// An asset setting's row: a slot bound to `id` (the value Save applies). Edit and
        /// reveal are left off: this is a modal dialog.
        void AddAssetRow(ui::FlexLayout& column, StringView label, Guid& id, StringView typeName,
                         StringView emptyText);

        /// A number, flag or choice setting's row: a numeric field (within the property's `range`),
        /// a check box, or a combo of the enum's values; recorded in m_values for Save.
        void AddValueRow(ui::FlexLayout& column, StringView label, const PropertyInfo& property,
                         const Instance& settings);

        /// An asset list setting's row: a list of slots (add, pick, drop, reorder, remove) over
        /// `m_assetLists[index]`, rebuilt after every change.
        void AddAssetListRow(ui::FlexLayout& column, StringView label, usize index);
        void RebuildAssetList(usize index);
        /// After the gesture that changed list `index`: its editor is running the callback, so it
        /// is replaced once the dispatch is over.
        void AssetListChanged(usize index);

        void Apply();

        /// A string setting's row and the reflected property Save writes it to.
        struct TextSetting
        {
            const PropertyInfo* property = nullptr;
            ui::EditText* edit = nullptr;
        };
        /// An asset setting: the reflected property and the value its slot holds.
        struct AssetSetting
        {
            const PropertyInfo* property = nullptr;
            Guid id;
        };

        /// A number, flag or choice setting: the reflected property and the control holding it.
        struct ValueSetting
        {
            const PropertyInfo* property = nullptr;
            ui::NumericField* number = nullptr;
            ui::CheckBox* flag = nullptr;
            ui::ComboBox* choice = nullptr;
        };

        /// An asset list setting: the reflected property, the list as edited (nil entries are
        /// slots not yet picked; Save drops them), the cell its editor sits in, and the editor.
        struct AssetListSetting
        {
            const PropertyInfo* property = nullptr;
            String assetType;
            Array<Guid> ids;
            ui::FlexLayout* host = nullptr;
            RefPtr<ContainerListEditor> list;
        };

        editor::EditorContext* m_context;
        UniquePtr<CategoryTabs> m_tabs;
        Array<TextSetting> m_texts;
        Array<AssetSetting> m_assets; // sized before the rows bind to it: never reallocates after
        Array<RefPtr<ResourceRefEditor>> m_assetRows; // the rows' editors; their views sit in rows
        Array<AssetListSetting> m_assetLists; // sized before the rows build: indices stay put
        Array<ValueSetting> m_values;
        RefPtr<ui::ComboBox> m_msaaCombo; // scene-pass MSAA: Off/2x/4x -> renderMsaaSamples 1/2/4
    };

    RTTI_DEFINE_OBJECT(ProjectSettingsDialog, "rtti::editor::editor::app")
}

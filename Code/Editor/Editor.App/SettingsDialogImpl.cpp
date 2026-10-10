// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :settings_dialog partition.
//
// ProjectSettingsDialog: a modal editor for the project manifest (Project.xml) - the settings
// ProjectSettings' reflection describes (its name, native module and asset settings, each asset
// slot filtered to the type the setting names) and the MSAA level, a tab per category the settings
// name. The engine version stamp is shown read-only under General (every save re-stamps it to the
// running engine; the launcher/project-manager owns migration).
//
// [Save] writes the fields back into EditorProject::Settings() and persists the manifest;
// [Cancel]/Escape discards. Every asset setting is a ResourceRefEditor row: pick, drop and clear
// set the instance GUID (rename/move-proof); Save keeps the path alongside as a mirror.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.content;
import foundation.ui;
import engine.render; // MsaaSamplesForIndex (the canonical MSAA level mapping)
import engine.project; // ProjectSettings' reflected settings
import editor.core;
import :resource_ref_editor;
import :container_list_editor;
import :asset_picker_dialog;

using namespace foundation::core;
namespace content = foundation::content;
namespace ui = foundation::ui;

namespace editor::app
{
    ui::FlexLayout* ProjectSettingsDialog::AddRow(ui::FlexLayout& column, StringView label)
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

    ui::EditText* ProjectSettingsDialog::AddTextRow(ui::FlexLayout& column, StringView label,
                                                    StringView value)
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

    void ProjectSettingsDialog::BuildSettingRows(editor::EditorProject* project)
    {
        namespace proj = engine::project;
        const TypeInfo& type = proj::ProjectSettings::StaticType();
        const Instance settings(project != nullptr ? &project->Settings() : nullptr, &type);
        // The asset settings first, all of them, so the slots bind to entries that stay put.
        for (const PropertyInfo& property : Properties(type))
        {
            if (proj::IsAssetSetting(property))
            {
                AssetSetting entry;
                entry.property = &property;
                if (project != nullptr)
                {
                    entry.id = *static_cast<const Guid*>(property.address(settings));
                }
                m_assets.PushBack(entry);
            }
            else if (proj::IsAssetListSetting(property))
            {
                AssetListSetting entry;
                entry.property = &property;
                entry.assetType = *proj::SettingAttribute(property, proj::kSettingAssetTypeAttribute);
                if (project != nullptr)
                {
                    entry.ids = *static_cast<const Array<Guid>*>(property.address(settings));
                }
                m_assetLists.PushBack(Move(entry));
            }
        }
        usize asset = 0;
        usize list = 0;
        for (const PropertyInfo& property : Properties(type))
        {
            const String* label = proj::SettingAttribute(property, proj::kSettingLabelAttribute);
            if (label == nullptr)
            {
                continue;
            }
            ui::FlexLayout& column = m_tabs->Column(proj::SettingCategory(property));
            if (proj::IsAssetListSetting(property))
            {
                AddAssetListRow(column, label->AsView(), list++);
                continue;
            }
            if (const String* assetType =
                    proj::SettingAttribute(property, proj::kSettingAssetTypeAttribute))
            {
                const String* emptyText =
                    proj::SettingAttribute(property, proj::kSettingEmptyTextAttribute);
                AddAssetRow(column, label->AsView(), m_assets[asset++].id, assetType->AsView(),
                            emptyText != nullptr ? emptyText->AsView() : StringView(u8"(none)"));
            }
            else if (property.type == &TypeOf<String>())
            {
                const StringView value =
                    project != nullptr
                        ? static_cast<const String*>(property.address(settings))->AsView()
                        : StringView(u8"");
                m_texts.PushBack(TextSetting{&property, AddTextRow(column, label->AsView(), value)});
            }
            else if (&property != FindProperty(type, "renderMsaaSamples")) // MSAA: its own row
            {
                AddValueRow(column, label->AsView(), property, settings);
            }
        }
    }

    void ProjectSettingsDialog::AddValueRow(ui::FlexLayout& column, StringView label,
                                            const PropertyInfo& property, const Instance& settings)
    {
        const void* address = settings.IsEmpty() ? nullptr : property.address(settings);
        ValueSetting entry;
        entry.property = &property;
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        grow.AlignSelf = ui::Align::Center;
        if (IsEnum(*property.type))
        {
            ui::FlexLayout* row = AddRow(column, label);
            auto combo = MakeRef<ui::ComboBox>(MemoryAllocator());
            const i64 current = address != nullptr ? ReadEnumValue(address, *property.type) : 0;
            i32 selected = 0;
            for (usize i = 0; i < EnumeratorCount(*property.type); ++i)
            {
                const EnumValue& value = EnumeratorAt(*property.type, i);
                (void)combo->AddItem(StringView(reinterpret_cast<const utf8char*>(value.name)));
                selected = value.value == current ? static_cast<i32>(i) : selected;
            }
            combo->SetSelectedIndex(selected);
            entry.choice = combo.Get();
            row->AddView(combo.Get(), grow);
        }
        else if (property.type == &TypeOf<bool>())
        {
            ui::FlexLayout* row = AddRow(column, label);
            auto check = MakeRef<ui::CheckBox>(MemoryAllocator(), StringView(),
                                               address != nullptr && *static_cast<const bool*>(address));
            entry.flag = check.Get();
            row->AddView(check.Get(), grow);
        }
        else if (property.type == &TypeOf<u32>())
        {
            ui::FlexLayout* row = AddRow(column, label);
            auto field = MakeRef<ui::NumericField>(MemoryAllocator());
            field->SetDecimalPlaces(0);
            field->SetStep(1.0);
            f64 least = 0.0;
            f64 most = 4294967295.0;
            if (const Attribute* range = FindAttribute(property, u8"range"))
            {
                if (const Float4* bounds = range->value.TryGet<Float4>())
                {
                    least = bounds->x;
                    most = bounds->y;
                }
            }
            field->SetMin(least);
            field->SetMax(most);
            field->SetValue(address != nullptr ? static_cast<f64>(*static_cast<const u32*>(address)) : least);
            entry.number = field.Get();
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(100));
            fixed.AlignSelf = ui::Align::Center;
            row->AddView(field.Get(), fixed);
        }
        else
        {
            return; // a kind no row edits
        }
        m_values.PushBack(entry);
    }

    void ProjectSettingsDialog::AddAssetRow(ui::FlexLayout& column, StringView label, Guid& id,
                                            StringView typeName, StringView emptyText)
    {
        ui::FlexLayout* row = AddRow(column, label);
        const StringView types[] = {typeName};
        auto editor = MakeRef<ResourceRefEditor>(MemoryAllocator(), label, emptyText, StringView{},
                                                 Span<const StringView>{types, 1});
        editor->SetEmptyText(emptyText);
        Guid* target = &id;
        editor->BindAsset(*m_context, [target]() { return *target; },
                          [target](const Guid& picked) { *target = picked; },
                          ResourceRefEditor::BindOptions{.edit = false, .reveal = false});
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        grow.AlignSelf = ui::Align::Center;
        row->AddView(editor->EditorView(), grow);
        m_assetRows.PushBack(Move(editor));
    }

    void ProjectSettingsDialog::AddAssetListRow(ui::FlexLayout& column, StringView label, usize index)
    {
        ui::FlexLayout* row = AddRow(column, label);
        auto host = MakeRef<ui::FlexLayout>(MemoryAllocator());
        host->Direction = ui::Orientation::Vertical;
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        row->AddView(host.Get(), grow);
        m_assetLists[index].host = host.Get();
        RebuildAssetList(index);
    }

    void ProjectSettingsDialog::AssetListChanged(usize index)
    {
        if (Context == nullptr)
        {
            return;
        }
        RefPtr<ProjectSettingsDialog> self(this); // alive until the rebuild runs
        Context->MutationQueueRef().QueueAction(Function<void()>{[self, index]() { self->RebuildAssetList(index); }});
    }

    void ProjectSettingsDialog::RebuildAssetList(usize index)
    {
        AssetListSetting& setting = m_assetLists[index];
        if (setting.list.Get() != nullptr)
        {
            setting.host->RemoveView(setting.list->EditorView());
        }
        const String* label = engine::project::SettingAttribute(*setting.property,
                                                                engine::project::kSettingLabelAttribute);
        auto list = MakeRef<ContainerListEditor>(MemoryAllocator(), label->AsView(), StringView(u8"Project"));
        for (const Guid& id : setting.ids)
        {
            list->slotNames.PushBack(id.IsNil() ? String(Format(u8"(pick a {})", setting.assetType.AsView()))
                                                : String(m_context->AssetNameFor(id)));
        }
        Array<String> accepted;
        accepted.PushBack(setting.assetType);
        list->SetAcceptedTypes(Move(accepted));
        ProjectSettingsDialog* self = this;
        list->OnAdd = [self, index]()
        {
            self->m_assetLists[index].ids.PushBack(Guid());
            self->AssetListChanged(index);
        };
        list->OnRemoveSlot = [self, index](usize slot)
        {
            Array<Guid>& ids = self->m_assetLists[index].ids;
            if (slot < ids.Size())
            {
                ids.RemoveAt(slot);
                self->AssetListChanged(index);
            }
        };
        list->OnMoveSlot = [self, index](usize slot, bool up)
        {
            Array<Guid>& ids = self->m_assetLists[index].ids;
            if ((up && slot == 0) || slot >= ids.Size())
            {
                return;
            }
            const usize other = up ? slot - 1 : slot + 1;
            if (other >= ids.Size())
            {
                return;
            }
            const Guid moved = ids[slot];
            ids[slot] = ids[other];
            ids[other] = moved;
            self->AssetListChanged(index);
        };
        list->OnAssignSlot = [self, index](usize slot, const Guid& picked)
        {
            Array<Guid>& ids = self->m_assetLists[index].ids;
            if (slot < ids.Size())
            {
                ids[slot] = picked;
                self->AssetListChanged(index);
            }
        };
        list->OnAppendDropped = [self, index](const Guid& picked)
        {
            self->m_assetLists[index].ids.PushBack(picked);
            self->AssetListChanged(index);
        };
        list->OnPickSlot = [self, index](usize slot)
        {
            if (self->Context == nullptr)
            {
                return;
            }
            Array<String> types;
            types.PushBack(self->m_assetLists[index].assetType);
            auto dialog = MakeRef<AssetPickerDialog>(self->MemoryAllocator(), *self->m_context, Move(types));
            dialog->OnPicked = [self, index, slot](const Guid& picked)
            {
                Array<Guid>& ids = self->m_assetLists[index].ids;
                if (slot < ids.Size())
                {
                    ids[slot] = picked;
                    self->AssetListChanged(index);
                }
            };
            dialog->Show(self->Context);
        };
        ui::LayoutStyle match;
        match.Width = ui::SizeSpec::Match();
        setting.host->AddView(list->EditorView(), match);
        setting.list = Move(list);
    }

    void ProjectSettingsDialog::Apply()
    {
        editor::EditorProject* project = m_context->Project();
        if (project == nullptr)
        {
            Close(ui::DialogResult::Cancel);
            return;
        }
        const Instance settings(&project->Settings(),
                                &engine::project::ProjectSettings::StaticType());
        for (const TextSetting& text : m_texts)
        {
            *static_cast<String*>(text.property->address(settings)) = String(text.edit->Text());
        }
        for (const AssetSetting& asset : m_assets)
        {
            *static_cast<Guid*>(asset.property->address(settings)) = asset.id;
        }
        for (const ValueSetting& value : m_values)
        {
            void* address = value.property->address(settings);
            if (value.choice != nullptr)
            {
                const i32 index = value.choice->SelectedIndex();
                if (index >= 0 && static_cast<usize>(index) < EnumeratorCount(*value.property->type))
                {
                    WriteEnumValue(address, *value.property->type,
                                   EnumeratorAt(*value.property->type, static_cast<usize>(index)).value);
                }
            }
            else if (value.flag != nullptr)
            {
                *static_cast<bool*>(address) = value.flag->IsChecked.Value();
            }
            else if (value.number != nullptr)
            {
                *static_cast<u32*>(address) = static_cast<u32>(value.number->Value());
            }
        }
        for (const AssetListSetting& list : m_assetLists)
        {
            // Picked slots only, each once.
            Array<Guid>& ids = *static_cast<Array<Guid>*>(list.property->address(settings));
            ids.Clear();
            for (const Guid& id : list.ids)
            {
                bool listed = id.IsNil();
                for (const Guid& other : ids)
                {
                    listed = listed || other == id;
                }
                if (!listed)
                {
                    ids.PushBack(id);
                }
            }
        }
        const i32 msaaIdx = (m_msaaCombo.Get() != nullptr) ? m_msaaCombo->SelectedIndex() : 0;
        project->Settings().renderMsaaSamples = engine::render::MsaaSamplesForIndex(msaaIdx);
        // The source-DB path mirrors beside the guids (display / v<6 fallback).
        project->Settings().RefreshPathMirrors(
            [project](const Guid& id)
            {
                content::Instance* instance = project->SourceDb().GetInstance(id);
                return instance != nullptr ? instance->Path() : String();
            });
        if (project->SaveSettings().IsOk())
        {
            m_context->SetStatus(u8"Project settings saved.");
            // Re-apply settings-derived session state NOW (default UI font/theme binds) -
            // without this, a changed default font kept the OLD bind until project reopen.
            m_context->NotifyProjectSettingsChanged();
            LOG_INFO(u8"Project", u8"settings saved (default scene: {})",
                              project->Settings().defaultScene.IsEmpty()
                                  ? StringView(u8"(none)")
                                  : project->Settings().defaultScene.AsView());
        }
        else
        {
            m_context->Notify(editor::NoticeKind::Error,
                              u8"Project settings save FAILED (see console).");
        }
        Close(ui::DialogResult::OK);
    }
}

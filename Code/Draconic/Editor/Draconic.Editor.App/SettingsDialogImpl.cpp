// Draconic::EditorApp - :settings_dialog partition.
//
// ProjectSettingsDialog: a modal editor for the project manifest (Project.xml) - the fields a
// user meaningfully changes from inside the editor: project name, the default scene (picked
// through the guid-authoritative AssetPickerDialog, filtered to scenes), and the startup game
// script path. The engine version stamp is shown read-only (every save re-stamps it to the
// running engine; the launcher/project-manager owns migration).
//
// [Save] writes the fields back into EditorProject::Settings() and persists the manifest;
// [Cancel]/Escape discards. The default-scene pick stores the instance GUID (rename/move-proof)
// with the path kept alongside as the human-readable mirror; the picker's [Clear] sets "none".

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.editor.app;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.editor.core;
import :asset_picker_dialog;

using namespace draconic::foundation;

namespace draconic::editor::app
{
    ui::FlexLayout* ProjectSettingsDialog::AddRow(ui::FlexLayout& column, StringView label)
    {
        auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = 8;
        {
            auto text = MakeRef<ui::Label>(DefaultAllocator(), label);
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(110));
            lp->AlignSelf = ui::Align::Center;
            row->AddView(text.Get(), lp);
        }
        ui::FlexLayout* raw = row.Get();
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            column.AddView(row.Get(), lp);
        }
        return raw;
    }

    ui::EditText* ProjectSettingsDialog::AddTextRow(ui::FlexLayout& column, StringView label,
                                                    StringView value)
    {
        ui::FlexLayout* row = AddRow(column, label);
        auto edit = MakeRef<ui::EditText>(DefaultAllocator());
        edit->SetText(value);
        ui::EditText* raw = edit.Get();
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Grow = 1.0f;
        row->AddView(edit.Get(), lp);
        return raw;
    }

    void ProjectSettingsDialog::PickBusLayout()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"AudioBusLayoutAsset"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_busLayoutId = id;
            if (content::Instance* layout =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_busLayoutLabel->SetText(layout->Path().AsView());
            }
            else
            {
                self->m_busLayoutLabel->SetText(u8"(built-in)");
            }
        };
        picker->Show(Context);
    }

    void ProjectSettingsDialog::PickStartupScript()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"ScriptClassAsset"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_scriptId = id;
            if (content::Instance* script =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_scriptLabel->SetText(script->Path().AsView());
            }
            else
            {
                self->m_scriptLabel->SetText(u8"(none)");
            }
        };
        picker->Show(Context);
    }

    void ProjectSettingsDialog::PickInputMap()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"InputMapAsset"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_inputMapId = id;
            if (content::Instance* map =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_inputMapLabel->SetText(map->Path().AsView());
            }
            else
            {
                self->m_inputMapLabel->SetText(u8"(none)");
            }
        };
        picker->Show(Context);
    }

    void ProjectSettingsDialog::PickUiTheme()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"UIThemeAsset"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_uiThemeId = id;
            if (content::Instance* theme =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_uiThemeLabel->SetText(theme->Path().AsView());
            }
            else
            {
                self->m_uiThemeLabel->SetText(u8"(built-in)");
            }
        };
        picker->Show(Context);
    }

    void ProjectSettingsDialog::PickLoadingDocument()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"UIDocumentAsset"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_loadingDocId = id;
            if (content::Instance* doc =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_loadingDocLabel->SetText(doc->Path().AsView());
            }
            else
            {
                self->m_loadingDocLabel->SetText(u8"(built-in)");
            }
        };
        picker->Show(Context);
    }

    void ProjectSettingsDialog::PickScene()
    {
        if (Context == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"SceneDocument"));
        auto picker = MakeRef<AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        ProjectSettingsDialog* self = this;
        picker->OnPicked = [self](const Guid& id)
        {
            self->m_sceneId = id;
            if (content::Instance* scene =
                    !id.IsNil() && self->m_context->Project() != nullptr
                        ? self->m_context->Project()->SourceDb().GetInstance(id)
                        : nullptr)
            {
                self->m_sceneLabel->SetText(scene->Path().AsView());
            }
            else
            {
                self->m_sceneLabel->SetText(u8"(none)");
            }
        };
        picker->Show(Context); // stacks above this dialog on the popup layer
    }

    void ProjectSettingsDialog::Apply()
    {
        draconic::editor::EditorProject* project = m_context->Project();
        if (project == nullptr)
        {
            Close(ui::DialogResult::Cancel);
            return;
        }
        project->Settings().name = String(m_nameEdit->Text());
        project->Settings().startupScriptId = m_scriptId;
        project->Settings().startupScript =
            String(); // the source-DB path mirror (display / v<6 fallback)
        if (content::Instance* script =
                !m_scriptId.IsNil() ? project->SourceDb().GetInstance(m_scriptId) : nullptr)
        {
            project->Settings().startupScript = script->Path();
        }
        project->Settings().defaultSceneId = m_sceneId;
        project->Settings().defaultInputMapId = m_inputMapId;
        project->Settings().defaultBusLayoutId = m_busLayoutId;
        project->Settings().defaultUiThemeId = m_uiThemeId;
        project->Settings().loadingDocumentId = m_loadingDocId;
        project->Settings().defaultScene = String();
        if (content::Instance* scene =
                !m_sceneId.IsNil() ? project->SourceDb().GetInstance(m_sceneId) : nullptr)
        {
            project->Settings().defaultScene = scene->Path();
        }
        if (project->SaveSettings().IsOk())
        {
            m_context->SetStatus(u8"Project settings saved.");
            DRACONIC_LOG_INFO(u8"Project", u8"settings saved (default scene: {})",
                              project->Settings().defaultScene.IsEmpty()
                                  ? StringView(u8"(none)")
                                  : project->Settings().defaultScene.AsView());
        }
        else
        {
            m_context->Notify(draconic::editor::NoticeKind::Error,
                              u8"Project settings save FAILED (see console).");
        }
        Close(ui::DialogResult::OK);
    }
}

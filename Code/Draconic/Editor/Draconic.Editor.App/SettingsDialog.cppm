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

export module draconic.editor.app:settings_dialog;

import draconic.foundation;
import draconic.content;
import draconic.ui;
import draconic.editor.core;
import :asset_picker_dialog;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;
    namespace content = draconic::content;

    class ProjectSettingsDialog final : public ui::Dialog
    {
        DRACONIC_OBJECT(ProjectSettingsDialog, ui::Dialog)
    public:
        explicit ProjectSettingsDialog(draconic::editor::EditorContext& context)
            : ui::Dialog(u8"Project Settings"), m_context(&context)
        {
            MinWidth.SetValue(460.0f);
            MinHeight.SetValue(240.0f);
            MaxWidth.SetValue(560.0f);
            MaxHeight.SetValue(560.0f); // taller so the ~8 rows fit; the ScrollView handles overflow

            draconic::editor::EditorProject* project = context.Project();

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 8;

            m_nameEdit = AddTextRow(*column, u8"Name",
                                    project != nullptr ? project->Settings().name.AsView()
                                                       : StringView(u8""));

            // Default scene: read-only path + [Pick...] (the picker owns clearing too).
            {
                ui::FlexLayout* row = AddRow(*column, u8"Default scene");
                m_sceneLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(none)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_sceneLabel.Get(), lp);
                }
                m_pickButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    m_pickButton->OnClick.Add([self](ui::ButtonBase*) { self->PickScene(); });
                    row->AddView(m_pickButton.Get());
                }
                if (project != nullptr)
                {
                    m_sceneId = project->Settings().defaultSceneId;
                    // Prefer the live instance's path over the stored mirror (never lies).
                    if (content::Instance* scene = !m_sceneId.IsNil()
                                                       ? project->SourceDb().GetInstance(m_sceneId)
                                                       : nullptr)
                    {
                        m_sceneLabel->SetText(scene->Path().AsView());
                    }
                    else if (!project->Settings().defaultScene.IsEmpty())
                    {
                        m_sceneLabel->SetText(project->Settings().defaultScene.AsView());
                    }
                }
            }

            // Startup script: the cooked ScriptClass asset the player (and the Game tab) binds at
            // startup - picked by GUID like the default scene, not a typed path.
            {
                ui::FlexLayout* row = AddRow(*column, u8"Startup script");
                m_scriptLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(none)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_scriptLabel.Get(), lp);
                }
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    pick->OnClick.Add([self](ui::ButtonBase*) { self->PickStartupScript(); });
                    row->AddView(pick.Get());
                }
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                {
                    ProjectSettingsDialog* self = this;
                    clear->OnClick.Add(
                        [self](ui::ButtonBase*)
                        {
                            self->m_scriptId = Guid{};
                            self->m_scriptLabel->SetText(u8"(none)");
                        });
                    row->AddView(clear.Get());
                }
                if (project != nullptr)
                {
                    m_scriptId = project->Settings().startupScriptId;
                    if (content::Instance* script =
                            !m_scriptId.IsNil() ? project->SourceDb().GetInstance(m_scriptId)
                                                : nullptr)
                    {
                        m_scriptLabel->SetText(script->Path().AsView());
                    }
                }
            }

            // Default input map: the cooked map the player (and the Game tab) binds at
            // startup - the input twin of the default scene.
            {
                ui::FlexLayout* row = AddRow(*column, u8"Default input map");
                m_inputMapLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(none)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_inputMapLabel.Get(), lp);
                }
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    pick->OnClick.Add([self](ui::ButtonBase*) { self->PickInputMap(); });
                    row->AddView(pick.Get());
                }
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                {
                    ProjectSettingsDialog* self = this;
                    clear->OnClick.Add(
                        [self](ui::ButtonBase*)
                        {
                            self->m_inputMapId = Guid{};
                            self->m_inputMapLabel->SetText(u8"(none)");
                        });
                    row->AddView(clear.Get());
                }
                if (project != nullptr)
                {
                    m_inputMapId = project->Settings().defaultInputMapId;
                    if (content::Instance* map = !m_inputMapId.IsNil()
                                                     ? project->SourceDb().GetInstance(m_inputMapId)
                                                     : nullptr)
                    {
                        m_inputMapLabel->SetText(map->Path().AsView());
                    }
                }
            }

            // Default audio bus layout: the cooked mixer applied at startup (player +
            // play-in-editor) - nil = the built-in neutral four-bus layout.
            {
                ui::FlexLayout* row = AddRow(*column, u8"Default bus layout");
                m_busLayoutLabel =
                    MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(built-in)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_busLayoutLabel.Get(), lp);
                }
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    pick->OnClick.Add([self](ui::ButtonBase*) { self->PickBusLayout(); });
                    row->AddView(pick.Get());
                }
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                {
                    ProjectSettingsDialog* self = this;
                    clear->OnClick.Add(
                        [self](ui::ButtonBase*)
                        {
                            self->m_busLayoutId = Guid{};
                            self->m_busLayoutLabel->SetText(u8"(built-in)");
                        });
                    row->AddView(clear.Get());
                }
                if (project != nullptr)
                {
                    m_busLayoutId = project->Settings().defaultBusLayoutId;
                    if (content::Instance* layout =
                            !m_busLayoutId.IsNil() ? project->SourceDb().GetInstance(m_busLayoutId)
                                                   : nullptr)
                    {
                        m_busLayoutLabel->SetText(layout->Path().AsView());
                    }
                }
            }

            // Default UI theme: the cooked UITheme the game UI defaults to (player and
            // embedded runtime alike); nil = the built-in GameTheme.
            {
                ui::FlexLayout* row = AddRow(*column, u8"Default UI theme");
                m_uiThemeLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(built-in)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_uiThemeLabel.Get(), lp);
                }
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    pick->OnClick.Add([self](ui::ButtonBase*) { self->PickUiTheme(); });
                    row->AddView(pick.Get());
                }
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                {
                    ProjectSettingsDialog* self = this;
                    clear->OnClick.Add(
                        [self](ui::ButtonBase*)
                        {
                            self->m_uiThemeId = Guid{};
                            self->m_uiThemeLabel->SetText(u8"(built-in)");
                        });
                    row->AddView(clear.Get());
                }
                if (project != nullptr)
                {
                    m_uiThemeId = project->Settings().defaultUiThemeId;
                    if (content::Instance* theme =
                            !m_uiThemeId.IsNil() ? project->SourceDb().GetInstance(m_uiThemeId)
                                                 : nullptr)
                    {
                        m_uiThemeLabel->SetText(theme->Path().AsView());
                    }
                }
            }

            // Loading screen: the cooked UIDocument shown as the boot splash while the default
            // scene streams (task #123); nil = the built-in default (status + progress ids).
            {
                ui::FlexLayout* row = AddRow(*column, u8"Loading screen");
                m_loadingDocLabel =
                    MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(built-in)"));
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_loadingDocLabel.Get(), lp);
                }
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                {
                    ProjectSettingsDialog* self = this;
                    pick->OnClick.Add([self](ui::ButtonBase*) { self->PickLoadingDocument(); });
                    row->AddView(pick.Get());
                }
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                {
                    ProjectSettingsDialog* self = this;
                    clear->OnClick.Add(
                        [self](ui::ButtonBase*)
                        {
                            self->m_loadingDocId = Guid{};
                            self->m_loadingDocLabel->SetText(u8"(built-in)");
                        });
                    row->AddView(clear.Get());
                }
                if (project != nullptr)
                {
                    m_loadingDocId = project->Settings().loadingDocumentId;
                    if (content::Instance* doc =
                            !m_loadingDocId.IsNil() ? project->SourceDb().GetInstance(m_loadingDocId)
                                                    : nullptr)
                    {
                        m_loadingDocLabel->SetText(doc->Path().AsView());
                    }
                }
            }

            // Engine stamp - informational; re-stamped by every save.
            {
                ui::FlexLayout* row = AddRow(*column, u8"Engine version");
                auto value =
                    MakeRef<ui::Label>(DefaultAllocator(), draconic::editor::kEngineVersionString);
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->AlignSelf = ui::Align::Center;
                row->AddView(value.Get(), lp);
            }

            // Scroll the settings column so a tall list can't spill over the modal button row
            // (the Dialog gives its content a fixed, Grow-shared area above the buttons; without
            // scrolling, a column taller than that area overflows onto them). User feedback.
            auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
            scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            {
                auto lp = MakeRef<ui::LayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                scroll->AddView(column.Get(), lp);
            }
            SetContent(scroll.Get());

            {
                ProjectSettingsDialog* self = this;
                ui::Button* save = AddButton(u8"Save", ui::DialogResult::None);
                save->OnClick.Add([self](ui::ButtonBase*) { self->Apply(); });
            }
            AddButton(u8"Cancel", ui::DialogResult::Cancel);
        }

    private:
        // A labeled horizontal row (fixed-width label, callers append the field views).
        ui::FlexLayout* AddRow(ui::FlexLayout& column, StringView label);

        ui::EditText* AddTextRow(ui::FlexLayout& column, StringView label, StringView value);

        void PickBusLayout();

        void PickStartupScript();

        void PickInputMap();

        void PickUiTheme();

        void PickLoadingDocument();

        void PickScene();

        void Apply();

        draconic::editor::EditorContext* m_context;
        Guid m_inputMapId{};
        Guid m_busLayoutId{};
        RefPtr<ui::Label> m_inputMapLabel;
        RefPtr<ui::Label> m_busLayoutLabel;
        Guid m_uiThemeId{};
        RefPtr<ui::Label> m_uiThemeLabel;
        Guid m_loadingDocId{};
        RefPtr<ui::Label> m_loadingDocLabel;
        ui::EditText* m_nameEdit = nullptr;
        RefPtr<ui::Label> m_scriptLabel;
        Guid m_scriptId{};
        RefPtr<ui::Label> m_sceneLabel;
        RefPtr<ui::Button> m_pickButton;
        Guid m_sceneId;
    };

    DRACONIC_DEFINE_OBJECT(ProjectSettingsDialog, "draconic::editor::app")
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :asset_creation partition.
//
// EditorAssetCreation: File > New's create-and-open, kept out of the application. A creator writes
// the new asset into a group (the root when none is given). A new scene becomes the project's
// default scene when the settings name none. The assets list shows the new row, and an asset its
// builders cook is cooked at once so it can be picked; then its page opens. While a cook reads the
// databases (their structure frozen) the create waits and runs when the cook is done.
module;
#include "Core/Prelude.h"

export module editor.app:asset_creation;

import foundation.core;
import foundation.content;
import pipeline.core;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    class EditorAssetCreation
    {
    public:
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr;
            pipeline::BuilderRegistry* builders = nullptr;
            /// Opens the new asset's page.
            Function<void(foundation::content::Instance& instance)> openPage;
            /// Called when a new asset exists, so the assets list shows it.
            Function<void(foundation::content::Instance& instance)> created;
        };

        explicit EditorAssetCreation(Services services) : m_services(Move(services)) {}

        /// Sets the project assets are created in (null between projects).
        void SetProject(editor::EditorProject* project) noexcept { m_project = project; }

        /// Has `creator` write a new asset into `group` (the root when null), then opens its page.
        void CreateAndOpen(const pipeline::AssetCreator& creator, foundation::content::Group* group = nullptr)
        {
            editor::EditorContext& context = *m_services.context;
            if (m_services.cook->MutationLocked())
            {
                const pipeline::AssetCreator* entry = &creator;
                EditorAssetCreation* self = this;
                m_services.cook->RunWhenIdle(Function<void()>{[self, entry, group]() { self->CreateAndOpen(*entry, group); }});
                context.Notify(editor::NoticeKind::Info, u8"Create queued until the current cook finishes.");
                return;
            }
            if (m_project == nullptr)
            {
                context.Notify(editor::NoticeKind::Error, u8"Create failed: no project is open.");
                return;
            }
            const String sourcesRoot(m_project->SourcesRoot().AsView());
            foundation::content::Instance* instance = creator.Create(group, m_project->SourceDb().RootGroup(), sourcesRoot.AsView());
            if (instance == nullptr)
            {
                context.Notify(editor::NoticeKind::Error, Format(u8"Create failed: the {} could not be written.", creator.label).AsView());
                return;
            }
            AfterCreate(creator, *instance);
            if (m_services.openPage)
                m_services.openPage(*instance);
        }

        /// What follows any create (the menu's or an agent's): the asset becomes the default scene
        /// when there is none, its row is shown, and it is cooked when its builders cook it.
        void AfterCreate(const pipeline::AssetCreator& creator, foundation::content::Instance& instance)
        {
            if (creator.setsDefaultScene && m_project != nullptr && m_project->Settings().defaultSceneId.IsNil() &&
                m_project->Settings().defaultScene.IsEmpty())
            {
                m_project->Settings().defaultSceneId = instance.Id();
                m_project->Settings().defaultScene = instance.Path();
                (void)m_project->SaveSettings();
            }
            if (m_services.created)
                m_services.created(instance);
            if (m_services.builders->FindByTypeName(instance.TypeName()) != nullptr)
                m_services.cook->RequestCook(false);
        }

    private:
        Services m_services;
        editor::EditorProject* m_project = nullptr; // borrowed (the session's)
    };
}

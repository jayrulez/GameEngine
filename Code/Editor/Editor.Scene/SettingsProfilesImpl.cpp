// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :settings_profiles implementation.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.content;
import foundation.scene;
import pipeline.core;
import render.pipeline; // SettingsProfileAsset (the values interface every profile asset has)
import editor.core;

using namespace foundation::core;
namespace scene = foundation::scene;

namespace editor
{
    const pipeline::AssetCreator* SettingsProfileCreator(EditorContext& context,
                                                         scene::SceneSystem& system)
    {
        const TypeInfo* product = system.SettingsProfileType();
        if (product == nullptr || !context.SourceAssetTypesOf)
        {
            return nullptr;
        }
        for (const TypeInfo* asset : context.SourceAssetTypesOf(*product))
        {
            for (const pipeline::AssetCreator& creator : context.Creators().All())
            {
                if (creator.type == asset)
                {
                    return &creator;
                }
            }
        }
        return nullptr;
    }

    Status WriteSettingsProfileValues(foundation::content::Instance& instance, const void* values)
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        auto* asset = Cast<pipeline::SettingsProfileAsset>(object.Get());
        if (asset == nullptr || values == nullptr)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        asset->SetValues(values);
        return instance.WriteObject(*asset);
    }

    void QueueSettingsProfileEdit(EditorContext& context, scene::SceneSystem& system,
                                  const Guid& profile)
    {
        EditorProject* project = context.Project();
        if (project == nullptr || profile.IsNil() || system.SettingsProfile() != profile)
        {
            return;
        }
        foundation::content::Instance* instance = project->SourceDb().GetInstance(profile);
        if (instance == nullptr)
        {
            return;
        }
        RefPtr<ISerializable> object = instance->ReadObject();
        auto* asset = Cast<pipeline::SettingsProfileAsset>(object.Get());
        if (asset == nullptr)
        {
            return;
        }
        asset->SetValues(system.EffectiveSettingsInstance()); // a snapshot: the drain may be later
        context.RegisterAssetEdit(
            profile,
            [object, profile](foundation::content::ContentDatabase& db) -> Status
            {
                foundation::content::Instance* target = db.GetInstance(profile);
                return target != nullptr ? target->WriteObject(*object)
                                         : Status{ErrorCode::NotFound};
            });
    }

    foundation::content::Instance* MakeSettingsProfile(EditorContext& context,
                                                       SceneEditContext& edit,
                                                       const TypeInfo* settingsType,
                                                       StringView name)
    {
        EditorProject* project = context.Project();
        scene::SceneSystem* system = edit.FindSystemBySettingsType(settingsType);
        if (project == nullptr || system == nullptr)
        {
            return nullptr;
        }
        if (context.IsCookBusy())
        {
            context.Notify(NoticeKind::Info,
                           u8"Make Profile: a cook is running; try again when it finishes.");
            return nullptr;
        }
        const pipeline::AssetCreator* creator = SettingsProfileCreator(context, *system);
        if (creator == nullptr)
        {
            context.Notify(NoticeKind::Error, u8"Make Profile: nothing creates this profile.");
            return nullptr;
        }
        const String sourcesRoot = project->SourcesRoot();
        foundation::content::Instance* instance =
            creator->Create(nullptr, project->SourceDb().RootGroup(), sourcesRoot.AsView(), name);
        if (instance == nullptr ||
            !WriteSettingsProfileValues(*instance, system->EffectiveSettingsInstance()).IsOk())
        {
            context.Notify(NoticeKind::Error, u8"Make Profile: the profile could not be written.");
            return instance;
        }
        const Guid id = instance->Id();
        (void)edit.MutateSceneSettings(settingsType,
                                       [id](scene::SceneSystem& s) { s.UseSettingsProfile(id); });
        context.RequestCook(); // the profile's product, so the block's reference binds
        return instance;
    }
}

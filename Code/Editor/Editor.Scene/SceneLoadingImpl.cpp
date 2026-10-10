// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_loading implementation (see SceneLoading.cppm).

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import foundation.resource; // AsyncBindScope (pop-in loads)
import editor.core;

using namespace foundation::core;
namespace scene = foundation::scene;

namespace editor
{
    Status LoadEditorScene(EditorContext& context, foundation::content::Instance& instance, scene::Scene& target,
                           SceneLoadTimes* times)
    {
        const Stopwatch clock = Stopwatch::StartNew();
        const Status loaded = scene::LoadScene(instance, target);
        SceneLoadTimes spent;
        spent.parseMs = static_cast<i64>(clock.Elapsed().AsMilliseconds());
        if (loaded.IsOk())
        {
            // Bind the scene's resource refs to cooked products, asynchronously: decodes go to
            // workers and the content pops in (a large scene never stalls the UI thread).
            if (context.Resources() != nullptr)
            {
                foundation::resource::AsyncBindScope asyncScope(*context.Resources());
                scene::ResolveSceneResources(target, *context.Resources());
            }
            spent.bindMs = static_cast<i64>(clock.Elapsed().AsMilliseconds()) - spent.parseMs;
            // Prefab instances load as ref + deltas: respawn them from the SOURCE database (the
            // payloads are edited assets, not cooked products), then bind what they spawned.
            if (target.PendingPrefabInstanceCount() > 0 && context.Project() != nullptr)
            {
                EditorContext* editorContext = &context;
                scene::ResolveScenePrefabs(
                    target, Function<UniquePtr<IStream>(const Guid&)>{
                                [editorContext](const Guid& prefabId) -> UniquePtr<IStream>
                                {
                                    foundation::content::Instance* prefab =
                                        editorContext->Project()->SourceDb().GetInstance(prefabId);
                                    return (prefab != nullptr) ? prefab->ReadData(u8"scene") : UniquePtr<IStream>{};
                                }});
                if (context.Resources() != nullptr)
                {
                    foundation::resource::AsyncBindScope asyncScope(*context.Resources());
                    scene::ResolveSceneResources(target, *context.Resources());
                }
                spent.prefabMs =
                    static_cast<i64>(clock.Elapsed().AsMilliseconds()) - spent.parseMs - spent.bindMs;
            }
        }
        if (times != nullptr)
        {
            *times = spent;
        }
        return loaded;
    }

    bool SceneWorldBounds(scene::Scene& target, AABB& out)
    {
        bool any = false;
        target.ForEachEntity(
            [&](scene::EntityHandle entity)
            {
                AABB part;
                if (!scene::EntityWorldBounds(target, entity, part))
                {
                    return;
                }
                out = any ? Merge(out, part) : part;
                any = true;
            });
        return any;
    }
}

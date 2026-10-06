// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - scene export support (Sedulous's SceneExportSupport): the scene stream
// transcode and the scene reference scan every exporting host shares, each over the full
// manager set so no component type is silently skipped.

module;
#include "Core/Prelude.h"

module engine.composition;

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import foundation.resource;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace content = foundation::content;

namespace engine
{
    bool IsSceneLike(const content::Instance& instance)
    {
        const StringView name = instance.TypeName();
        return name == StringView(u8"SceneDocument") || name == StringView(u8"PrefabDocument");
    }

    void CollectSceneStreams(IAllocator& allocator, content::Group& group,
                             HashMap<Guid, Array<byte>>& outStreams)
    {
        for (content::Instance* instance : group.Instances())
        {
            if (!IsSceneLike(*instance))
            {
                continue;
            }
            UniquePtr<IStream> stream = instance->ReadData(kSceneStream);
            if (stream.Get() == nullptr)
            {
                continue;
            }
            scene::Scene scratch(allocator, u8"__export_transcode");
            AddAllSceneManagers(scratch);
            const bool isScene = instance->TypeName() == StringView(u8"SceneDocument");
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*stream, scratch, /*includeSettings=*/isScene);
            if (bytes.HasValue())
            {
                outStreams.InsertOrAssign(instance->Id(), Move(bytes.Value()));
            }
        }
        for (content::Group* child : group.Groups())
        {
            CollectSceneStreams(allocator, *child, outStreams);
        }
    }

    bool ScanSceneReferences(IAllocator& allocator, content::Instance& instance,
                             content::IContentDatabase& db, Array<Guid>& outResources,
                             Array<Guid>& outPrefabs)
    {
        scene::Scene scratch(allocator, u8"__export_scan");
        AddAllSceneManagers(scratch);
        if (!scene::LoadScene(instance, scratch).IsOk())
        {
            return false;
        }
        foundation::resource::ResourceManager collector(allocator, db);
        // Unresolved is the answer here: each bind would otherwise log a missing factory.
        collector.SetReportsMissingFactories(false);
        scene::ResolveSceneResources(scratch, collector);
        collector.CollectUnresolved(outResources);
        scratch.ForEachPendingPrefabInstance([&outPrefabs](scene::Scene::PendingPrefabInstance& pending)
                                             { outPrefabs.PushBack(pending.prefabId); });
        return true;
    }
}

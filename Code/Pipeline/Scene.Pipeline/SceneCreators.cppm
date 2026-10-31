// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Scene - the `scene.pipeline` module: the scene domain's New Asset creators. A scene
// is seeded with a directional sun under
// Scenes/, a prefab with a single root entity under Prefabs/, unless a group was picked.

module;
#include "Core/Prelude.h"

export module scene.pipeline;
export import :model_prefab;

import foundation.core;
import foundation.content;
import pipeline.core;

using namespace foundation::core;

export namespace pipeline
{
    /// A scene document in `target`, named `baseName` made unique, seeded with a directional sun
    /// so a fresh scene is lit out of the box (an authored entity, free to edit or delete).
    [[nodiscard]] foundation::content::Instance* CreateSceneInstance(
        foundation::content::Group* target, StringView baseName, IAllocator& allocator);

    /// A prefab document in `target`, named `baseName` made unique, seeded with one root entity
    /// so it opens in the single-root shape and spawns at once.
    [[nodiscard]] foundation::content::Instance* CreatePrefabInstance(
        foundation::content::Group* target, StringView baseName, IAllocator& allocator);

    /// File > New's scene and prefab creators (pipeline.registration composes every domain's).
    void RegisterSceneCreators(AssetCreatorRegistry& registry);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_loading partition.
//
// A scene document into a live scene, as the editor opens one: the document parsed, its resource
// references bound to their cooked products (asynchronously: the content pops in over the next
// frames, never stalling the UI thread), and its prefab instances respawned from the source
// database (the edited payloads, not the cooked ones) and bound in turn. The scene page opens its
// scene this way, and a profile page's preview loads the scene it previews on the same way, so
// both show a scene alike.

module;
#include "Core/Prelude.h"

export module editor.scene:scene_loading;

import foundation.core;
import foundation.content;
import foundation.scene;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// How long the UI thread spent on each part of a load (the async decodes are not in it).
    struct SceneLoadTimes
    {
        i64 parseMs = 0;
        i64 bindMs = 0;
        i64 prefabMs = 0;
    };

    /// Load `instance` (a scene document) into `scene`. NotFound for a scene with no stream yet
    /// (a new scene); another failure when the document does not parse.
    [[nodiscard]] Status LoadEditorScene(EditorContext& context, foundation::content::Instance& instance,
                                         foundation::scene::Scene& scene, SceneLoadTimes* times = nullptr);

    /// The world bounds of everything the scene's systems measure (meshes, colliders, ...), merged;
    /// false when nothing has a size. What a view frames to show the whole scene.
    [[nodiscard]] bool SceneWorldBounds(foundation::scene::Scene& scene, AABB& out);
}

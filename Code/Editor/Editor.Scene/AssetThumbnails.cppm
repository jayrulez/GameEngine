// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :asset_thumbnails partition.
//
// GPU thumbnail generators for the scene-domain assets. Meshes (the cooked mesh framed by its
// bounds, lit by a private sun, rendered with the neutral PBR default) and materials (the cooked
// material on a unit sphere) stage into the shared persistent scene through entities the generator
// owns and activates per job. Prefabs (an instance spawned and framed by its bounds), scene
// documents (loaded whole, viewed through their own primary camera when present) and particle
// effects (an emitter, prewarmed by the stage) each take a private scene per job, so arbitrary
// content never leaks into the shared stage. Registered from RegisterSceneEditor; the stage
// (editor.preview) drives Stage/Unstage and all GPU work.

module;
#include "Core/Prelude.h"

export module editor.scene:asset_thumbnails;

import foundation.core;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// Registers the scene-domain thumbnail generators: mesh, material, prefab, scene document and
    /// particle effect. Call from RegisterSceneEditor; the composition root's tripwire counts them,
    /// and `context` resolves source payloads per job.
    void RegisterSceneThumbnailGenerators(ThumbnailService& service, EditorContext& context);
}

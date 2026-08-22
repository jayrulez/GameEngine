// Engine::SceneSurface - the `engine.scenesurface` module.
//
// The SCENE-SURFACE composition root: the serialization sibling of Engine::ScriptSurface. A
// headless scene consumer - the CLI export's transcode scratch, the MCP server's scene_validate,
// any future tool that must LOAD a scene without a running engine - needs every serializable
// component manager and settings-bearing scene system present, or the scene reader silently skips
// their records ("skipping records of unknown component type"). This module declares the FULL scene
// composition (every domain's managers + reflection), built once from a single module list -
// the single source of truth that replaces the old imperative `AddAllSceneManagers` list and its
// `kSceneSystemCount` count tripwire (see scene-composition.md).
//
// Managers are plain value pools and the settings systems construct inert (no device, no engine,
// no run host), so the aggregate is safe in a fully headless process. The RUNTIME assembles from
// THIS SAME composition (DefaultApplication::SetComposition(FullSceneComposition()) - the sole
// assembly path since the ISceneAware removal): every scene carries the full system set whether
// or not the matching subsystem exists, and absent subsystems simply leave their systems unwired
// (inert value pools / no-op ticks). Per-configuration compositions (a minimal headless server, an
// editor-only set) build from the same per-domain modules when a consumer wants a subset.
//
// The wide subsystem imports live in the implementation unit, keeping this interface BMI lean
// (GCC module-interface hygiene).

module;
#include "Core/Prelude.h"

export module engine.scenesurface;

import foundation.core;
import foundation.scene;

using namespace foundation::core;

export namespace engine
{
    /// The FULL scene-surface composition: every serializable component manager + settings-bearing
    /// scene system across all engine domains + net, in dependency order. `Instantiate(scratch)`
    /// reproduces the old AddAllSceneManagers set; `RegisterReflection()` reproduces
    /// RegisterAllSceneComponentReflection. A manager added to a domain's Add<Domain>SceneManagers
    /// function is picked up automatically - there is no parallel list to forget.
    [[nodiscard]] const foundation::scene::SceneComposition& FullSceneComposition();

    /// Add EVERY serializable component manager + settings-bearing scene system to `scene`. For
    /// headless scratch scenes that deserialize arbitrary scene streams - export transcode, MCP
    /// validation. Thin wrapper over FullSceneComposition().Instantiate; retained for the many
    /// existing call sites that name it directly.
    void AddAllSceneManagers(foundation::scene::Scene& scene);

    /// Register every domain's component reflection (data-version gates + field metadata) - must
    /// run once before any scene stream with component payloads deserializes. Idempotent. Thin
    /// wrapper over FullSceneComposition().RegisterReflection.
    void RegisterAllSceneComponentReflection();
}
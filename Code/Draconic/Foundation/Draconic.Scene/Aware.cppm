/// Draconic::Scene - the `:aware` partition.
///
/// ISceneAware: implemented by a Context-level Subsystem that needs to react to scene
/// lifecycle - chiefly to inject its per-scene systems (component managers) into a new
/// scene. The SceneSubsystem brokers it (two-pass: OnSceneCreated, then OnSceneReady so
/// cross-subsystem per-scene state already exists). `Scene` is forward-declared (the
/// interface only needs the reference type), so this partition stays free of the heavy
/// Scene definition and of any runtime dependency.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene:aware;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::scene
{

    class Scene; // defined in :scene (same module)

    class ISceneAware
    {
    public:
        virtual ~ISceneAware() = default;

        // A new scene was created - inject per-scene systems here.
        virtual void OnSceneCreated(Scene& scene) = 0;
        // All scene-aware subsystems have run OnSceneCreated - safe to reach another
        // subsystem's per-scene state now.
        virtual void OnSceneReady(Scene& /*scene*/) {}
        // The scene is being destroyed - drop references to it.
        virtual void OnSceneDestroyed(Scene& /*scene*/) {}
    };

} // namespace draconic::scene

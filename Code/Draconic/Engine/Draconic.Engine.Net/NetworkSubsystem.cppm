/// Draconic::NetworkSubsystem - the `draconic.engine.net` module.
///
/// A Context-level subsystem (once-per-context BY CONTRACT) that integrates networking into scenes:
/// it injects the NetworkComponentManager into every scene (via ISceneAware), so authoring a
/// NetworkComponent on an entity is all a game needs for that entity to be replicable. It also
/// registers the replicated-component reflection.
///
/// This is the SCENE-INTEGRATION half of networking, deliberately separate from the per-instance
/// ENDPOINT (draconic.net.manager's NetworkManager, one per running game). The distinction: injecting
/// the component manager is a once-per-context concern (a Subsystem, like PhysicsSubsystem injecting
/// its managers); the live endpoint (server vs client) is per-GameInstance. The subsystem does no
/// per-frame work - the endpoint drives replication over the manager the subsystem installed.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.net;

import draconic.foundation;
import draconic.runtime;         // Subsystem, Context
import draconic.scene;           // Scene, ISceneAware
import draconic.engine.scene; // SceneSubsystem (to register as scene-aware)
import draconic.net.replication; // NetworkComponentManager + RegisterReplicationComponents

export namespace draconic::net
{

    class NetworkSubsystem final : public draconic::runtime::Subsystem,
                                   public draconic::scene::ISceneAware
    {
    public:
        // Inject the NetworkComponent manager into each new scene so authored NetworkComponents (and
        // the server's runtime AssignNetworkId) have a home. Replicated-state component managers (e.g.
        // the transform) are injected by their own subsystems - this adds only the identity tag pool.
        void OnSceneCreated(draconic::scene::Scene& scene) override
        {
            scene.AddSystem<NetworkComponentManager>(); // identity (NetworkId + authority + prefab)
            scene.AddSystem<
                NetworkedTransformComponentManager>(); // replicated transform (the common case)
        }

    protected:
        void OnInit() override
        {
            RegisterReplicationComponents(); // tooling: the reflected NetworkComponent (idempotent)
        }

        void OnReady() override
        {
            if (draconic::runtime::Context* ctx = GetContext())
            {
                if (auto* scenes = ctx->GetSubsystem<draconic::scene::SceneSubsystem>())
                {
                    scenes->RegisterSceneAware(this);
                }
            }
        }

        void OnShutdown() override
        {
            if (draconic::runtime::Context* ctx = GetContext())
            {
                if (auto* scenes = ctx->GetSubsystem<draconic::scene::SceneSubsystem>())
                {
                    scenes->UnregisterSceneAware(this);
                }
            }
        }
    };

} // namespace draconic::net

/// Engine::Net - the `engine.net` module.
///
/// A Context-level subsystem (once-per-context BY CONTRACT) that integrates networking into scenes:
/// it contributes the NetworkComponentManager to the declarative scene composition (SceneModule), so
/// authoring a NetworkComponent on an entity is all a game needs for that entity to be replicable. It
/// also registers the replicated-component reflection.
///
/// This is the SCENE-INTEGRATION half of networking, deliberately separate from the per-instance
/// ENDPOINT (foundation.net.manager's NetworkManager, one per running game). The distinction: injecting
/// the component manager is a once-per-context concern (a Subsystem, like PhysicsSubsystem injecting
/// its managers); the live endpoint (server vs client) is per-GameInstance. The subsystem does no
/// per-frame work - the endpoint drives replication over the manager the subsystem installed.

module;
#include "Core/Prelude.h"

export module engine.net;

import foundation.core;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene
import engine.scene; // SceneSubsystem (to register as scene-aware)
import foundation.net.replication; // NetworkComponentManager + RegisterReplicationComponents

using namespace foundation::net;

export namespace engine::net
{

    class NetworkSubsystem final : public foundation::runtime::Subsystem
    {
    protected:
        void OnInit() override
        {
            RegisterReplicationComponents(); // tooling: the reflected NetworkComponent (idempotent)
        }
    };

} // namespace foundation::net

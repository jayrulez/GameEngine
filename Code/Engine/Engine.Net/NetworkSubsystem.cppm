// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Net - the `engine.net` module.
///
/// An application-level subsystem (once per subsystem manager) that integrates networking into
/// scenes: it contributes the NetworkComponentManager to the declarative scene composition
/// (SceneModule), so authoring a NetworkComponent on an entity is all a game needs for that entity
/// to be replicable. It also registers the replicated-component reflection.
///
/// This is the scene-integration half of networking, separate from the per-instance endpoint
/// (foundation.net.manager's NetworkManager, one per running game). Injecting the component manager
/// is a once-per-context concern (a Subsystem, like PhysicsSubsystem injecting its managers); the
/// live endpoint (server or client) is per GameInstance. Replication runs per scene on the fixed
/// lane; this subsystem pumps the endpoints' transport each frame.

module;
#include "Core/Prelude.h"

export module engine.net;

import foundation.core;
import engine.domain;
import foundation.runtime;         // Subsystem, SubsystemManager
import foundation.run;             // the runs whose endpoints it pumps
import foundation.scene; // Scene and SceneSystem
import engine.scene; // SceneSubsystem (to register as scene-aware)
import foundation.net.replication; // NetworkComponentManager, RegisterReplicationComponents
import foundation.net.manager;     // NetworkManager (the endpoint the scene system drives)

using namespace foundation::core;

export namespace engine::net
{
    namespace scene = foundation::scene;
    namespace net = foundation::net;

    // The per-scene replication driver: installed into every scene by the net SceneModule and
    // ticked on the scene's fixed lane (deterministic, in lockstep with physics). It holds the
    // endpoint that replicates this scene, null (inert) unless this scene is the endpoint's current
    // replicated scene. The per-instance NetworkController wires and clears the endpoint at the
    // edges (SetReplicatedScene, StartServer, Connect, StopNetworking); this is a plain setter, not
    // a service.
    class NetworkSceneSystem final : public scene::SceneSystem
    {
    public:
        void SetEndpoint(net::NetworkManager* endpoint) noexcept { m_endpoint = endpoint; }
        [[nodiscard]] net::NetworkManager* Endpoint() const noexcept { return m_endpoint; }

    protected:
        void OnFixedUpdate(f32 /*fixedDeltaTime*/) override
        {
            if (m_endpoint != nullptr)
            {
                m_endpoint->UpdateReplication(); // server: capture and send; client: interpolate and apply
            }
        }

    private:
        net::NetworkManager* m_endpoint = nullptr; // borrowed; owned by the run's NetworkController
    };

    // The net SceneModule installer (engine level): the foundation component managers plus the
    // scene system, so the fixed-lane replication driver ships with the net managers as one
    // composition module. The same shape as the physics and audio installers
    // (engine::physics::AddPhysicsSceneManagers, etc.).
    inline void AddNetworkSceneManagers(scene::Scene& scene)
    {
        net::AddNetworkSceneManagers(scene); // foundation.net.replication: identity and transform managers
        scene.AddSystem<NetworkSceneSystem>();
    }

    // The once-per-context networking subsystem. It registers the reflected components, installs
    // the net scene managers and the fixed-lane replication driver through the SceneModule, and
    // owns the transport pump: each frame it drives every live endpoint's UpdateTransport on the
    // Context lane. Replication itself rides the per-scene fixed lane (NetworkSceneSystem); this is
    // the socket receive and send half.
    class NetworkSubsystem final : public foundation::runtime::Subsystem
    {
    protected:
        void OnInit() override
        {
            net::RegisterReplicationComponents(); // tooling: the reflected NetworkComponent (idempotent)
        }

        // Transport pump, per frame on the Context lane. PostUpdate rather than BeginFrame, so a
        // server's replication sends, queued this frame by the per-scene fixed lane
        // (SceneSubsystem::BeginFrame, then NetworkSceneSystem), flush in the same frame regardless
        // of subsystem sort order. Received deltas are buffered here and applied by the next
        // frame's scene fixed lane (interpolation absorbs the lag). Receive and send are not split
        // across BeginFrame and PostUpdate: NetSession::Update receives and flushes in one call,
        // and separating them is a transport-layer change.
        //
        // The endpoints are the runs': each run's network controller (its INetworkController
        // service) holds its live endpoint, read again every frame, so no endpoint pointer is held
        // across frames.
        void PostUpdate(f32 deltaTime) override
        {
            foundation::run::IRunManager* runs = Manager() != nullptr ? Manager()->RunManager() : nullptr;
            if (runs == nullptr)
            {
                return;
            }
            const f32 deltaMs = deltaTime * 1000.0f;
            runs->ForEachRun(
                [deltaMs](foundation::run::IRun& run)
                {
                    if (net::INetworkController* controller = run.Get<net::INetworkController>())
                    {
                        if (net::NetworkManager* endpoint = controller->NetEndpoint())
                        {
                            endpoint->UpdateTransport(deltaMs);
                        }
                    }
                });
        }
    };

} // namespace engine::net

export namespace engine::net
{
    /// This domain's declaration: what it brings to a scene, to reflection and to the script
    /// surface, and which resource modules come with it. Defined in the implementation unit (one
    /// instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& NetDomain() noexcept;
    /// Both net facades (the endpoint's and the replicated component's) behind one registrar.
    void RegisterNetworkScriptFacades();
}

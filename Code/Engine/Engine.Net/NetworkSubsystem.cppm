// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Net - the `engine.net` module.
///
/// An application-level subsystem (once per subsystem manager BY CONTRACT) that integrates networking into scenes:
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
import engine.domain;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene + SceneSystem
import engine.scene; // SceneSubsystem (to register as scene-aware)
import foundation.net.replication; // NetworkComponentManager + RegisterReplicationComponents
import foundation.net.manager;     // NetworkManager (the endpoint the scene system drives)

using namespace foundation::core;

export namespace engine::net
{
    namespace scene = foundation::scene;
    namespace net = foundation::net;

    // The per-scene REPLICATION driver: installed into every scene by the
    // net SceneModule and ticked on the scene's FIXED lane (deterministic, physics-lockstep). It holds
    // the endpoint that replicates THIS scene - null (inert) unless this scene is the endpoint's current
    // replicated scene. The per-instance NetworkController wires/clears the endpoint at the edges
    // (SetReplicatedScene / StartServer / Connect / StopNetworking); this is a plain setter, no service.
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
                m_endpoint->UpdateReplication(); // server: capture+send; client: sample-interp+apply
            }
        }

    private:
        net::NetworkManager* m_endpoint = nullptr; // borrowed; owned by the run's NetworkController
    };

    // The net SceneModule installer (engine level): the foundation component managers PLUS the scene
    // system, so the fixed-lane replication driver ships with the net managers as ONE composition module
    // (kNetModule stays a single module - ModuleCount unchanged). Mirrors the physics/audio installer
    // shape (engine::physics::AddPhysicsSceneManagers, etc.).
    inline void AddNetworkSceneManagers(scene::Scene& scene)
    {
        net::AddNetworkSceneManagers(scene); // foundation.net.replication: identity + transform managers
        scene.AddSystem<NetworkSceneSystem>();
    }

    // The app sets this once: it visits every LIVE endpoint across all instances (pull model), so the
    // subsystem can pump each one per-frame without knowing about GameInstances or NetworkControllers.
    // Re-fetched each frame (null endpoints skipped), so it never holds an endpoint pointer across frames.
    using EndpointVisitor = Function<void(const Function<void(net::NetworkManager&)>&)>;

    // The once-per-context networking subsystem. It (a) registers the reflected components, (b) installs
    // the net scene managers + the fixed-lane replication driver via the SceneModule, and (c) OWNS THE
    // TRANSPORT PUMP: each frame it drives every live endpoint's UpdateTransport on the Context lane
    // Replication itself
    // rides the per-scene fixed lane (NetworkSceneSystem); this is the socket recv/send half.
    class NetworkSubsystem final : public foundation::runtime::Subsystem
    {
    public:
        /// The app provides the endpoint enumerator (it owns the instance list); the subsystem owns the
        /// tick. Clear it (default-construct) at shutdown so the stored callback never outlives the app.
        void SetEndpointSource(EndpointVisitor source)
        {
            m_endpoints = static_cast<EndpointVisitor&&>(source);
        }

    protected:
        void OnInit() override
        {
            net::RegisterReplicationComponents(); // tooling: the reflected NetworkComponent (idempotent)
        }

        // Transport pump, per-frame on the Context lane. PostUpdate (not BeginFrame) so a server's
        // replication sends - queued this frame by the per-scene fixed lane (SceneSubsystem::BeginFrame ->
        // NetworkSceneSystem) - flush the SAME frame, independent of subsystem sort order. Received deltas
        // are buffered here and applied by next frame's scene fixed lane (interpolation absorbs the lag).
        // NOTE: the finer BeginFrame-recv / PostUpdate-send split is
        // NOT done - NetSession::Update recv+flushes in one call, and separating them is a transport-layer
        // change. A single UpdateTransport per frame is the pump.
        void PostUpdate(f32 deltaTime) override
        {
            if (!m_endpoints)
            {
                return;
            }
            const f32 deltaMs = deltaTime * 1000.0f;
            m_endpoints([deltaMs](net::NetworkManager& endpoint) { endpoint.UpdateTransport(deltaMs); });
        }

    private:
        EndpointVisitor m_endpoints; // app-provided; visits live endpoints across all instances
    };

} // namespace engine::net

export namespace engine::net
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& NetDomain() noexcept;
    /// Both net facades (the endpoint's and the replicated component's) behind one registrar.
    void RegisterNetworkScriptFacades();
}

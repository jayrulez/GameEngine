// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GameInstance :networkcontroller partition - a running game's networking, held apart from
// the GameInstance object. It owns the endpoint, the INetworkController implementation, the role
// lifecycle (StartServer/Connect/Stop), the net script binding and the replicated-scene selection.
// GameInstance composes one and forwards to it; it does not inherit net::INetworkController.
//
// This is a role separation only: the wire protocol, the replication model and the endpoint
// factories live elsewhere. It sits in engine.gameinstance beside its owner; it could move to
// engine.net if the script surface wants NetworkController.of(context).

export module engine.gameinstance:networkcontroller;

import foundation.core;
import foundation.scene;        // scene::Scene (the replicated scene) and GetSystem
import foundation.net.manager;  // NetworkManager, INetworkController, NetScriptBinding
import foundation.net.replication; // StateReplication::SpawnHandler (the net-spawn resolver type)
import foundation.script;       // IScriptContext (InstallNetScriptService)
import foundation.net.tls;      // ITlsBackend (a wss join checks the host with it)
import engine.net;              // NetworkSceneSystem (the per-scene fixed-lane replication driver)

using namespace foundation::core;

export namespace engine::runtime
{
    namespace core = foundation::core;
    namespace net = foundation::net;
    namespace scene = foundation::scene;
    namespace script = foundation::script;

    // The prefab net-spawn resolver factory: injected once by the app at wiring (it needs app
    // state, the content DB) and invoked by the controller to make a fresh resolver for every
    // endpoint it opens, so a reconnect keeps it. It is a factory rather than the resolver itself
    // because StateReplication::SpawnHandler is move-only, so the controller must produce one per
    // endpoint instead of copying one. The app hands over the factory once and the controller owns
    // the wiring.
    using SpawnResolverFactory = Function<net::StateReplication::SpawnHandler()>;

    // A running game's networking. Owns the endpoint (null when offline), implements
    // INetworkController for the Net facade, and caches the current scene so a freshly created
    // endpoint replicates it. The controller outlives every endpoint it creates, so the script
    // binding (controller=this) never dangles. A stable member of GameInstance, constructed and
    // destroyed with it.
    class NetworkController final : public net::INetworkController
    {
    public:
        /// The scene this run replicates. Cached so StartServer/Connect can set it on a fresh
        /// endpoint; applied live when the endpoint already exists (GameInstance::SetScene calls
        /// this on change). The old replicated scene's NetworkSceneSystem is detached (endpoint
        /// null) and the new one attached (endpoint live, or null offline), so only the replicated
        /// scene's fixed lane ever drives this endpoint's replication.
        void SetReplicatedScene(scene::Scene* scene)
        {
            if (scene == m_scene)
            {
                return; // unchanged: the endpoint and scene-system wiring are already correct
            }
            WireSceneSystem(nullptr); // detach the old replicated scene's system (uses current m_scene)
            m_scene = scene;
            if (m_net)
            {
                m_net->SetReplicatedScene(scene);
            }
            WireSceneSystem(m_net.Get()); // attach the new scene's system (live endpoint, or null offline)
        }

        /// Inject the prefab net-spawn resolver factory (app-owned, content-DB-backed). The controller
        /// invokes it to make a fresh resolver for each endpoint it opens, so reconnect keeps it.
        void SetSpawnResolverFactory(SpawnResolverFactory factory)
        {
            m_spawnResolverFactory = static_cast<SpawnResolverFactory&&>(factory);
        }

        /// Where a wss join's TLS backend comes from (the run's, made on first use).
        void SetTlsBackendSource(Function<net::ITlsBackend*()> source) { m_tlsSource = static_cast<Function<net::ITlsBackend*()>&&>(source); }

        /// Point `context` at this controller's net binding (controller=this), so the Net facade
        /// resolves this run's controller. Idempotent; a no-op when the context is null.
        void InstallScriptBinding(script::IScriptContext* scriptContext)
        {
            m_binding.controller = this; // stable; the endpoint m_net points at may come and go
            if (scriptContext != nullptr)
            {
                net::InstallNetScriptService(*scriptContext, m_binding);
            }
        }

        // The per-frame transport pump lives in engine::net::NetworkSubsystem::PostUpdate, which
        // drives each live endpoint's UpdateTransport.

        // INetworkController: the Net facade calls these. StartServer/Connect open a real UDP
        // socket and enter the role (returning false if that fails); StopNetworking drops the
        // endpoint. The live endpoint replicates this run's current scene.
        bool StartServer(u16 port, bool dedicated) override;
        bool Connect(core::StringView host, u16 port) override;
        void StopNetworking() override;
        [[nodiscard]] net::NetworkManager* NetEndpoint() const override { return m_net.Get(); }

    private:
        /// Point the current replicated scene's NetworkSceneSystem at `endpoint` (its live endpoint
        /// when attaching, null when detaching or offline). No-op when there is no scene, or for a
        /// scene built without the net module (no NetworkSceneSystem). The scene system dies with
        /// its scene, so this is only called while m_scene is alive (the controller clears m_scene
        /// before a scene dies).
        void WireSceneSystem(net::NetworkManager* endpoint)
        {
            if (m_scene == nullptr)
            {
                return;
            }
            if (engine::net::NetworkSceneSystem* system =
                    m_scene->GetSystem<engine::net::NetworkSceneSystem>())
            {
                system->SetEndpoint(endpoint);
            }
        }

        core::UniquePtr<net::NetworkManager> m_net; // this run's endpoint (null = offline)
        net::NetScriptBinding m_binding;            // stable; the facade resolves controller=this
        SpawnResolverFactory m_spawnResolverFactory; // app-injected; makes a resolver per endpoint
        scene::Scene* m_scene = nullptr;            // the scene a fresh endpoint replicates
        Function<net::ITlsBackend*()> m_tlsSource;  // the run's TLS backend, for a wss join
    };
}

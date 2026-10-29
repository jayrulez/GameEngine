// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.manager - the Net facade PROVEN end-to-end on both script backends.
//
// ManagerTests.cpp checks the facade TYPE registers; this drives a real AngelScript
// VM: a server manager installs its net.runtime service into the context, then a script calls
// Net.isServer()/isClient()/peerCount() and we read the results back. This is the acceptance test
// for the extensibility hook (registry -> AngelScript).
#include <doctest/doctest.h>
#include <filesystem>
#include <utility>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.manager;
import foundation.net.replication;
import foundation.scene;
import foundation.scene.resource;
import foundation.content;
import foundation.vfs;
import foundation.script;
import foundation.script.facades;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
namespace net = foundation::net;

namespace
{
    // A server endpoint over the sim network + a controller the Net facade resolves through. The
    // endpoint is already live here, so the runtime role hooks are no-ops (they are exercised
    // per-instance in the runtime tests); NetEndpoint() hands the facade the live server.
    struct ServerFixture final : net::INetworkController
    {
        net::SimDatagramNetwork network{foundation::core::DefaultAllocator(),
                                        net::SimConditions{}};
        net::NetworkManager server{*network.CreateSocket()};
        net::NetScriptBinding binding;
        ServerFixture()
        {
            server.StartServer(/*dedicated=*/true);
            binding.controller = this;
        }

        bool StartServer(u16, bool) override { return true; }
        bool Connect(StringView, u16) override { return false; }
        void StopNetworking() override {}
        [[nodiscard]] net::NetworkManager* NetEndpoint() const override
        {
            return const_cast<net::NetworkManager*>(&server);
        }
    };
}

namespace
{
    // A controller over an endpoint that may not exist yet (Net.on before hosting or joining).
    struct PeerController final : net::INetworkController
    {
        net::NetworkManager* endpoint = nullptr;
        net::NetScriptBinding binding;
        PeerController() { binding.controller = this; }
        bool StartServer(u16, bool) override { return false; }
        bool Connect(StringView, u16) override { return false; }
        void StopNetworking() override {}
        [[nodiscard]] net::NetworkManager* NetEndpoint() const override { return endpoint; }
    };

    // A host and a client over the sim network, each with its own controller (and so its own
    // script context's binding). The host's controller has no endpoint until Open: its script
    // registers Net.on first, as a game does before it hosts.
    struct HostAndClient
    {
        net::SimDatagramNetwork network{foundation::core::DefaultAllocator(), net::SimConditions{}};
        net::IDatagramSocket* hostSocket = network.CreateSocket();
        net::NetworkManager host{*hostSocket};
        net::NetworkManager client{*network.CreateSocket()};
        PeerController hostController;
        PeerController clientController;

        void Open()
        {
            host.StartServer(/*dedicated=*/false);
            hostController.endpoint = &host;
            net::InstallScriptRpcHandlers(hostController.binding, host); // what the controller does
            (void)client.ConnectTo(hostSocket->LocalEndpoint());
            clientController.endpoint = &client;
            net::InstallScriptRpcHandlers(clientController.binding, client);
            Pump(40);
            REQUIRE(host.Session().PeerCount() == 1u);
        }

        void Pump(int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                network.Advance(10.0f);
                host.Update(10.0f);
                client.Update(10.0f);
            }
        }

        [[nodiscard]] f64 ClientPeerOnHost() { return static_cast<f64>(host.Session().Peers()[0].id); }
    };
}

namespace
{
    // A content database on disk holding one prefab, "Guest": a root with a Networked Transform.
    struct GuestPrefab
    {
        std::filesystem::path dir = "scratch_net_spawn";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        Guid guest;
        utf8char text[37] = {};

        GuestPrefab()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), u8"scratch_net_spawn",
                                                                   DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                                  BinarySerializerFactory(), u8".rasset");
            foundation::scene::Scene author(DefaultAllocator(), u8"author");
            net::AddNetworkSceneManagers(author);
            const foundation::scene::EntityHandle root = author.CreateEntity(u8"Guest");
            author.GetSystem<net::NetworkedTransformComponentManager>()->Add(root);
            MemoryStream payload;
            REQUIRE(foundation::scene::CapturePrefab(author, root, payload).IsOk());
            foundation::content::Instance* prefab =
                db->RootGroup()->CreateInstance(u8"Guest", foundation::scene::PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            guest = prefab->Id();
            guest.ToChars(text);
        }
        ~GuestPrefab()
        {
            db.Reset();
            mount.Reset();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };

    // The host's script spawns the Guest at (1, 2, 3) with Net.spawn; the client's own copy
    // appears through its spawn handler and follows; a client's Net.spawn spawns nothing.
    void SpawnOverTheNetwork(RefPtr<IScriptManager> manager, const Function<String(StringView)>& hostScript,
                             const Function<String(StringView)>& clientScript, StringView name)
    {
        RegisterReflectedTypes(*manager);
        GuestPrefab prefab;
        HostAndClient fx;
        foundation::scene::Scene hostScene(DefaultAllocator(), u8"host");
        net::AddNetworkSceneManagers(hostScene);
        hostScene.AddSystem<foundation::scene::PrefabSpawnSystem>()->SetSource(prefab.db.Get(), nullptr);
        foundation::scene::Scene clientScene(DefaultAllocator(), u8"client");
        net::AddNetworkSceneManagers(clientScene);
        fx.host.SetReplicatedScene(&hostScene);
        fx.client.SetReplicatedScene(&clientScene);
        foundation::content::ContentDatabase* db = prefab.db.Get();
        fx.client.Replication().SetSpawnHandler(
            [db](foundation::scene::Scene& s, const Guid& p, net::NetworkId) -> foundation::scene::EntityHandle
            { return foundation::scene::PrefabSpawnSystem::SpawnInto(s, db, nullptr, p); });
        fx.Open();

        RefPtr<IScriptContext> hostCtx = manager->CreateContext();
        RefPtr<IScriptContext> clientCtx = manager->CreateContext();
        net::InstallNetScriptService(*hostCtx, fx.hostController.binding);
        net::InstallNetScriptService(*clientCtx, fx.clientController.binding);
        REQUIRE(hostCtx->Load(hostScript(StringView(prefab.text)).AsView(), name).IsOk());
        REQUIRE(clientCtx->Load(clientScript(StringView(prefab.text)).AsView(), name).IsOk());
        CHECK(hostCtx->GetGlobal(u8"Spawned").Get<bool>());
        CHECK_FALSE(clientCtx->GetGlobal(u8"Spawned").Get<bool>()); // only the host spawns
        fx.Pump(80);

        const foundation::scene::EntityHandle onHost = hostScene.FindEntityByName(u8"Guest");
        REQUIRE(hostScene.IsValid(onHost));
        CHECK(hostScene.GetSystem<net::NetworkComponentManager>()->Get(onHost) != nullptr);
        const foundation::scene::EntityHandle onClient = clientScene.FindEntityByName(u8"Guest");
        REQUIRE(clientScene.IsValid(onClient)); // the client spawned its own from the prefab
        const Float3 at = clientScene.GetLocalTransform(onClient).position;
        CHECK(at.x == doctest::Approx(1.0f));
        CHECK(at.y == doctest::Approx(2.0f));
        CHECK(at.z == doctest::Approx(3.0f));
    }
}

namespace
{
    // The host and two clients, each with a scene, a controller and a script context, and the
    // Guest prefab to spawn: what slice 3 (owners, one-player sends, joins and leaves) needs.
    struct Players
    {
        GuestPrefab prefab;
        net::SimDatagramNetwork network{foundation::core::DefaultAllocator(), net::SimConditions{}};
        net::IDatagramSocket* hostSocket = network.CreateSocket();
        net::NetworkManager host{*hostSocket};
        net::NetworkManager a{*network.CreateSocket()};
        net::NetworkManager b{*network.CreateSocket()};
        PeerController hostController, aController, bController;
        foundation::scene::Scene hostScene{DefaultAllocator(), u8"host"};
        foundation::scene::Scene aScene{DefaultAllocator(), u8"a"};
        foundation::scene::Scene bScene{DefaultAllocator(), u8"b"};
        RefPtr<IScriptContext> hostCtx, aCtx, bCtx;

        explicit Players(IScriptManager& manager)
        {
            net::AddNetworkSceneManagers(hostScene);
            hostScene.AddSystem<foundation::scene::PrefabSpawnSystem>()->SetSource(prefab.db.Get(), nullptr);
            host.SetReplicatedScene(&hostScene);
            foundation::content::ContentDatabase* db = prefab.db.Get();
            for (auto [endpoint, scene] : {std::pair{&a, &aScene}, std::pair{&b, &bScene}})
            {
                net::AddNetworkSceneManagers(*scene);
                endpoint->SetReplicatedScene(scene);
                endpoint->Replication().SetSpawnHandler(
                    [db](foundation::scene::Scene& s, const Guid& p, net::NetworkId)
                    { return foundation::scene::PrefabSpawnSystem::SpawnInto(s, db, nullptr, p); });
            }
            hostCtx = manager.CreateContext();
            aCtx = manager.CreateContext();
            bCtx = manager.CreateContext();
            net::InstallNetScriptService(*hostCtx, hostController.binding);
            net::InstallNetScriptService(*aCtx, aController.binding);
            net::InstallNetScriptService(*bCtx, bController.binding);
        }

        void Open()
        {
            host.StartServer(/*dedicated=*/false);
            hostController.endpoint = &host;
            net::InstallScriptRpcHandlers(hostController.binding, host);
            for (auto [endpoint, controller] : {std::pair{&a, &aController}, std::pair{&b, &bController}})
            {
                (void)endpoint->ConnectTo(hostSocket->LocalEndpoint());
                controller->endpoint = endpoint;
                net::InstallScriptRpcHandlers(controller->binding, *endpoint);
            }
            Pump(60);
            REQUIRE(host.Session().PeerCount() == 2u);
        }

        void Pump(int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                network.Advance(10.0f);
                host.Update(10.0f);
                a.Update(10.0f);
                b.Update(10.0f);
            }
        }
    };

    // Each backend's script for the host and for a client, the same game in its own words.
    struct PlayerScripts
    {
        StringView host;   // Joins, LastJoin, Left
        StringView client; // Me, Told, From
    };

    void PlayTogether(RefPtr<IScriptManager> manager, const PlayerScripts& scripts)
    {
        RegisterReflectedTypes(*manager);
        Players p(*manager);
        REQUIRE(p.hostCtx->Load(scripts.host, u8"host").IsOk());
        REQUIRE(p.aCtx->Load(scripts.client, u8"a").IsOk());
        REQUIRE(p.bCtx->Load(scripts.client, u8"b").IsOk());
        p.Open();

        // Joins: the host hears both clients; each client learns its own id.
        CHECK(p.hostCtx->GetGlobal(u8"Joins").Get<f64>() == doctest::Approx(2.0));
        const f64 aId = p.aCtx->GetGlobal(u8"Me").Get<f64>();
        const f64 bId = p.bCtx->GetGlobal(u8"Me").Get<f64>();
        CHECK(aId == doctest::Approx(static_cast<f64>(p.a.Session().LocalPeer())));
        CHECK(bId == doctest::Approx(static_cast<f64>(p.b.Session().LocalPeer())));
        CHECK(aId != bId);
        {
            ScriptCallScope scope(p.hostCtx.Get());
            CHECK(net::Net::localPlayer() == 0);
            // A number to client a alone, from the host (player 0).
            net::Net::rpcNumberTo(static_cast<i32>(aId), String(u8"secret"), 7.0);
        }
        p.Pump(30);
        CHECK(p.aCtx->GetGlobal(u8"Told").Get<f64>() == doctest::Approx(7.0));
        CHECK(p.aCtx->GetGlobal(u8"From").Get<f64>() == doctest::Approx(0.0));
        CHECK(p.bCtx->GetGlobal(u8"Told").Get<f64>() == doctest::Approx(0.0)); // not b's

        // A Guest for client a: a's, on every peer; neither the host's nor b's.
        foundation::script::Entity onHost;
        {
            ScriptCallScope scope(p.hostCtx.Get());
            onHost = net::Net::spawnFor(static_cast<i32>(aId), p.prefab.guest, 1, 0, 0);
            REQUIRE(onHost.Live());
            CHECK_FALSE(net::Net::isMine(onHost));
        }
        p.Pump(40);
        auto guestIn = [](foundation::scene::Scene& scene)
        {
            foundation::script::Entity e;
            e.scene = &scene;
            const foundation::scene::EntityHandle h = scene.FindEntityByName(u8"Guest");
            e.entityIndex = h.index;
            e.entityGeneration = h.generation;
            return e;
        };
        const foundation::script::Entity onA = guestIn(p.aScene);
        const foundation::script::Entity onB = guestIn(p.bScene);
        REQUIRE(onA.Live());
        REQUIRE(onB.Live());
        {
            ScriptCallScope scope(p.aCtx.Get());
            CHECK(net::Net::isMine(onA));
        }
        {
            ScriptCallScope scope(p.bCtx.Get());
            CHECK_FALSE(net::Net::isMine(onB));
        }
        CHECK(p.bScene.GetSystem<net::NetworkComponentManager>()->Get(onB.Handle())->owner ==
              static_cast<u32>(aId)); // b sees whose it is

        // Client b leaves: the host hears its id.
        p.b.Session().Disconnect(p.b.Session().ServerPeer());
        p.Pump(60);
        CHECK(p.hostCtx->GetGlobal(u8"Left").Get<f64>() == doctest::Approx(bId));
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("net-facade: AngelScript players join, are told things alone, own what is spawned for them, leave")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    net::RegisterReplicationComponents();
    net::RegisterNetScriptFacade();
    PlayTogether(angelscript::CreateScriptManager(DefaultAllocator()),
                 PlayerScripts{u8"double Joins = 0; double LastJoin = -1; double Left = -1;\n"
                               u8"void joined(int p) { Joins++; LastJoin = p; }\n"
                               u8"void left(int p) { Left = p; }\n"
                               u8"void main() { NetHandler @j = joined; Net::onJoin(j);\n"
                               u8"  NetHandler @l = left; Net::onLeave(l); }\n",
                               u8"double Me = -1; double Told = 0; double From = -1;\n"
                               u8"void joined(int p) { Me = p; }\n"
                               u8"void secret(int sender, double value) { Told = value; From = sender; }\n"
                               u8"void main() { NetHandler @j = joined; Net::onJoin(j);\n"
                               u8"  NetNumberHandler @s = secret; Net::on(\"secret\", s); }\n"});
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("net-facade: Luau players join, are told things alone, own what is spawned for them, leave")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection();
    net::RegisterReplicationComponents();
    net::RegisterNetScriptFacade();
    PlayTogether(CreateLuauScriptManager(DefaultAllocator()),
                 PlayerScripts{u8"Joins = 0; LastJoin = -1; Left = -1\n"
                               u8"Net.onJoin(function(p) Joins = Joins + 1; LastJoin = p end)\n"
                               u8"Net.onLeave(function(p) Left = p end)\n",
                               u8"Me = -1; Told = 0; From = -1\n"
                               u8"Net.onJoin(function(p) Me = p end)\n"
                               u8"Net.on('secret', function(sender, value) Told = value; From = sender end)\n"});
}
#endif // OPTION_HAS_LUAU

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("net-facade: AngelScript Net.spawn puts a prefab in every peer's scene, from the host only")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection(); // Entity, which Net.spawn returns
    net::RegisterReplicationComponents(); // the Networked Transform the prefab carries
    net::RegisterNetScriptFacade();
    SpawnOverTheNetwork(angelscript::CreateScriptManager(DefaultAllocator()),
                        [](StringView id)
                        {
                            return Format(u8"bool Spawned = false;\n"
                                          u8"void main() {{ Spawned = Net::spawn(Guid(\"{}\"), 1, 2, 3).isValid(); }}\n",
                                          id);
                        },
                        [](StringView id)
                        {
                            return Format(u8"bool Spawned = true;\n"
                                          u8"void main() {{ Spawned = Net::spawn(Guid(\"{}\"), 4, 5, 6).isValid(); }}\n",
                                          id);
                        },
                        u8"spawn");
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("net-facade: Luau Net.spawn puts a prefab in every peer's scene, from the host only")
{
    RegisterCoreTypes();
    RegisterScriptFacadeReflection(); // Entity, which Net.spawn returns
    net::RegisterReplicationComponents(); // the Networked Transform the prefab carries
    net::RegisterNetScriptFacade();
    SpawnOverTheNetwork(CreateLuauScriptManager(DefaultAllocator()),
                        [](StringView id)
                        { return Format(u8"Spawned = Net.spawn(Guid.new(\"{}\"), 1, 2, 3):isValid()\n", id); },
                        [](StringView id)
                        { return Format(u8"Spawned = Net.spawn(Guid.new(\"{}\"), 4, 5, 6):isValid()\n", id); },
                        u8"spawn");
}
#endif // OPTION_HAS_LUAU

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("net-facade: AngelScript Net.on receives a number, a text and a bare RPC, and replies")
{
    RegisterCoreTypes();
    net::RegisterNetScriptFacade();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> hostCtx = manager->CreateContext();
    RefPtr<IScriptContext> clientCtx = manager->CreateContext();
    HostAndClient fx;
    net::InstallNetScriptService(*hostCtx, fx.hostController.binding);
    net::InstallNetScriptService(*clientCtx, fx.clientController.binding);

    // The host registers before it hosts; its number handler answers the sender at once.
    REQUIRE(hostCtx
                ->Load(u8"double Score = 0; int ScoreFrom = -1; string Greeting; int Pings = 0;\n"
                       u8"void onScore(int sender, double value) { Score = value; ScoreFrom = sender;\n"
                       u8"  Net::rpcNumber(\"ack\", value * 2); }\n"
                       u8"void onHello(int sender, const string &in text) { Greeting = text; }\n"
                       u8"void onPing(int sender) { Pings++; }\n"
                       u8"void main() {\n"
                       u8"  NetNumberHandler @score = onScore; Net::on(\"score\", score);\n"
                       u8"  NetTextHandler @hello = onHello; Net::on(\"hello\", hello);\n"
                       u8"  NetHandler @ping = onPing; Net::on(\"ping\", ping);\n"
                       u8"}\n",
                       u8"host")
                .IsOk());
    REQUIRE(fx.hostController.binding.rpcHandlers.Size() == 3);
    fx.Open();
    REQUIRE(clientCtx
                ->Load(u8"double Ack = 0;\n"
                       u8"void onAck(int sender, double value) { Ack = value; }\n"
                       u8"void main() {\n"
                       u8"  NetNumberHandler @ack = onAck; Net::on(\"ack\", ack);\n"
                       u8"  Net::rpcNumber(\"score\", 42.5);\n"
                       u8"  Net::rpcText(\"hello\", \"masked\");\n"
                       u8"  Net::rpc(\"ping\"); Net::rpc(\"ping\");\n"
                       u8"}\n",
                       u8"client")
                .IsOk());
    fx.Pump(60);

    CHECK(hostCtx->GetGlobal(u8"Score").Get<f64>() == doctest::Approx(42.5));
    CHECK(hostCtx->GetGlobal(u8"ScoreFrom").Get<f64>() == doctest::Approx(fx.ClientPeerOnHost()));
    CHECK(hostCtx->GetGlobal(u8"Greeting").Get<String>() == StringView(u8"masked"));
    CHECK(hostCtx->GetGlobal(u8"Pings").Get<f64>() == doctest::Approx(2.0));
    CHECK(clientCtx->GetGlobal(u8"Ack").Get<f64>() == doctest::Approx(85.0)); // the reply, from inside a handler
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("net-facade: Luau Net.on receives a number, a text and a bare RPC, and replies")
{
    RegisterCoreTypes();
    net::RegisterNetScriptFacade();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> hostCtx = manager->CreateContext();
    RefPtr<IScriptContext> clientCtx = manager->CreateContext();
    HostAndClient fx;
    net::InstallNetScriptService(*hostCtx, fx.hostController.binding);
    net::InstallNetScriptService(*clientCtx, fx.clientController.binding);

    REQUIRE(hostCtx
                ->Load(u8"Score = 0; ScoreFrom = -1; Greeting = ''; Pings = 0\n"
                       u8"Net.on('score', function(sender, value) Score = value; ScoreFrom = sender\n"
                       u8"  Net.rpcNumber('ack', value * 2) end)\n"
                       u8"Net.on('hello', function(sender, text) Greeting = text end)\n"
                       u8"Net.on('ping', function(sender) Pings = Pings + 1 end)\n",
                       u8"host")
                .IsOk());
    fx.Open();
    REQUIRE(clientCtx
                ->Load(u8"Ack = 0\n"
                       u8"Net.on('ack', function(sender, value) Ack = value end)\n"
                       u8"Net.rpcNumber('score', 42.5)\n"
                       u8"Net.rpcText('hello', 'masked')\n"
                       u8"Net.rpc('ping'); Net.rpc('ping')\n",
                       u8"client")
                .IsOk());
    fx.Pump(60);

    CHECK(hostCtx->GetGlobal(u8"Score").Get<f64>() == doctest::Approx(42.5));
    CHECK(hostCtx->GetGlobal(u8"ScoreFrom").Get<f64>() == doctest::Approx(fx.ClientPeerOnHost()));
    CHECK(hostCtx->GetGlobal(u8"Greeting").Get<String>() == StringView(u8"masked"));
    CHECK(hostCtx->GetGlobal(u8"Pings").Get<f64>() == doctest::Approx(2.0));
    CHECK(clientCtx->GetGlobal(u8"Ack").Get<f64>() == doctest::Approx(85.0));
}
#endif // OPTION_HAS_LUAU

TEST_CASE("net-facade: a truncated script RPC never reaches the handler")
{
    // A raw RPC carrying the number kind and no number: the handler is not called.
    HostAndClient fx;
    struct Counting final : IScriptDelegate
    {
        int calls = 0;
        Result<Variant> Invoke(Span<Variant>) override
        {
            ++calls;
            return Variant{};
        }
    };
    RefPtr<Counting> counting = MakeRef<Counting>(DefaultAllocator());
    fx.Open();
    net::Net::InstallScriptRpcHandler(fx.host, u8"score", RefPtr<IScriptDelegate>(counting.Get()));
    fx.client.Rpc().Call(fx.client.Session(), fx.client.Session().ServerPeer(), u8"score",
                         [](net::BitWriter& w) { w.WriteU8(static_cast<u8>(net::ScriptRpcKind::Number)); });
    fx.Pump(40);
    CHECK(counting->calls == 0);
    fx.client.Rpc().Call(fx.client.Session(), fx.client.Session().ServerPeer(), u8"score",
                         [](net::BitWriter& w) { w.WriteU8(static_cast<u8>(net::ScriptRpcKind::None)); });
    fx.Pump(40);
    CHECK(counting->calls == 1);
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("net-facade: AngelScript reads the live session through the Net facade")
{
    RegisterCoreTypes();
    net::RegisterNetScriptFacade();

    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    ServerFixture fx;
    net::InstallNetScriptService(*ctx, fx.binding);

    // AngelScript: statics live in the type's namespace (Net::isServer()); logic in main().
    const Status status = ctx->Load(u8"bool IsServer;\n"
                                    u8"bool IsClient;\n"
                                    u8"int Peers;\n"
                                    u8"bool CanHost;\n"
                                    u8"void main() {\n"
                                    u8"  IsServer = Net::isServer();\n"
                                    u8"  IsClient = Net::isClient();\n"
                                    u8"  Peers = Net::peerCount();\n"
                                    u8"  CanHost = Net::canHost();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"IsServer").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"IsClient").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Peers").Get<f64>() ==
          0.0); // the AS backend unifies integer globals to f64
    CHECK(ctx->GetGlobal(u8"CanHost").Get<bool>() == net::NetworkManager::CanHost()); // the build's, not the session's
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("net-facade: Luau reads the live session through the Net facade")
{
    RegisterCoreTypes();
    net::RegisterNetScriptFacade();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    ServerFixture fx;
    net::InstallNetScriptService(*ctx, fx.binding);

    // Luau: statics via `.`; the chunk runs at top level, so the globals it assigns are readable.
    const Status status = ctx->Load(u8"IsServer = Net.isServer()\n"
                                    u8"IsClient = Net.isClient()\n"
                                    u8"Peers = Net.peerCount()\n"
                                    u8"CanHost = Net.canHost()\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"IsServer").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"IsClient").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Peers").Get<f64>() == 0.0);
    CHECK(ctx->GetGlobal(u8"CanHost").Get<bool>() == net::NetworkManager::CanHost());
}
#endif // OPTION_HAS_LUAU

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.gameinstance - the extracted run bracket owns the script run state + time scale.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import engine.gameinstance;
import foundation.runtime;      // foundation runtime Context/Subsystem (was transitively reachable pre-namespace-split)
import foundation.scene;
import foundation.scene.resource;    // SceneDocument + LoadScene round-trip
import foundation.content;           // ContentDatabase / Instance
import foundation.resource;          // ResourceManager
import foundation.vfs;               // NativeFileSystem
import foundation.script;
import foundation.script.facades; // RegisterScriptFacadeReflection (the Scene facade)
import foundation.script.resource; // ScriptClass (the cooked Game class a run starts from)
import foundation.script.angelscript;
import foundation.script.luau;
import foundation.net;         // NetSession queries (IsServer/PeerCount)
import foundation.net.manager; // NetworkManager (the endpoint the instance owns)
import foundation.net.replication; // RegisterReplicationComponents (net scene managers)
import engine.net;                 // NetworkSceneSystem (the endpoint <-> scene wiring under test)
import foundation.input;       // ActionRuntime / IInputSourceProvider (per-instance input)
import foundation.shell;       // IKeyboard / KeyCode (a minimal fake device)

using namespace foundation::core;
namespace script = foundation::script; // raw manager/context for the run facade battery
namespace scene = foundation::scene;
namespace messaging = foundation::messaging; // EventBus
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace input = foundation::input;
namespace shell = foundation::shell;
using foundation::vfs::NativeFileSystem;

namespace
{
    // A minimal input source: one keyboard reporting a single held key, everything else absent.
    class OneKeyKeyboard final : public shell::IKeyboard
    {
    public:
        shell::KeyCode key{};
        bool down = false;
        [[nodiscard]] bool IsKeyDown(shell::KeyCode k) const override { return down && k == key; }
        [[nodiscard]] bool IsKeyPressed(shell::KeyCode k) const override
        {
            return down && k == key;
        }
        [[nodiscard]] bool IsKeyReleased(shell::KeyCode) const override { return false; }
        [[nodiscard]] shell::KeyModifiers Modifiers() const override
        {
            return shell::KeyModifiers::None;
        }
    };
    class OneKeySource final : public input::IInputSourceProvider
    {
    public:
        OneKeyKeyboard keyboard;
        [[nodiscard]] shell::IKeyboard* Keyboard() override { return &keyboard; }
        [[nodiscard]] shell::IMouse* Mouse() override { return nullptr; }
        [[nodiscard]] i32 GamepadCount() const override { return 0; }
        [[nodiscard]] shell::IGamepad* Gamepad(i32) override { return nullptr; }
        [[nodiscard]] shell::ITouch* Touch() override { return nullptr; }
    };
    [[nodiscard]] input::InputMap MakeFireMap(shell::KeyCode key)
    {
        input::InputMap map;
        input::ActionSet set;
        set.name = String(u8"S");
        input::Action a;
        a.name = String(u8"fire");
        a.kind = input::ActionKind::Button;
        input::Binding b;
        b.source = input::BindingSource::Key;
        b.code = static_cast<u32>(key);
        a.bindings.PushBack(b);
        set.actions.PushBack(static_cast<input::Action&&>(a));
        map.sets.PushBack(static_cast<input::ActionSet&&>(set));
        return map;
    }
}

TEST_CASE("script.facades: reserved contract-class names (Game/Level) are refused as facades")
{
    // A user's own class MUST take these names (the game orchestrator is `Game`, the scene
    // tier is `Level`), so a facade sharing one would clash. Registration must be refused.
    script::RegisterExtraFacadeName(u8"Game");
    script::RegisterExtraFacadeName(u8"Level");
    // A non-reserved name still registers (idempotently) - the control.
    script::RegisterExtraFacadeName(u8"run");

    bool sawGame = false;
    bool sawLevel = false;
    bool sawRun = false;
    for (StringView facade : script::ExtraFacadeNames())
    {
        sawGame = sawGame || facade == StringView(u8"Game");
        sawLevel = sawLevel || facade == StringView(u8"Level");
        sawRun = sawRun || facade == StringView(u8"run");
    }
    CHECK_FALSE(sawGame);  // refused
    CHECK_FALSE(sawLevel); // refused
    CHECK(sawRun);         // registered
}

TEST_CASE("game-instance: instance time scale defaults to 1 and is settable; fresh instance idle")
{
    engine::runtime::GameInstance gi;
    CHECK(gi.InstanceTimeScale() == doctest::Approx(1.0f));
    gi.SetInstanceTimeScale(0.5f);
    CHECK(gi.InstanceTimeScale() == doctest::Approx(0.5f));
    CHECK_FALSE(gi.ScriptRunning());
    CHECK(gi.GetScene() == nullptr);
    CHECK(gi.ScriptContext() == nullptr);
    CHECK_FALSE(
        gi.RunHost().IsActive()); // the instance owns its run host (idle until a run starts)

    // The instance owns a usable scene group (its SceneManager).
    CHECK(gi.Scenes().SceneCount() == 0u);
    scene::Scene* level = gi.Scenes().CreateScene(u8"L1");
    REQUIRE(level != nullptr);
    CHECK(gi.Scenes().SceneCount() == 1u);
    CHECK(gi.Scenes().CurrentScene() == level);
}

TEST_CASE("game-instance: an instance is its scenes' and scripts' run")
{
    // Run-scoped systems (audio) group a run's scenes and its script calls by one key: the
    // instance. Two instances are two runs.
    engine::runtime::GameInstance a;
    engine::runtime::GameInstance b;
    scene::Scene* level = a.Scenes().CreateScene(u8"L1");
    REQUIRE(level != nullptr);
    CHECK(level->Run() == &a);
    CHECK(a.RunHost().Binding().run == &a);
    CHECK(b.RunHost().Binding().run == &b);

    // A scene adopted into the run joins it.
    scene::SceneManager elsewhere{DefaultAllocator()};
    scene::Scene* adopted = elsewhere.CreateScene(u8"page");
    CHECK(adopted->Run() == nullptr);
    b.SetScene(adopted);
    CHECK(adopted->Run() == &b);
    b.SetScene(nullptr);
}

TEST_CASE("game-instance: each instance's input runtime reads ONLY its own source (per-instance "
          "isolation)")
{
    // The multi-instance-PIE fix: each GameInstance has its OWN ActionRuntime bound to its OWN source,
    // so one tab's keys never reach another tab's game (the shared-runtime bug that flipped the server
    // tab into a client). Same map, same key, two sources - only the source with the key held fires.
    engine::runtime::GameInstance a;
    engine::runtime::GameInstance b;
    OneKeySource srcA;
    srcA.keyboard.key = shell::KeyCode::H;
    srcA.keyboard.down = true; // A holds H
    OneKeySource srcB;
    srcB.keyboard.key = shell::KeyCode::H;
    srcB.keyboard.down = false; // B does not
    a.SetInputSource(&srcA);
    a.SetInputMap(MakeFireMap(shell::KeyCode::H));
    b.SetInputSource(&srcB);
    b.SetInputMap(MakeFireMap(shell::KeyCode::H));

    a.DriveInput(0.016f, 1.0f);
    b.DriveInput(0.016f, 1.0f);

    CHECK(a.InputRuntime().IsDown(a.InputRuntime().Resolve(u8"fire")) == true); // A's source has it
    CHECK(b.InputRuntime().IsDown(b.InputRuntime().Resolve(u8"fire")) ==
          false); // B's does NOT (no cross-feed)

    // Flip which source holds the key: isolation holds the other way too.
    srcA.keyboard.down = false;
    srcB.keyboard.down = true;
    a.DriveInput(0.016f, 1.0f);
    b.DriveInput(0.016f, 1.0f);
    CHECK(a.InputRuntime().IsDown(a.InputRuntime().Resolve(u8"fire")) == false);
    CHECK(b.InputRuntime().IsDown(b.InputRuntime().Resolve(u8"fire")) == true);
}

TEST_CASE("game-instance: each instance owns an independent networked endpoint (server + client "
          "over UDP)")
{
    // The per-instance networking model: a GameInstance COMPOSES a NetworkController (the
    // INetworkController), opening its OWN real UDP endpoint on StartServer/Connect. The GameInstance
    // forwards to it, so this end-to-end path is unchanged. Two instances = two isolated endpoints.
    engine::runtime::GameInstance server;
    engine::runtime::GameInstance client;
    CHECK(server.NetEndpoint() == nullptr); // offline until a role is entered

    REQUIRE(server.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(server.NetEndpoint() != nullptr);
    CHECK(server.NetEndpoint()->Session().IsServer());
    const u16 port = server.NetEndpoint()->BoundPort();
    CHECK(port != 0u);

    REQUIRE(client.Connect(u8"127.0.0.1", port));
    REQUIRE(client.NetEndpoint() != nullptr);
    CHECK(client.NetEndpoint()->Session().IsClient());
    CHECK(server.NetEndpoint() != client.NetEndpoint()); // independent endpoints

    for (int i = 0; i < 400 && server.NetEndpoint()->Session().PeerCount() == 0u; ++i)
    {
        // Transport pump is NetworkManager::UpdateTransport.
        server.NetEndpoint()->UpdateTransport(16.0f);
        client.NetEndpoint()->UpdateTransport(16.0f);
        SleepMilliseconds(1);
    }
    CHECK(server.NetEndpoint()->Session().PeerCount() == 1u);

    client.StopNetworking(); // disconnect drops the endpoint
    CHECK(client.NetEndpoint() == nullptr);
    CHECK(server.NetEndpoint() != nullptr); // the server is unaffected (isolation)
}

TEST_CASE("network-controller: role lifecycle - start, stop, reconnect, and the online hook")
{
    // NetworkController, tested directly: it owns the
    // endpoint + INetworkController role. The app-injected spawn resolver is applied to EVERY endpoint
    // the controller opens (reconnect too - not consumed), asserted via Replication().HasSpawnHandler().
    // The final destruct with a LIVE endpoint exercises the socket + session teardown path (ASAN).
    engine::runtime::NetworkController controller;
    controller.SetSpawnResolverFactory(
        []() -> foundation::net::StateReplication::SpawnHandler
        {
            return [](foundation::scene::Scene&, const Guid&,
                      foundation::net::NetworkId) -> foundation::scene::EntityHandle
            { return foundation::scene::EntityHandle::Invalid(); };
        });

    CHECK(controller.NetEndpoint() == nullptr); // offline until a role is entered

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr);
    CHECK(controller.NetEndpoint()->Session().IsServer());
    CHECK(controller.NetEndpoint()->BoundPort() != 0u);
    CHECK(controller.NetEndpoint()->Replication().HasSpawnHandler()); // resolver applied to the endpoint

    controller.StopNetworking();
    CHECK(controller.NetEndpoint() == nullptr);     // the endpoint drops

    // Reconnect: a fresh endpoint, and the resolver is applied AGAIN (the controller owns it, not consumed).
    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr);
    CHECK(controller.NetEndpoint()->Replication().HasSpawnHandler());
    // controller destructs here holding a live endpoint - the socket + session teardown path.
}

TEST_CASE("network-controller: a fresh endpoint replicates the cached scene")
{
    // SetReplicatedScene cached BEFORE going online is applied to the endpoint at StartServer (a server
    // assigns NetworkIds from the scene), and a live change is forwarded to the running endpoint too.
    engine::runtime::NetworkController controller;
    scene::SceneManager scenes{DefaultAllocator()};
    scene::Scene* level = scenes.CreateScene(u8"Level");
    REQUIRE(level != nullptr);
    controller.SetReplicatedScene(level); // cached while offline

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(controller.NetEndpoint() != nullptr); // came up; the cached scene was applied to it

    controller.SetReplicatedScene(nullptr); // live change forwarded to the running endpoint (clean)
    controller.StopNetworking();
    CHECK(controller.NetEndpoint() == nullptr);
}

TEST_CASE("network-controller: the replicated scene's NetworkSceneSystem drives the endpoint; "
          "stop detaches; reconnect re-wires (P2)")
{
    // The endpoint is wired into ONLY the replicated
    // scene's NetworkSceneSystem, at the controller's edges. endpoint-dies-before-scene: StopNetworking
    // detaches the system before the endpoint dies. stop-start-reconnect against a LIVE scene re-wires.
    // (ASAN covers the teardown - the controller + scene destruct here holding a live endpoint.)
    foundation::net::RegisterReplicationComponents();
    engine::runtime::NetworkController controller;
    scene::Scene s{DefaultAllocator()};
    engine::net::AddNetworkSceneManagers(s); // installs the NetworkSceneSystem
    engine::net::NetworkSceneSystem* sys = s.GetSystem<engine::net::NetworkSceneSystem>();
    REQUIRE(sys != nullptr);

    controller.SetReplicatedScene(&s);
    CHECK(sys->Endpoint() == nullptr); // attached while offline -> inert (no endpoint yet)

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true));
    CHECK(sys->Endpoint() == controller.NetEndpoint()); // the scene's fixed lane now drives the endpoint

    controller.StopNetworking();
    CHECK(sys->Endpoint() == nullptr); // detached BEFORE the endpoint died (endpoint-dies-before-scene)
    CHECK(controller.NetEndpoint() == nullptr);

    REQUIRE(controller.StartServer(/*port=*/0, /*dedicated=*/true)); // reconnect on the live scene
    CHECK(sys->Endpoint() == controller.NetEndpoint()); // re-wired to the fresh endpoint
}

TEST_CASE("game-instance: destroying the replicated scene clears the endpoint's scene (no dangling)")
{
    // Scene-dies-before-endpoint. DestroyScene of the CURRENT
    // scene runs SetScene(nullptr) first, so the endpoint's replicated-scene pointer + the controller's
    // cache clear BEFORE the scene is freed. A later transport pump / reconnect never touches dead memory
    // (ASAN is the real assertion here).
    engine::runtime::GameInstance gi;
    scene::Scene* level = gi.CreateScene(u8"Level");
    REQUIRE(level != nullptr);
    gi.SetScene(level);
    REQUIRE(gi.StartServer(/*port=*/0, /*dedicated=*/true));
    REQUIRE(gi.NetEndpoint() != nullptr);

    gi.DestroyScene(level); // level == current -> SetScene(nullptr) clears the endpoint's scene + cache
    CHECK(gi.GetScene() == nullptr);     // controller cache cleared
    CHECK(gi.NetEndpoint() != nullptr);  // still online (the endpoint outlives the scene)

    gi.NetEndpoint()->UpdateTransport(16.0f); // transport pump must not touch the freed scene
    REQUIRE(gi.StartServer(/*port=*/0, /*dedicated=*/true));  // reconnect with no current scene is safe
    gi.StopNetworking();
}

TEST_CASE("game-instance: fallback path starts, ticks, and stops a Game script")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"class Game {\n"
                                   u8"  Game() {}\n"
                                   u8"  void launch() {}\n"
                                   u8"  void update(double dt) {}\n"
                                   u8"  void exit() {}\n"
                                   u8"}\n",
                                   u8"game.as");
    REQUIRE(ok);
    CHECK(gi.ScriptRunning());
    CHECK(gi.ScriptContext() != nullptr);
    CHECK(gi.RunHost().IsActive()); // the game script runs on the instance's own run host

    gi.DriveRunHost(0.016f);     // advance the run host clock/GC (must not fault)
    gi.TickScript(0.016f, 1.0f); // must not fault
    CHECK(gi.ScriptRunning());

    gi.StopScript();
    CHECK_FALSE(gi.ScriptRunning());
}

TEST_CASE("game-instance: starts, ticks, and stops a LUAU Game orchestrator (the Game tier)")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:launch() end\n"
                                   u8"function Game:update(dt) end\n"
                                   u8"function Game:exit() end\n",
                                   u8"game.luau"); // the .luau extension resolves the Luau backend
    REQUIRE(ok);
    CHECK(gi.ScriptRunning());
    CHECK(gi.ScriptContext() != nullptr);
    CHECK(gi.RunHost().IsActive());

    gi.DriveRunHost(0.016f);     // advance the run host clock/GC (must not fault)
    gi.TickScript(0.016f, 1.0f); // update() (must not fault)
    CHECK(gi.ScriptRunning());

    gi.StopScript();
    CHECK_FALSE(gi.ScriptRunning());
}

// task #123 boot reorder: the game script now launches BEFORE any scene, so a `class Game`
// orchestrator's launch()/update() may call the Scene + run facades with NO current scene.
// Two properties under test: (1) the facades are NULL-SCENE-SAFE (no deref of a null currentScene -
// the run must launch + tick without faulting); (2) the load facade binds LOWERCASE as `run`, NOT Game -
// a facade named Game is a hard AngelScript name conflict with the mandatory `Game` orchestrator
// class, so THIS test compiling at all is the regression guard for that.
TEST_CASE("game-instance: Scene/run facades are null-scene-safe from a pre-scene "
          "orchestrator")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // run facade (owned by this project)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    CHECK_FALSE(gi.SceneReady()); // no scene yet - the orchestrator-first condition
    const bool ok = gi.StartScript(
        u8"class Game {\n"
        u8"  Game() {}\n"
        u8"  void launch() {\n"
        u8"    run::currentScene().find(\"nobody\");\n"
        u8"    run::currentScene().findByPath(\"a/b\");\n"
        u8"    run::sceneReady();\n"
        u8"    run::loadComplete(0);\n"
        u8"    run::loadProgress(0);\n"
        u8"  }\n"
        u8"  void update(double dt) {\n"
        u8"    run::currentScene().find(\"x\");\n"
        u8"    run::sceneReady();\n"
        u8"  }\n"
        u8"  void exit() {}\n"
        u8"}\n",
        u8"game.as");
    REQUIRE(ok); // compiled (no Game-name clash) + launch() ran the pre-scene facade calls, no fault
    CHECK(gi.ScriptRunning());
    gi.DriveRunHost(0.016f);
    gi.TickScript(0.016f, 1.0f); // update() calls them again - still no fault
    CHECK(gi.ScriptRunning());
    gi.StopScript();
}

TEST_CASE("game-instance: Scene/run facades are null-scene-safe from a pre-scene "
          "orchestrator (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // run facade (owned by this project)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    // A facade named `Game` would fail HERE with "Name conflict. 'Game' is an extended data type" -
    // binding the load facade lowercase as `run` is exactly what lets this `class Game` compile alongside it.
    const bool ok = gi.StartScript(
        u8"class Game {\n"
        u8"  Game() {}\n"
        u8"  void launch() {\n"
        u8"    run::currentScene().find(\"nobody\");\n"
        u8"    run::currentScene().findByPath(\"a/b\");\n"
        u8"    run::sceneReady();\n"
        u8"    run::loadComplete(0);\n"
        u8"    run::loadProgress(0);\n"
        u8"  }\n"
        u8"  void update(double dt) { run::currentScene().find(\"x\"); "
        u8"run::sceneReady(); }\n"
        u8"  void exit() {}\n"
        u8"}\n",
        u8"game.as");
    REQUIRE(ok);
    CHECK(gi.ScriptRunning());
    gi.DriveRunHost(0.016f);
    gi.TickScript(0.016f, 1.0f);
    CHECK(gi.ScriptRunning());
    gi.StopScript();
}

// The run.* load facade routing PROVEN end-to-end on both backends (raw context, Net-style): a
// fake RunScriptBinding (standing in for the app's content-DB-backed load pointers) is
// installed as the run.runtime service, then a script kicks an async load and polls the
// ticket to completion. What is under test is the facade->binding routing + the ticket round-trip.
namespace
{
    // Fake load host: hands back a fixed ticket, reports complete on the 2nd poll of THAT ticket.
    struct RunFake
    {
        engine::runtime::RunScriptBinding binding;
        int asyncCalls = 0;
        Guid requested;
        int completePolls = 0;
        i32 ticketSeen = -1;
        static constexpr i32 kTicket = 7;

        RunFake()
        {
            binding.loadSceneAsync = Function<i32(const Guid&)>{[this](const Guid& id) -> i32
                                                               {
                                                                   ++asyncCalls;
                                                                   requested = id;
                                                                   return kTicket;
                                                               }};
            binding.loadProgress = Function<f64(i32)>{
                [](i32 t) -> f64 { return t == kTicket ? 0.5 : -1.0; }};
            binding.loadComplete = Function<bool(i32)>{[this](i32 t) -> bool
                                                       {
                                                           ++completePolls;
                                                           ticketSeen = t;
                                                           return completePolls >= 2;
                                                       }};
        }
    };
}

TEST_CASE("game-instance: run.loadSceneAsync -> ticket, polled to completion (Luau)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::CreateLuauScriptManager(DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    RunFake fake;
    engine::runtime::InstallRunScriptService(*ctx, fake.binding);

    // Kick the load, then poll to completion; record the observable results in module globals.
    const Status status = ctx->Load(u8"t = run.loadSceneAsync(Guid.new(0xABC, 0xDEF))\n"
                                    u8"Poll1 = run.loadComplete(t)\n"
                                    u8"Prog = run.loadProgress(t)\n"
                                    u8"Poll2 = run.loadComplete(t)\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == RunFake::kTicket); // the exact ticket round-tripped
    CHECK(ctx->GetGlobal(u8"Poll1").Get<bool>() == false); // first poll: not complete
    CHECK(ctx->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(ctx->GetGlobal(u8"Poll2").Get<bool>() == true); // second poll: complete
}

TEST_CASE("game-instance: run.loadSceneAsync -> ticket, polled to completion (AngelScript)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    RunFake fake;
    engine::runtime::InstallRunScriptService(*ctx, fake.binding);

    const Status status = ctx->Load(u8"int t;\n"
                                    u8"bool Poll1; double Prog; bool Poll2;\n"
                                    u8"void main() {\n"
                                    u8"  t = run::loadSceneAsync(Guid(0xABC, 0xDEF));\n"
                                    u8"  Poll1 = run::loadComplete(t);\n"
                                    u8"  Prog = run::loadProgress(t);\n"
                                    u8"  Poll2 = run::loadComplete(t);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == RunFake::kTicket);
    CHECK(ctx->GetGlobal(u8"Poll1").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(ctx->GetGlobal(u8"Poll2").Get<bool>() == true);
}

// run.requestExit routing PROVEN on both backends: a fake RunScriptBinding whose requestExit records
// the code stands in for the app's host route (standalone stops the loop; the editor stops the Game
// tab). A tiny script calls run::requestExit(2) / run.requestExit(2), and the arity-0 overload routes
// code 0. What is under test is the facade->binding routing + the arity family, NOT the host itself.
namespace
{
    struct ExitFake
    {
        engine::runtime::RunScriptBinding binding;
        int calls = 0;
        i32 lastCode = -999;

        ExitFake()
        {
            binding.requestExit = Function<void(i32)>{[this](i32 code)
                                                      {
                                                          ++calls;
                                                          lastCode = code;
                                                      }};
        }
    };
}

TEST_CASE("game-instance: run.requestExit routes the exit code to the binding (AngelScript)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    ExitFake fake;
    engine::runtime::InstallRunScriptService(*ctx, fake.binding);

    // The 1-arg overload routes the code; the 0-arg overload routes code 0.
    const Status status = ctx->Load(u8"void main() {\n"
                                    u8"  run::requestExit(2);\n"
                                    u8"  run::requestExit();\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(fake.calls == 2);
    CHECK(fake.lastCode == 0); // the trailing no-arg call routed code 0
}

TEST_CASE("game-instance: run.requestExit routes the exit code to the binding (Luau)")
{
    RegisterCoreTypes();
    engine::runtime::RegisterRunScriptFacade();

    RefPtr<script::IScriptManager> manager = foundation::script::CreateLuauScriptManager(DefaultAllocator());
    foundation::script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    ExitFake fake;
    engine::runtime::InstallRunScriptService(*ctx, fake.binding);

    const Status status = ctx->Load(u8"run.requestExit(2)\n"
                                    u8"Seen = 1\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(fake.calls == 1);
    CHECK(fake.lastCode == 2); // the exact code round-tripped through the facade

    // The arity-0 overload routes code 0.
    const Status status0 = ctx->Load(u8"run.requestExit()\n", u8"main0");
    REQUIRE(status0.IsOk());
    CHECK(fake.calls == 2);
    CHECK(fake.lastCode == 0);
}

TEST_CASE("game-instance: a debugger suspension in update is not a fault - the script survives")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"class Game {\n"               // 1
                                   u8"  void launch() {}\n"         // 2
                                   u8"  void update(double dt) {\n" // 3
                                   u8"    int a = 1;\n"             // 4  <- breakpoint
                                   u8"    int b = a + 1;\n"         // 5
                                   u8"  }\n"
                                   u8"  void exit() {}\n"
                                   u8"}\n",
                                   u8"game.as");
    REQUIRE(ok);
    REQUIRE(gi.ScriptRunning());

    foundation::script::IScriptDebugger* debugger = nullptr;
    gi.RunHost().RequestDebugger(Function<void(foundation::script::IScriptDebugger&)>{
        [&debugger](foundation::script::IScriptDebugger& created)
        {
            created.SetBreakpoint(u8"game.as", 4);
            debugger = &created;
        }});
    REQUIRE(debugger != nullptr);

    struct BreakCounter final : foundation::script::IScriptDebuggerListener
    {
        int breaks = 0;
        void OnDebuggerStateChanged(foundation::script::ScriptDebuggerState state) override
        {
            if (state == foundation::script::ScriptDebuggerState::Breakpoint)
            {
                ++breaks;
            }
        }
    };
    BreakCounter counter;
    gi.RunHost().SetExternalDebugListener(&counter);

    // Hitting the breakpoint suspends update MID-CALL. The suspension surfaces as an error
    // result - the regression was TickScript reading it as a fault and killing the game
    // script ("Here" logged once, breakpoint never hit again).
    gi.TickScript(0.016f, 1.0f);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.ScriptRunning()); // the script SURVIVES the pause
    CHECK(counter.breaks == 1);

    // Ticking WHILE paused must not start a new update call (the per-frame re-break /
    // locals-flicker regression): no new pause events, still paused, still alive.
    gi.TickScript(0.016f, 1.0f);
    gi.TickScript(0.016f, 1.0f);
    CHECK(counter.breaks == 1);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.ScriptRunning());

    // Continue completes the held call; the next tick hits the breakpoint AGAIN.
    debugger->Continue();
    CHECK_FALSE(gi.RunHost().IsDebugPaused());
    CHECK(gi.ScriptRunning());
    gi.TickScript(0.016f, 1.0f);
    CHECK(gi.RunHost().IsDebugPaused());
    CHECK(gi.ScriptRunning());
    CHECK(counter.breaks == 2);

    // Removing the breakpoint while paused: Continue finishes the held call and the next
    // ticks run FREELY (the user's remove-during-pause flow, at the backend level).
    debugger->RemoveBreakpoint(u8"game.as", 4);
    debugger->Continue();
    gi.TickScript(0.016f, 1.0f);
    gi.TickScript(0.016f, 1.0f);
    CHECK_FALSE(gi.RunHost().IsDebugPaused());
    CHECK(gi.ScriptRunning());
    CHECK(counter.breaks == 2);

    gi.RunHost().SetExternalDebugListener(nullptr);
    gi.StopScript();
    CHECK_FALSE(gi.ScriptRunning());
}

TEST_CASE("game-instance: two instances own separate, isolated run-host contexts")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    const char8_t* src =
        u8"class Game { Game() {}\n void launch() {}\n void update(double dt) {}\n void exit() {}\n}\n";
    engine::runtime::GameInstance a;
    engine::runtime::GameInstance b;
    REQUIRE(a.StartScript(src, u8"game.as"));
    REQUIRE(b.StartScript(src, u8"game.as"));

    REQUIRE(a.RunHost().Context() != nullptr);
    REQUIRE(b.RunHost().Context() != nullptr);
    CHECK(a.RunHost().Context() !=
          b.RunHost().Context()); // distinct contexts = no shared script globals

    a.SetHeadless(true);
    CHECK(a.IsHeadless());
    CHECK_FALSE(b.IsHeadless());

    a.StopScript();
    b.StopScript();
    CHECK_FALSE(a.ScriptRunning());
    CHECK_FALSE(b.ScriptRunning());
}

TEST_CASE("game-instance: the run-scoped event bus - deferred delivery, order, cascade, isolation")
{
    RegisterCoreTypes();

    engine::runtime::GameInstance gi;
    messaging::EventBus& bus = gi.RunEvents();

    // Deferred delivery + payload + subscription order (the native run bus, zero script).
    Array<i32> order;
    i32 received = 0;
    (void)bus.Subscribe(StringHash(StringView(u8"Ping")),
                        [&](const Variant& p) { order.PushBack(1); received = p.Get<i32>(); });
    (void)bus.Subscribe(StringHash(StringView(u8"Ping")), [&](const Variant&) { order.PushBack(2); });
    bus.Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    CHECK(order.IsEmpty()); // nothing fires before Drain (delivery is deferred)
    gi.DrainRunEvents();
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == 1); // subscription order preserved
    CHECK(order[1] == 2);
    CHECK(received == 42); // payload delivered

    // Cascade: a handler's Publish is delivered within the SAME drain.
    bool cascaded = false;
    (void)bus.Subscribe(StringHash(StringView(u8"First")), [&](const Variant&)
                        { bus.Publish(StringHash(StringView(u8"Second")), Variant{}); });
    (void)bus.Subscribe(StringHash(StringView(u8"Second")), [&](const Variant&) { cascaded = true; });
    bus.Publish(StringHash(StringView(u8"First")), Variant{});
    gi.DrainRunEvents();
    CHECK(cascaded);

    // Two instances own INDEPENDENT buses (the multi-instance correctness check).
    engine::runtime::GameInstance other;
    i32 aHits = 0, bHits = 0;
    (void)gi.RunEvents().Subscribe(StringHash(StringView(u8"Only")), [&](const Variant&) { ++aHits; });
    (void)other.RunEvents().Subscribe(StringHash(StringView(u8"Only")),
                                      [&](const Variant&) { ++bHits; });
    gi.RunEvents().Publish(StringHash(StringView(u8"Only")), Variant{});
    gi.DrainRunEvents();
    other.DrainRunEvents();
    CHECK(aHits == 1);
    CHECK(bHits == 0); // other's bus never saw gi's event
}

TEST_CASE("game-instance: the Game tier's on<Event> inbox harvests the run bus (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    const String handlers[] = {String(u8"onPing")}; // the cooked ScriptClass's handler list
    const bool ok = gi.StartScript(u8"double Pinged = 0;\n"
                                   u8"class Game {\n"
                                   u8"  Game() {}\n"
                                   u8"  void launch() {}\n"
                                   u8"  void update(float dt) {}\n"
                                   u8"  void exit() {}\n"
                                   u8"  void onPing(int x) { Pinged = x; }\n"
                                   u8"}\n",
                                   u8"game.as", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* ctx = gi.RunHost().Context();
    REQUIRE(ctx != nullptr);
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // nothing fired yet

    // C++ publishes on the run bus -> onPing(42) fires on the Game class at the next drain.
    gi.RunEvents().Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // deferred until Drain
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0)); // delivered with payload

    // An event with NO matching on<Event> handler is ignored (only declared handlers subscribe).
    gi.RunEvents().Publish(StringHash(StringView(u8"Unheard")), Variant::From<i32>(99));
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0)); // unchanged

    gi.StopScript();
}

// The cooked class carries its handlers: a game started from it (the player's and the Game tab's
// way) hears the run bus without the caller spelling the list out. The player once started from
// the source alone, so a shipped game never heard its coins or its flag.
TEST_CASE("game-instance: a Game started from its cooked class hears the run bus through its handlers")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    foundation::script::ScriptClass cooked;
    cooked.language = String(u8"angelscript");
    cooked.className = String(u8"Game");
    cooked.sourceName = String(u8"game.as");
    cooked.source = String(u8"double Reached = 0;\n"
                           u8"class Game {\n"
                           u8"  Game() {}\n"
                           u8"  void launch() {}\n"
                           u8"  void update(float dt) {}\n"
                           u8"  void exit() {}\n"
                           u8"  void onGoalReached(int x) { Reached = x + 1; }\n"
                           u8"}\n");
    cooked.handlers.PushBack(String(u8"onGoalReached"));

    engine::runtime::GameInstance gi;
    REQUIRE(gi.StartScript(cooked));
    auto* ctx = gi.RunHost().Context();
    REQUIRE(ctx != nullptr);
    gi.RunEvents().Publish(StringHash(StringView(u8"GoalReached")), Variant::From<i32>(0));
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Reached").Get<f64>() == doctest::Approx(1.0));
    gi.StopScript();
}

TEST_CASE("game-instance: the Game tier's on<Event> inbox harvests the run bus (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartScript(u8"Pinged = 0\n"
                                   u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:launch() end\n"
                                   u8"function Game:update(dt) end\n"
                                   u8"function Game:exit() end\n"
                                   u8"function Game:onPing(x) Pinged = x end\n",
                                   u8"game.luau", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* ctx = gi.ScriptContext();
    REQUIRE(ctx != nullptr);
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0));

    // C++ publishes on the run bus -> Game:onPing(42) fires at drain (deferred).
    gi.RunEvents().Publish(StringHash(StringView(u8"Ping")), Variant::From<i32>(42));
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0));

    // A non-subscribed event is ignored.
    gi.RunEvents().Publish(StringHash(StringView(u8"Unheard")), Variant::From<i32>(99));
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(42.0));

    gi.StopScript();
}

TEST_CASE("game-instance: run.events().emit publishes on the run bus, round-tripping to on<Event> (AS)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade(); // the run.* facade (bound as `run`)
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartScript(
        u8"double Pinged = 0;\n"
        u8"class Game {\n"
        u8"  Game() {}\n"
        u8"  void launch() { run::events().emit(\"Ping\", 7); }\n" // publish on the RUN bus from script
        u8"  void update(float dt) {}\n"
        u8"  void exit() {}\n"
        u8"  void onPing(int x) { Pinged = x; }\n" // the Game's own inbox hears it
        u8"}\n",
        u8"game.as", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* ctx = gi.RunHost().Context();
    REQUIRE(ctx != nullptr);
    // launch() emitted, but the run bus delivers deferred - nothing fires until the drain.
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0));
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(7.0)); // script -> run bus -> script
    gi.StopScript();
}

TEST_CASE("game-instance: run.setTimeScale routes from script to the run binding")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    f32 captured = -1.0f;
    gi.RunBinding().setTimeScale = Function<void(f32)>{[&captured](f32 s) { captured = s; }};
    const bool ok = gi.StartScript(u8"class Game {\n"
                                   u8"  Game() {}\n"
                                   u8"  void launch() { run::setTimeScale(0.5f); }\n" // pause/slow-mo API
                                   u8"  void update(float dt) {}\n"
                                   u8"  void exit() {}\n"
                                   u8"}\n",
                                   u8"game.as", Span<const String>{});
    REQUIRE(ok);
    CHECK(captured == doctest::Approx(0.5f)); // launch() -> run::setTimeScale -> binding (synchronous)
    gi.StopScript();
}

TEST_CASE("game-instance: run.setTimeScale/timeScale route from script to the run binding (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    f32 captured = -1.0f;
    gi.RunBinding().setTimeScale = Function<void(f32)>{[&captured](f32 s) { captured = s; }};
    gi.RunBinding().timeScale = Function<f32()>{[&captured]() { return captured; }};
    const bool ok = gi.StartScript(u8"Scale = -1\n"
                                   u8"Game = {}\n"
                                   u8"Game.__index = Game\n"
                                   u8"function Game.new() return setmetatable({}, Game) end\n"
                                   u8"function Game:launch()\n"
                                   u8"  run.setTimeScale(0.5)\n" // pause/slow-mo API
                                   u8"  Scale = run.timeScale()\n" // read it back through the binding
                                   u8"end\n"
                                   u8"function Game:update(dt) end\n"
                                   u8"function Game:exit() end\n",
                                   u8"game.luau", Span<const String>{});
    REQUIRE(ok);
    CHECK(captured == doctest::Approx(0.5f)); // launch() -> run.setTimeScale -> binding (synchronous)
    auto* ctx = gi.ScriptContext();
    REQUIRE(ctx != nullptr);
    // The exact scale round-tripped back through run.timeScale() (getter reads the binding).
    CHECK(ctx->GetGlobal(u8"Scale").Get<f64>() == doctest::Approx(0.5));
    gi.StopScript();
}

TEST_CASE("game-instance: ClearScenes resets the run-scoped group time scale to 1")
{
    engine::runtime::GameInstance gi;
    (void)gi.Scenes().CreateScene(u8"L1");

    // A game paused via run.setTimeScale(0) then stopped: the SceneManager survives a stop on the
    // persistent editor instance, so the RUN-scoped scale must not leak into the next Play.
    gi.Scenes().SetTimeScale(0.0f);
    CHECK(gi.Scenes().TimeScale() == doctest::Approx(0.0f));

    gi.ClearScenes();
    CHECK(gi.Scenes().SceneCount() == 0u);                    // the group is gone
    CHECK(gi.Scenes().TimeScale() == doctest::Approx(1.0f)); // the next run starts unfrozen
}

TEST_CASE("game-instance: run.events():emit publishes on the run bus, round-tripping to on<Event> (Luau)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    const String handlers[] = {String(u8"onPing")};
    const bool ok = gi.StartScript(
        u8"Pinged = 0\n"
        u8"Game = {}\n"
        u8"Game.__index = Game\n"
        u8"function Game.new() return setmetatable({}, Game) end\n"
        u8"function Game:launch() run.events():emit(\"Ping\", 7) end\n" // publish on the run bus
        u8"function Game:update(dt) end\n"
        u8"function Game:exit() end\n"
        u8"function Game:onPing(x) Pinged = x end\n",
        u8"game.luau", Span<const String>(handlers, 1));
    REQUIRE(ok);
    auto* ctx = gi.ScriptContext();
    REQUIRE(ctx != nullptr);
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(0.0)); // deferred until drain
    gi.DrainRunEvents();
    CHECK(ctx->GetGlobal(u8"Pinged").Get<f64>() == doctest::Approx(7.0)); // script -> run bus -> script
    gi.StopScript();
}

TEST_CASE("game-instance: a missing Game class fails to start cleanly")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();

    engine::runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"int X = 1;\n", u8"game.as");
    CHECK_FALSE(ok); // no `Game` class
    CHECK_FALSE(gi.ScriptRunning());
}

TEST_CASE("game-instance: LoadScene / LoadSceneAsync own the scene load orchestration (task #123)")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();

    FileDelete(u8"scratch_gi_load_db/level.rasset");
    FileDelete(u8"scratch_gi_load_db/level.scene.bin");
    FileDelete(u8"scratch_gi_load_db/level.scene.data");
    RemoveDirectory(u8"scratch_gi_load_db");
    NativeFileSystem mount(u8"scratch_gi_load_db", DefaultAllocator());

    Guid sceneId;
    {
        // A tiny entities-only scene round-trips with no component managers.
        scene::Scene authored(DefaultAllocator(), u8"level");
        (void)authored.CreateEntity(u8"a");
        (void)authored.CreateEntity(u8"b");
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        sceneId = inst->Id();
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
    }

    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    resource::ResourceManager resources(DefaultAllocator(), db);
    auto* sceneInst = db.GetInstance(sceneId);
    REQUIRE(sceneInst != nullptr);

    SUBCASE("sync LoadScene returns the active, resolved scene")
    {
        engine::runtime::GameInstance gi;
        scene::Scene* s =
            gi.LoadScene(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(s != nullptr);
        CHECK(s->EntityCount() == 2u);
        CHECK(gi.Scenes().IsActive(s)); // sync path activates immediately (unchanged behavior)
    }

    SUBCASE("async LoadSceneAsync creates the scene INACTIVE until ActivateLoadedScene")
    {
        engine::runtime::GameInstance gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(handle.Scene() != nullptr);
        CHECK_FALSE(handle.Failed());
        CHECK_FALSE(gi.Scenes().IsActive(handle.Scene())); // inactive while "loading"
        CHECK(handle.IsComplete());                        // no async resources -> complete at once
        CHECK(handle.Progress() == doctest::Approx(1.0f));

        scene::Scene* activated = gi.ActivateLoadedScene(handle);
        REQUIRE(activated == handle.Scene());
        CHECK(gi.Scenes().IsActive(activated)); // now ticked + rendered
        CHECK(activated->EntityCount() == 2u);
    }

    SUBCASE("a load with no scene stream fails cleanly")
    {
        auto* empty =
            db.RootGroup()->CreateInstance(u8"empty", scene::SceneDocument::StaticType());
        engine::runtime::GameInstance gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*empty, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        CHECK(handle.Failed());
        CHECK(handle.IsComplete());
        CHECK(handle.Scene() == nullptr);
        CHECK(gi.LoadScene(*empty, resources, Function<UniquePtr<IStream>(const Guid&)>{}) ==
              nullptr);
    }

    SUBCASE("a level load replaces the scene it lands over, on the pump and synchronously")
    {
        engine::runtime::GameInstance gi;
        gi.SetSceneActivationPolicy(Function<void(scene::Scene*)>{[](scene::Scene*) {}});

        engine::runtime::SceneLoadHandle first =
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        (void)gi.TrackScriptLoad(first);
        gi.PumpScriptLoads();
        scene::Scene* level1 = gi.GetScene();
        REQUIRE(level1 != nullptr);
        CHECK(gi.Scenes().SceneCount() == 1u);

        // The same level again, as a restart loads it: both exist while the new one loads, then
        // the new copy is the run's scene and the manager's current, and the old one is gone.
        engine::runtime::SceneLoadHandle second =
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        scene::Scene* level2 = second.Scene();
        (void)gi.TrackScriptLoad(second);
        CHECK(gi.Scenes().SceneCount() == 2u);
        gi.PumpScriptLoads();
        CHECK(gi.GetScene() == level2);
        CHECK(gi.Scenes().CurrentScene() == level2);
        CHECK(gi.Scenes().IsActive(level2));
        CHECK(gi.Scenes().SceneCount() == 1u);

        // The synchronous path (run.loadScene) adopts the same way.
        scene::Scene* level3 =
            gi.LoadScene(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(level3 != nullptr);
        gi.AdoptLoadedScene(level3);
        CHECK(gi.GetScene() == level3);
        CHECK(gi.Scenes().SceneCount() == 1u);
    }

    SUBCASE("script-load registry: ticket -> poll -> PumpScriptLoads activates + runs the policy")
    {
        engine::runtime::GameInstance gi;
        CHECK_FALSE(gi.SceneReady()); // no current scene at launch (orchestrator-first boot)

        // The app owns the render/sim policy; here a fake records which scene it activated.
        scene::Scene* policyRanOn = nullptr;
        gi.SetSceneActivationPolicy(
            Function<void(scene::Scene*)>{[&](scene::Scene* s) { policyRanOn = s; }});

        // The Game facade hands the in-flight handle to the instance and gets a ticket back.
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(handle.Scene() != nullptr);
        scene::Scene* pending = handle.Scene();
        const i32 ticket = gi.TrackScriptLoad(handle);
        CHECK(ticket == 1); // 1-based; 0 is reserved for "did not start"

        // Registered but not yet pumped: not complete, scene still inactive, policy not run.
        CHECK_FALSE(gi.ScriptLoadComplete(ticket));
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK(gi.ScriptLoadProgress(ticket) == doctest::Approx(1.0f)); // no async resources pending
        CHECK_FALSE(gi.Scenes().IsActive(pending));
        CHECK(policyRanOn == nullptr);

        gi.PumpScriptLoads(); // resources already complete -> activate + SetScene + policy
        CHECK(gi.ScriptLoadComplete(ticket));
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK(gi.ScriptLoadProgress(ticket) == doctest::Approx(1.0f));
        CHECK(gi.Scenes().IsActive(pending));
        CHECK(policyRanOn == pending);   // the app policy ran on the freshly activated scene
        CHECK(gi.GetScene() == pending); // SetScene bookkeeping happened
        CHECK(gi.SceneReady());          // Game.sceneReady() now true

        gi.PumpScriptLoads(); // idempotent: an already-activated load is skipped
        CHECK(policyRanOn == pending);

        // An unknown/expired ticket never hangs a `while (!complete) yield` loop.
        CHECK(gi.ScriptLoadComplete(999));
        CHECK_FALSE(gi.ScriptLoadFailed(999));
        CHECK(gi.ScriptLoadProgress(999) == doctest::Approx(1.0f));
    }

    SUBCASE("script-load registry: a failed load reports terminal-complete + failed by ticket")
    {
        auto* empty =
            db.RootGroup()->CreateInstance(u8"empty2", scene::SceneDocument::StaticType());
        engine::runtime::GameInstance gi;
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*empty, resources, Function<UniquePtr<IStream>(const Guid&)>{}));
        CHECK(ticket == 1);
        gi.PumpScriptLoads(); // a failed handle is skipped (never activated), stays terminal
        CHECK(gi.ScriptLoadComplete(ticket)); // terminal (failed counts as complete)
        CHECK(gi.ScriptLoadFailed(ticket));
        CHECK_FALSE(gi.SceneReady()); // nothing became current
    }

    SUBCASE("destroying a pending scene sweeps its tracked load (no dangling activation)")
    {
        // Fable review finding: the tracked handle holds a raw Scene*. Destroying the pending
        // scene before it activates must drop the tracked load, or PumpScriptLoads would later
        // activate freed memory.
        engine::runtime::GameInstance gi;
        engine::runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(handle.Scene() != nullptr);
        scene::Scene* pending = handle.Scene();
        const i32 ticket = gi.TrackScriptLoad(handle);
        CHECK_FALSE(gi.ScriptLoadComplete(ticket)); // in flight (not yet activated)

        gi.DestroyScene(pending);   // sweeps the tracked entry BEFORE freeing the scene
        gi.PumpScriptLoads();       // must be a safe no-op (nothing to activate)
        CHECK(gi.ScriptLoadComplete(ticket));   // the ticket now reads terminal-safe via the fallback
        CHECK_FALSE(gi.ScriptLoadFailed(ticket));
        CHECK_FALSE(gi.SceneReady());           // nothing became current
    }

    SUBCASE("ClearScenes drops in-flight tracked loads + tears the group down (no dangling activation)")
    {
        // The editor Game tab's Stop tears the run's scenes down while KEEPING the instance. An
        // async load still in flight at that moment holds a raw Scene* in the tracked registry;
        // ClearScenes must drop it BEFORE Scenes().Clear() frees the scene, or the next
        // PumpScriptLoads (still driven every frame) would activate + Start() freed memory.
        engine::runtime::GameInstance gi;
        scene::Scene* live =
            gi.LoadScene(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(live != nullptr);
        gi.SetScene(live);
        REQUIRE(gi.SceneReady());

        // A second load left IN FLIGHT (inactive, tracked) - the scene that would dangle.
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{}));
        CHECK_FALSE(gi.ScriptLoadComplete(ticket)); // in flight
        CHECK(gi.Scenes().SceneCount() == 2u);      // the live scene + the pending inactive one

        bool policyRan = false;
        gi.SetSceneActivationPolicy(
            Function<void(scene::Scene*)>{[&](scene::Scene*) { policyRan = true; }});

        gi.ClearScenes(); // drop tracked loads, clear current, destroy every scene
        CHECK(gi.Scenes().SceneCount() == 0u);
        CHECK(gi.GetScene() == nullptr);
        CHECK_FALSE(gi.SceneReady());

        gi.PumpScriptLoads(); // the dropped ticket must NOT activate a freed scene
        CHECK_FALSE(policyRan);
        CHECK(gi.ScriptLoadComplete(ticket)); // terminal-safe via the unknown-ticket fallback
        CHECK(gi.GetScene() == nullptr);      // still no current scene
    }

    SUBCASE("script-load registry: a successful load is RETIRED on activation (bounded growth)")
    {
        // m_scriptLoads must not grow monotonically. After activation the
        // entry is dropped, so re-destroying the (now active) scene is a safe no-op and the ticket
        // still reads terminal-safe - the externally observable contract is unchanged.
        engine::runtime::GameInstance gi;
        const i32 ticket = gi.TrackScriptLoad(
            gi.LoadSceneAsync(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{}));
        gi.PumpScriptLoads(); // activates + retires the entry
        REQUIRE(gi.SceneReady());
        scene::Scene* live = gi.GetScene();
        CHECK(gi.ScriptLoadComplete(ticket)); // retired -> fallback (complete)
        gi.DestroyScene(live);                // no tracked entry references it anymore: safe
        gi.PumpScriptLoads();                 // still a safe no-op
        CHECK(gi.ScriptLoadComplete(ticket));
    }

    FileDelete(u8"scratch_gi_load_db/level.rasset");
    FileDelete(u8"scratch_gi_load_db/level.scene.bin");
    FileDelete(u8"scratch_gi_load_db/level.scene.data");
    RemoveDirectory(u8"scratch_gi_load_db");
}

TEST_CASE("game-instance: created scenes share the run bus - cross-scene delivery, single drain (no relay)")
{
    // Every scene the instance creates borrows THE run bus, so scene.events and the run
    // bus are one object. A subscriber in scene A hears an emit from scene B (and the Game tier would too)
    // with NO relay, delivered exactly ONCE by the instance's drain - the borrowing scenes never drain it.
    engine::runtime::GameInstance gi;
    scene::Scene* a = gi.CreateScene(u8"A");
    scene::Scene* b = gi.CreateScene(u8"B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->Events() == &gi.RunEvents()); // both borrow the ONE run bus
    CHECK(b->Events() == &gi.RunEvents());

    int hits = 0;
    (void)a->Events()->Subscribe(StringHash(u8"Ping"), [&](const Variant&) { ++hits; });
    b->Events()->Publish(StringHash(u8"Ping"), Variant{}); // emit from a DIFFERENT scene

    // Ticking the borrowing scenes must NOT deliver it (they do not drain a borrowed bus).
    a->Update(0.016f);
    b->Update(0.016f);
    CHECK(hits == 0);

    gi.DrainRunEvents(); // the instance drains the shared bus ONCE
    CHECK(hits == 1);    // delivered exactly once, cross-scene, no relay
}

// agent-playtesting-and-asset-creation.md P3 (Sedulous 8d873538): a faulting game script is
// remembered with where it faulted and why, and the run's clock is the gameplay time the script
// moved by. A new start clears the fault; a clean stop is not one.
TEST_CASE("game-instance: a faulted game script says where and why, and the run clock counts "
          "gameplay time")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    const StringView faulting = u8"Game = {}\n"
                                u8"Game.__index = Game\n"
                                u8"function Game.new() return setmetatable({ n = 0 }, Game) end\n"
                                u8"function Game:launch() end\n"
                                u8"function Game:update(dt)\n"
                                u8"  self.n = self.n + 1\n"
                                u8"  if self.n == 4 then error(\"boom\") end\n"
                                u8"end\n"
                                u8"function Game:exit() end\n";
    REQUIRE(gi.StartScript(faulting, u8"game.luau"));
    CHECK(gi.ScriptFault().IsEmpty());
    for (int i = 0; i < 4; ++i)
    {
        gi.TickScript(1.0f / 60.0f, 1.0f);
    }
    CHECK_FALSE(gi.ScriptRunning());
    CHECK(gi.ScriptFault().StartsWith(u8"faulted in update"));
    CHECK(gi.ScriptFault().ContainsIgnoreCase(u8"boom"));
    CHECK(gi.RunTime() == doctest::Approx(4.0 / 60.0).epsilon(1e-5));
    gi.ResetRunClock();
    CHECK(gi.RunTime() == 0.0);

    // A new start clears it; a clean stop is not a fault.
    REQUIRE(gi.StartScript(faulting, u8"game.luau"));
    CHECK(gi.ScriptFault().IsEmpty());
    gi.StopScript();
    CHECK(gi.ScriptFault().IsEmpty());

    // A script with no Game class says it did not instantiate (a fresh instance: a run's host
    // keeps the one script context it made).
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::runtime::GameInstance other;
    CHECK_FALSE(other.StartScript(u8"class Other { Other() {} }\n", u8"game.as"));
    CHECK(other.ScriptFault().ContainsIgnoreCase(u8"did not instantiate"));
}

// A project with no game script (a scene of behaviours alone) still runs, and a playtest times its
// run by this clock: it counts the frames either way (Snowline's first scene had none, and every
// pie_run on it waited out its timeout at run time 0).
TEST_CASE("game-instance: the run clock counts frames without a game script")
{
    RegisterCoreTypes();
    engine::runtime::GameInstance gi;
    CHECK_FALSE(gi.ScriptRunning());
    for (int i = 0; i < 5; ++i)
    {
        gi.TickScript(1.0f / 60.0f, 1.0f);
    }
    CHECK(gi.RunTime() == doctest::Approx(5.0 / 60.0).epsilon(1e-5));
    gi.ResetRunClock();
    CHECK(gi.RunTime() == 0.0);
}

// A project with no game script runs its behaviors on the run's host all the same, and they need
// the run's services: Snowline's board read Input on a context that only a game script's start
// had given the run's input runtime, so no action ever fired (it read the shared runtime, which has
// no project map). Every context the run's host makes now takes them.
TEST_CASE("game-instance: a context for behaviors gets the run's input, run and save services without a game script")
{
    RegisterCoreTypes();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::runtime::GameInstance gi;
    CHECK_FALSE(gi.ScriptRunning());
    foundation::script::IScriptContext* context = gi.RunHost().EnsureContextForFile(u8"behaviors.as");
    REQUIRE(context != nullptr);
    CHECK(context->GetService(foundation::input::kInputScriptService) == &gi.InputRuntime());
    CHECK(context->GetService(engine::runtime::kRunScriptService) != nullptr);
    CHECK(context->GetService(engine::runtime::kSaveScriptService) == &gi.Saves());
}

// agent-playtesting-and-asset-creation.md P4 (Sedulous aaf5ff78): the run clock is the host's
// unscaled delta, so it keeps going while the game pauses its scene (behind a menu) and a
// playtest's timeline goes on; and a playtest reads the running game script's properties.
TEST_CASE("game-instance: the run clock ignores time scale, and a game script's properties read")
{
    RegisterCoreTypes();
    foundation::script::RegisterLuauScriptBackend();

    engine::runtime::GameInstance gi;
    CHECK_FALSE(gi.GetScriptProperty(u8"score").HasValue()); // no script running
    const StringView game = u8"Game = {}\n"
                            u8"Game.__index = Game\n"
                            u8"function Game.new() return setmetatable({ score = 0, real = 0 }, Game) end\n"
                            u8"function Game:launch() end\n"
                            u8"function Game:update(dt)\n"
                            u8"  self.score = self.score + 1\n"
                            u8"  self.real = self.real + run.realDeltaTime()\n"
                            u8"end\n"
                            u8"function Game:exit() end\n";
    REQUIRE(gi.StartScript(game, u8"game.luau"));
    gi.SetInstanceTimeScale(0.0f); // the scene paused: the run clock still moves
    for (int i = 0; i < 3; ++i)
    {
        gi.TickScript(1.0f / 60.0f, 1.0f);
    }
    CHECK(gi.RunTime() == doctest::Approx(3.0 / 60.0).epsilon(1e-5));

    auto score = gi.GetScriptProperty(u8"score");
    REQUIRE(score.HasValue());
    CHECK(score.Value().Get<f64>() == doctest::Approx(3.0));
    CHECK_FALSE(gi.GetScriptProperty(u8"lives").HasValue());
    // Sedulous 2ad3026c: a script reads the real clock too, for a pause menu to time by.
    CHECK(gi.GetScriptProperty(u8"real").Value().Get<f64>() == doctest::Approx(3.0 / 60.0).epsilon(1e-4));
    CHECK(gi.RunBinding().realSeconds == doctest::Approx(3.0 / 60.0).epsilon(1e-5));
    CHECK_FALSE(gi.GetScriptProperty(u8"update").HasValue()); // a method is not a property
    gi.StopScript();
    CHECK_FALSE(gi.GetScriptProperty(u8"score").HasValue());
}

// ---- save data: the values a game keeps between runs (Documentation/Specs/save-data.md) ----

namespace
{
    // A fresh scratch file under the build tree the tests run in.
    String FreshSavePath(StringView name)
    {
        (void)CreateDirectories(u8"scratch_save");
        String path = PathJoin(u8"scratch_save", name);
        (void)FileDelete(path.AsView());
        return path;
    }
}

TEST_CASE("game-instance: a run save writes what changed, reads it back, and survives a bad file")
{
    const String path = FreshSavePath(u8"run_save.xml");

    engine::runtime::RunSave save(DefaultAllocator());
    CHECK_FALSE(save.Flush()); // no file named: nowhere to write
    save.Open(path.AsView());  // absent: an empty save
    CHECK(save.IsOpen());
    CHECK(save.Values().Count() == 0u);
    CHECK(save.Flush()); // nothing changed: nothing to write, and that is fine
    CHECK_FALSE(FileExists(path.AsView()));

    save.Values().SetInt(u8"best.level2", 4210);
    save.Values().SetFloat(u8"time.level2", 41.5f);
    save.MarkChanged();
    REQUIRE(save.Flush());
    CHECK_FALSE(save.HasChanges());
    CHECK(FileExists(path.AsView()));

    engine::runtime::RunSave reread(DefaultAllocator());
    reread.Open(path.AsView());
    CHECK(reread.Values().GetInt(u8"best.level2", 0) == 4210);
    CHECK(reread.Values().GetFloat(u8"time.level2", 0.0f) == 41.5f);

    // A file that is not a save: an empty save, and the file is left alone until the game writes.
    const char garbage[] = "not a save";
    REQUIRE(WriteFile(path.AsView(), Span<const byte>{reinterpret_cast<const byte*>(garbage), sizeof(garbage) - 1})
                .IsOk());
    engine::runtime::RunSave damaged(DefaultAllocator());
    damaged.Open(path.AsView());
    CHECK(damaged.Values().Count() == 0u);
    CHECK(damaged.Flush());
    Result<Array<byte>> bytes = ReadFile(path.AsView());
    REQUIRE(bytes.HasValue());
    CHECK(bytes.Value().Size() == sizeof(garbage) - 1);
}

TEST_CASE("game-instance: a game keeps its values between runs through Save (AngelScript)")
{
    RegisterCoreTypes();
    foundation::script::RegisterScriptFacadeReflection();
    engine::runtime::RegisterRunScriptFacade();
    engine::runtime::RegisterSaveScriptFacade();
    foundation::script::angelscript::RegisterAngelScriptBackend();
    const String path = FreshSavePath(u8"game_as.xml");

    // Each run counts itself and keeps a best time; the second sees the first's values. The
    // first never calls flush: the run writes what changed as it stops.
    const StringView source = u8"class Game {\n"
                              u8"  Game() {}\n"
                              u8"  void launch() {\n"
                              u8"    int runs = Save::getInt(\"runs\", 0);\n"
                              u8"    Save::setInt(\"runs\", runs + 1);\n"
                              u8"    if (Save::getFloat(\"best\", 999.0f) > 41.5f) Save::setFloat(\"best\", 41.5f);\n"
                              u8"    Save::setBool(\"seen\", Save::has(\"runs\"));\n"
                              u8"    Save::setString(\"name\", \"Hopper\");\n"
                              u8"    Save::setInt(\"scratch\", 1);\n"
                              u8"    Save::remove(\"scratch\");\n"
                              u8"  }\n"
                              u8"  void update(double dt) {}\n"
                              u8"  void exit() {}\n"
                              u8"}\n";
    for (i32 run = 1; run <= 2; ++run)
    {
        engine::runtime::GameInstance gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartScript(source, u8"game.as"));
        CHECK(gi.Saves().Values().GetInt(u8"runs", 0) == run);
        gi.StopScript();
    }

    engine::runtime::RunSave reread(DefaultAllocator());
    reread.Open(path.AsView());
    CHECK(reread.Values().GetInt(u8"runs", 0) == 2);
    CHECK(reread.Values().GetFloat(u8"best", 0.0f) == 41.5f);
    CHECK(reread.Values().GetBool(u8"seen", false));
    CHECK(reread.Values().GetText(u8"name", u8"") == u8"Hopper");
    CHECK_FALSE(reread.Values().Has(u8"scratch"));
}

TEST_CASE("game-instance: Save flushes on request and clears (Luau); a run with no file writes nowhere")
{
    RegisterCoreTypes();
    engine::runtime::RegisterSaveScriptFacade();
    foundation::script::RegisterLuauScriptBackend();
    const String path = FreshSavePath(u8"game_luau.xml");

    {
        engine::runtime::GameInstance gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartScript(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:launch()\n"
                               u8"  Save.setInt('coins', 37)\n"
                               u8"  self.flushed = Save.flush()\n"
                               u8"end\n"
                               u8"function Game:update(dt) end\n"
                               u8"function Game:exit() end\n",
                               u8"game.luau"));
        // Written at the flush, while the run is still going.
        engine::runtime::RunSave reread(DefaultAllocator());
        reread.Open(path.AsView());
        CHECK(reread.Values().GetInt(u8"coins", 0) == 37);
        gi.StopScript();
    }
    {
        engine::runtime::GameInstance gi;
        gi.SetSaveFile(path.AsView());
        REQUIRE(gi.StartScript(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:launch() Save.clear() end\n"
                               u8"function Game:update(dt) end\n"
                               u8"function Game:exit() end\n",
                               u8"game.luau"));
        gi.StopScript();
        engine::runtime::RunSave reread(DefaultAllocator());
        reread.Open(path.AsView());
        CHECK(reread.Values().Count() == 0u);
    }
    {
        // No file named: the game's values last the run, its reads see them, nothing is written.
        engine::runtime::GameInstance gi;
        REQUIRE(gi.StartScript(u8"Game = {}\n"
                               u8"Game.__index = Game\n"
                               u8"function Game.new() return setmetatable({}, Game) end\n"
                               u8"function Game:launch() Save.setInt('coins', 5) end\n"
                               u8"function Game:update(dt) end\n"
                               u8"function Game:exit() end\n",
                               u8"game.luau"));
        CHECK(gi.Saves().Values().GetInt(u8"coins", 0) == 5);
        gi.StopScript();
    }
}

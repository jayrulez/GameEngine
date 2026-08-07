// draconic.engine.gameinstance - the extracted run bracket owns the script run state + time scale.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.engine.gameinstance;
import draconic.scene;
import draconic.scene.resource;    // SceneDocument + LoadScene round-trip
import draconic.content;           // ContentDatabase / Instance
import draconic.resource;          // ResourceManager
import draconic.vfs;               // NativeFileSystem
import draconic.script;
import draconic.script.facades; // RegisterScriptFacadeReflection (the Scene facade)
import draconic.script.wren;
import draconic.script.angelscript;
import draconic.net;         // NetSession queries (IsServer/PeerCount)
import draconic.net.manager; // NetworkManager (the endpoint the instance owns)
import draconic.input;       // ActionRuntime / IInputSourceProvider (per-instance input)
import draconic.shell;       // IKeyboard / KeyCode (a minimal fake device)

using namespace draconic::foundation;
namespace runtime = draconic::runtime;
namespace script = draconic::script; // raw manager/context for the SceneLoader facade battery
namespace scene = draconic::scene;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace net = draconic::net;
namespace input = draconic::input;
namespace shell = draconic::shell;
using draconic::vfs::NativeFileSystem;

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
    script::RegisterExtraFacadeName(u8"SceneLoader");

    bool sawGame = false;
    bool sawLevel = false;
    bool sawSceneLoader = false;
    for (StringView facade : script::ExtraFacadeNames())
    {
        sawGame = sawGame || facade == StringView(u8"Game");
        sawLevel = sawLevel || facade == StringView(u8"Level");
        sawSceneLoader = sawSceneLoader || facade == StringView(u8"SceneLoader");
    }
    CHECK_FALSE(sawGame);  // refused
    CHECK_FALSE(sawLevel); // refused
    CHECK(sawSceneLoader); // registered
}

TEST_CASE("game-instance: instance time scale defaults to 1 and is settable; fresh instance idle")
{
    runtime::GameInstance gi;
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

TEST_CASE("game-instance: each instance's input runtime reads ONLY its own source (per-instance "
          "isolation)")
{
    // The multi-instance-PIE fix: each GameInstance has its OWN ActionRuntime bound to its OWN source,
    // so one tab's keys never reach another tab's game (the shared-runtime bug that flipped the server
    // tab into a client). Same map, same key, two sources - only the source with the key held fires.
    runtime::GameInstance a;
    runtime::GameInstance b;
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
    // The per-instance networking model: a GameInstance IS the INetworkController, opening its OWN
    // real UDP endpoint on StartServer/Connect. Two instances in one process = two isolated endpoints.
    runtime::GameInstance server;
    runtime::GameInstance client;
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
        server.DriveNetwork(16.0f);
        client.DriveNetwork(16.0f);
        SleepMilliseconds(1);
    }
    CHECK(server.NetEndpoint()->Session().PeerCount() == 1u);

    client.StopNetworking(); // disconnect drops the endpoint
    CHECK(client.NetEndpoint() == nullptr);
    CHECK(server.NetEndpoint() != nullptr); // the server is unaffected (isolation)
}

TEST_CASE("game-instance: fallback path starts, ticks, and stops a Game script")
{
    RegisterFoundationTypes();
    draconic::script::wren::RegisterWrenScriptBackend();

    runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"class Game {\n"
                                   u8"  construct new() {}\n"
                                   u8"  launch() {}\n"
                                   u8"  update(dt) {}\n"
                                   u8"  exit() {}\n"
                                   u8"}\n",
                                   u8"game.wren");
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

// task #123 boot reorder: the game script now launches BEFORE any scene, so a `class Game`
// orchestrator's launch()/update() may call the Scene + SceneLoader facades with NO current scene.
// Two properties under test: (1) the facades are NULL-SCENE-SAFE (no deref of a null currentScene -
// the run must launch + tick without faulting); (2) the load facade is named SceneLoader, NOT Game -
// a facade named Game is a hard AngelScript name conflict with the mandatory `Game` orchestrator
// class (and a Wren import clash), so THIS test compiling at all is the regression guard for that.
TEST_CASE("game-instance: Scene/SceneLoader facades are null-scene-safe from a pre-scene "
          "orchestrator (Wren)")
{
    RegisterFoundationTypes();
    draconic::script::RegisterScriptFacadeReflection();
    runtime::RegisterSceneLoaderScriptFacade(); // SceneLoader facade (owned by this project)
    draconic::script::wren::RegisterWrenScriptBackend();

    runtime::GameInstance gi;
    CHECK_FALSE(gi.SceneReady()); // no scene yet - the orchestrator-first condition
    const bool ok = gi.StartScript(
        u8"import \"main\" for SceneLoader\n"
        u8"class Game {\n"
        u8"  construct new() {}\n"
        u8"  launch() {\n"
        u8"    SceneLoader.currentScene().find(\"nobody\")\n"
        u8"    SceneLoader.currentScene().findByPath(\"a/b\")\n"
        u8"    SceneLoader.sceneReady()\n"
        u8"    SceneLoader.loadComplete(0)\n"
        u8"    SceneLoader.loadProgress(0)\n"
        u8"  }\n"
        u8"  update(dt) {\n"
        u8"    SceneLoader.currentScene().find(\"x\")\n"
        u8"    SceneLoader.sceneReady()\n"
        u8"  }\n"
        u8"  exit() {}\n"
        u8"}\n",
        u8"game.wren");
    REQUIRE(ok); // compiled (no Game-name clash) + launch() ran the pre-scene facade calls, no fault
    CHECK(gi.ScriptRunning());
    gi.DriveRunHost(0.016f);
    gi.TickScript(0.016f, 1.0f); // update() calls them again - still no fault
    CHECK(gi.ScriptRunning());
    gi.StopScript();
}

TEST_CASE("game-instance: Scene/SceneLoader facades are null-scene-safe from a pre-scene "
          "orchestrator (AngelScript)")
{
    RegisterFoundationTypes();
    draconic::script::RegisterScriptFacadeReflection();
    runtime::RegisterSceneLoaderScriptFacade(); // SceneLoader facade (owned by this project)
    draconic::script::angelscript::RegisterAngelScriptBackend();

    runtime::GameInstance gi;
    // A facade named `Game` would fail HERE with "Name conflict. 'Game' is an extended data type" -
    // the SceneLoader rename is exactly what lets this `class Game` compile alongside the facade.
    const bool ok = gi.StartScript(
        u8"class Game {\n"
        u8"  Game() {}\n"
        u8"  void launch() {\n"
        u8"    SceneLoader::currentScene().find(\"nobody\");\n"
        u8"    SceneLoader::currentScene().findByPath(\"a/b\");\n"
        u8"    SceneLoader::sceneReady();\n"
        u8"    SceneLoader::loadComplete(0);\n"
        u8"    SceneLoader::loadProgress(0);\n"
        u8"  }\n"
        u8"  void update(double dt) { SceneLoader::currentScene().find(\"x\"); "
        u8"SceneLoader::sceneReady(); }\n"
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

// The SceneLoader.* facade routing PROVEN end-to-end on both backends (raw context, Net-style): a
// fake SceneLoaderScriptBinding (standing in for the app's content-DB-backed load pointers) is
// installed as the sceneloader.runtime service, then a script kicks an async load and polls the
// ticket to completion. What is under test is the facade->binding routing + the ticket round-trip.
namespace
{
    // Fake load host: hands back a fixed ticket, reports complete on the 2nd poll of THAT ticket.
    struct SceneLoaderFake
    {
        runtime::SceneLoaderScriptBinding binding;
        int asyncCalls = 0;
        Guid requested;
        int completePolls = 0;
        i32 ticketSeen = -1;
        static constexpr i32 kTicket = 7;

        SceneLoaderFake()
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

TEST_CASE("game-instance: SceneLoader.loadSceneAsync -> ticket, polled to completion (Wren)")
{
    RegisterFoundationTypes();
    runtime::RegisterSceneLoaderScriptFacade();

    RefPtr<script::IScriptManager> manager = draconic::script::wren::CreateScriptManager();
    script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    SceneLoaderFake fake;
    runtime::InstallSceneLoaderScriptService(*ctx, fake.binding);

    // Kick the load, then poll to completion; record the observable results in module globals.
    const Status status =
        ctx->Load(u8"var t = SceneLoader.loadSceneAsync(Guid.new(2748, 3567))\n" // 0xABC, 0xDEF
                  u8"var Poll1 = SceneLoader.loadComplete(t)\n"
                  u8"var Prog = SceneLoader.loadProgress(t)\n"
                  u8"var Poll2 = SceneLoader.loadComplete(t)\n",
                  u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == SceneLoaderFake::kTicket); // the exact ticket round-tripped
    CHECK(ctx->GetGlobal(u8"Poll1").Get<bool>() == false); // first poll: not complete
    CHECK(ctx->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(ctx->GetGlobal(u8"Poll2").Get<bool>() == true); // second poll: complete
}

TEST_CASE("game-instance: SceneLoader.loadSceneAsync -> ticket, polled to completion (AngelScript)")
{
    RegisterFoundationTypes();
    runtime::RegisterSceneLoaderScriptFacade();

    RefPtr<script::IScriptManager> manager = draconic::script::angelscript::CreateScriptManager();
    script::RegisterReflectedTypes(*manager);
    RefPtr<script::IScriptContext> ctx = manager->CreateContext();

    SceneLoaderFake fake;
    runtime::InstallSceneLoaderScriptService(*ctx, fake.binding);

    const Status status = ctx->Load(u8"int t;\n"
                                    u8"bool Poll1; double Prog; bool Poll2;\n"
                                    u8"void main() {\n"
                                    u8"  t = SceneLoader::loadSceneAsync(Guid(0xABC, 0xDEF));\n"
                                    u8"  Poll1 = SceneLoader::loadComplete(t);\n"
                                    u8"  Prog = SceneLoader::loadProgress(t);\n"
                                    u8"  Poll2 = SceneLoader::loadComplete(t);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(fake.asyncCalls == 1);
    CHECK(fake.requested == Guid{0xABC, 0xDEF});
    CHECK(fake.ticketSeen == SceneLoaderFake::kTicket);
    CHECK(ctx->GetGlobal(u8"Poll1").Get<bool>() == false);
    CHECK(ctx->GetGlobal(u8"Prog").Get<f64>() == doctest::Approx(0.5));
    CHECK(ctx->GetGlobal(u8"Poll2").Get<bool>() == true);
}

TEST_CASE("game-instance: a debugger suspension in update is not a fault - the script survives")
{
    RegisterFoundationTypes();
    draconic::script::angelscript::RegisterAngelScriptBackend();

    runtime::GameInstance gi;
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

    draconic::script::IScriptDebugger* debugger = nullptr;
    gi.RunHost().RequestDebugger(Function<void(draconic::script::IScriptDebugger&)>{
        [&debugger](draconic::script::IScriptDebugger& created)
        {
            created.SetBreakpoint(u8"game.as", 4);
            debugger = &created;
        }});
    REQUIRE(debugger != nullptr);

    struct BreakCounter final : draconic::script::IScriptDebuggerListener
    {
        int breaks = 0;
        void OnDebuggerStateChanged(draconic::script::ScriptDebuggerState state) override
        {
            if (state == draconic::script::ScriptDebuggerState::Breakpoint)
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
    RegisterFoundationTypes();
    draconic::script::wren::RegisterWrenScriptBackend();

    const char8_t* src =
        u8"class Game { construct new() {}\n launch() {}\n update(dt) {}\n exit() {}\n}\n";
    runtime::GameInstance a;
    runtime::GameInstance b;
    REQUIRE(a.StartScript(src, u8"game.wren"));
    REQUIRE(b.StartScript(src, u8"game.wren"));

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

TEST_CASE("game-instance: a missing Game class fails to start cleanly")
{
    RegisterFoundationTypes();
    draconic::script::wren::RegisterWrenScriptBackend();

    runtime::GameInstance gi;
    const bool ok = gi.StartScript(u8"var X = 1\n", u8"game.wren");
    CHECK_FALSE(ok); // no `Game` class
    CHECK_FALSE(gi.ScriptRunning());
}

TEST_CASE("game-instance: LoadScene / LoadSceneAsync own the scene load orchestration (task #123)")
{
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();

    FileDelete(u8"draconic_gi_load_db/level.rasset");
    FileDelete(u8"draconic_gi_load_db/level.scene.bin");
    RemoveDirectory(u8"draconic_gi_load_db");
    NativeFileSystem mount(u8"draconic_gi_load_db");

    Guid sceneId;
    {
        // A tiny entities-only scene round-trips with no component managers.
        scene::Scene authored(u8"level");
        (void)authored.CreateEntity(u8"a");
        (void)authored.CreateEntity(u8"b");
        content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        sceneId = inst->Id();
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
    }

    content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");
    resource::ResourceManager resources(db);
    auto* sceneInst = db.GetInstance(sceneId);
    REQUIRE(sceneInst != nullptr);

    SUBCASE("sync LoadScene returns the active, resolved scene")
    {
        runtime::GameInstance gi;
        scene::Scene* s =
            gi.LoadScene(*sceneInst, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        REQUIRE(s != nullptr);
        CHECK(s->EntityCount() == 2u);
        CHECK(gi.Scenes().IsActive(s)); // sync path activates immediately (unchanged behavior)
    }

    SUBCASE("async LoadSceneAsync creates the scene INACTIVE until ActivateLoadedScene")
    {
        runtime::GameInstance gi;
        runtime::SceneLoadHandle handle =
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
        runtime::GameInstance gi;
        runtime::SceneLoadHandle handle =
            gi.LoadSceneAsync(*empty, resources, Function<UniquePtr<IStream>(const Guid&)>{});
        CHECK(handle.Failed());
        CHECK(handle.IsComplete());
        CHECK(handle.Scene() == nullptr);
        CHECK(gi.LoadScene(*empty, resources, Function<UniquePtr<IStream>(const Guid&)>{}) ==
              nullptr);
    }

    SUBCASE("script-load registry: ticket -> poll -> PumpScriptLoads activates + runs the policy")
    {
        runtime::GameInstance gi;
        CHECK_FALSE(gi.SceneReady()); // no current scene at launch (orchestrator-first boot)

        // The app owns the render/sim policy; here a fake records which scene it activated.
        scene::Scene* policyRanOn = nullptr;
        gi.SetSceneActivationPolicy(
            Function<void(scene::Scene*)>{[&](scene::Scene* s) { policyRanOn = s; }});

        // The Game facade hands the in-flight handle to the instance and gets a ticket back.
        runtime::SceneLoadHandle handle =
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
        runtime::GameInstance gi;
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
        runtime::GameInstance gi;
        runtime::SceneLoadHandle handle =
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

    SUBCASE("script-load registry: a successful load is RETIRED on activation (bounded growth)")
    {
        // Fable review finding: m_scriptLoads used to grow monotonically. After activation the
        // entry is dropped, so re-destroying the (now active) scene is a safe no-op and the ticket
        // still reads terminal-safe - the externally observable contract is unchanged.
        runtime::GameInstance gi;
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

    FileDelete(u8"draconic_gi_load_db/level.rasset");
    FileDelete(u8"draconic_gi_load_db/level.scene.bin");
    RemoveDirectory(u8"draconic_gi_load_db");
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GameInstance - the `engine.gameinstance` module.
//
// A single running game as a first-class object: its scene pairing, its script run context, `Game`
// object and error sink, and its instance time scale. The player owns one; the editor owns an
// Array<GameInstance> (several play-in-editor instances and an in-editor headless dedicated
// server). It holds the run bracket the player and the editor's Game tab would otherwise each
// write.
//
// It owns the script run state, its bracket and the instance time-scale term. The caller creates
// the scene and pairs it in with SetScene. The ScriptSubsystem and the app's subsystems are
// borrowed, never owned.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the run facade (RTTI_OBJECT)

export module engine.gameinstance;

export import :networkcontroller; // the run's networking, composed off this god object
export import :save;              // the run's save (RunSave) and the Save facade

import foundation.core;
import engine.domain;
import foundation.scene;
import foundation.shell;
import foundation.scene.resource; // LoadScene / ResolveSceneResources / ResolveScenePrefabs
import foundation.content;        // content::Instance (the cooked scene record)
import foundation.resource;       // ResourceManager and AsyncBindScope (async level load)
import foundation.script;
import foundation.script.resource; // ScriptClass (the cooked Game class a run starts from)
import engine.script;
import foundation.script.facades; // RegisterExtraFacadeName (the run behavior-prelude hook)
import foundation.net.manager; // NetworkManager, INetworkController, NetScriptBinding
import foundation.input;       // ActionRuntime, IInputSourceProvider, InputMap (per-instance input)
import foundation.net.tls;     // the run's TLS backend (https requests, wss joins)
import foundation.http.client; // CreateHttpClient: the platform's client
import foundation.http.script; // HttpScriptBinding: the run's HTTP for its scripts (the Http facade)
import foundation.run;         // RunBase: a GameInstance is its run

using namespace foundation::core;
using namespace foundation;
namespace content = foundation::content;
namespace core = foundation::core;
namespace resource = foundation::resource;

export namespace engine::runtime
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace net = foundation::net;
    namespace scene = foundation::scene;
    namespace script = foundation::script;


    namespace scene = foundation::scene;
    namespace script = foundation::script;
    namespace net = foundation::net;
    namespace input = foundation::input;
    namespace messaging = foundation::messaging; // EventBus

    // NetworkController and its SpawnResolver live in the :networkcontroller partition (re-exported
    // above).

    // ---- run.* script facade: the run and app tier surfaced to scripts, including the running
    // instance's level-load control. It lives here, in the library that owns load orchestration,
    // rather than in the neutral Foundation facade library, the same out-of-tree pattern
    // foundation.net's Net facade uses. A RunScriptBinding is installed as a per-context service;
    // the app fills its pointers, backed by this instance's LoadSceneAsync, ticket registry and the
    // content DB (guid -> cooked scene). Scripts see it in lowercase as `run`, not `Game`: a facade
    // sharing the name of the mandatory `Game` orchestrator class is a hard AngelScript name
    // conflict.

    inline constexpr StringView kRunScriptService = u8"run.runtime";

    // Installed as a per-context script service; the run facade resolves it. The app fills the
    // pointers (backed by the owning GameInstance and content DB). Null pointers are safe no-ops.
    struct RunScriptBinding
    {
        core::Function<i32(const core::Guid&)> loadSceneAsync; // -> ticket (0 = failed to start)
        core::Function<f64(i32)> loadProgress;                 // ticket -> 0..1
        core::Function<bool(i32)> loadComplete;                // ticket -> complete?
        core::Function<bool(i32)> loadFailed;                  // ticket -> failed?
        core::Function<bool(const core::Guid&)> loadScene;     // sync load -> success
        core::Function<bool()> sceneReady;                     // current scene loaded and active?
        core::Function<scene::Scene*()> currentScene;          // the instance's live scene (or null)
        core::Function<void(i32)> requestExit;                 // end the run (standalone stops the loop;
                                                               // the editor stops the Game tab's session)
        core::Function<void(f32)> setTimeScale;                // set the run's scene-group time scale
                                                               // (0 = pause gameplay, 0.5 = slow-mo, 1 =
                                                               // real time); the Game orchestrator keeps
                                                               // running so it can resume.
        core::Function<f32()> timeScale;                       // the run's current scene-group time scale
        // The app's profiler overlay: show or hide it, and whether the app can show one (a build
        // with the profiler, an app that has an overlay). Unwired, nothing shows and it reports
        // unavailable, so a script calls them the same way in every build.
        core::Function<void(bool)> setProfilerOverlay;
        core::Function<bool()> profilerAvailable;
        // This frame's seconds before any time scale, and the seconds of frames since the run
        // started (RunTime): what a pause menu, a banner or a fade times itself by, since
        // update(dt) and coroutine waits stand still at time scale 0. The GameInstance writes them.
        f32 realDeltaSeconds = 0.0f;
        f64 realSeconds = 0.0;
        // This run's event bus; the same service carries the load controls and the run bus.
        // run.events() publishes here. The GameInstance fills it with &Events(), the run's bus.
        messaging::EventBus* runEvents = nullptr;
    };

    inline void InstallRunScriptService(script::IScriptContext& scriptContext, RunScriptBinding& binding)
    {
        scriptContext.SetService(kRunScriptService, &binding);
    }
    inline void ClearRunScriptService(script::IScriptContext& scriptContext)
    {
        scriptContext.SetService(kRunScriptService, nullptr);
    }

    // ---- run.* script facade: the run and app tier surfaced to scripts. A static facade bound to
    // scripts in lowercase as `run` (the ScriptName alias). run.events() returns the run-bus handle
    // (publish to the run tier); the load methods control this instance's level loads. Nothing else
    // may bind the name `run` (the FinalizeTypes collision check enforces it).

    // A bound run-event-bus handle: `run.events().emit("Delivered", n)` publishes a named event
    // onto this run's bus (deferred; delivered at the next run-bus drain). Shaped like SceneEvents;
    // the payload is a Variant, the one type that crosses between C++ and script and between
    // AngelScript and Luau. A value type.
    struct RunEvents
    {
        messaging::EventBus* bus = nullptr; // not reflected; resolved from the run context at run.events()
        void emit(core::String name) const;
        void emit(core::String name, core::Variant payload) const;
    };

    /// run.*: the run and app tier. `run.events()` is the run-bus handle; `loadSceneAsync` starts
    /// an async scene load and returns a ticket the script polls: `var t = run.loadSceneAsync(id);
    /// while (!run.loadComplete(t)) yield`. loadScene is a synchronous convenience; sceneReady
    /// reports whether the instance's current scene is live. A static facade (resolved per call
    /// through CurrentScriptContext), bound to scripts as `run` through ScriptName. It resolves its
    /// own per-context service; unwired, every call is a safe no-op (an empty RunEvents whose emit
    /// does nothing; loadComplete returns true so a poll never hangs).
    class Run final : public Object
    {
        RTTI_OBJECT(Run, Object)
    public:
        [[nodiscard]] static RunScriptBinding* Resolve()
        {
            script::IScriptContext* scriptContext = script::CurrentScriptContext();
            return scriptContext != nullptr
                       ? static_cast<RunScriptBinding*>(scriptContext->GetService(kRunScriptService))
                       : nullptr;
        }
        [[nodiscard]] static RunEvents events()
        {
            RunScriptBinding* b = Resolve();
            RunEvents handle;
            handle.bus = (b != nullptr) ? b->runEvents : nullptr;
            return handle;
        }
        [[nodiscard]] static i32 loadSceneAsync(core::Guid scene)
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->loadSceneAsync && !scene.IsNil()) ? b->loadSceneAsync(scene)
                                                                         : 0;
        }
        [[nodiscard]] static f64 loadProgress(i32 ticket)
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->loadProgress) ? b->loadProgress(ticket) : 1.0;
        }
        [[nodiscard]] static bool loadComplete(i32 ticket)
        {
            RunScriptBinding* b = Resolve();
            return (b == nullptr || !b->loadComplete) ? true : b->loadComplete(ticket);
        }
        [[nodiscard]] static bool loadFailed(i32 ticket)
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->loadFailed) ? b->loadFailed(ticket) : false;
        }
        [[nodiscard]] static bool loadScene(core::Guid scene)
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->loadScene && !scene.IsNil()) ? b->loadScene(scene) : false;
        }
        [[nodiscard]] static bool sceneReady()
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->sceneReady) ? b->sceneReady() : false;
        }
        /// The instance's current scene as a bound Scene handle: the orchestrator's way to
        /// `scene.spawn/find`, since it has no entity to reach a scene through. Resolved at the
        /// call from instance state, so it is the same whoever invoked the script. Before a scene
        /// exists, or unwired, it is a null-scene handle whose methods are safe no-ops.
        [[nodiscard]] static script::Scene currentScene()
        {
            RunScriptBinding* b = Resolve();
            script::Scene handle;
            handle.scene = (b != nullptr && b->currentScene) ? b->currentScene() : nullptr;
            return handle;
        }
        /// run.requestExit(code): end the run with an exit code (default 0). Routes to the app
        /// host: standalone stops the loop; the editor's embedded host stops the Game tab's play
        /// session. Unwired, a safe no-op.
        static void requestExit(i32 code)
        {
            RunScriptBinding* b = Resolve();
            if (b != nullptr && b->requestExit)
            {
                b->requestExit(code);
            }
        }
        /// Arity-0 convenience: exit with code 0 (the C++ host's default). Exposed as its own
        /// overload so `run::requestExit()` and `run.requestExit()` bind on both backends.
        static void requestExit() { requestExit(0); }

        /// run.setTimeScale(scale): scale the run's gameplay time. 0 pauses the scene (behaviors
        /// and physics freeze; the timer stops), 0.5 is slow motion, 1 is real time; the general
        /// pause is setTimeScale(0). It scales the run's scene group, not the whole context, so the
        /// Game orchestrator (this script tier) keeps running to resume, and the editor is
        /// unaffected. Unwired, a safe no-op.
        static void setTimeScale(f32 scale)
        {
            RunScriptBinding* b = Resolve();
            if (b != nullptr && b->setTimeScale)
            {
                b->setTimeScale(scale);
            }
        }
        /// The run's current gameplay time scale (1 if unwired).
        [[nodiscard]] static f32 timeScale()
        {
            RunScriptBinding* b = Resolve();
            return (b != nullptr && b->timeScale) ? b->timeScale() : 1.0f;
        }
        /// This frame's seconds before any time scale: the clock that runs while the game is
        /// paused, for a pause menu or a fade (0 if unwired).
        [[nodiscard]] static f32 realDeltaTime()
        {
            RunScriptBinding* b = Resolve();
            return b != nullptr ? b->realDeltaSeconds : 0.0f;
        }
        /// Seconds of frames since the run started, unscaled (0 if unwired).
        [[nodiscard]] static f64 realTime()
        {
            RunScriptBinding* b = Resolve();
            return b != nullptr ? b->realSeconds : 0.0;
        }
        /// run.setProfilerOverlay(shown): show or hide the app's profiler overlay (frame times, the
        /// longest scopes and GPU passes, the counters). The app decides what that means; in a
        /// build without the profiler (Shipping) nothing shows. Unwired, a safe no-op.
        static void setProfilerOverlay(bool shown)
        {
            RunScriptBinding* b = Resolve();
            if (b != nullptr && b->setProfilerOverlay)
            {
                b->setProfilerOverlay(shown);
            }
        }
        /// Whether setProfilerOverlay can show anything here: false in Shipping and where the app has
        /// no overlay, so a debug menu can hide its entry (false if unwired).
        [[nodiscard]] static bool profilerAvailable()
        {
            RunScriptBinding* b = Resolve();
            return b != nullptr && b->profilerAvailable && b->profilerAvailable();
        }
    };

    // Registers the `Run` facade (bound to scripts as `run` through ScriptName), its RunEvents
    // handle type and the behavior-prelude name. Idempotent; call before a script manager is
    // created (the run host and the cook builder do).
    void RegisterRunScriptFacade();

    // Handle for an in-flight async scene load. LoadSceneAsync creates the scene inactive (not
    // ticked or rendered) and starts its resources on workers; poll IsComplete() and Progress() to
    // drive a loading screen, then call GameInstance::ActivateLoadedScene() to bring it live.
    // Scene() is the pending scene; its resources are ready only once IsComplete(). A small value
    // type, safe to copy and store.
    //
    // Limitation: IsComplete and Progress poll ResourceManager::PendingCount, which counts the
    // whole manager, and m_total snapshots that global count. Two overlapping loads (or any
    // unrelated BindAsync in flight) conflate: each handle waits on all pending work (the safe
    // direction) and progress is distorted. That is fine for boot and single level switches;
    // overlapping or background loads need per-batch tracking, which resource::AsyncLoadBatch
    // provides.
    class SceneLoadHandle
    {
    public:
        SceneLoadHandle() = default;

        [[nodiscard]] bool Failed() const noexcept { return m_failed; }
        [[nodiscard]] scene::Scene* Scene() const noexcept { return m_scene; }
        [[nodiscard]] bool IsComplete() const noexcept
        {
            return m_failed || m_resources == nullptr || m_resources->PendingCount() == 0;
        }
        // 0..1; 1 when complete or failed.
        [[nodiscard]] core::f32 Progress() const noexcept
        {
            if (m_failed || m_resources == nullptr || m_total == 0)
            {
                return 1.0f;
            }
            const core::usize remaining = m_resources->PendingCount();
            if (remaining == 0)
            {
                return 1.0f;
            }
            const core::usize done = (remaining >= m_total) ? 0u : (m_total - remaining);
            return static_cast<core::f32>(done) / static_cast<core::f32>(m_total);
        }

    private:
        friend class GameInstance;
        scene::Scene* m_scene = nullptr;
        resource::ResourceManager* m_resources = nullptr;
        core::usize m_total = 0;
        bool m_failed = false;
    };

    // A running game, and its run: it is its own IRun, through RunBase (its id, name, sessions,
    // event bus, and the services below registered on it). Its networking is composed: GameInstance
    // holds a NetworkController (which is the INetworkController the Net facade drives) and
    // forwards to it. The controller is a stable member, so its binding never dangles; the endpoint
    // inside it comes and goes.
    class GameInstance final : public foundation::run::RunBase
    {
    public:
        GameInstance() : RunBase(DefaultAllocator(), u8"Game")
        {
            // Every scene this run creates borrows the run's bus: set it on the group so
            // CreateScene injects it before the script systems bind (OnSceneCreate), not after.
            m_sceneManager.SetSceneEventBus(&Events());
            // This instance is the run: its scenes and its scripts carry it, so run-scoped systems
            // (audio) group them by it.
            m_sceneManager.SetSceneRun(this);
            m_runHost.Binding().run = this;
            // Every context of this run reads this run's services (run.*, its input, its save), a
            // behavior context as much as a game script's: a project with no game script still has
            // input.
            m_runHost.SetOwnerServices(core::Function<void(script::IScriptContext&)>{
                [this](script::IScriptContext& scriptContext) { InstallRunServices(scriptContext); }});
            // The run's HTTP client is made on its scripts' first request, and https and a wss
            // join use the run's TLS backend, made when first needed.
            m_http.SetFactory([this]() { return foundation::http::CreateHttpClient(*m_allocator, TlsBackend()); });
            m_network.SetTlsBackendSource([this]() { return TlsBackend(); });
            // What the run owns and others reach through it (run.Get<T>()): borrowed, members all.
            Register<GameInstance>(*this);
            Register<NetworkController>(m_network);
            Register<foundation::net::INetworkController>(m_network); // how engine.net reaches it (below this library)
            Register<input::ActionRuntime>(m_inputRuntime);
            Register<RunSave>(m_save);
            Register<engine::script::ScriptRunHost>(m_runHost);
            Register<foundation::http::HttpScriptBinding>(m_http);
        }

        // Disposed here, while the members registered on it still exist (their stop hooks run,
        // and the run's manager hears it go).
        ~GameInstance() override { Dispose(); }

        /// The platform's TLS backend for this run (Mbed TLS natively; none in a browser, which
        /// does TLS itself), made on first use.
        [[nodiscard]] foundation::net::ITlsBackend* TlsBackend();
        /// This run's HTTP for its scripts (pumped by TickScript, reset by StopScript).
        [[nodiscard]] foundation::http::HttpScriptBinding& Http() noexcept { return m_http; }

        /// This run's services on a script context: run.* (and its event bus), the run's own
        /// input runtime (over the shared one the app's configurator installs) and its save.
        void InstallRunServices(script::IScriptContext& scriptContext);

        void SetScene(scene::Scene* scene) noexcept
        {
            m_scene = scene;
            m_network.SetReplicatedScene(scene); // keep replication on the current scene across loads
            if (scene != nullptr)
            {
                scene->SetEventBus(&Events()); // the adopt path shares this run's bus
                scene->SetRun(this);
            }
        }
        [[nodiscard]] scene::Scene* GetScene() const noexcept { return m_scene; }

        void SetScriptErrorHandler(script::IScriptErrorHandler* handler) noexcept
        {
            m_errorHandler = handler;
        }
        [[nodiscard]] script::IScriptErrorHandler* ScriptErrorHandler() const noexcept
        {
            return m_errorHandler;
        }

        /// Headless: simulate and run scripts, but the host does not render this instance (no
        /// camera or swapchain needed). Used for the in-editor dedicated server.
        void SetHeadless(bool headless) noexcept { m_headless = headless; }
        [[nodiscard]] bool IsHeadless() const noexcept { return m_headless; }

        /// This run's global time scale: the `instance` term in the time model (dt a scene sees =
        /// host dt x context scale x instance scale x scene scale). Defaults to 1.
        void SetInstanceTimeScale(f32 scale) noexcept { m_instanceTimeScale = scale; }
        [[nodiscard]] f32 InstanceTimeScale() const noexcept { return m_instanceTimeScale; }

        /// Compile and launch the `Game` script (a class with launch()/update(dt)/exit()) on this
        /// instance's run host. The host must be configured first (the app's
        /// ScriptSubsystem::ConfigureRunHost exposes the facades and routing); a bare test only
        /// needs a backend registered. Restarting stops a prior run first. Returns false if it
        /// fails to compile or has no `Game`.
        bool StartScript(core::StringView source, core::StringView name)
        {
            return StartScript(source, name, core::Span<const core::String>{});
        }

        /// As above, plus the Game class's declared handler names (from the cooked ScriptClass) so
        /// the Game tier's on<Event> inbox can subscribe to the run bus. A caller with no handler
        /// list (raw-source tests) gets a Game with no inbox.
        bool StartScript(core::StringView source, core::StringView name,
                         core::Span<const core::String> gameHandlers);

        /// The cooked `Game` class: its source, its file identity and its declared handlers, so its
        /// on<Event> inbox hears the run bus. Every host that starts a cooked game (the player, the
        /// editor's Game tab) starts it this way.
        bool StartScript(const foundation::script::ScriptClass& scriptClass)
        {
            return StartScript(scriptClass.source.AsView(), scriptClass.sourceName.AsView(),
                               core::Span<const core::String>(scriptClass.handlers.Data(),
                                                              scriptClass.handlers.Size()));
        }

        /// exit() the `Game` and release the game-script hold (idempotent; the update-fault path
        /// lands here). The run host tears down when nothing else pins it (the scene-stop observer
        /// drives that).
        void StopScript();

        /// Create a scene in this instance's group and bind its behaviors to this instance's run
        /// host. Use this instead of Scenes().CreateScene so the re-bind happens. `activate`
        /// (default true) makes it active at once; false creates it inactive for an async load (see
        /// LoadSceneAsync).
        scene::Scene* CreateScene(core::StringView name, bool activate = true);
        /// Destroy a scene in this instance's group. Drops any tracked async load whose pending
        /// target is this scene before freeing it, since the handle holds a raw Scene* and
        /// PumpScriptLoads must never activate a dangling one. A dropped ticket then reads
        /// terminal-safe (complete=true, failed=false) through the unknown-ticket fallback.
        void DestroyScene(scene::Scene* scene)
        {
            for (core::usize i = m_scriptLoads.Size(); i > 0; --i)
            {
                if (m_scriptLoads[i - 1].handle.Scene() == scene)
                {
                    m_scriptLoads.RemoveAt(i - 1);
                }
            }
            // If this is the replicated scene, clear the cache and detach its NetworkSceneSystem
            // before it is freed, so a later Start/Connect never wires a dead scene and the live
            // endpoint stops replicating it.
            if (scene == m_scene)
            {
                SetScene(nullptr);
            }
            m_sceneManager.DestroyScene(scene);
        }

        /// Tear down all of this instance's scenes for a run stop that keeps the instance alive
        /// (the editor Game tab stopping between plays). Drops every in-flight tracked script load
        /// first, since the handles hold a raw Scene* into the group and a later PumpScriptLoads
        /// must not activate a freed scene; then clears the current-scene pointer (net replication
        /// follows) and destroys the whole group (aware subsystems are notified). ReleaseInstance
        /// and OnShutdown free the entire instance, so they only need Scenes().Clear(); this is for
        /// the persistent-instance stop.
        void ClearScenes()
        {
            m_scriptLoads.Clear();
            SetScene(nullptr);
            m_sceneManager.Clear();
            // The group time scale is run-scoped (run.setTimeScale): the SceneManager survives a
            // stop on the persistent editor instance, so a game paused at Stop must not leave the
            // next Play frozen at scale 0.
            m_sceneManager.SetTimeScale(1.0f);
        }

        // ---- scene and level load: the load orchestration lives on the instance (the
        // user-controllable unit), so the player and the editor's Game page share it. The app still
        // owns the policy: what to load, and EnsureCamera, Start and SetScene afterwards.

        /// Load a cooked scene instance into this group and resolve its resources synchronously.
        /// Returns the loaded, active, fully resolved scene, not yet started: the caller runs its
        /// policy (EnsureCamera, Start, SetSimulationEnabled, SetScene). Null on a LoadScene
        /// failure. `prefabProvider` reads a nested-prefab payload by guid (an empty function means
        /// no prefabs).
        scene::Scene*
        LoadScene(content::Instance& sceneInstance, resource::ResourceManager& resources,
                  core::Function<core::UniquePtr<core::IStream>(const core::Guid&)> prefabProvider);

        /// Async load: creates the scene inactive, deserializes it, and starts its resource binds
        /// on workers (BindAsync). Returns a SceneLoadHandle: poll IsComplete()/Progress() for a
        /// loading screen, then call ActivateLoadedScene(). The scene never ticks or renders while
        /// loading.
        [[nodiscard]] SceneLoadHandle LoadSceneAsync(
            content::Instance& sceneInstance, resource::ResourceManager& resources,
            core::Function<core::UniquePtr<core::IStream>(const core::Guid&)> prefabProvider);

        /// Activate a completed async-loaded scene (add it to the active and render set and make it
        /// current). Returns the now-active scene, or null if the handle failed or is not yet
        /// complete. The caller then runs the same policy as the sync path (EnsureCamera, Start,
        /// ...).
        scene::Scene* ActivateLoadedScene(SceneLoadHandle& handle);

        // ---- script-driven scene loads: the run facade's model of loadSceneAsync, a ticket, then
        // polling. The instance owns the in-flight handles keyed by ticket; the app drives
        // completion each frame through PumpScriptLoads, which activates a finished load, makes it
        // the current scene (SetScene), then runs the app's render and simulation policy hook. This
        // sits on the instance rather than the app so an orchestrator script that launched with no
        // scene resolves its loads through its own instance's host.

        /// Makes a just-loaded, active scene the run's level: the instance's bookkeeping (current
        /// scene, net replication, the scene manager's current), then the app's policy, then the
        /// level it replaces is destroyed. Left alive, the old level would keep ticking unseen
        /// beside the new one (its player reading input, its events reaching the run bus), and
        /// every restart would stack one more. A behaviour that loads a level from inside the scene
        /// it replaces is safe: the scene manager defers a destroy asked for during its update to
        /// the end of that update. The async pump and the synchronous run.loadScene both use this.
        void AdoptLoadedScene(scene::Scene* activated);

        /// The post-activation policy the app owns (EnsureCamera, Start, SetSimulationEnabled: the
        /// render and simulation half; SetScene is the instance's own bookkeeping and runs first).
        /// Set once by the app; PumpScriptLoads invokes it on a scene the moment its tracked load
        /// completes.
        using SceneActivationPolicy = core::Function<void(scene::Scene*)>;
        void SetSceneActivationPolicy(SceneActivationPolicy policy)
        {
            m_activatePolicy = core::Move(policy);
        }

        /// Register an in-flight async load under a fresh ticket (1-based; 0 is never issued, so it
        /// also means "failed to start"). The run facade hands this ticket to the script;
        /// ScriptLoad{Progress,Complete,Failed} query by it. The handle is a value type (stored).
        ///
        /// The handle holds a raw Scene*. DestroyScene and ClearScenes drop the matching tracked
        /// loads before freeing a scene; any other path that destroys a pending scene must do the
        /// same, or PumpScriptLoads would activate freed memory.
        i32 TrackScriptLoad(SceneLoadHandle handle);

        /// Drive tracked script loads: any whose resources finished get ActivateLoadedScene,
        /// SetScene and the activation policy, exactly once. Call each frame (the app's OnUpdate,
        /// after the resource pump). Cheap when nothing is in flight.
        void PumpScriptLoads();

        /// Ticket queries for the run facade. An unknown ticket reads Progress 1, Complete true (a
        /// bad or expired ticket never hangs a `while (!complete) yield` loop), Failed false.
        [[nodiscard]] f32 ScriptLoadProgress(i32 ticket) const;
        [[nodiscard]] bool ScriptLoadComplete(i32 ticket) const; // true once terminal (live or failed)
        [[nodiscard]] bool ScriptLoadFailed(i32 ticket) const;

        /// run.sceneReady: this instance has a live current scene. A script on the app-driven
        /// boot path waits `while (!run.sceneReady()) yield` instead of assuming a scene at
        /// launch().
        [[nodiscard]] bool SceneReady() const noexcept { return m_scene != nullptr; }

        /// The run facade's per-context binding for this instance. The app fills its pointers
        /// (backed by LoadSceneAsync, the ticket registry and the content DB); GameInstance
        /// installs it on the run context when the game script starts. A stable member, so it never
        /// dangles.
        [[nodiscard]] RunScriptBinding& RunBinding() noexcept
        {
            return m_runBinding;
        }

        /// Tick the `Game` script with gameplay time: hostDt x contextScale x instanceScale x
        /// sceneScale. A faulting update disables this instance's script (drops the `Game`), not
        /// the app.
        void TickScript(f32 hostDeltaTime, f32 contextTimeScale);

        /// Drive this instance's run host each frame: advance the script binding clock
        /// (Time.now/delta) and step its GC. The subsystem drives its own (editor) host; each
        /// instance drives its own.
        void DriveRunHost(f32 deltaTime);

    private:
        // A fault in `handler`: the script stops, and the reason is kept (ScriptFault).
        void FaultScript(core::StringView handler);

    public:

        [[nodiscard]] bool ScriptRunning() const noexcept { return m_game.Get() != nullptr; }
        /// Reads a property of the running game script (a score, a lives count). NotFound with
        /// no script running or no such property.
        [[nodiscard]] core::Result<core::Variant> GetScriptProperty(core::StringView name) const
        {
            if (m_game.Get() == nullptr)
            {
                return core::Err(core::ErrorCode::NotFound);
            }
            return m_game->GetProperty(name);
        }

        /// Why the game script stopped on its own: the handler it faulted in and the error the
        /// run reported, or that it did not compile or instantiate. Empty while it runs or after
        /// a clean stop; a start clears it.
        [[nodiscard]] core::StringView ScriptFault() const noexcept { return m_scriptFault.AsView(); }
        /// Seconds of frames since ResetRunClock: the host's delta, not scaled by the context's,
        /// the instance's or the scene's time scale, so it keeps going while the game pauses its
        /// scene at time scale 0 (a menu over a paused scene still takes clicks); a scripted
        /// playtest times its input by it. Stands still while the debugger holds the run.
        [[nodiscard]] f64 RunTime() const noexcept { return m_runTime; }
        void ResetRunClock() noexcept
        {
            m_runTime = 0.0;
            m_runBinding.realDeltaSeconds = 0.0f;
            m_runBinding.realSeconds = 0.0;
        }
        [[nodiscard]] script::IScriptContext* ScriptContext() const noexcept
        {
            return m_scriptContext.Get();
        }

        /// This run's script host: the gameplay context shared by the game script and this
        /// instance's scenes' behaviors (one gameplay context per instance). Owned here; the
        /// ScriptSubsystem borrows it, so each instance in an Array<GameInstance> runs its own.
        [[nodiscard]] engine::script::ScriptRunHost& RunHost() noexcept { return m_runHost; }

        /// This run's scene group: the set of scenes the run manages and its current scene, ticked
        /// on the Context lane once registered with the SceneSubsystem. Wire its aware-registry
        /// (from the SceneSubsystem) before creating scenes in it.
        [[nodiscard]] scene::SceneManager& Scenes() noexcept { return m_sceneManager; }

        /// This run's one event bus: app-owned, injected into every scene this instance creates or
        /// adopts (Scene::SetEventBus), so `scene.events` and the run bus are the same object. The
        /// Game tier's on<Event> inbox subscribes here, and a behavior in any of the run's scenes
        /// emits straight onto it with no relay in between (a Level may still re-emit an event to
        /// translate it). The instance drains it once per frame (DrainRunEvents); the borrowing
        /// scenes do not. The run's bus is IRun::Events().

        /// Deliver this frame's queued run-bus events. Called on the instance tick after TickScript
        /// (no VM call active), so a subscriber may dispatch script handlers directly.
        /// Cascade-bounded like the scene bus; safe with no game script (native run-bus subscribers
        /// still fire).
        void DrainRunEvents() { Events().Drain(); }

        // ---- input (this instance's own action runtime, the input counterpart of the per-instance
        // scene group and net endpoint) ----

        /// This run's input source (the editor Game tab's gated viewport, or the player's shell
        /// devices). The host sets it; the per-instance runtime reads only this source, so
        /// per-surface focus gating isolates input across Game tabs (only the focused tab's source
        /// reports keys). Null means no input.
        void SetInputSource(input::IInputSourceProvider* source) noexcept
        {
            m_inputSource = source;
        }
        [[nodiscard]] input::IInputSourceProvider* InputSource() const noexcept { return m_inputSource; }

        /// Install the action map (the game's controls, from the project's input-map asset) on this
        /// instance's runtime. Each instance has its own runtime and its own copy of the map.
        void SetInputMap(const input::InputMap& map) { m_inputRuntime.SetMap(map); }

        /// This run's action runtime. The Input facade resolves it per script context (installed
        /// into this instance's context, so instance A's script never sees instance B's keys).
        [[nodiscard]] input::ActionRuntime& InputRuntime() noexcept { return m_inputRuntime; }

        /// Evaluate this instance's action runtime against its source (call before TickScript so the game
        /// sees this frame's input). No-op when no source is set.
        void DriveInput(f32 deltaTime, f32 contextTimeScale);

        // ---- save (the values this game keeps between runs) ----

        /// Names this run's save file and reads it (the player's user data directory, the editor's
        /// project Editor/ folder). The run's scripts reach it through the Save facade, and the run
        /// writes it when its script stops if anything changed. Empty: values kept for the run only.
        void SetSaveFile(core::StringView path) { m_save.Open(path); }
        [[nodiscard]] RunSave& Saves() noexcept { return m_save; }

        // ---- networking (this instance's NetworkController; the forwards below) ----

        /// This instance's networking controller (the endpoint owner and INetworkController), for
        /// callers that reach it directly; GameInstance's own net methods below are thin forwards
        /// to it.
        [[nodiscard]] NetworkController& Network() noexcept { return m_network; }

        // The prefab net-spawn resolver factory is injected once through
        // Network().SetSpawnResolverFactory(...); the controller makes one per endpoint.

        // The per-frame transport pump lives in engine::net::NetworkSubsystem::PostUpdate: the
        // subsystem enumerates live endpoints (through an app-provided source) and drives
        // UpdateTransport.

        // Net facade role controls, forwarded to the composed controller. StartServer/Connect open
        // a real UDP socket and enter the role (false if that fails); StopNetworking drops the
        // endpoint.
        bool StartServer(u16 port, bool dedicated) { return m_network.StartServer(port, dedicated); }
        bool Connect(core::StringView host, u16 port) { return m_network.Connect(host, port); }
        void StopNetworking() { m_network.StopNetworking(); }
        [[nodiscard]] net::NetworkManager* NetEndpoint() const noexcept { return m_network.NetEndpoint(); }

    private:
        /// The run-bus sink for the Game tier: dispatch `on<Event>(payload)` to the Game object.
        /// Called at run-bus drain time (no VM active), so it invokes directly; a faulting handler
        /// disables the game (m_game = nullptr) as a faulting update() does, and a debugger
        /// suspension is not a fault.
        void DispatchGameEvent(core::StringView eventName, const core::Variant& payload);

        engine::script::ScriptRunHost m_runHost{
            foundation::core::DefaultAllocator()}; // owned: the game's script context (from the
                                                   // process root allocator)
        scene::SceneManager m_sceneManager{DefaultAllocator()}; // owned; registered with the SceneSubsystem to tick
        scene::Scene* m_scene = nullptr;
        script::IScriptErrorHandler* m_errorHandler = nullptr;
        bool m_headless = false;
        f32 m_instanceTimeScale = 1.0f;
        core::RefPtr<script::IScriptContext>
            m_scriptContext; // the game script's ref to the run host's context
        core::RefPtr<script::ScriptObject> m_game;
        core::String m_scriptFault; // why the game script stopped on its own (ScriptFault)
        f64 m_runTime = 0.0;        // unscaled seconds of the run's frames, game script or not (RunTime)
        engine::script::ScriptEventSubscriptions m_gameEventSubs; // Game tier's run-bus on<Event> inbox

        core::IAllocator* m_allocator = &DefaultAllocator(); // what the run's TLS and HTTP are made from
        // Before the networking and the HTTP binding, so it outlives every session they made from it.
        core::UniquePtr<foundation::net::ITlsBackend> m_tls; // made on first use (TlsBackend)
        NetworkController m_network; // this run's networking (endpoint and INetworkController)
        RunScriptBinding
            m_runBinding; // stable; app fills its pointers, installed per context (StartScript)

        input::ActionRuntime m_inputRuntime; // this run's action state (per-instance)
        RunSave m_save{DefaultAllocator()};  // this run's save, installed per context (StartScript)
        foundation::http::HttpScriptBinding m_http;          // installed per context; its client on first use
        void TraceInput(); // ENV_INPUT_TRACE: the input path, logged twice a second
        input::IInputSourceProvider* m_inputSource =
            nullptr; // borrowed: the viewport / shell devices

        // Script-driven scene loads: in-flight handles keyed by ticket. An entry lives only while
        // its load is in flight or has failed; PumpScriptLoads retires an entry the moment it
        // activates. A retired ticket reads terminal-safe through the unknown-ticket fallback
        // (progress 1, complete true, failed false), which is the post-activation answer anyway, so
        // dropping it is invisible to the script. Failed entries stay so ScriptLoadFailed remains
        // truthful; failures are rare, so the array stays bounded across level switches.
        // m_nextScriptTicket only grows, so tickets never alias.
        struct TrackedScriptLoad
        {
            i32 ticket = 0;
            SceneLoadHandle handle;
        };
        core::Array<TrackedScriptLoad> m_scriptLoads;
        i32 m_nextScriptTicket = 0;
        SceneActivationPolicy m_activatePolicy; // app-set render/sim policy, run on completion
    };

}

export namespace engine::runtime
{
    /// This domain's declaration: what it brings to a scene, to reflection and to the script
    /// surface, and which resource modules come with it. Defined in the implementation unit (one
    /// instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& RunDomain() noexcept;
}

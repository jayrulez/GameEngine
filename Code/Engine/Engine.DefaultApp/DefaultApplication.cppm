// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the `engine.defaultapp` module.
//
// DefaultApplication: an opinionated IApplication base that registers the standard
// engine subsystems. A game that wants the batteries-included engine writes
// `class MyGame : DefaultApplication` and adds its own subsystems in Configure
// (calling the base first); a game that wants only its own subsystems implements
// IApplication directly and links none of this.
//
// This lives in its OWN library - separate from foundation.runtime.client - precisely
// so the base client never pulls in the engine subsystem libraries. It registers ALL
// standard gameplay subsystems (the editor embeds THIS same class
// against its runtime context, so subsystem registration lives here, not in entry
// points) and owns the GAME-SCRIPT lifecycle (the `Game` class bracket) - the
// player and the editor's Game tab both consume it instead of hand-rolling copies.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module engine.defaultapp;
export import :screenshot; // ScreenshotCapture + the --screenshot flags

import foundation.core;
import foundation.rhi;
import foundation.runtime.client;       // IApplication, IApplicationHost
import engine.gameinstance; // GameInstance - this app's running game (scene + script bracket)
import foundation.shell;                // IShell, IKeyboard, KeyCode (the profile-dump hotkey)
import foundation.graphics;             // GraphicsDevice, FrameContext
import foundation.vfs;                  // the data root: ResolveDataRoot + the NativeFileSystem mounted over it
import foundation.scene;                // Scene
import engine.scene;      // SceneSubsystem (the standard scene driver)
import engine.render;     // RenderSubsystem (the standard renderer)
import engine.animation; // AnimationSubsystem (drives skeletal animation from the scene)
import engine.particles; // ParticleSubsystem (scene-driven CPU sim)
import foundation.physics;             // ContactKind/EntityContact (the contact bridge)
import engine.physics;   // PhysicsSubsystem (Jolt worlds + interpolation)
import foundation.input;               // the action model/runtime
import engine.input;     // InputSubsystem + the Input facade
import foundation.script;              // IScriptManager/Context (the game script)
import foundation.script.resource;     // ScriptClass (the cooked game class StartGameScript takes)
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript; // the AngelScript backend (second backend; OPTION_ENABLE_ANGELSCRIPT)
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;         // the Luau backend (OPTION_ENABLE_LUAU)
#endif
import engine.script;    // ScriptSubsystem (behaviors + the run's shared context)
import engine.integration; // ScriptPhysicsContactBridge (physics contacts -> script ingress)
import foundation.resource;            // ResourceManager (owned or borrowed - see the preset seam)
import foundation.content;             // IContentDatabase (preset by the entry point)
import foundation.scene.resource;      // SceneDocument (product-type registration)
import engine.ui;        // the game screen tier (canvases + overlay + consumption)
import engine.ui.script;   // the `ui` script facade + its per-context service binding
import foundation.ui.gamekit; // ScreenStack (the binding points at UISubsystem::Screens())
import engine.composition; // FullSceneComposition (the single source of truth for scene assembly)
import foundation.audio;               // AudioEngine (owned by the audio subsystem)
import engine.audio;     // AudioSubsystem (voices/buses/one-shots + scene sync)
import foundation.net;                 // UdpSocket / DatagramEndpoint (the transport)
import foundation.net.replication;     // NetworkId / StateReplication (the spawn-handler seam)
import foundation.net.manager;   // NetworkManager + NetworkStartup/StartNetworking + the Net facade
import engine.net; // NetworkSubsystem (injects the NetworkComponentManager into scenes)
import foundation.profiler;      // the CPU scope profiler (P-key dump)

namespace core = foundation::core;
namespace net = foundation::net;   // NetworkManager + NetworkStartup + the Net facade
using namespace foundation::runtime; // foundation runtime: IApplication/IApplicationHost/Subsystem/Context
using namespace foundation::shell; // IShell + input/window types (moved from foundation::runtime)
using namespace foundation::graphics; // GraphicsDevice/RenderWindow/FrameContext (moved from foundation::runtime)

export namespace engine::runtime
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace net = foundation::net;

    class DefaultApplication : public IApplication, public foundation::scene::ISceneObserver
    {
    public:
        // Press P to print the previous frame's CPU scope tree + per-pass GPU timing. A game
        // subclass that overrides OnUpdate should call DefaultApplication::OnUpdate(host, dt) to
        // keep the hotkey. (Reads the GPU timestamps after a device stall - fine for an on-demand dump.)
        void OnUpdate(IApplicationHost& host, core::f32 deltaTime) override;

        // Ticks the game script with GAMEPLAY time: dt x context scale x the primary
        // scene's scale (per-scene time, H1). Subclasses overriding OnUpdate call the
        // base to keep the script (and the profile hotkey) alive.
        void TickGameScript(IApplicationHost& host, core::f32 deltaTime);

        // Registers ALL standard engine subsystems. A game subclass overrides this,
        // calls DefaultApplication::Configure(host) first, then adds its own. Entry
        // points (player, editor) do NOT register gameplay subsystems - this is the
        // one place.
        void Configure(IApplicationHost& host) override;

        [[nodiscard]] engine::script::ScriptSubsystem* Scripts() const noexcept;

        /// The app's running game (N=1 today). The launch flow creates the game scene in its
        /// Scenes() manager so it groups + ticks + renders as this run's scenes.
        [[nodiscard]] GameInstance& Instance() noexcept { return m_instance; }

        /// The primary instance's scene group - a scene created here renders (the app renders instance
        /// scenes; there is no default manager). Returns SceneManager& directly so callers need not
        /// name GameInstance.
        [[nodiscard]] foundation::scene::SceneManager& PrimaryScenes() noexcept;

        /// Create an ADDITIONAL running game (multi-instance PIE / an in-editor headless dedicated
        /// server). Wired like the primary - its scene group ticks
        /// on the Context lane and its run host gets the app services. Stable address (UniquePtr), so the
        /// SceneSubsystem's borrowed manager pointer stays valid. Returns null before Configure ran.
        [[nodiscard]] GameInstance* CreateInstance(bool headless = false);

        /// Destroy an EXTRA instance (a "Play New Instance" tab closing). Unregisters its scene manager
        /// from the SceneSubsystem (else a dangling borrowed pointer is ticked/rendered), tears down its
        /// run host, and frees it. The PRIMARY instance is permanent (a member) - a no-op for it.
        void ReleaseInstance(GameInstance* instance);

        [[nodiscard]] engine::input::InputSubsystem* Input() const noexcept { return m_input; }
        [[nodiscard]] engine::physics::PhysicsSubsystem* Physics() const noexcept;
        [[nodiscard]] engine::audio::AudioSubsystem* Audio() const noexcept { return m_audio; }

        /// Preset BEFORE Configure: the PRIMARY instance enters a server/client role at startup
        /// (default = single-player, no socket). The player's launch flow / editor Game tab fills this
        /// from project settings. Extra instances go online at runtime via the Net facade instead.
        void SetNetworkStartup(const net::NetworkStartup& startup) { m_netStartup = startup; }
        /// The primary instance's live endpoint (null when offline / single-player).
        [[nodiscard]] net::NetworkManager* Net() const noexcept { return m_instance.NetEndpoint(); }

        // Networking's per-frame transport pump lives in engine::net::NetworkSubsystem::PostUpdate;
        // the app does not override OnFixedUpdate for it (the base app-level fixed hook stays
        // available for other apps).
        /// Preset BEFORE Configure: audio engine tuning (listener count for split-screen,
        /// voice pool sizes). Defaults suit a single-listener game.
        void SetAudioEngineSettings(const foundation::audio::AudioEngineSettings& settings);
        [[nodiscard]] engine::ui::UISubsystem* UI() const noexcept { return m_ui; }

        /// The game draws at `width` x `height`, fitted into the window by `fit` (the bars black),
        /// and its screen UI lays out at that size and draws crisp at the window's; its pointer
        /// reads in render pixels. Nought on either axis draws at the window's own size. After
        /// Configure.
        void SetRenderResolution(core::u32 width, core::u32 height, core::FitMode fit);
        [[nodiscard]] bool HasRenderResolution() const noexcept { return m_renderWidth > 0 && m_renderHeight > 0; }

        /// Preset BEFORE Configure: an explicit data-root directory (from `--data-root`);
        /// empty = discover it (the `Data/.dataroot` walk from the executable, then the cwd).
        /// Configure resolves it ONCE, mounts a filesystem over it, and hands that mount to
        /// every subsystem that reads engine data (shaders, the built-in UI font). No root =
        /// a loud error and RequestExit(1) - there is no compile-time path fallback.
        void SetDataRoot(core::StringView directory) { m_dataRootOverride = core::String(directory); }
        /// The resolved data-root directory (valid after Configure; empty = not found).
        [[nodiscard]] core::StringView DataRoot() const noexcept { return m_dataRoot.AsView(); }
        /// The mount over the data root (valid after Configure). Subsystems and samples read
        /// engine data through it by data-relative path ("Assets/fonts/...", "Shaders/...").
        [[nodiscard]] foundation::vfs::IFileSystem& DataFileSystem() noexcept
        {
            return *m_dataFileSystem;
        }

        // ---- infrastructure preset (Sedulous PresetInfrastructure lineage): shared
        // pieces are handed in BEFORE Startup; anything not preset the app creates for
        // itself and owns. The editor presets its EXISTING manager (a second manager
        // over the same cooked DB would load every product twice); the player presets
        // its cooked DB and lets the app build the manager. ----

        /// Borrow an existing manager (editor). Wins over SetContentDatabase.
        void SetResourceManager(foundation::resource::ResourceManager* borrowed) noexcept;
        /// LATE-bind a borrowed manager after OnStartup (the editor's project manager opens
        /// and closes projects at runtime): sets the borrow AND registers the app's standard
        /// resource factories into it - exactly what OnStartup does for a preset manager.
        /// Pass null to detach (the editor destroys the old manager after this returns; the
        /// app's lazy Resources() consumers all tolerate null between projects).
        void AttachResourceManager(foundation::resource::ResourceManager* borrowed,
                                   IApplicationHost& host);
        /// The cooked-content database the app should build its OWN manager over (player).
        void SetContentDatabase(foundation::content::IContentDatabase* database) noexcept;
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept;

        // Registers the runtime product types + the STANDARD resource factories into the
        // preset/created manager. Subclasses overriding OnStartup call the base AFTER
        // presetting the database/manager.
        void OnStartup(IApplicationHost& host) override;

        void OnShutdown(IApplicationHost&) override;

        // ---- screenshots (legacy Sedulous CaptureScreenshot, finished: it now writes the file) ----

        /// Capture the next presented frame of the main window to `path` as a PNG. F11 does this
        /// with a timestamped name in the working directory; --screenshot does it at a chosen
        /// frame. The write lands one frame later (the GPU has to finish the copy first).
        void CaptureScreenshot(core::StringView path);
        /// The --screenshot flags (ScreenshotOptionsFromArguments): capture at frame N or after
        /// S seconds, optionally exit once written. OnCommandLine reads them for every app.
        void SetScreenshotOptions(ScreenshotOptions options) { m_screenshotOptions = core::Move(options); }
        void OnCommandLine(int argc, char** argv) override;
        /// Exit the host after this many seconds of updates (0 = never). The player's --exit-after.
        void SetExitAfterSeconds(core::f32 seconds) noexcept { m_exitAfterSeconds = seconds; }

    protected:
        /// The base OnRenderWindow is RenderFrame then FinishFrame. A subclass that draws its own
        /// overlay (ImGui, a HUD) overrides OnRenderWindow and calls the two around it, so the
        /// overlay is in the screenshot: RenderFrame(host, frame); <overlay>; FinishFrame(host, frame).
        /// RenderFrame: every instance's scenes + the window-space overlays into the backbuffer.
        void RenderFrame(IApplicationHost& host, FrameContext& frame);
        /// FinishFrame: the last thing before the host presents - the armed screenshot copy off
        /// the backbuffer (in RenderTarget state, left in RenderTarget state).
        void FinishFrame(IApplicationHost& host, FrameContext& frame);

    public:
        /// ISceneObserver: every composed scene's PrefabSpawnSystem gets the app's content
        /// database and resource manager - the scene knows nothing of content, the app owns it.
        void OnSystemsReady(foundation::scene::Scene& scene) override;

        // ---- the game script (a `Game` class: construct new(), launch(), update(dt),
        // exit() - all optional except the class). Faults disable the SCRIPT, not the game. ----

        /// The scene whose time scale the script's update(dt) follows (and, later, the
        /// scene game services bind against). Set by the launch flow; null = context time.
        void SetPrimaryScene(foundation::scene::Scene* scene) noexcept;
        [[nodiscard]] foundation::scene::Scene* PrimaryScene() const noexcept;

        /// Optional per-run error sink (the editor surfaces notices); set BEFORE
        /// StartGameScript, cleared automatically on StopGameScript.
        void SetGameScriptErrorHandler(foundation::script::IScriptErrorHandler* handler) noexcept;

        /// Compiles + launches the cooked game class - delegated to the run's GameInstance, with the
        /// class's declared handlers so its on<Event> inbox hears the run bus (as Sedulous's
        /// StartGameScript(ScriptClass)). The CALLER binds the class (the player: its content DB).
        bool StartGameScript(const foundation::script::ScriptClass& scriptClass);

        /// exit() + release (idempotent; the update fault path also lands here). The run host tears
        /// down via the scene-stop observer once the run's scene stops.
        void StopGameScript() { m_instance.StopScript(); }
        [[nodiscard]] bool GameScriptRunning() const noexcept { return m_instance.ScriptRunning(); }

        // Default render: draw every active scene into the window via the RenderSubsystem.
        // A game overrides this for custom rendering. (Single-scene: multiple
        // active scenes would each clear; compositing is not handled here.)
        void OnRenderWindow(IApplicationHost& host, FrameContext& frame) override;

    protected:
        // The render/sim POLICY applied to a scene the moment an async load completes (task #123:
        // Game.loadSceneAsync). Base = Start + SetSimulationEnabled (the generic half). The player
        // overrides to seed a default camera first (EnsureCamera) so a script-loaded level renders.
        // SetScene (current-scene bookkeeping) is done by GameInstance::PumpScriptLoads BEFORE this.
        virtual void ApplyLoadedSceneActivation(foundation::scene::Scene* scene);

    private:
        // Install the Game.* level-load facade on ONE instance's run host (task #123): the six
        // binding pointers forward to gi's ticket registry, and gi's activation policy is wired to
        // this app's ApplyLoadedSceneActivation. Called per instance right after ConfigureRunHost,
        // capturing the specific instance (the orchestrator script has no scene to route by). The
        // host is threaded in so run.requestExit routes to it (standalone stops the loop; the editor
        // stops the Game tab's session) - borrowed pointer, the host outlives the binding.
        void InstallInstanceLoadFacade(GameInstance& gi, IApplicationHost& host);

        // The standard factory set - the composition's, created with this host's services -
        // registered into whichever manager the app uses (preset at OnStartup or late-attached by
        // the editor's project manager).
        void RegisterStandardFactories(foundation::resource::ResourceManager& resources,
                                       IApplicationHost& host);

        // Bridges physics contacts to the script subsystem's neutral ingress. The one place
        // physics and script meet for contacts; the adapter itself lives out-of-tree in
        // engine.integration so the two subsystems stay mutually independent.
        engine::integration::ScriptPhysicsContactBridge m_contactBridge;

        // The standard factory set: composed from the engine composition's resource modules
        // (engine-composition.md D6) - every domain the runtime links brings its factories; the
        // set owns them. Device- and shader-gated factories are created when the host offers the
        // service (the runtime's graphics device, the render subsystem's shader system).
        foundation::resource::ResourceFactorySet m_factories;
        foundation::audio::AudioEngineSettings m_audioEngineSettings;
        foundation::resource::ResourceManager* m_borrowedResources = nullptr;
        foundation::content::IContentDatabase* m_contentDatabase = nullptr;
        core::UniquePtr<foundation::resource::ResourceManager> m_ownedResources;
        engine::input::InputSubsystem* m_input = nullptr;
        engine::ui::UISubsystem* m_ui = nullptr;
        // The resolution the game draws at, fitted into the window by m_renderFit; nought draws at
        // the window's size. With it, the pointer the game and its screen UI read is in render
        // pixels (m_fittedInput).
        core::u32 m_renderWidth = 0;
        core::u32 m_renderHeight = 0;
        core::FitMode m_renderFit = core::FitMode::Letterbox;
        core::UniquePtr<engine::input::FittedInputSource> m_fittedInput;
        // The source a game instance reads: the fitted one with a render resolution, else the shell's.
        [[nodiscard]] foundation::input::IInputSourceProvider* GameInputSource() noexcept;
        // Backs the `ui` script facade: a binding pointing at the UISubsystem's
        // screen-tier root + ScreenStack + a cooked-UIDocument instantiator, installed on every run
        // context by the context configurator. App-owned (the screen tier is app-wide).
        engine::uiscript::UiScreenScriptBinding m_uiScreenBinding;
        core::String m_dataRootOverride; // preset (SetDataRoot); empty = discover
        core::String m_dataRoot;         // resolved in Configure
        core::UniquePtr<foundation::vfs::NativeFileSystem> m_dataFileSystem; // the data mount (owned)
        engine::physics::PhysicsSubsystem* m_physics = nullptr;
        engine::audio::AudioSubsystem* m_audio = nullptr;
        engine::render::RenderSubsystem* m_render = nullptr; // for the `DebugDraw.of(scene)` service

        net::NetworkStartup m_netStartup; // preset before Configure (default = single-player)
        engine::script::ScriptSubsystem* m_scripts = nullptr;
        // The app host, captured in Configure (stable for the app's lifetime). Used to route
        // run.requestExit through a GameInstance's load facade when an extra instance is created
        // outside a host-bearing call (CreateInstance). Borrowed - the host outlives the app.
        IApplicationHost* m_host = nullptr;
        // Every running game: the primary (a stable member) + any extras (stable UniquePtr addresses,
        // required because the SceneSubsystem borrows each SceneManager's pointer).
        template <typename Fn>
        void ForEachInstance(Fn&& fn)
        {
            fn(m_instance);
            for (core::UniquePtr<GameInstance>& gi : m_extraInstances)
            {
                fn(*gi);
            }
        }

        // The client-side prefab net-spawn resolver injected onto every GameInstance's NetworkController
        // a replicated prefab id -> a live prefab from the content DB
        // (replication then applies the transform + fields on top). The server assigns ids; game rules
        // set relevancy. The controller applies it to each endpoint, so reconnect keeps it - the app just
        // hands it over once at wiring (it needs the content DB), no longer wiring the endpoint itself.
        [[nodiscard]] net::StateReplication::SpawnHandler MakeSpawnResolver();
        /// The resolver's body: the ONE runtime spawn recipe (PrefabSpawnSystem::SpawnInto) with
        /// the app's database and manager supplied - a script's scene.spawn runs the same one
        /// through the scene's own system. Invalid (never a partial spawn) when there is no
        /// database, no such instance, or an instance with no scene stream.
    public:
        [[nodiscard]] static foundation::scene::EntityHandle
        ResolveNetworkPrefab(foundation::content::IContentDatabase* database,
                             foundation::resource::ResourceManager* resources,
                             foundation::scene::Scene& scene, const core::Guid& prefabId);

    private:

        // Enter the preset startup role on the primary instance (None = single-player, no-op). The
        // reliable-config tuning uses the endpoint defaults here; the preset path is the CLI/dedicated
        // launch (scripts go online via the Net facade instead).
        void ApplyNetworkStartup(GameInstance& instance);

        /// Re-points every live scene's spawn system at the current content database + resource
        /// manager: SetContentDatabase and the manager's creation can both land after scenes exist.
        void PointSpawnersAtContent();

        ScreenshotCapture m_screenshot;
        ScreenshotOptions m_screenshotOptions;
        core::u64 m_renderedFrames = 0; // frames FinishFrame saw (the --screenshot-frame count)
        core::u32 m_screenshotsTaken = 0; // of the --screenshot run (--screenshot-count frames, then done)
        bool m_screenshotExitPending = false;
        core::f32 m_exitAfterSeconds = 0.0f;
        core::f32 m_runSeconds = 0.0f;

        engine::scene::SceneSubsystem* m_scenes = nullptr;
        GameInstance m_instance; // the primary running game (app-level ops target this one)
        core::Array<core::UniquePtr<GameInstance>>
            m_extraInstances; // multi-instance PIE / headless server
    };
}

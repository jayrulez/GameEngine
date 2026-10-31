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
// This lives in its own library, separate from foundation.runtime.client, so the base client
// never pulls in the engine subsystem libraries. It registers all standard gameplay subsystems
// (the editor embeds this same class against its runtime context, so subsystem registration
// lives here, not in entry points) and owns the game-script lifecycle (the `Game` class
// bracket), which the player and the editor's Game tab both use instead of keeping copies.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module engine.defaultapp;
export import foundation.rhi.capture; // ScreenshotCapture and the --screenshot flags (this API takes ScreenshotOptions)
export import :profiler_overlay; // ProfilerOverlayHost (F3, --profile-overlay)

import foundation.core;
import foundation.rhi;
import foundation.runtime.client;       // IApplication, IApplicationHost
import engine.gameinstance; // GameInstance: this app's running game (scene and script bracket)
import foundation.shell;                // IShell, IKeyboard, KeyCode (the profile-dump hotkey)
import foundation.graphics;             // GraphicsDevice, FrameContext
import foundation.vfs;                  // the data root: ResolveDataRoot and the NativeFileSystem mounted over it
import foundation.scene;                // Scene
import engine.scene;      // SceneSubsystem (the standard scene driver)
import engine.render;     // RenderSubsystem (the standard renderer)
import engine.animation; // AnimationSubsystem (drives skeletal animation from the scene)
import engine.particles; // ParticleSubsystem (scene-driven CPU sim)
import foundation.physics;             // ContactKind/EntityContact (the contact bridge)
import engine.physics;   // PhysicsSubsystem (Jolt worlds and interpolation)
import foundation.input;               // the action model/runtime
import engine.input;     // InputSubsystem and the Input facade
import foundation.script;              // IScriptManager/Context (the game script)
import foundation.script.resource;     // ScriptClass (the cooked game class StartGameScript takes)
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript; // the AngelScript backend (OPTION_ENABLE_ANGELSCRIPT)
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;         // the Luau backend (OPTION_ENABLE_LUAU)
#endif
import engine.script;    // ScriptSubsystem (behaviors and the run's shared context)
import engine.integration; // ScriptPhysicsContactBridge (physics contacts to script ingress)
import foundation.resource;            // ResourceManager (owned or borrowed: see the infrastructure preset)
import foundation.content;             // IContentDatabase (preset by the entry point)
import foundation.scene.resource;      // SceneDocument (product-type registration)
import engine.gui; // the engine's GUI on foundation.gui
import engine.composition; // FullSceneComposition (the one source of scene assembly)
import foundation.audio;               // AudioEngine (owned by the audio subsystem)
import engine.audio;     // AudioSubsystem (voices, buses, one-shots and scene sync)
import foundation.net;                 // UdpSocket / DatagramEndpoint (the transport)
import foundation.net.replication;     // NetworkId / StateReplication (the spawn handler)
import foundation.net.manager;   // NetworkManager, NetworkStartup/StartNetworking and the Net facade
import foundation.run;           // RunRegistry: the live runs, for every subsystem
import engine.net; // NetworkSubsystem (injects the NetworkComponentManager into scenes)
import foundation.profiler;      // the CPU scope profiler (P-key dump)

namespace core = foundation::core;
namespace net = foundation::net;   // NetworkManager, NetworkStartup and the Net facade
using namespace foundation::runtime; // foundation runtime: IApplication/IApplicationHost/Subsystem/Context
using namespace foundation::shell; // IShell and the input/window types
using namespace foundation::graphics; // GraphicsDevice/RenderWindow/FrameContext

export namespace engine::runtime
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace net = foundation::net;

    class DefaultApplication : public IApplication, public foundation::scene::ISceneObserver
    {
    public:
        // Press P to print the previous frame's CPU scope tree and per-pass GPU timing. A game
        // subclass that overrides OnUpdate should call DefaultApplication::OnUpdate(host, dt) to
        // keep the hotkey. (It reads the GPU timestamps after a device stall, which is fine for an
        // on-demand dump.)
        void OnUpdate(IApplicationHost& host, core::f32 deltaTime) override;

        // Ticks the game script with gameplay time: dt x the context scale x the primary scene's
        // scale (per-scene time). Subclasses overriding OnUpdate call the base to keep the script
        // (and the profile hotkey) alive.
        void TickGameScript(IApplicationHost& host, core::f32 deltaTime);

        // Registers all standard engine subsystems. A game subclass overrides this, calls
        // DefaultApplication::Configure(host) first, then adds its own. Entry points (player,
        // editor) do not register gameplay subsystems; this is the one place.
        void Configure(IApplicationHost& host) override;

        [[nodiscard]] engine::script::ScriptSubsystem* Scripts() const noexcept;

        /// The app's primary running game. The launch flow creates the game scene in its
        /// Scenes() manager so it groups, ticks and renders as this run's scenes.
        [[nodiscard]] GameInstance& Instance() noexcept { return m_instance; }
        /// The live runs (null before Configure); also on the subsystem manager, for subsystems.
        [[nodiscard]] foundation::run::IRunManager* Runs() noexcept { return m_runs.Get(); }

        /// The primary instance's scene group: a scene created here renders (the app renders
        /// instance scenes; there is no default manager). Returns SceneManager& directly so callers
        /// need not name GameInstance.
        [[nodiscard]] foundation::scene::SceneManager& PrimaryScenes() noexcept;

        /// Create an additional running game (multi-instance play in editor, an in-editor headless
        /// dedicated server). Wired like the primary: its scene group ticks on the Context lane and
        /// its run host gets the app services. Stable address (UniquePtr), so the SceneSubsystem's
        /// borrowed manager pointer stays valid. Returns null before Configure ran.
        [[nodiscard]] GameInstance* CreateInstance(bool headless = false);

        /// Destroy an extra instance (a "Play New Instance" tab closing). Unregisters its scene
        /// manager from the SceneSubsystem (else a dangling borrowed pointer is ticked and
        /// rendered), tears down its run host, and frees it. The primary instance is permanent (a
        /// member), so this is a no-op for it.
        void ReleaseInstance(GameInstance* instance);

        [[nodiscard]] engine::input::InputSubsystem* Input() const noexcept { return m_input; }
        [[nodiscard]] engine::physics::PhysicsSubsystem* Physics() const noexcept;
        [[nodiscard]] engine::audio::AudioSubsystem* Audio() const noexcept { return m_audio; }

        /// Preset before Configure: the primary instance enters a server or client role at startup
        /// (default: single-player, no socket). The player's launch flow and the editor's Game tab
        /// fill this from project settings. Extra instances go online at runtime via the Net facade.
        void SetNetworkStartup(const net::NetworkStartup& startup) { m_netStartup = startup; }
        /// The primary instance's live endpoint (null when offline or single-player).
        [[nodiscard]] net::NetworkManager* Net() const noexcept { return m_instance.NetEndpoint(); }

        // Networking's per-frame transport pump lives in engine::net::NetworkSubsystem::PostUpdate;
        // the app does not override OnFixedUpdate for it (the base app-level fixed hook stays
        // available for other apps).
        /// Preset before Configure: audio engine tuning (listener count for split-screen,
        /// voice pool sizes). Defaults suit a single-listener game.
        void SetAudioEngineSettings(const foundation::audio::AudioEngineSettings& settings);
        /// The engine's GUI subsystem, there whenever the GUI is built.
        [[nodiscard]] engine::gui::GuiSubsystem* GUI() const noexcept { return m_gui; }

        /// The game draws at `width` x `height`, fitted into the window by `fit` (the bars black),
        /// and its screen UI lays out at that size and draws crisp at the window's; its pointer
        /// reads in render pixels. Nought on either axis draws at the window's own size. After
        /// Configure.
        void SetRenderResolution(core::u32 width, core::u32 height, core::FitMode fit);
        /// The shape the game is made for: on a window of the other shape it is presented turned a
        /// quarter, with input turned back. Any takes the window's shape.
        void SetOrientation(core::ScreenOrientation orientation);
        [[nodiscard]] core::ScreenOrientation Orientation() const noexcept { return m_orientation; }
        [[nodiscard]] bool HasRenderResolution() const noexcept { return m_renderWidth > 0 && m_renderHeight > 0; }

        /// Preset before Configure: an explicit data-root directory (from `--data-root`);
        /// empty means discover it (the `Data/.dataroot` walk from the executable, then the cwd).
        /// Configure resolves it once, mounts a filesystem over it, and hands that mount to every
        /// subsystem that reads engine data (shaders, the built-in UI font). With no root it logs
        /// an error and calls RequestExit(1); there is no compile-time path fallback.
        void SetDataRoot(core::StringView directory) { m_dataRootOverride = core::String(directory); }
        /// The resolved data-root directory (valid after Configure; empty = not found).
        [[nodiscard]] core::StringView DataRoot() const noexcept { return m_dataRoot.AsView(); }
        /// The mount over the data root (valid after Configure). Subsystems and samples read
        /// engine data through it by data-relative path ("Engine/Fonts/...", "Engine/Shaders/...").
        [[nodiscard]] foundation::vfs::IFileSystem& DataFileSystem() noexcept
        {
            return *m_dataFileSystem;
        }

        // ---- infrastructure preset: shared pieces are handed in before Startup; anything not
        // preset the app creates for itself and owns. The editor presets its existing manager (a
        // second manager over the same cooked DB would load every product twice); the player
        // presets its cooked DB and lets the app build the manager. ----

        /// Borrow an existing manager (editor). Wins over SetContentDatabase.
        void SetResourceManager(foundation::resource::ResourceManager* borrowed) noexcept;
        /// Late-bind a borrowed manager after OnStartup (the editor's project manager opens and
        /// closes projects at runtime): sets the borrow and registers the app's standard resource
        /// factories into it, as OnStartup does for a preset manager. Pass null to detach (the
        /// editor destroys the old manager after this returns; the app's lazy Resources()
        /// consumers all tolerate null between projects).
        void AttachResourceManager(foundation::resource::ResourceManager* borrowed,
                                   IApplicationHost& host);
        /// The cooked-content database the app should build its own manager over (player).
        void SetContentDatabase(foundation::content::IContentDatabase* database) noexcept;
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept;

        // Registers the runtime product types and the standard resource factories into the
        // preset or created manager. Subclasses overriding OnStartup call the base after
        // presetting the database or manager.
        void OnStartup(IApplicationHost& host) override;

        void OnShutdown(IApplicationHost&) override;

        // ---- screenshots ----

        /// Capture the next presented frame of the main window to `path` as a PNG. F11 does this
        /// with a timestamped name in the working directory; --screenshot does it at a chosen
        /// frame. The write lands one frame later (the GPU has to finish the copy first).
        void CaptureScreenshot(core::StringView path);
        /// The --screenshot flags (ScreenshotOptionsFromArguments): capture at frame N or after
        /// S seconds, optionally exit once written. OnCommandLine reads them for every app.
        void SetScreenshotOptions(foundation::rhi::ScreenshotOptions options) { m_screenshotOptions = core::Move(options); }
        void OnCommandLine(int argc, char** argv) override; // --screenshot ..., --profile-overlay
        /// Show the profiler overlay once the GUI is up (`--profile-overlay`; the web player's
        /// `?profile-overlay`). F3 toggles it at any time.
        void SetProfilerOverlayAtStart(bool shown) noexcept { m_profilerOverlayAtStart = shown; }
        /// The overlay host, made on first use; it shows on this app's GUI.
        [[nodiscard]] ProfilerOverlayHost& ProfilerOverlay();
        /// Exit the host after this many seconds of updates (0 = never). The player's --exit-after.
        void SetExitAfterSeconds(core::f32 seconds) noexcept { m_exitAfterSeconds = seconds; }

    protected:
        /// The base OnRenderWindow is RenderFrame then FinishFrame. A subclass that draws its own
        /// overlay (ImGui, a HUD) overrides OnRenderWindow and calls the two around it, so the
        /// overlay is in the screenshot: RenderFrame(host, frame); <overlay>; FinishFrame(host, frame).
        /// RenderFrame: every instance's scenes and the window-space overlays into the backbuffer.
        void RenderFrame(IApplicationHost& host, FrameContext& frame);
        /// FinishFrame: the last thing before the host presents, the armed screenshot copy off
        /// the backbuffer (in RenderTarget state, left in RenderTarget state).
        void FinishFrame(IApplicationHost& host, FrameContext& frame);

    public:
        /// ISceneObserver: every composed scene's PrefabSpawnSystem gets the app's content
        /// database and resource manager; the scene knows nothing of content, the app owns it.
        void OnSystemsReady(foundation::scene::Scene& scene) override;

        // ---- the game script (a `Game` class: construct new(), launch(), update(dt),
        // exit(), all optional except the class). Faults disable the script, not the game. ----

        /// The scene whose time scale the script's update(dt) follows. Set by the launch flow;
        /// null means context time.
        void SetPrimaryScene(foundation::scene::Scene* scene) noexcept;
        [[nodiscard]] foundation::scene::Scene* PrimaryScene() const noexcept;

        /// Optional per-run error sink (the editor surfaces notices); set before
        /// StartGameScript, cleared automatically on StopGameScript.
        void SetGameScriptErrorHandler(foundation::script::IScriptErrorHandler* handler) noexcept;

        /// Compiles and launches the cooked game class, delegated to the run's GameInstance, with
        /// the class's declared handlers so its on<Event> inbox hears the run bus. The caller binds
        /// the class (the player: from its content DB).
        bool StartGameScript(const foundation::script::ScriptClass& scriptClass);

        /// exit() and release (idempotent; the update fault path also lands here). The run host tears
        /// down via the scene-stop observer once the run's scene stops.
        void StopGameScript() { m_instance.StopScript(); }
        [[nodiscard]] bool GameScriptRunning() const noexcept { return m_instance.ScriptRunning(); }

        // Default render: draw every active scene into the window via the RenderSubsystem.
        // A game overrides this for custom rendering. (Single scene: multiple active scenes
        // would each clear; compositing is not handled here.)
        void OnRenderWindow(IApplicationHost& host, FrameContext& frame) override;

    protected:
        // The render and simulation policy applied to a scene the moment an async load completes
        // (Game.loadSceneAsync). The base starts it and enables simulation (the generic half). The
        // player overrides it to seed a default camera first (EnsureCamera) so a script-loaded
        // level renders. GameInstance::PumpScriptLoads does SetScene (current-scene bookkeeping)
        // before this.
        virtual void ApplyLoadedSceneActivation(foundation::scene::Scene* scene);

    private:
        // Install the Game.* level-load facade on one instance's run host: the six binding
        // pointers forward to gi's ticket registry, and gi's activation policy is wired to this
        // app's ApplyLoadedSceneActivation. Called per instance right after ConfigureRunHost,
        // capturing the specific instance (the orchestrator script has no scene to route by). The
        // host is threaded in so run.requestExit routes to it (standalone stops the loop; the
        // editor stops the Game tab's session). Borrowed pointer: the host outlives the binding.
        void InstallInstanceLoadFacade(GameInstance& gi, IApplicationHost& host);

        // The standard factory set, the composition's, created with this host's services and
        // registered into whichever manager the app uses (preset at OnStartup or late-attached by
        // the editor's project manager).
        void RegisterStandardFactories(foundation::resource::ResourceManager& resources,
                                       IApplicationHost& host);

        // Bridges physics contacts to the script subsystem's neutral ingress. The one place
        // physics and script meet for contacts; the adapter itself lives out-of-tree in
        // engine.integration so the two subsystems stay mutually independent.
        engine::integration::ScriptPhysicsContactBridge m_contactBridge;

        // The standard factory set, composed from the engine composition's resource modules:
        // every domain the runtime links brings its factories, and the set owns them. Device- and
        // shader-gated factories are created when the host offers the service (the runtime's
        // graphics device, the render subsystem's shader system).
        foundation::resource::ResourceFactorySet m_factories;
        foundation::audio::AudioEngineSettings m_audioEngineSettings;
        foundation::resource::ResourceManager* m_borrowedResources = nullptr;
        foundation::content::IContentDatabase* m_contentDatabase = nullptr;
        core::UniquePtr<foundation::resource::ResourceManager> m_ownedResources;
        engine::input::InputSubsystem* m_input = nullptr;
        engine::gui::GuiSubsystem* m_gui = nullptr;
        // The resolution the game draws at, fitted into the window by m_renderFit; nought draws at
        // the window's size. With it, the pointer the game and its screen UI read is in render
        // pixels (m_fittedInput).
        core::u32 m_renderWidth = 0;
        core::u32 m_renderHeight = 0;
        core::FitMode m_renderFit = core::FitMode::Letterbox;
        core::UniquePtr<engine::input::FittedInputSource> m_fittedInput;
        core::ScreenOrientation m_orientation = core::ScreenOrientation::Any;
        // The fitted input while a render resolution or an orientation may move the game's frame
        // off the window's; the shell's devices directly otherwise.
        void UpdateFittedInput();
        // The source a game instance reads: the fitted one with a render resolution, else the shell's.
        [[nodiscard]] foundation::input::IInputSourceProvider* GameInputSource() noexcept;
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
        // outside a host-bearing call (CreateInstance). Borrowed: the host outlives the app.
        IApplicationHost* m_host = nullptr;
        // Every running game: the primary (a stable member) and any extras (stable UniquePtr
        // addresses, required because the SceneSubsystem borrows each SceneManager's pointer).
        template <typename Fn>
        void ForEachInstance(Fn&& fn)
        {
            fn(m_instance);
            for (core::UniquePtr<GameInstance>& gi : m_extraInstances)
            {
                fn(*gi);
            }
        }

        // The client-side prefab net-spawn resolver injected onto every GameInstance's
        // NetworkController: it turns a replicated prefab id into a live prefab from the content DB
        // (replication then applies the transform and fields on top). The server assigns ids; game
        // rules set relevancy. The controller applies it to each endpoint, so reconnecting keeps
        // it; the app hands it over once at wiring, since it needs the content DB.
        [[nodiscard]] net::StateReplication::SpawnHandler MakeSpawnResolver();
        /// The resolver's body: the one runtime spawn recipe (PrefabSpawnSystem::SpawnInto) with
        /// the app's database and manager supplied; a script's scene.spawn runs the same one
        /// through the scene's own system. Invalid (never a partial spawn) when there is no
        /// database, no such instance, or an instance with no scene stream.
    public:
        [[nodiscard]] static foundation::scene::EntityHandle
        ResolveNetworkPrefab(foundation::content::IContentDatabase* database,
                             foundation::resource::ResourceManager* resources,
                             foundation::scene::Scene& scene, const core::Guid& prefabId);

    private:

        // Enter the preset startup role on the primary instance (None = single-player, no-op). The
        // reliable-config tuning uses the endpoint defaults here; the preset path is the CLI or
        // dedicated launch (scripts go online via the Net facade instead).
        void ApplyNetworkStartup(GameInstance& instance);

        /// Re-points every live scene's spawn system at the current content database and resource
        /// manager: SetContentDatabase and the manager's creation can both land after scenes exist.
        void PointSpawnersAtContent();

        foundation::rhi::ScreenshotCapture m_screenshot;
        foundation::rhi::ScreenshotOptions m_screenshotOptions;
        core::u64 m_renderedFrames = 0; // frames FinishFrame saw (the --screenshot-frame count)
        core::u32 m_screenshotsTaken = 0; // of the --screenshot run (--screenshot-count frames, then done)
        bool m_screenshotExitPending = false;
        bool m_profilerArmedByKey = false; // the P key armed the profiler (for the rest of the run)
        bool m_profilerOverlayAtStart = false; // --profile-overlay: the overlay shown from the start
        core::UniquePtr<ProfilerOverlayHost> m_profilerOverlay; // made on first show (F3, --profile-overlay)
        core::f32 m_exitAfterSeconds = 0.0f;
        core::f32 m_runSeconds = 0.0f;

        engine::scene::SceneSubsystem* m_scenes = nullptr;
        // The live runs (every GameInstance), handed to the subsystem manager for every subsystem.
        // Made in Configure; declared before the instances so it outlives them and hears each one
        // go.
        core::UniquePtr<foundation::run::RunRegistry> m_runs;
        core::u32 m_runsMade = 1; // names the extra instances' runs ("Game 2", ...)
        GameInstance m_instance; // the primary running game (app-level ops target this one)
        core::Array<core::UniquePtr<GameInstance>>
            m_extraInstances; // multi-instance play in editor / headless server
    };
}

// Draconic::RuntimeDefaultApp - the `draconic.engine.defaultapp` module.
//
// DefaultApplication: an opinionated IApplication base that registers the standard
// engine subsystems. A game that wants the batteries-included engine writes
// `class MyGame : DefaultApplication` and adds its own subsystems in Configure
// (calling the base first); a game that wants only its own subsystems implements
// IApplication directly and links none of this.
//
// This lives in its OWN library - separate from draconic.runtime.client - precisely
// so the base client never pulls in the engine subsystem libraries. It registers ALL
// standard gameplay subsystems (runtime-host.md v3: the editor embeds THIS same class
// against its runtime context, so subsystem registration lives here, not in entry
// points) and owns the GAME-SCRIPT lifecycle (the Wren `Game` class bracket) - the
// player and the editor's Game tab both consume it instead of hand-rolling copies.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.engine.defaultapp;

import draconic.foundation;
import draconic.rhi;
import draconic.runtime.client;       // IApplication, IApplicationHost
import draconic.engine.gameinstance; // GameInstance - this app's running game (scene + script bracket)
import draconic.shell;                // IShell, IKeyboard, KeyCode (the profile-dump hotkey)
import draconic.graphics;             // GraphicsDevice, FrameContext
import draconic.scene;                // Scene
import draconic.engine.scene;      // SceneSubsystem (the standard scene driver)
import draconic.engine.render;     // RenderSubsystem (the standard renderer)
import draconic.engine.animation; // AnimationSubsystem (drives skeletal animation from the scene)
import draconic.engine.particles; // ParticleSubsystem (scene-driven CPU sim)
import draconic.physics;             // ContactKind/EntityContact (the contact bridge)
import draconic.engine.physics;   // PhysicsSubsystem (Jolt worlds + interpolation)
import draconic.input;               // the action model/runtime
import draconic.engine.input;     // InputSubsystem + the Wren Input facade
import draconic.script;              // IScriptManager/Context (the game script)
#ifdef DRACONIC_HAS_WREN
import draconic.script.wren;         // the Wren backend (primary; toggle via DRACONIC_ENABLE_WREN)
#endif
#ifdef DRACONIC_HAS_ANGELSCRIPT
import draconic.script.angelscript; // the AngelScript backend (second backend; DRACONIC_ENABLE_ANGELSCRIPT)
#endif
import draconic.script.resource;     // cooked script classes + factory (entity behaviors)
import draconic.engine.script;    // ScriptSubsystem (behaviors + the run's shared context)
import draconic.engine.integration; // ScriptPhysicsContactBridge (physics contacts -> script ingress)
import draconic.resource;            // ResourceManager (owned or borrowed - see the preset seam)
import draconic.content;             // IContentDatabase (preset by the entry point)
import draconic.scene.resource;      // SceneDocument (product-type registration)
import draconic.geometry.resource;   // mesh factories
import draconic.materials.resource;  // material factory
import draconic.animation.resource;  // skeleton/clip/graph factories
import draconic.particles.resource;  // particle-effect factory
import draconic.input.resource;      // input-map factory
import draconic.physics.resource;    // collision-shape/physical-material factories
import draconic.texture.resource;    // texture factory (device-backed)
import draconic.image.resource;      // image resource registration
import draconic.model.resource;      // cooked-model family types + registration
import draconic.ui.resource;         // cooked UI documents/themes (game-ui)
import draconic.engine.ui;        // the game screen tier (canvases + overlay + consumption)
import draconic.audio;               // AudioEngine (owned by the audio subsystem)
import draconic.audio.resource;      // cooked audio clips + factory
import draconic.engine.audio;     // AudioSubsystem (voices/buses/one-shots + scene sync)
import draconic.net;                 // UdpSocket / DatagramEndpoint (the transport)
import draconic.net.replication;     // NetworkId / StateReplication (the spawn-handler seam)
import draconic.net.manager;   // NetworkManager + NetworkStartup/StartNetworking + the Net facade
import draconic.engine.net; // NetworkSubsystem (injects the NetworkComponentManager into scenes)
import draconic.profiler;      // the CPU scope profiler (P-key dump)

namespace rhi = draconic::rhi;
namespace foundation = draconic::foundation;
namespace net = draconic::net;   // NetworkManager + NetworkStartup + the Net facade
using namespace draconic::shell; // IShell + input/window types (moved from draconic::runtime)
using namespace draconic::
    graphics; // GraphicsDevice/RenderWindow/FrameContext (moved from draconic::runtime)

export namespace draconic::runtime
{
    class DefaultApplication : public IApplication
    {
    public:
        // Press P to print the previous frame's CPU scope tree + per-pass GPU timing. A game
        // subclass that overrides OnUpdate should call DefaultApplication::OnUpdate(host, dt) to
        // keep the hotkey. (Reads the GPU timestamps after a device stall - fine for an on-demand dump.)
        void OnUpdate(IApplicationHost& host, foundation::f32 deltaTime) override;

        // Ticks the game script with GAMEPLAY time: dt x context scale x the primary
        // scene's scale (per-scene time, H1). Subclasses overriding OnUpdate call the
        // base to keep the script (and the profile hotkey) alive.
        void TickGameScript(IApplicationHost& host, foundation::f32 deltaTime);

        // Registers ALL standard engine subsystems. A game subclass overrides this,
        // calls DefaultApplication::Configure(host) first, then adds its own. Entry
        // points (player, editor) do NOT register gameplay subsystems - this is the
        // one place (runtime-host.md v3).
        void Configure(IApplicationHost& host) override;

        [[nodiscard]] draconic::script::ScriptSubsystem* Scripts() const noexcept;

        /// The app's running game (N=1 today). The launch flow creates the game scene in its
        /// Scenes() manager so it groups + ticks + renders as this run's scenes.
        [[nodiscard]] GameInstance& Instance() noexcept { return m_instance; }

        /// The primary instance's scene group - a scene created here renders (the app renders instance
        /// scenes; there is no default manager). Returns SceneManager& directly so callers need not
        /// name GameInstance.
        [[nodiscard]] draconic::scene::SceneManager& PrimaryScenes() noexcept;

        /// Create an ADDITIONAL running game (multi-instance PIE / an in-editor headless dedicated
        /// server, game-instance.md §11 / networking.md). Wired like the primary - its scene group ticks
        /// on the Context lane and its run host gets the app services. Stable address (UniquePtr), so the
        /// SceneSubsystem's borrowed manager pointer stays valid. Returns null before Configure ran.
        [[nodiscard]] GameInstance* CreateInstance(bool headless = false);

        /// Destroy an EXTRA instance (a "Play New Instance" tab closing). Unregisters its scene manager
        /// from the SceneSubsystem (else a dangling borrowed pointer is ticked/rendered), tears down its
        /// run host, and frees it. The PRIMARY instance is permanent (a member) - a no-op for it.
        void ReleaseInstance(GameInstance* instance);

        [[nodiscard]] draconic::input::InputSubsystem* Input() const noexcept { return m_input; }
        [[nodiscard]] draconic::physics::PhysicsSubsystem* Physics() const noexcept;
        [[nodiscard]] draconic::audio::AudioSubsystem* Audio() const noexcept { return m_audio; }

        /// Preset BEFORE Configure: the PRIMARY instance enters a server/client role at startup
        /// (default = single-player, no socket). The player's launch flow / editor Game tab fills this
        /// from project settings. Extra instances go online at runtime via the Net facade instead.
        void SetNetworkStartup(const net::NetworkStartup& startup) { m_netStartup = startup; }
        /// The primary instance's live endpoint (null when offline / single-player).
        [[nodiscard]] net::NetworkManager* Net() const noexcept { return m_instance.NetEndpoint(); }

        // Drives networking on the FIXED lane (deterministic step) for EVERY instance: pump datagrams,
        // dispatch RPCs, push per-peer deltas / sample interpolation. Runs even with no game script (a
        // dedicated server has none). A subclass overriding OnFixedUpdate calls the base to keep it alive.
        void OnFixedUpdate(IApplicationHost& host, foundation::f32 fixedDeltaTime) override;
        /// Preset BEFORE Configure: audio engine tuning (listener count for split-screen,
        /// voice pool sizes). Defaults suit a single-listener game.
        void SetAudioEngineSettings(const draconic::audio::AudioEngineSettings& settings);
        [[nodiscard]] draconic::ui::UISubsystem* UI() const noexcept { return m_ui; }

        /// TTF for the game UI's default font (preset BEFORE Configure; the editor passes
        /// its own font path, the player defaults to the dev-tree Roboto).
        void SetUIFontPath(foundation::StringView path) { m_uiFontPath = foundation::String(path); }

        // ---- infrastructure preset (Sedulous PresetInfrastructure lineage): shared
        // pieces are handed in BEFORE Startup; anything not preset the app creates for
        // itself and owns. The editor presets its EXISTING manager (a second manager
        // over the same cooked DB would load every product twice); the player presets
        // its cooked DB and lets the app build the manager. ----

        /// Borrow an existing manager (editor). Wins over SetContentDatabase.
        void SetResourceManager(draconic::resource::ResourceManager* borrowed) noexcept;
        /// LATE-bind a borrowed manager after OnStartup (the editor's project manager opens
        /// and closes projects at runtime): sets the borrow AND registers the app's standard
        /// resource factories into it - exactly what OnStartup does for a preset manager.
        /// Pass null to detach (the editor destroys the old manager after this returns; the
        /// app's lazy Resources() consumers all tolerate null between projects).
        void AttachResourceManager(draconic::resource::ResourceManager* borrowed,
                                   IApplicationHost& host);
        /// The cooked-content database the app should build its OWN manager over (player).
        void SetContentDatabase(draconic::content::IContentDatabase* database) noexcept;
        [[nodiscard]] draconic::resource::ResourceManager* Resources() const noexcept;

        // Registers the runtime product types + the STANDARD resource factories into the
        // preset/created manager. Subclasses overriding OnStartup call the base AFTER
        // presetting the database/manager.
        void OnStartup(IApplicationHost& host) override;

        void OnShutdown(IApplicationHost&) override;

        // ---- the game script (a Wren class `Game`: construct new(), launch(), update(dt),
        // exit() - all optional except the class). Faults disable the SCRIPT, not the game. ----

        /// The scene whose time scale the script's update(dt) follows (and, later, the
        /// scene game services bind against). Set by the launch flow; null = context time.
        void SetPrimaryScene(draconic::scene::Scene* scene) noexcept;
        [[nodiscard]] draconic::scene::Scene* PrimaryScene() const noexcept;

        /// Optional per-run error sink (the editor surfaces notices); set BEFORE
        /// StartGameScript, cleared automatically on StopGameScript.
        void SetGameScriptErrorHandler(draconic::script::IScriptErrorHandler* handler) noexcept;

        /// Compiles + launches the game script from source text - delegated to the run's GameInstance.
        /// The CALLER resolves where the source lives (player: project file / pak entry; editor:
        /// SourceDb). The exposeServices lambda binds the per-context script facades on the fallback
        /// path (the normal path uses the ScriptSubsystem's configured shared context).
        bool StartGameScript(foundation::StringView source, foundation::StringView name);

        /// exit() + release (idempotent; the update fault path also lands here). The run host tears
        /// down via the scene-stop observer once the run's scene stops.
        void StopGameScript() { m_instance.StopScript(); }
        [[nodiscard]] bool GameScriptRunning() const noexcept { return m_instance.ScriptRunning(); }

        // Default render: draw every active scene into the window via the RenderSubsystem.
        // A game overrides this for custom rendering. (Single-scene for now - multiple
        // active scenes would each clear; compositing is a later concern.)
        void OnRenderWindow(IApplicationHost& host, FrameContext& frame) override;

    protected:
        // The render/sim POLICY applied to a scene the moment an async load completes (task #123:
        // Game.loadSceneAsync). Base = Start + SetSimulationEnabled (the generic half). The player
        // overrides to seed a default camera first (EnsureCamera) so a script-loaded level renders.
        // SetScene (current-scene bookkeeping) is done by GameInstance::PumpScriptLoads BEFORE this.
        virtual void ApplyLoadedSceneActivation(draconic::scene::Scene* scene);

    private:
        // Install the Game.* level-load facade on ONE instance's run host (task #123): the six
        // binding pointers forward to gi's ticket registry, and gi's activation policy is wired to
        // this app's ApplyLoadedSceneActivation. Called per instance right after ConfigureRunHost,
        // capturing the specific instance (the orchestrator script has no scene to route by).
        void InstallInstanceLoadFacade(GameInstance& gi);

        // The standard factory set, registered into whichever manager the app uses (preset at
        // OnStartup or late-attached by the editor's project manager).
        void RegisterStandardFactories(draconic::resource::ResourceManager& resources,
                                       IApplicationHost& host);

        // Bridges physics contacts to the script subsystem's neutral ingress. The one place
        // physics and script meet for contacts; the adapter itself lives out-of-tree in
        // draconic.engine.integration so the two subsystems stay mutually independent.
        draconic::integration::ScriptPhysicsContactBridge m_contactBridge;

        draconic::geometry::StaticMeshFactory m_meshFactory;
        draconic::geometry::SkinnedMeshFactory m_skinnedMeshFactory;
        draconic::materials::MaterialFactory m_materialFactory;
        draconic::animation::SkeletonFactory m_skeletonFactory;
        draconic::animation::AnimationClipFactory m_animationClipFactory;
        draconic::animation::AnimationGraphFactory m_animationGraphFactory;
        draconic::particles::ParticleEffectFactory m_particleEffectFactory;
        draconic::input::InputMapFactory m_inputMapFactory;
        draconic::physics::CollisionShapeFactory m_collisionShapeFactory;
        draconic::physics::PhysicalMaterialFactory m_physicalMaterialFactory;
        draconic::audio::AudioClipFactory m_audioClipFactory;
        draconic::audio::AudioBusLayoutFactory m_busLayoutFactory;
        draconic::audio::SoundCueFactory m_soundCueFactory;
        draconic::script::ScriptClassFactory m_scriptClassFactory;
        draconic::audio::AudioEngineSettings m_audioEngineSettings;
        draconic::model::ModelFactory m_modelFactory;
        draconic::ui::UIDocumentFactory m_uiDocumentFactory;
        draconic::ui::UIThemeFactory m_uiThemeFactory;
        foundation::UniquePtr<draconic::texture::TextureFactory> m_textureFactory;
        draconic::resource::ResourceManager* m_borrowedResources = nullptr;
        draconic::content::IContentDatabase* m_contentDatabase = nullptr;
        foundation::UniquePtr<draconic::resource::ResourceManager> m_ownedResources;
        draconic::input::InputSubsystem* m_input = nullptr;
        draconic::ui::UISubsystem* m_ui = nullptr;
        // Backs the Ui.* script facade with the live screen tier (task #123 step 3.5): the host owns
        // the overlay map + control ops; the binding routes into it and is installed on every run
        // context by the context configurator. App-owned (the screen tier is app-wide).
        draconic::ui::UiScriptHost m_uiScriptHost;
        draconic::ui::UiScriptBinding m_uiScriptBinding;
        foundation::String m_uiFontPath;
        draconic::physics::PhysicsSubsystem* m_physics = nullptr;
        draconic::audio::AudioSubsystem* m_audio = nullptr;
        net::NetworkStartup m_netStartup; // preset before Configure (default = single-player)
        draconic::script::ScriptSubsystem* m_scripts = nullptr;
        // Every running game: the primary (a stable member) + any extras (stable UniquePtr addresses,
        // required because the SceneSubsystem borrows each SceneManager's pointer).
        template <typename Fn>
        void ForEachInstance(Fn&& fn)
        {
            fn(m_instance);
            for (foundation::UniquePtr<GameInstance>& gi : m_extraInstances)
            {
                fn(*gi);
            }
        }

        // The online hook wired onto every GameInstance: when its endpoint goes online, install the
        // client-side prefab net-spawn resolver (a replicated prefab id -> a live prefab from the
        // content DB; replication then applies the transform + fields on top). The server assigns ids;
        // game rules set relevancy. Built fresh each go-online so a reconnect re-wires correctly.
        [[nodiscard]] EndpointOnlineHook MakeEndpointOnlineHook();

        // Enter the preset startup role on the primary instance (None = single-player, no-op). The
        // reliable-config tuning uses the endpoint defaults here; the preset path is the CLI/dedicated
        // launch (scripts go online via the Net facade instead).
        void ApplyNetworkStartup(GameInstance& instance);

        draconic::scene::SceneSubsystem* m_scenes = nullptr;
        GameInstance m_instance; // the primary running game (app-level ops target this one)
        foundation::Array<foundation::UniquePtr<GameInstance>>
            m_extraInstances; // multi-instance PIE / headless server
    };
}

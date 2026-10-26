// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - :game_page partition.
//
// GameEditorPage: a singleton
// "Game" dock tab hosting the PLAYER behavior - a FRESH run of the project's default scene,
// exactly what Engine.Player does, in-process. Distinct from the ScenePage's Simulate
// (in-place snapshot -> run -> restore): nothing here is edited, so Play builds everything
// from scratch (fresh Scene + resolve + Start) and Stop tears it all down - total cleanup IS
// the restore. Renders through the scene's own primary camera (RenderScene with no override;
// EnsureCamera frames the origin when the scene ships none, like the player).
//
// On the GUI (Documentation/Specs/editor-gui.md, 5.3, layer 7, part 3), from its markup
// (Data/Editor/Game.gml): the kit's transport (Restart, Play, Pause, Stop, the state named beside
// them and the scene it runs, a Play waiting for the cook said so), the resolution and the phone,
// the debugger's toggle; the game on GUI.Viewport, which keeps every key while it runs and has
// the focus (Shift+F1 gives them back), the script debugger beside it.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT (the game-audio settings section)

export module editor.play:game_page;

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;
import engine.render;
import foundation.runtime;
import foundation.runtime.client;
import foundation.graphics;
import engine.scene;
import foundation.rhi;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import foundation.script;
import foundation.script.resource;  // ScriptClass (the cooked game script, bound from the content DB)
import engine.script; // ScriptSubsystem / ScriptRunHost (debugger wiring)
import foundation.shell;
import foundation.input;
import foundation.input.resource;
import engine.input;
import engine.physics;
import foundation.audio;
import foundation.audio.resource;
import engine.audio;
import engine.defaultapp;
import engine.gameinstance; // GameInstance - this tab drives its OWN run (multi-instance PIE)
import editor.core;
import :game_resolution;
import editor.app;
import editor.scene;         // ViewportCapture, ViewportCaptureRecorder (pie_screenshot), IGameScenePage
import :pie_page_interface;   // IPieInstancePage (published on the page)

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace editor
{
    // The editor's game-audio preference (a user settings section, the Preferences dialog's
    // "Game audio" category): with several Game tabs running, hear only the focused one (the
    // default) or every instance at once.
    class GameAudioEditorSettings final : public ISerializable
    {
        RTTI_OBJECT(GameAudioEditorSettings, ISerializable)
    public:
        bool hearAllInstances = false;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "hearAllInstances", hearAllInstances);
        }
    };

    RTTI_DEFINE_OBJECT(GameAudioEditorSettings, "rtti::editor")

    /// TYPE registration, before the app constructs (the user settings file loads during boot).
    void RegisterGameAudioEditorSettingsTypes();
    /// The Preferences contribution (needs the live context).
    void RegisterGameAudioEditorSettings(EditorContext& context);
    /// The preference's value (the default when no store or section exists).
    [[nodiscard]] bool HearAllGameInstances(const EditorContext& context);

    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace script = foundation::script;

    // The debugger panel beside the game (its markup in Game.gml): Continue, Step Into, Step
    // Over and Break; the call stack; the locals, an object's members one level under it. It
    // consumes ONLY the neutral IScriptDebugger and its snapshot types, so the same panel would
    // drive a remote debugger. The Game page rebuilds it from OnUpdate, never mid-script-dispatch.
    class DebuggerPanel
    {
    public:
        DebuggerPanel() = default;
        DebuggerPanel(const DebuggerPanel&) = delete;
        DebuggerPanel& operator=(const DebuggerPanel&) = delete;
        ~DebuggerPanel();

        /// Over `pane` (the markup's `game-debugger-continue`, `-step-into`, `-step-over`,
        /// `-break`, `-state`, `-stack` and `-locals`, by id).
        void Bind(foundation::gui::Node& pane);

        /// The active debugger for the run (null when not debugging). Clears the panel.
        void SetDebugger(script::IScriptDebugger* debugger);
        [[nodiscard]] script::IScriptDebugger* Debugger() const noexcept { return m_debugger; }
        /// Rebuild the stack and locals from the debugger (at a break, from OnUpdate).
        void Refresh();
        void Clear();
        void SetIdle();

        // === What it shows (for tests) ===
        [[nodiscard]] StringView State() const;
        [[nodiscard]] usize StackCount() const;

    private:
        static void AppendInt(String& out, i32 value);

        foundation::gui::Label* m_status = nullptr;
        foundation::gui::TreeView* m_stack = nullptr;
        foundation::gui::TreeView* m_locals = nullptr;
        foundation::gui::TreeModel m_stackModel;
        foundation::gui::TreeModel m_localsModel;
        script::IScriptDebugger* m_debugger = nullptr; // borrowed (owned by the run host)
    };

    // The Game page's debugger sink: it never touches views (that would be mid-script-dispatch);
    // it only records the latest state + a dirty flag the page drains from OnUpdate.
    class GameDebugListener final : public script::IScriptDebuggerListener
    {
    public:
        script::ScriptDebuggerState state = script::ScriptDebuggerState::Running;
        bool changed = false;
        void OnDebuggerStateChanged(script::ScriptDebuggerState newState) override;
    };

    // The play-in-editor device seam: keyboard/mouse come from the Game
    // viewport's GATED InputSurface facades (hover = mouse, focus = keyboard - click the
    // viewport to play), gamepads pass through from the shell only while the viewport has
    // focus. This is the by-construction fix for "the editor viewport forwards nothing".
    // Pure forwarding: EVERY facade comes from the viewport's InputSurface, which owns all
    // gating and transformation (mouse hover+transform, keyboard focus, gamepads gated on
    // focus via SurfaceGamepad, touch transformed + spatially gated). This class only
    // adapts the surface to the input runtime's provider seam - no gating logic here.
    class GameViewportInputSource final : public foundation::input::IInputSourceProvider
    {
    public:
        foundation::gui::Viewport* viewport = nullptr;         // borrowed
        foundation::shell::IInputManager* shellInput = nullptr; // borrowed (count only)

        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() override;
        [[nodiscard]] foundation::shell::IMouse* Mouse() override;
        [[nodiscard]] i32 GamepadCount() const override;
        [[nodiscard]] foundation::shell::IGamepad* Gamepad(i32 index) override;
        [[nodiscard]] foundation::shell::ITouch* Touch() override;
        [[nodiscard]] Span<const foundation::shell::InputEvent> Events() override;
    };

    // Script runtime faults during play surface as editor notices, not console-only lines.
    class GameScriptErrorSink final : public foundation::script::IScriptErrorHandler
    {
    public:
        EditorContext* context = nullptr;
        void OnError(const foundation::script::ScriptError& error) override;
    };

    class GameEditorPage final : public app::UIEditorPage,
                                 public IPieInstancePage,
                                 public IGameRunPage
    {
    public:
        static constexpr const char8_t* kPath = u8"Editor/Game.gml";

        /// What the tab asks of the editor.
        struct Services
        {
            foundation::gui::GuiHost* gui = nullptr; // hosts its viewport (null: none, a test)
            /// Its markup by its path under the data root (unset: a bare panel, a test).
            Function<RefPtr<foundation::gui::Node>(StringView path)> loadMarkup;
        };

        /// `pieId` names the tab: the id its dock panel persists under and the PIE tools address
        /// it by (`game-page`, then `game-page-1`, ...).
        GameEditorPage(EditorContext& context, runtime::IApplicationHost& host, Services services, engine::runtime::DefaultApplication* embeddedApp,
                       engine::runtime::GameInstance* instance, StringView pieId);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        /// A Game tab's name from its play id: the first "Game", Play New Instance's "Game 2",
        /// "Game 3" (its id's number and one), so tabs side by side are told apart.
        [[nodiscard]] static String TitleFor(StringView pieId);
        [[nodiscard]] Status Save() override { return Status{}; } // nothing here is a document
        [[nodiscard]] RefPtr<foundation::gui::Node> Content() override { return m_content; }

        // The scene group this tab's game scene belongs to: the embedded app's GameInstance manager
        // so the game scene groups + ticks + is bound to the instance's run
        // host. Falls back to THIS page's own (unregistered) manager if there's no embedded app - a
        // defensive placeholder, since without a runtime there is no game to run anyway.
        [[nodiscard]] scene::SceneManager& SceneGroup() noexcept;

        /// Fresh player run, cook-gated: kicks an incremental cook and latches the start;
        /// OnUpdate runs StartRunNow on the first frame the cook service is idle, so the
        /// run never binds against a half-written cooked DB.
        void Play() override;

        /// Total teardown - the fresh-run model's whole cleanup story. Also cancels a
        /// Play still waiting on the cook.
        void Stop() override;
        void StopRun() override { Stop(); }

        // ---- IPieInstancePage: the run as the PIE tools see it ----
        [[nodiscard]] StringView PieId() const noexcept override { return m_pieId.AsView(); }
        [[nodiscard]] bool IsRunning() const noexcept override { return m_running; }
        [[nodiscard]] bool IsStarting() const noexcept override { return m_pendingPlay; }
        [[nodiscard]] StringView SceneName() const noexcept override
        {
            return m_scene != nullptr ? m_scene->Name() : StringView{};
        }
        [[nodiscard]] f64 RunTime() const noexcept override
        {
            return (m_running && m_gameInstance != nullptr) ? m_gameInstance->RunTime()
                                                            : m_stoppedRunTime;
        }
        [[nodiscard]] u64 FrameCount() const noexcept override { return m_frameCount; }
        [[nodiscard]] PieScriptState ScriptState() const noexcept override
        {
            if (m_gameInstance == nullptr)
            {
                return PieScriptState::None;
            }
            if (m_gameInstance->ScriptRunning())
            {
                return PieScriptState::Running;
            }
            return m_gameInstance->ScriptFault().IsEmpty() ? PieScriptState::None
                                                           : PieScriptState::Faulted;
        }
        [[nodiscard]] StringView ScriptFault() const noexcept override
        {
            return m_gameInstance != nullptr ? m_gameInstance->ScriptFault() : StringView{};
        }
        void RequestViewportCapture(StringView path) override { m_capture.Request(path); }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override
        {
            return m_capture.State();
        }
        [[nodiscard]] scene::Scene* RunningScene() noexcept override
        {
            return m_running ? m_scene : nullptr;
        }
        [[nodiscard]] Result<Variant> GetScriptProperty(StringView name) const override
        {
            if (m_gameInstance == nullptr)
            {
                return Err(ErrorCode::NotFound);
            }
            return m_gameInstance->GetScriptProperty(name);
        }
        void BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource> source) override;
        void EndScriptedInput() override;
        [[nodiscard]] bool IsScripted() const noexcept override
        {
            return m_scripted.Get() != nullptr;
        }
        [[nodiscard]] Span<const GameResolutionChoice> ResolutionChoices() override;
        [[nodiscard]] StringView ResolutionKey() const noexcept override { return m_resolutionKey.AsView(); }
        bool SelectResolution(StringView key) override;
        [[nodiscard]] bool PhonePreview() const noexcept override { return m_phonePreview; }
        /// The game's view (for tests: its focus and capture).
        [[nodiscard]] foundation::gui::Viewport* View() const noexcept { return m_viewport.Get(); }
        /// Whether the script debugger is beside the game now.
        [[nodiscard]] bool DebuggerShown() const;
        void SelectPhonePreview(bool on) override;

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;

        void OnAfterSceneRender(runtime::IApplicationHost& host,
                                foundation::graphics::FrameContext& frame) override;

        void OnClose() override;

    private:
        // Follow the instance's CURRENT scene: when a scripted switch (run.loadScene / an async load
        // that just activated) repoints m_gameInstance->GetScene(), adopt it as this tab's borrowed
        // render/input pointer and retire the scene(s) we were leaving. Polled once per OnUpdate.
        void FollowInstanceScene();

        // Authored game scenes should carry a camera; a bare scene shouldn't play as a black
        // screen - frame the origin like Engine.Player does.
        void EnsureCamera(scene::Scene& scene);

        // Play-in-editor input: the project's default map into the shared InputSubsystem,
        // devices swapped to the Game viewport's gated facades. Stop restores the shell.
        void BindInput();

        // The project's default audio bus layout into the embedded engine (the player's
        // startup twin) - nil/unresolved = the built-in neutral layout stays.
        void BindBusLayout(runtime::IApplicationHost& host);

        // Resolves the startup script's SOURCE (editor project layout); the lifecycle -
        // facades, services, launch/update/exit, fault handling - is the embedded app's.
        void StartGameScriptFromProject();

        // Make this run debuggable: request a debugger on the run's script manager and, once it
        // exists, apply the editor's breakpoints + hand it to the panel. Lazy - the debugger is
        // created when the script context is (first behavior / game script).
        void EnableDebugging();

        // Drain the debugger's state changes at the top level (never mid-script-dispatch): on a
        // break, freeze the whole scene simulation (physics + behaviors) and populate the panel;
        // on resume, thaw and clear it. A locals-expand click also rebuilds here.
        void DrainDebuggerState();

        // Diff-apply the shared breakpoint store onto the live run's debugger (gutter
        // toggles during a run take effect without a restart).
        void SyncBreakpointsToDebugger();
        // The run's sound follows the tab: the host pause, the focus, the hear-all preference.
        void SyncRunAudio();

        // The resolution dropdown (:game_resolution): built once, its choices refreshed when
        // what they come from changes (looked at once a second), the choice kept per project.
        // The bar from the markup: the transport, the resolutions and the phone, the debugger.
        void BindBar();
        void BindResolution();
        [[nodiscard]] i32 SelectedResolution() const;
        void TickResolution(f32 dt);
        void RefreshResolutionChoices(bool force);
        // The chosen size, fitted by the project's render fit; the panel's own size stretches,
        // there being nothing to fit.
        void ApplyResolution();
        void LoadResolutionKey();
        // The game on a `width` x `height` target (the panel, or the previewed phone's screen), as
        // the player puts it in its window: its render resolution fitted by its render fit, turned
        // a quarter when its orientation is not the target's shape (then at the target's pixels
        // turned when it has no resolution). Plain (none of that) draws straight at the target.
        [[nodiscard]] ContentFit GameFitIn(u32 width, u32 height) const noexcept
        {
            return GameFitOn(width, height, m_renderWidth, m_renderHeight, m_renderFit, m_orientation);
        }
        [[nodiscard]] bool GameFitted(const ContentFit& fit) const noexcept
        {
            return (m_renderWidth > 0 && m_renderHeight > 0) || fit.rotation != ContentRotation::None;
        }
        // The input surface's content follows the game's fit on the tab's current target.
        void SyncGameFit();
        // The phone preview: the tab's screen becomes a portrait phone's, or the panel again.
        void SetPhonePreview(bool on);
        void SaveResolutionKey();

        // The transport as the run is now: what is enabled, the state named, the scene beside it.
        void RefreshToolbar();
        void ShowDebugger(bool shown);
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        // The deferred half of Play(): the actual boot (script launch + default scene),
        // run by OnUpdate once the cook is idle.
        void StartRunNow();

        // Scripted input (pie_run): a timeline source in place of the viewport's, on this tab's
        // game instance and, while this tab holds it, the input subsystem, so the other tabs
        // keep their own. What this tab's game reads: the script while one runs, else the
        // viewport.
        [[nodiscard]] foundation::input::IInputSourceProvider* ActiveSource() noexcept;
        // Once per frame from OnUpdate: the script advances by the run's time; an ended one
        // lets go of everything for a frame, then the viewport's input returns.
        void AdvanceScriptedInput();
        // The viewport source back where the script was installed, and the script gone.
        void DropScriptedInput();

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        Services m_services;
        engine::runtime::DefaultApplication* m_app = nullptr; // the embedded game application (v3)
        engine::runtime::GameInstance* m_gameInstance =
            nullptr; // THIS tab's running game (its own run host + scenes)
        engine::scene::SceneSubsystem* m_scenes = nullptr;
        engine::render::RenderSubsystem* m_render = nullptr;
        engine::audio::AudioSubsystem* m_audio = nullptr; // the run's sound: stop, pause, focus
        scene::Scene* m_scene = nullptr;
        engine::input::InputSubsystem* m_input = nullptr;
        foundation::shell::IInputManager* m_shellInput = nullptr;
        GameViewportInputSource m_viewportSource;

        RefPtr<foundation::gui::Node> m_content;
        foundation::gui::ButtonBase* m_restartButton = nullptr;
        foundation::gui::ButtonBase* m_playButton = nullptr;
        foundation::gui::ButtonBase* m_pauseToggle = nullptr;
        foundation::gui::ButtonBase* m_stopButton = nullptr;
        foundation::gui::Label* m_stateLabel = nullptr; // the transport's state: Playing, Paused, Script error
        foundation::gui::Label* m_sceneLabel = nullptr; // the scene it runs
        foundation::gui::Label* m_cookBadge = nullptr;  // while a Play waits for the cook
        foundation::gui::ButtonBase* m_debuggerToggle = nullptr; // shows the script debugger
        RefPtr<foundation::gui::Node> m_debuggerPane; // held while it is out of the split
        foundation::gui::Splitter* m_stage = nullptr;  // the game beside the debugger (the markup's)
        f32 m_debuggerShare = 0.22f;                    // its share of the split, kept while it is off
        foundation::gui::ComboBox* m_resolutionCombo = nullptr;
        Array<GameResolutionChoice> m_resolutionChoices;
        String m_resolutionSignature; // what the choices were built from
        String m_resolutionKey;       // the chosen entry, kept across rebuilds, saved per project
        f32 m_resolutionRefresh = 0.0f;
        bool m_resolutionRebuilding = false; // the rebuild's own selection is not a user's
        // What the game draws at, fitted into the panel by m_renderFit; nought draws at the panel's.
        u32 m_renderWidth = 0;
        u32 m_renderHeight = 0;
        FitMode m_renderFit = FitMode::Letterbox;
        // How the game is held (the chosen preset's, else the project's), and whether the tab
        // previews a phone held upright (Specs/present-transform.md).
        ScreenOrientation m_orientation = ScreenOrientation::Any;
        bool m_phonePreview = false;
        foundation::gui::ButtonBase* m_phoneToggle = nullptr;
        GameScriptErrorSink m_scriptErrors;
        DebuggerPanel m_debuggerPanel;      // the debugger UI (contract-only)
        GameDebugListener m_debugListener;  // debugger state sink (drained in OnUpdate)
        bool m_simPausedByDebugger = false; // we disabled sim for a breakpoint
        Array<EditorContext::ScriptBreakpoint> m_appliedBreakpoints; // mirror on the debugger
        RefPtr<foundation::gui::Viewport> m_viewport;
        scene::SceneManager m_fallbackScenes{Allocator()}; // no-embedded-app placeholder group (see SceneGroup)

        String m_sceneTitle;
        String m_pieId;                // IPieInstancePage::PieId
        String m_title;                // TitleFor(m_pieId)
        u64 m_frameCount = 0;          // frames rendered since the run started
        f64 m_stoppedRunTime = 0.0;    // the run time a stopped run ended at
        UniquePtr<foundation::input::ScriptedInputSource> m_scripted; // while a run scripts this tab
        f64 m_scriptedStart = 0.0;     // the run time the script's zero is
        bool m_scriptedEnding = false; // end asked: a release frame, then the viewport returns
        bool m_scriptedReleased = false;
        // The PIE capture (pie_screenshot): armed by RequestViewportCapture, recorded in
        // OnAfterSceneRender after the overlays, completed in the next OnUpdate.
        ViewportCaptureRecorder m_capture;
        bool m_renderedThisFrame = false; // the scene rendered into the viewport this frame, at:
        u32 m_captureX = 0; // where in the target the capture starts: the fitted image's corner
        u32 m_captureY = 0;
        u32 m_captureWidth = 0;
        u32 m_captureHeight = 0;
        bool m_running = false;
        bool m_pendingPlay = false; // Play latched, waiting for the cook to go idle
        bool m_exitRequested = false; // the game asked to exit; the run stops next OnUpdate
    };
}

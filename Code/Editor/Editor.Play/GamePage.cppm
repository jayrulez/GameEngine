// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - :game_page partition.
//
// GameEditorPage is a "Game" dock tab that runs the project as the player does: a fresh run of
// the project's default scene, in-process, exactly as Engine.Player runs it. This differs from the
// scene page's Simulate, which snapshots the scene in place, runs it and restores it. Nothing here
// is edited, so Play builds everything from scratch (a new Scene, resolved and started) and Stop
// tears it all down; that teardown is the restore. It renders through the scene's own primary
// camera (RenderScene with no override), and EnsureCamera frames the origin when the scene has no
// camera, as the player does.
//
// The tab is built from its markup (Data/Editor/Game.xml). Its bar holds the kit's transport
// (Restart, Play, Pause and Stop, with the run's state and scene named beside them and a badge while
// Play waits for the cook), the resolution and phone preview, and the debugger's toggle. The game
// runs on a GUI Viewport, which takes every key while the game runs and has the focus (Shift+F1
// gives the keys back), with the script debugger beside it.

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
import engine.script; // ScriptSubsystem and ScriptRunHost (debugger wiring)
import foundation.shell;
import foundation.input;
import foundation.input.resource;
import engine.input;
import engine.physics;
import foundation.audio;
import foundation.audio.resource;
import engine.audio;
import engine.defaultapp;
import engine.gameinstance; // GameInstance: each tab drives its own run (several PIE instances)
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

    /// Registers the settings type before the app is constructed (the user settings file loads
    /// during boot).
    void RegisterGameAudioEditorSettingsTypes();
    /// Registers the Preferences contribution (needs the live context).
    void RegisterGameAudioEditorSettings(EditorContext& context);
    /// The preference's value (the default when no store or section exists).
    [[nodiscard]] bool HearAllGameInstances(const EditorContext& context);

    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace script = foundation::script;

    // The debugger panel beside the game (its markup is in Game.xml): Continue, Step Into, Step
    // Over and Break; the call stack; and the locals, with an object's members one level under it.
    // It uses only the neutral IScriptDebugger and its snapshot types, so the same panel could
    // drive a remote debugger. The Game page rebuilds it from OnUpdate, never during a script
    // dispatch.
    class DebuggerPanel
    {
    public:
        DebuggerPanel() = default;
        DebuggerPanel(const DebuggerPanel&) = delete;
        DebuggerPanel& operator=(const DebuggerPanel&) = delete;
        ~DebuggerPanel();

        /// Binds to the controls under `pane`, by id: the markup's `game-debugger-continue`,
        /// `-step-into`, `-step-over`, `-break`, `-state`, `-stack` and `-locals`.
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

    // The Game page's debugger sink. It is called during a script dispatch, so it never touches
    // views; it only records the latest state and a dirty flag that the page drains from OnUpdate.
    class GameDebugListener final : public script::IScriptDebuggerListener
    {
    public:
        script::ScriptDebuggerState state = script::ScriptDebuggerState::Running;
        bool changed = false;
        void OnDebuggerStateChanged(script::ScriptDebuggerState newState) override;
    };

    // The play-in-editor input source. Keyboard and mouse come from the Game viewport's gated
    // InputSurface facades (the mouse while it hovers, the keyboard while the viewport has focus,
    // so the user clicks the viewport to play); gamepads pass through from the shell only while the
    // viewport has focus. Every facade comes from the viewport's InputSurface, which owns all gating
    // and transformation (mouse hover and transform, keyboard focus, gamepads gated on focus
    // through SurfaceGamepad, touch transformed and gated by position). This class only adapts the
    // surface to the input runtime's provider interface and holds no gating logic.
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
        static constexpr const char8_t* kPath = u8"Editor/Game.xml";

        /// What the tab asks of the editor.
        struct Services
        {
            foundation::gui::GuiHost* gui = nullptr; // hosts its viewport (null in a test)
            /// Loads its markup by its path under the data root (unset in a test, which gets a bare panel).
            Function<RefPtr<foundation::gui::Node>(StringView path)> loadMarkup;
        };

        /// `pieId` names the tab: the id its dock panel persists under and the PIE tools address
        /// it by (`game-page`, then `game-page-1`, ...).
        GameEditorPage(EditorContext& context, runtime::IApplicationHost& host, Services services, engine::runtime::DefaultApplication* embeddedApp,
                       engine::runtime::GameInstance* instance, StringView pieId);

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        /// A Game tab's title from its play id: "Game" for the first, then "Game 2", "Game 3" for
        /// Play New Instance's tabs (the id's number plus one), so tabs side by side can be told apart.
        [[nodiscard]] static String TitleFor(StringView pieId);
        [[nodiscard]] Status Save() override { return Status{}; } // nothing here is a document
        [[nodiscard]] RefPtr<foundation::gui::Node> Content() override { return m_content; }

        // The scene group this tab's game scene belongs to: the embedded app's GameInstance manager,
        // so the game scene is grouped, ticked and bound to the instance's run host. Without an
        // embedded app it falls back to this page's own unregistered manager, a placeholder, since
        // without a runtime there is no game to run.
        [[nodiscard]] scene::SceneManager& SceneGroup() noexcept;

        /// Starts a fresh player run after a cook: kicks an incremental cook and latches the
        /// start, and OnUpdate runs StartRunNow on the first frame the cook service is idle, so
        /// the run never binds against a half-written cooked DB.
        void Play() override;

        /// Tears the whole run down, which is all the cleanup a fresh run needs. Also cancels a
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
        /// Whether the script debugger is shown beside the game.
        [[nodiscard]] bool DebuggerShown() const;
        void SelectPhonePreview(bool on) override;

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;

        void OnAfterSceneRender(runtime::IApplicationHost& host,
                                foundation::graphics::FrameContext& frame) override;

        void OnClose() override;

    private:
        // Follows the instance's current scene. When a scripted switch (run.loadScene, or an async
        // load that just activated) repoints m_gameInstance->GetScene(), adopt it as this tab's
        // borrowed render and input pointer and retire the scenes being left. Polled once per OnUpdate.
        void FollowInstanceScene();

        // Authored game scenes should carry a camera, but a bare scene should not play as a black
        // screen, so frame the origin as Engine.Player does.
        void EnsureCamera(scene::Scene& scene);

        // Play-in-editor input: the project's default map goes into the shared InputSubsystem, with
        // the devices swapped to the Game viewport's gated facades. Stop restores the shell's.
        void BindInput();

        // Applies the project's default audio bus layout to the embedded engine, as the player does
        // at startup. With none set, or one that does not resolve, the built-in neutral layout stays.
        void BindBusLayout(runtime::IApplicationHost& host);

        // Resolves the startup script's source from the editor project layout. The lifecycle
        // (facades, services, launch, update and exit, fault handling) belongs to the embedded app.
        void StartGameScriptFromProject();

        // Makes this run debuggable: requests a debugger on the run's script manager and, once it
        // exists, applies the editor's breakpoints and hands it to the panel. The debugger is
        // created lazily, with the script context (the first behavior or game script).
        void EnableDebugging();

        // Drains the debugger's state changes at the top level, never during a script dispatch. On
        // a break it freezes the whole scene simulation (physics and behaviors) and fills the panel;
        // on resume it thaws the simulation and clears the panel. A click expanding a local also
        // rebuilds here.
        void DrainDebuggerState();

        // Applies the differences between the shared breakpoint store and the live run's debugger,
        // so gutter toggles during a run take effect without a restart.
        void SyncBreakpointsToDebugger();
        // The run's sound follows the tab: the host pause, the focus, the hear-all preference.
        void SyncRunAudio();

        // The resolution dropdown (:game_resolution) is built once. Its choices refresh when what
        // they come from changes (checked once a second), and the choice is kept per project.
        // BindBar binds the bar from the markup: the transport, the resolutions, the phone preview
        // and the debugger toggle.
        void BindBar();
        void BindResolution();
        [[nodiscard]] i32 SelectedResolution() const;
        void TickResolution(f32 dt);
        void RefreshResolutionChoices(bool force);
        // Applies the chosen size, fitted by the project's render fit. The panel's own size
        // stretches, since there is nothing to fit.
        void ApplyResolution();
        void LoadResolutionKey();
        // How the game sits on a `width` x `height` target (the panel, or the previewed phone's
        // screen), as the player puts it in its window: its render resolution fitted by its render
        // fit, and turned a quarter when its orientation does not match the target's shape (with no
        // render resolution, it is drawn turned at the target's pixels). When GameFitted is false,
        // the game draws straight at the target.
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
        // The phone preview: the tab's screen becomes a portrait phone's, or the whole panel again.
        void SetPhonePreview(bool on);
        void SaveResolutionKey();

        // Updates the transport for the run's current state: what is enabled, the state's name and
        // the scene beside it.
        void RefreshToolbar();
        void ShowDebugger(bool shown);
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        // The deferred half of Play(): the actual boot (script launch and default scene), run by
        // OnUpdate once the cook is idle.
        void StartRunNow();

        // Scripted input (pie_run) installs a timeline source in place of the viewport's, on this
        // tab's game instance and, while this tab holds it, on the input subsystem, so the other
        // tabs keep their own. ActiveSource is what this tab's game reads: the script while one
        // runs, otherwise the viewport.
        [[nodiscard]] foundation::input::IInputSourceProvider* ActiveSource() noexcept;
        // Called once per frame from OnUpdate: the script advances by the run's time. When it has
        // ended, everything is released for one frame, then the viewport's input returns.
        void AdvanceScriptedInput();
        // Puts the viewport source back where the script was installed and drops the script.
        void DropScriptedInput();

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        Services m_services;
        engine::runtime::DefaultApplication* m_app = nullptr; // the embedded game application
        engine::runtime::GameInstance* m_gameInstance =
            nullptr; // this tab's running game (its own run host and scenes)
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
        RefPtr<foundation::gui::Node> m_debuggerPane; // kept alive while it is out of the split
        foundation::gui::Splitter* m_stage = nullptr;  // the game beside the debugger, from the markup
        f32 m_debuggerShare = 0.22f;                    // the debugger's share of the split, kept while hidden
        foundation::gui::ComboBox* m_resolutionCombo = nullptr;
        Array<GameResolutionChoice> m_resolutionChoices;
        String m_resolutionSignature; // what the choices were built from
        String m_resolutionKey;       // the chosen entry, kept across rebuilds, saved per project
        f32 m_resolutionRefresh = 0.0f;
        bool m_resolutionRebuilding = false; // set during a rebuild, whose selection is not the user's
        // The size the game draws at, fitted into the panel by m_renderFit; nought draws at the panel's size.
        u32 m_renderWidth = 0;
        u32 m_renderHeight = 0;
        FitMode m_renderFit = FitMode::Letterbox;
        // How the game is held (the chosen preset's orientation, else the project's), and whether
        // the tab previews a phone held upright.
        ScreenOrientation m_orientation = ScreenOrientation::Any;
        bool m_phonePreview = false;
        foundation::gui::ButtonBase* m_phoneToggle = nullptr;
        GameScriptErrorSink m_scriptErrors;
        DebuggerPanel m_debuggerPanel;      // the debugger UI (uses only the debugger interface)
        GameDebugListener m_debugListener;  // debugger state sink (drained in OnUpdate)
        bool m_simPausedByDebugger = false; // the simulation was disabled for a breakpoint
        Array<EditorContext::ScriptBreakpoint> m_appliedBreakpoints; // as last applied to the debugger
        RefPtr<foundation::gui::Viewport> m_viewport;
        scene::SceneManager m_fallbackScenes{Allocator()}; // placeholder with no embedded app (see SceneGroup)

        String m_sceneTitle;
        String m_pieId;                // IPieInstancePage::PieId
        String m_title;                // TitleFor(m_pieId)
        u64 m_frameCount = 0;          // frames rendered since the run started
        f64 m_stoppedRunTime = 0.0;    // the run time a stopped run ended at
        UniquePtr<foundation::input::ScriptedInputSource> m_scripted; // while a run scripts this tab
        f64 m_scriptedStart = 0.0;     // the run time the script's zero is
        bool m_scriptedEnding = false; // an end was asked: one release frame, then the viewport returns
        bool m_scriptedReleased = false;
        // The PIE capture (pie_screenshot): armed by RequestViewportCapture, recorded in
        // OnAfterSceneRender after the overlays, completed in the next OnUpdate.
        ViewportCaptureRecorder m_capture;
        bool m_renderedThisFrame = false; // the scene rendered into the viewport this frame, at the place below
        u32 m_captureX = 0; // where in the target the capture starts: the fitted image's corner
        u32 m_captureY = 0;
        u32 m_captureWidth = 0;
        u32 m_captureHeight = 0;
        bool m_running = false;
        bool m_pendingPlay = false; // Play latched, waiting for the cook to go idle
        bool m_exitRequested = false; // the game asked to exit; the run stops next OnUpdate
    };
}

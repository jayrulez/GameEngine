// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :game_page partition.
//
// GameEditorPage: a singleton
// "Game" dock tab hosting the PLAYER behavior - a FRESH run of the project's default scene,
// exactly what Engine.Player does, in-process. Distinct from the ScenePage's Simulate
// (in-place snapshot -> run -> restore): nothing here is edited, so Play builds everything
// from scratch (fresh Scene + resolve + Start) and Stop tears it all down - total cleanup IS
// the restore. Renders through the scene's own primary camera (RenderScene with no override;
// EnsureCamera frames the origin when the scene ships none, like the player).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT (the game-audio settings section)

export module editor.scene:game_page;

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
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.vg.renderer;
import foundation.ui.viewport;
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
import :scene_page_interface; // ViewportCapture
import :viewport_capture;     // ViewportCaptureRecorder (pie_screenshot)
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
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace vg = foundation::vg;
    namespace script = foundation::script;

    // The debugger panel: a Break/Continue/StepInto/StepOver toolbar, a
    // call-stack list, and a locals tree with one level of lazy object expansion. It consumes
    // ONLY the neutral IScriptDebugger + the snapshot types - no in-process assumptions - so the
    // same panel would drive a remote debugger. It never mutates views mid-event-dispatch: the
    // Game page rebuilds it from OnUpdate (top level); a locals-expand click only flags dirty.
    class DebuggerPanel
    {
    public:
        DebuggerPanel()
        {
            auto column = MakeRef<ui::FlexLayout>(editor::EditorRootAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 4.0f;
            column->Padding = ui::Thickness{6, 6};

            auto bar = MakeRef<ui::FlexLayout>(editor::EditorRootAllocator());
            bar->Direction = ui::Orientation::Horizontal;
            bar->Spacing = 4.0f;
            DebuggerPanel* self = this;
            AddToolButton(*bar, u8"Continue",
                          [self]()
                          {
                              if (self->m_debugger)
                              {
                                  self->m_debugger->Continue();
                              }
                          });
            AddToolButton(*bar, u8"Step Into",
                          [self]()
                          {
                              if (self->m_debugger)
                              {
                                  self->m_debugger->StepInto();
                              }
                          });
            AddToolButton(*bar, u8"Step Over",
                          [self]()
                          {
                              if (self->m_debugger)
                              {
                                  self->m_debugger->StepOver();
                              }
                          });
            AddToolButton(*bar, u8"Break",
                          [self]()
                          {
                              if (self->m_debugger)
                              {
                                  self->m_debugger->Break();
                              }
                          });
            column->AddView(bar.Get(), MatchWidth());

            m_status =
                MakeRef<ui::Label>(editor::EditorRootAllocator(), StringView(u8"Debugger: not running"));
            m_status->FontSize.SetValue(12.0f);
            column->AddView(m_status.Get(), MatchWidth());

            column->AddView(
                MakeRef<ui::Label>(editor::EditorRootAllocator(), StringView(u8"Call Stack")).Get(),
                MatchWidth());
            m_stackList = MakeRef<ui::FlexLayout>(editor::EditorRootAllocator());
            m_stackList->Direction = ui::Orientation::Vertical;
            column->AddView(m_stackList.Get(), MatchWidth());

            column->AddView(MakeRef<ui::Label>(editor::EditorRootAllocator(), StringView(u8"Locals")).Get(),
                            MatchWidth());
            m_localsList = MakeRef<ui::FlexLayout>(editor::EditorRootAllocator());
            m_localsList->Direction = ui::Orientation::Vertical;
            {
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(m_localsList.Get(), lp);
            }
            m_root = column;
        }

        [[nodiscard]] ui::View* RootView() const noexcept { return m_root.Get(); }

        /// The active debugger for the run (null when not debugging). Clears the panel.
        void SetDebugger(script::IScriptDebugger* debugger);
        [[nodiscard]] script::IScriptDebugger* Debugger() const noexcept { return m_debugger; }

        /// Rebuild the stack + locals from the debugger (called at a break, from OnUpdate).
        void Refresh();

        void Clear();

        void SetIdle();

        /// A locals-expand click flagged a rebuild (consumed by the Game page's OnUpdate, so the
        /// view tree is never mutated mid-event-dispatch).
        [[nodiscard]] bool ConsumeDirty() noexcept;

    private:
        [[nodiscard]] static ui::LayoutStyle MatchWidth();

        template <typename Fn>
        void AddToolButton(ui::FlexLayout& bar, StringView label, Fn onClick)
        {
            auto button = MakeRef<ui::Button>(editor::EditorRootAllocator(), label);
            button->FontSize.SetValue(12.0f);
            button->OnClick.Add([onClick](ui::ButtonBase*) { onClick(); });
            bar.AddView(button.Get());
        }

        void AddRow(ui::FlexLayout& list, StringView text, f32 indent);

        void AddLocalRow(const script::ScriptVariable& variable, f32 indent);

        [[nodiscard]] bool IsExpanded(u64 ref) const;

        void ToggleExpand(u64 ref);

        static void AppendInt(String& out, i32 value);

        RefPtr<ui::View> m_root;
        RefPtr<ui::Label> m_status;
        RefPtr<ui::FlexLayout> m_stackList;
        RefPtr<ui::FlexLayout> m_localsList;
        script::IScriptDebugger* m_debugger = nullptr; // borrowed (owned by the run host)
        Array<u64> m_expanded;                         // expanded object refs (per break)
        bool m_dirty = false;
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
        ui::viewport::ViewportView* viewport = nullptr;       // borrowed
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
        /// `pieId` names the tab: the id its dock panel persists under and the PIE tools address
        /// it by (`game-page`, then `game-page-1`, ...).
        GameEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                       ui::runtime::UIHost& uiHost, engine::runtime::DefaultApplication* embeddedApp,
                       engine::runtime::GameInstance* instance, StringView pieId)
            : app::UIEditorPage(context.Allocator()),
              m_context(&context), m_host(&host), m_uiHost(&uiHost), m_app(embeddedApp),
              m_gameInstance(instance), m_pieId(pieId)
        {
            Provide<IPieInstancePage>(*this); // what the PIE tools act through
            Provide<IGameRunPage>(*this);     // what the editor stops before a native reload
            m_scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();
            m_audio = host.Ctx().GetSubsystem<engine::audio::AudioSubsystem>();
            m_input = host.Ctx().GetSubsystem<engine::input::InputSubsystem>();
            m_shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;

            m_viewport = MakeRef<ui::viewport::ViewportView>(Allocator());
            m_viewport->ClearColor = rhi::ClearColor{0.248f, 0.248f, 0.272f, 1.0f};
            // The finished game, as the player draws it into its sRGB window: 8-bit sRGB rather
            // than the viewports' float default, so the Game tab matches the player and its screen
            // UI can blend encoded values through the target's UNORM view (RenderOverlays).
            m_viewport->SetFormats(rhi::TextureFormat::RGBA8UnormSrgb, m_viewport->DepthFormat());

            // THIS instance's exit request stops THIS tab's run, whichever tab it is: the
            // script asking fires inside the embedded app's update, which runs before the
            // pages, so the stop is deferred to this page's next OnUpdate, no script call on
            // the stack.
            if (m_gameInstance != nullptr)
            {
                m_gameInstance->RunBinding().requestExit = Function<void(i32)>{
                    [this](i32 code)
                    {
                        LOG_INFO(u8"Editor", u8"Game: '{}' requested exit({})", m_pieId, code);
                        m_exitRequested = true;
                    }};
            }

            // Toolbar: Play / Stop / Restart + the run-state readout.
            GameEditorPage* self = this;
            m_toolbar = MakeRef<ui::toolkit::Toolbar>(Allocator());
            m_playButton = m_toolbar->AddButton(u8"Play");
            m_playButton->OnClick.Add([self](ui::toolkit::ToolbarButton*) { self->Play(); });
            m_pauseToggle = m_toolbar->AddToggle(u8"Pause");
            m_pauseToggle->OnCheckedChanged.Add(
                [self](ui::toolkit::ToolbarToggle*, bool paused)
                {
                    if (self->m_scene != nullptr && self->m_running)
                    {
                        self->m_scene->SetSimulationEnabled(!paused);
                    }
                });
            m_stopButton = m_toolbar->AddButton(u8"Stop");
            m_stopButton->OnClick.Add([self](ui::toolkit::ToolbarButton*) { self->Stop(); });
            m_restartButton = m_toolbar->AddButton(u8"Restart");
            m_restartButton->OnClick.Add(
                [self](ui::toolkit::ToolbarButton*)
                {
                    self->Stop();
                    self->Play();
                });
            // The script debugger beside the viewport: off by default (it takes room the game
            // wants); opened here, or by a breakpoint or step that pauses the run.
            m_debuggerToggle = m_toolbar->AddToggle(u8"Debugger");
            m_debuggerToggle->OnCheckedChanged.Add(
                [self](ui::toolkit::ToolbarToggle*, bool shown)
                {
                    ui::View* panel = self->m_debuggerPanel.RootView();
                    panel->Visibility = shown ? ui::Visibility::Visible : ui::Visibility::Gone;
                    panel->Invalidate(); // the stage gives the viewport the room back
                });
            m_statusLabel = MakeRef<foundation::ui::Label>(Allocator(), StringView(u8""));
            m_statusLabel->FontSize.SetValue(13.0f);
            // The resolution the game draws at, the project's by default (mouse and touch map
            // through the same fit).
            CreateResolutionCombo();
            {
                foundation::ui::LayoutStyle lp;
                lp.Height = foundation::ui::SizeSpec::Match();
                m_toolbar->AddView(m_statusLabel.Get(), lp);
            }

            auto column = MakeRef<foundation::ui::FlexLayout>(Allocator());
            column->Direction = foundation::ui::Orientation::Vertical;
            {
                foundation::ui::LayoutStyle lp;
                lp.Width = foundation::ui::SizeSpec::Match();
                lp.Height = foundation::ui::SizeSpec::Fixed(foundation::ui::Unit::Dp(30));
                column->AddView(m_toolbar.Get(), lp);
            }
            // The play stage: the game viewport (grows) beside the debugger panel (fixed).
            auto stage = MakeRef<ui::FlexLayout>(Allocator());
            stage->Direction = ui::Orientation::Horizontal;
            {
                foundation::ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.Height = foundation::ui::SizeSpec::Match();
                stage->AddView(m_viewport.Get(), lp);
            }
            {
                foundation::ui::LayoutStyle lp;
                lp.Width = foundation::ui::SizeSpec::Fixed(foundation::ui::Unit::Dp(300));
                lp.Height = foundation::ui::SizeSpec::Match();
                stage->AddView(m_debuggerPanel.RootView(), lp);
                m_debuggerPanel.RootView()->Visibility = ui::Visibility::Gone; // until shown
            }
            {
                foundation::ui::LayoutStyle lp;
                lp.Width = foundation::ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                column->AddView(stage.Get(), lp);
            }
            m_content = column;
            RefreshToolbar();
        }

        [[nodiscard]] StringView Title() const override { return u8"Game"; }
        [[nodiscard]] Status Save() override { return Status{}; } // nothing here is a document
        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }

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
        void SelectPhonePreview(bool on) override;

        // Bind (and re-bind after dock/float moves) the viewport to the window hosting it -
        // the same RendererFor dance as ScenePage. Without this the viewport never gets a
        // DEVICE: no color target, IsReady() false, and the tab renders only its clear color.
        void EnsureViewportBound();

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
        void CreateResolutionCombo();
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

        void RefreshToolbar();

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
        ui::runtime::UIHost* m_uiHost = nullptr;
        engine::runtime::DefaultApplication* m_app = nullptr; // the embedded game application (v3)
        engine::runtime::GameInstance* m_gameInstance =
            nullptr; // THIS tab's running game (its own run host + scenes)
        foundation::graphics::RenderWindow* m_hostWindow =
            nullptr; // borrowed; tracks dock/float moves
        engine::scene::SceneSubsystem* m_scenes = nullptr;
        engine::render::RenderSubsystem* m_render = nullptr;
        engine::audio::AudioSubsystem* m_audio = nullptr; // the run's sound: stop, pause, focus
        scene::Scene* m_scene = nullptr;
        engine::input::InputSubsystem* m_input = nullptr;
        foundation::shell::IInputManager* m_shellInput = nullptr;
        GameViewportInputSource m_viewportSource;

        RefPtr<foundation::ui::View> m_content;
        RefPtr<ui::toolkit::Toolbar> m_toolbar;
        ui::toolkit::ToolbarButton* m_playButton = nullptr;
        ui::toolkit::ToolbarButton* m_stopButton = nullptr;
        ui::toolkit::ToolbarToggle* m_pauseToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_debuggerToggle = nullptr; // shows the script debugger
        ui::toolkit::ToolbarButton* m_restartButton = nullptr;
        RefPtr<ui::ComboBox> m_resolutionCombo;
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
        ui::toolkit::ToolbarToggle* m_phoneToggle = nullptr;
        GameScriptErrorSink m_scriptErrors;
        DebuggerPanel m_debuggerPanel;      // the debugger UI (contract-only)
        GameDebugListener m_debugListener;  // debugger state sink (drained in OnUpdate)
        bool m_simPausedByDebugger = false; // we disabled sim for a breakpoint
        Array<EditorContext::ScriptBreakpoint> m_appliedBreakpoints; // mirror on the debugger
        RefPtr<foundation::ui::Label> m_statusLabel;
        RefPtr<ui::viewport::ViewportView> m_viewport;
        UniquePtr<foundation::shell::InputRouter>
            m_router;                         // gates the viewport surface (hover/focus)
        scene::SceneManager m_fallbackScenes{Allocator()}; // no-embedded-app placeholder group (see SceneGroup)

        String m_sceneTitle;
        String m_pieId;                // IPieInstancePage::PieId
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

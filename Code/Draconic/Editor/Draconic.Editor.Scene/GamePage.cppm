// Draconic::EditorScene - :game_page partition.
//
// GameEditorPage (play-in-editor phase 8b, docs/design/roadmap.md MVP item 4): a singleton
// "Game" dock tab hosting the PLAYER behavior - a FRESH run of the project's default scene,
// exactly what Draconic.Engine.Player does, in-process. Distinct from the ScenePage's Simulate
// (in-place snapshot -> run -> restore): nothing here is edited, so Play builds everything
// from scratch (fresh Scene + resolve + Start) and Stop tears it all down - total cleanup IS
// the restore. Renders through the scene's own primary camera (RenderScene with no override;
// EnsureCamera frames the origin when the scene ships none, like the player).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.scene:game_page;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.resource;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.render;
import draconic.runtime;
import draconic.runtime.client;
import draconic.graphics;
import draconic.engine.scene;
import draconic.rhi;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.vg.renderer;
import draconic.ui.viewport;
import draconic.script;
import draconic.script.resource;  // ScriptClass (the cooked game script, bound from the content DB)
import draconic.engine.script; // ScriptSubsystem / ScriptRunHost (debugger wiring)
import draconic.shell;
import draconic.input;
import draconic.input.resource;
import draconic.engine.input;
import draconic.engine.physics;
import draconic.audio;
import draconic.audio.resource;
import draconic.engine.audio;
import draconic.engine.defaultapp;
import draconic.engine.gameinstance; // GameInstance - this tab drives its OWN run (multi-instance PIE)
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace scene = draconic::scene;
    namespace render = draconic::render;
    namespace vg = draconic::vg;
    namespace script = draconic::script;

    // The debugger panel (script-debugger.md P1): a Break/Continue/StepInto/StepOver toolbar, a
    // call-stack list, and a locals tree with one level of lazy object expansion. It consumes
    // ONLY the neutral IScriptDebugger + the snapshot types - no in-process assumptions - so the
    // same panel would drive a remote debugger. It never mutates views mid-event-dispatch: the
    // Game page rebuilds it from OnUpdate (top level); a locals-expand click only flags dirty.
    class DebuggerPanel
    {
    public:
        DebuggerPanel()
        {
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 4.0f;
            column->Padding = ui::Thickness{6, 6};

            auto bar = MakeRef<ui::FlexLayout>(DefaultAllocator());
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
                MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Debugger: not running"));
            m_status->FontSize.SetValue(12.0f);
            column->AddView(m_status.Get(), MatchWidth());

            column->AddView(
                MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Call Stack")).Get(),
                MatchWidth());
            m_stackList = MakeRef<ui::FlexLayout>(DefaultAllocator());
            m_stackList->Direction = ui::Orientation::Vertical;
            column->AddView(m_stackList.Get(), MatchWidth());

            column->AddView(MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"Locals")).Get(),
                            MatchWidth());
            m_localsList = MakeRef<ui::FlexLayout>(DefaultAllocator());
            m_localsList->Direction = ui::Orientation::Vertical;
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Width = ui::SizeSpec::Match();
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
        [[nodiscard]] static RefPtr<ui::FlexLayoutParams> MatchWidth();

        template <typename Fn>
        void AddToolButton(ui::FlexLayout& bar, StringView label, Fn onClick)
        {
            auto button = MakeRef<ui::Button>(DefaultAllocator(), label);
            button->FontSize.SetValue(12.0f);
            button->OnClick.Add([onClick](ui::ButtonBase*) { onClick(); });
            bar.AddView(button.Get(), RefPtr<ui::FlexLayoutParams>{});
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

    // The play-in-editor device seam (input P3): keyboard/mouse come from the Game
    // viewport's GATED InputSurface facades (hover = mouse, focus = keyboard - click the
    // viewport to play), gamepads pass through from the shell only while the viewport has
    // focus. This is the by-construction fix for "the editor viewport forwards nothing".
    // Pure forwarding: EVERY facade comes from the viewport's InputSurface, which owns all
    // gating and transformation (mouse hover+transform, keyboard focus, gamepads gated on
    // focus via SurfaceGamepad, touch transformed + spatially gated). This class only
    // adapts the surface to the input runtime's provider seam - no gating logic here.
    class GameViewportInputSource final : public draconic::input::IInputSourceProvider
    {
    public:
        ui::viewport::ViewportView* viewport = nullptr;       // borrowed
        draconic::shell::IInputManager* shellInput = nullptr; // borrowed (count only)

        [[nodiscard]] draconic::shell::IKeyboard* Keyboard() override;
        [[nodiscard]] draconic::shell::IMouse* Mouse() override;
        [[nodiscard]] i32 GamepadCount() const override;
        [[nodiscard]] draconic::shell::IGamepad* Gamepad(i32 index) override;
        [[nodiscard]] draconic::shell::ITouch* Touch() override;
        [[nodiscard]] Span<const draconic::shell::InputEvent> Events() override;
    };

    // Wren runtime faults during play surface as editor notices, not console-only lines.
    class GameScriptErrorSink final : public draconic::script::IScriptErrorHandler
    {
    public:
        EditorContext* context = nullptr;
        void OnError(const draconic::script::ScriptError& error) override;
    };

    class GameEditorPage final : public app::UIEditorPage
    {
    public:
        GameEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                       ui::runtime::UIHost& uiHost, runtime::DefaultApplication* embeddedApp,
                       runtime::GameInstance* instance)
            : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_app(embeddedApp),
              m_gameInstance(instance)
        {
            m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();
            m_input = host.Ctx().GetSubsystem<draconic::input::InputSubsystem>();
            m_shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;

            m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
            m_viewport->ClearColor = rhi::ClearColor{0.05f, 0.05f, 0.06f, 1.0f};

            // "Exit" from embedded game code = stop this play session (deferred by the
            // app to after the page-update loop - never torn down mid-script-dispatch).
            context.StopGameRun = Function<void()>{[this]() { Stop(); }};

            // Toolbar: Play / Stop / Restart + the run-state readout.
            GameEditorPage* self = this;
            m_toolbar = MakeRef<ui::toolkit::Toolbar>(DefaultAllocator());
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
            // Preview resolution: Auto (panel size) / Deck 1280x800 / 1080p - letterboxed,
            // with mouse AND touch input mapping through the same fit.
            m_resolutionButton = m_toolbar->AddButton(u8"Res: Auto");
            m_resolutionButton->OnClick.Add([self](ui::toolkit::ToolbarButton*)
                                            { self->CycleResolution(); });
            m_statusLabel = MakeRef<draconic::ui::Label>(DefaultAllocator(), StringView(u8""));
            m_statusLabel->FontSize.SetValue(13.0f);
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Height = draconic::ui::SizeSpec::Match();
                m_toolbar->AddView(m_statusLabel.Get(), lp);
            }

            auto column = MakeRef<draconic::ui::FlexLayout>(DefaultAllocator());
            column->Direction = draconic::ui::Orientation::Vertical;
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = draconic::ui::SizeSpec::Match();
                lp->Height = draconic::ui::SizeSpec::Fixed(draconic::ui::Unit::Px(30));
                column->AddView(m_toolbar.Get(), lp);
            }
            // The play stage: the game viewport (grows) beside the debugger panel (fixed).
            auto stage = MakeRef<ui::FlexLayout>(DefaultAllocator());
            stage->Direction = ui::Orientation::Horizontal;
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Height = draconic::ui::SizeSpec::Match();
                stage->AddView(m_viewport.Get(), lp);
            }
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = draconic::ui::SizeSpec::Fixed(draconic::ui::Unit::Px(300));
                lp->Height = draconic::ui::SizeSpec::Match();
                stage->AddView(m_debuggerPanel.RootView(), lp);
            }
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = draconic::ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                column->AddView(stage.Get(), lp);
            }
            m_content = column;
            RefreshToolbar();
        }

        [[nodiscard]] StringView Title() const override { return u8"Game"; }
        [[nodiscard]] Status Save() override { return Status{}; } // nothing here is a document
        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }

        // The scene group this tab's game scene belongs to: the embedded app's GameInstance manager
        // (game-instance.md §11), so the game scene groups + ticks + is bound to the instance's run
        // host. Falls back to THIS page's own (unregistered) manager if there's no embedded app - a
        // defensive placeholder, since without a runtime there is no game to run anyway.
        [[nodiscard]] scene::SceneManager& SceneGroup() noexcept;

        /// Fresh player run: the project's default scene from the DBs, simulation on.
        void Play();

        /// Total teardown - the fresh-run model's whole cleanup story.
        void Stop();

        // Bind (and re-bind after dock/float moves) the viewport to the window hosting it -
        // the same RendererFor dance as ScenePage. Without this the viewport never gets a
        // DEVICE: no color target, IsReady() false, and the tab renders only its clear color.
        void EnsureViewportBound();

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost&,
                            draconic::graphics::FrameContext& frame) override;

        void OnAfterSceneRender(runtime::IApplicationHost& host,
                                draconic::graphics::FrameContext& frame) override;

        void OnClose() override;

    private:
        // Authored game scenes should carry a camera; a bare scene shouldn't play as a black
        // screen - frame the origin like Draconic.Engine.Player does.
        void EnsureCamera();

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

        void CycleResolution();

        void RefreshToolbar();

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        runtime::DefaultApplication* m_app = nullptr; // the embedded game application (v3)
        runtime::GameInstance* m_gameInstance =
            nullptr; // THIS tab's running game (its own run host + scenes)
        draconic::graphics::RenderWindow* m_hostWindow =
            nullptr; // borrowed; tracks dock/float moves
        scene::SceneSubsystem* m_scenes = nullptr;
        render::RenderSubsystem* m_render = nullptr;
        scene::Scene* m_scene = nullptr;
        draconic::input::InputSubsystem* m_input = nullptr;
        draconic::shell::IInputManager* m_shellInput = nullptr;
        GameViewportInputSource m_viewportSource;

        RefPtr<draconic::ui::View> m_content;
        RefPtr<ui::toolkit::Toolbar> m_toolbar;
        ui::toolkit::ToolbarButton* m_playButton = nullptr;
        ui::toolkit::ToolbarButton* m_stopButton = nullptr;
        ui::toolkit::ToolbarToggle* m_pauseToggle = nullptr;
        ui::toolkit::ToolbarButton* m_restartButton = nullptr;
        ui::toolkit::ToolbarButton* m_resolutionButton = nullptr;
        u32 m_resolutionMode = 0;
        GameScriptErrorSink m_scriptErrors;
        DebuggerPanel m_debuggerPanel;      // the debugger UI (contract-only)
        GameDebugListener m_debugListener;  // debugger state sink (drained in OnUpdate)
        bool m_simPausedByDebugger = false; // we disabled sim for a breakpoint
        Array<EditorContext::ScriptBreakpoint> m_appliedBreakpoints; // mirror on the debugger
        RefPtr<draconic::ui::Label> m_statusLabel;
        RefPtr<ui::viewport::ViewportView> m_viewport;
        UniquePtr<draconic::shell::InputRouter>
            m_router;                         // gates the viewport surface (hover/focus)
        scene::SceneManager m_fallbackScenes; // no-embedded-app placeholder group (see SceneGroup)

        String m_sceneTitle;
        bool m_running = false;
    };
}

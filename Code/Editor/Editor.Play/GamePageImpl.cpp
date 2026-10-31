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

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.play;

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.run; // IRunManager: the runs, their focus
import foundation.resource;
import foundation.scene;
import foundation.settings; // the project's editor store (the Game tab's settings)
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
import editor.app;

using namespace foundation::core;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace script = foundation::script;
namespace gui = foundation::gui;

namespace editor
{
    DebuggerPanel::~DebuggerPanel()
    {
        if (m_stack != nullptr)
            m_stack->SetModel(nullptr);
        if (m_locals != nullptr)
            m_locals->SetModel(nullptr);
    }

    void DebuggerPanel::Bind(gui::Node& pane)
    {
        const auto find = [&pane](StringView id) -> gui::Node*
        {
            Array<gui::Node*> pending;
            pending.PushBack(&pane);
            while (!pending.IsEmpty())
            {
                gui::Node* node = pending.Back();
                pending.PopBack();
                if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                    return node;
                for (usize i = 0; i < node->ChildCount(); ++i)
                    pending.PushBack(node->GetChildAt(i));
            }
            return nullptr;
        };
        m_status = Cast<gui::Label>(find(u8"game-debugger-state"));
        m_stack = Cast<gui::TreeView>(find(u8"game-debugger-stack"));
        m_locals = Cast<gui::TreeView>(find(u8"game-debugger-locals"));
        if (m_stack != nullptr)
            m_stack->SetModel(&m_stackModel);
        if (m_locals != nullptr)
            m_locals->SetModel(&m_localsModel);
        DebuggerPanel* self = this;
        const auto verb = [&find, self](StringView id, void (*run)(script::IScriptDebugger&))
        {
            if (auto* button = Cast<gui::ButtonBase>(find(id)))
                button->SetOnClick([self, run]()
                                   {
                                       if (self->m_debugger != nullptr)
                                           run(*self->m_debugger);
                                   });
        };
        verb(u8"game-debugger-continue", [](script::IScriptDebugger& d) { d.Continue(); });
        verb(u8"game-debugger-step-into", [](script::IScriptDebugger& d) { d.StepInto(); });
        verb(u8"game-debugger-step-over", [](script::IScriptDebugger& d) { d.StepOver(); });
        verb(u8"game-debugger-break", [](script::IScriptDebugger& d) { d.Break(); });
        SetIdle();
    }

    void DebuggerPanel::SetDebugger(script::IScriptDebugger* debugger)
    {
        m_debugger = debugger;
        if (debugger == nullptr)
            Clear();
    }

    void DebuggerPanel::Refresh()
    {
        if (m_debugger == nullptr)
        {
            Clear();
            return;
        }
        if (m_status != nullptr)
            m_status->SetText(u8"Paused");
        m_stackModel.Clear();
        for (const script::ScriptStackFrame& frame : m_debugger->CaptureStackFrames())
        {
            String text(frame.function.AsView());
            text += u8"  (";
            text += frame.file;
            text += u8":";
            AppendInt(text, frame.line);
            text += u8")";
            (void)m_stackModel.AddNode(gui::kTreeModelRoot, text.AsView());
        }
        // The locals, an object's members one level under it (closed until opened).
        m_localsModel.Clear();
        for (const script::ScriptVariable& local : m_debugger->CaptureLocals(0))
        {
            String text(local.name.AsView());
            text += u8" = ";
            text += local.value;
            if (!local.typeName.IsEmpty())
            {
                text += u8"  (";
                text += local.typeName;
                text += u8")";
            }
            const i32 node = m_localsModel.AddNode(gui::kTreeModelRoot, text.AsView());
            if (local.objectRef == 0)
                continue;
            for (const script::ScriptVariable& member : m_debugger->CaptureObject(local.objectRef))
            {
                String memberText(member.name.AsView());
                memberText += u8" = ";
                memberText += member.value;
                (void)m_localsModel.AddNode(node, memberText.AsView());
            }
        }
    }

    void DebuggerPanel::Clear()
    {
        if (m_status != nullptr)
            m_status->SetText(u8"Running");
        m_stackModel.Clear();
        m_localsModel.Clear();
    }

    void DebuggerPanel::SetIdle()
    {
        m_debugger = nullptr;
        if (m_status != nullptr)
            m_status->SetText(u8"Not running");
        m_stackModel.Clear();
        m_localsModel.Clear();
    }

    StringView DebuggerPanel::State() const { return m_status != nullptr ? m_status->GetText() : StringView{}; }
    usize DebuggerPanel::StackCount() const { return m_stackModel.RowCount(gui::ModelIndex{}); }

    void DebuggerPanel::AppendInt(String& out, i32 value)
    {
        if (value < 0)
        {
            out.PushBack(utf8char('-'));
            value = -value;
        }
        utf8char digits[16];
        i32 n = 0;
        u32 v = static_cast<u32>(value);
        do
        {
            digits[n++] = static_cast<utf8char>('0' + v % 10);
            v /= 10;
        } while (v > 0 && n < 16);
        while (n > 0)
        {
            out.PushBack(digits[--n]);
        }
    }
    void GameDebugListener::OnDebuggerStateChanged(script::ScriptDebuggerState newState)
    {
        state = newState;
        changed = true;
    }
    GameEditorPage::GameEditorPage(EditorContext& context, runtime::IApplicationHost& host, Services services,
                                   engine::runtime::DefaultApplication* embeddedApp, engine::runtime::GameInstance* instance, StringView pieId)
        : app::UIEditorPage(context.Allocator()), m_context(&context), m_host(&host), m_services(Move(services)), m_app(embeddedApp),
          m_gameInstance(instance), m_pieId(pieId), m_title(TitleFor(pieId))
    {
        Provide<IPieInstancePage>(*this); // what the PIE tools act through
        Provide<IGameScenePage>(*this);   // what the scene tools read of the run
        Provide<IGameRunPage>(*this);     // what the editor stops before a native reload
        m_scenes = host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>();
        m_render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>();
        m_audio = host.Subsystems().GetSubsystem<engine::audio::AudioSubsystem>();
        m_input = host.Subsystems().GetSubsystem<engine::input::InputSubsystem>();
        m_shellInput = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;

        m_viewport = MakeRef<gui::Viewport>(Allocator());
        m_viewport->AddClass(u8"game-view");
        m_viewport->ClearColor = Color{0.248f, 0.248f, 0.272f, 1.0f};
        // The finished game, as the player draws it into its sRGB window: 8-bit sRGB rather than
        // the viewports' float default, so the Game tab matches the player and its screen UI can
        // blend encoded values through the target's twin view (RenderOverlays).
        m_viewport->SetFormats(rhi::TextureFormat::RGBA8UnormSrgb, m_viewport->DepthFormat());
        // Shift+F1 (the GUI's capture release) gives the keyboard back to the editor: the game
        // view lets the focus go too, as the game reads its keys while the view has it; a click
        // in the view takes it, and the capture, again.
        {
            gui::Viewport* view = m_viewport.Get();
            (void)m_viewport->AddNamedListener(u8"keysreleased", [view](const gui::Event&) { view->ReleaseFocus(); });
        }
        // The game's input is the viewport's surface, gated to it (hover the mouse, focus the keys).
        m_viewportSource.viewport = m_viewport.Get();
        m_viewportSource.shellInput = m_shellInput;
        if (m_input != nullptr)
            m_input->SetSourceProvider(ActiveSource(), nullptr);

        // THIS instance's exit request stops THIS tab's run, whichever tab it is: the script asking
        // fires inside the embedded app's update, which runs before the pages, so the stop waits for
        // this page's next OnUpdate, no script call on the stack.
        if (m_gameInstance != nullptr)
        {
            GameEditorPage* self = this;
            m_gameInstance->RunBinding().requestExit = Function<void(i32)>{[self](i32 code)
                                                                          {
                                                                              LOG_INFO(u8"Editor", u8"Game: '{}' requested exit({})", self->m_pieId, code);
                                                                              self->m_exitRequested = true;
                                                                          }};
        }

        // The tab from its markup: the bar over the stage, the game beside the debugger.
        m_content = m_services.loadMarkup ? m_services.loadMarkup(StringView(kPath)) : RefPtr<gui::Node>{};
        if (!m_content)
            m_content = MakeRef<gui::Panel>(Allocator()); // no markup: still a tab, with its viewport
        gui::Node* stage = Find<gui::Node>(u8"game-view-pane");
        (void)(stage != nullptr ? stage : m_content.Get())->AddChildAt(m_viewport.Get(), 0);
        if (m_services.gui != nullptr)
            m_viewport->HostIn(*m_services.gui);
        m_stage = Find<gui::Splitter>(u8"game-stage");
        m_debuggerPane = RefPtr<gui::Node>(Find<gui::Node>(u8"game-debugger"));
        if (m_debuggerPane)
        {
            m_debuggerPanel.Bind(*m_debuggerPane);
            ShowDebugger(false); // it takes room the game wants: opened by its toggle or a break
        }
        BindBar();
        RefreshToolbar();
    }

    template <typename T>
    T* GameEditorPage::Find(StringView id) const
    {
        if (!m_content)
            return nullptr;
        Array<gui::Node*> pending;
        pending.PushBack(m_content.Get());
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    }

    void GameEditorPage::BindBar()
    {
        GameEditorPage* self = this;
        m_restartButton = Find<gui::ButtonBase>(u8"game-restart");
        m_playButton = Find<gui::ButtonBase>(u8"game-play");
        m_pauseToggle = Find<gui::ButtonBase>(u8"game-pause");
        m_stopButton = Find<gui::ButtonBase>(u8"game-stop");
        m_stateLabel = Find<gui::Label>(u8"game-state");
        m_sceneLabel = Find<gui::Label>(u8"game-scene");
        m_cookBadge = Find<gui::Label>(u8"game-cook");
        m_debuggerToggle = Find<gui::ButtonBase>(u8"game-debugger-toggle");
        if (m_restartButton != nullptr)
            m_restartButton->SetOnClick([self]()
                                        {
                                            self->Stop();
                                            self->Play();
                                        });
        if (m_playButton != nullptr)
            m_playButton->SetOnClick([self]() { self->Play(); });
        if (m_pauseToggle != nullptr)
            m_pauseToggle->SetOnToggled([self](bool paused)
                                        {
                                            if (self->m_scene != nullptr && self->m_running)
                                                self->m_scene->SetSimulationEnabled(!paused);
                                            self->RefreshToolbar();
                                        });
        if (m_stopButton != nullptr)
            m_stopButton->SetOnClick([self]() { self->Stop(); });
        if (m_debuggerToggle != nullptr)
            m_debuggerToggle->SetOnToggled([self](bool shown) { self->ShowDebugger(shown); });
        BindResolution();
    }

    String GameEditorPage::TitleFor(StringView pieId)
    {
        constexpr StringView kNewInstance = u8"game-page-";
        if (!pieId.StartsWith(kNewInstance))
            return String(u8"Game");
        const Optional<i64> number = ParseInt(pieId.SubStr(kNewInstance.Size(), pieId.Size() - kNewInstance.Size()));
        return number.HasValue() ? Format(u8"Game {}", number.Value() + 1) : String(pieId);
    }

    void GameEditorPage::ShowDebugger(bool shown)
    {
        if (!m_debuggerPane)
            return;
        if (m_stage == nullptr)
        {
            m_debuggerPane->SetDisplayed(shown); // no split: the stage gives the game the room back
            return;
        }
        // In the split while it is on, at the share it was last given; out of it while it is off,
        // the game taking the whole stage (a hidden pane would keep its share empty).
        const bool in = m_debuggerPane->GetParent() == m_stage;
        if (shown && !in)
        {
            m_stage->AddChild(m_debuggerPane.Get());
            const f32 weights[2] = {1.0f - m_debuggerShare, m_debuggerShare};
            m_stage->SetWeights(Span<const f32>(weights, 2u));
        }
        else if (!shown && in)
        {
            const Array<f32> weights = m_stage->GetWeights();
            if (weights.Size() == 2u && weights[1] > 0.0f)
                m_debuggerShare = weights[1];
            m_stage->RemoveChild(m_debuggerPane.Get());
        }
    }

    bool GameEditorPage::DebuggerShown() const
    {
        if (!m_debuggerPane)
            return false;
        return m_stage != nullptr ? m_debuggerPane->GetParent() == m_stage : m_debuggerPane->IsDisplayed();
    }

    scene::SceneManager& GameEditorPage::SceneGroup() noexcept
    {
        return (m_gameInstance != nullptr) ? m_gameInstance->Scenes() : m_fallbackScenes;
    }

    void GameEditorPage::Play()
    {
        if (m_running || m_pendingPlay)
        {
            return;
        }
        // PIE waits for the cook: kick an incremental cook NOW and
        // defer the actual start to OnUpdate once the service is idle - a run that starts
        // mid-cook binds stale or missing products (fonts, scripts, input maps) and the
        // "heal via hot reload" path never covered a script that did not exist yet.
        // Unwired CookBusy (tests, no cook service) starts on the next OnUpdate tick.
        if (m_context->OnCookRequested)
        {
            m_context->OnCookRequested(false);
        }
        m_pendingPlay = true;
        if (m_context->IsCookBusy())
        {
            m_context->SetStatus(u8"Game: waiting for cook...");
        }
    }

    void GameEditorPage::StartRunNow()
    {
        if (m_running)
        {
            return;
        }
        if (m_scenes == nullptr || m_context->Project() == nullptr)
        {
            return;
        }

        // Resolution order mirrors Engine.Player: the manifest's guid (authoritative,
        // rename-proof), then the path mirror.
        editor::EditorProject& project = *m_context->Project();
        foundation::content::Instance* instance = nullptr;
        if (!project.Settings().defaultSceneId.IsNil())
        {
            instance = project.SourceDb().GetInstance(project.Settings().defaultSceneId);
        }
        if (instance == nullptr && !project.Settings().defaultScene.IsEmpty())
        {
            instance = project.SourceDb().GetInstance(project.Settings().defaultScene.AsView());
        }
        if (instance == nullptr)
        {
            // No default scene: a scripted game owns boot (the player's "the game script owns boot"
            // path). The run still starts - it just renders nothing until the game loads its first
            // scene via run.loadScene, which the follow-poll in OnUpdate then adopts. If the project
            // has neither a default scene nor a startup script, the tab simply shows nothing.
            LOG_INFO(u8"Editor", u8"Game: no default scene - the game script owns boot.");
        }

        // The cook was requested at Play() and has finished by the time OnUpdate routes here
        // (the m_pendingPlay gate) - products bound below are fresh, not healed-later.
        // The play bracket + game script run on THIS tab's GameInstance: its
        // own scene pairing, run host, error sink - so multiple tabs are isolated. Launch the game
        // script FIRST (matching Engine.Player): launch()/update(dt)
        // run before any scene, so a script tested here boots exactly like the player. The
        // page only resolves the script SOURCE (editor project layout) and surfaces notices.
        m_frameCount = 0;
        m_stoppedRunTime = 0.0;
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->ResetRunClock(); // run time counts from this start
            m_scriptErrors.context = m_context;
            m_gameInstance->SetScriptErrorHandler(&m_scriptErrors);
            EnableDebugging();
            StartGameScriptFromProject();
        }

        // A resolved default scene loads + starts here, exactly like the player's default-scene boot
        // (the script launched first, above). A project with none skips this block: the run starts
        // with no scene, and the follow-poll in OnUpdate adopts whatever the game script loads.
        if (instance != nullptr)
        {
            // Via THIS tab's instance (not just SceneGroup) so behaviors bind to its run host.
            scene::Scene* startScene = (m_gameInstance != nullptr)
                                           ? m_gameInstance->CreateScene(instance->Name())
                                           : m_fallbackScenes.CreateScene(instance->Name());
            if (startScene == nullptr || !scene::LoadScene(*instance, *startScene).IsOk())
            {
                m_context->Notify(NoticeKind::Error, u8"Game: default scene failed to load.");
                if (startScene != nullptr)
                {
                    SceneGroup().DestroyScene(startScene);
                }
                if (m_gameInstance != nullptr)
                {
                    m_gameInstance
                        ->StopScript(); // the script launched first (above) - do not leave it running
                }
                return;
            }
            // Products bind from the cooked DB (the editor's shared manager); prefab payloads
            // come from the source DB - the same split Engine.Player uses in project mode.
            if (m_context->Resources() != nullptr)
            {
                scene::ResolveSceneResources(*startScene, *m_context->Resources());
            }
            if (startScene->PendingPrefabInstanceCount() > 0)
            {
                EditorContext* context = m_context;
                scene::ResolveScenePrefabs(
                    *startScene,
                    Function<UniquePtr<IStream>(const Guid&)>{
                        [context](const Guid& prefabId) -> UniquePtr<IStream>
                        {
                            if (context->Project() == nullptr)
                            {
                                return UniquePtr<IStream>{};
                            }
                            foundation::content::Instance* prefab =
                                context->Project()->SourceDb().GetInstance(prefabId);
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : UniquePtr<IStream>{};
                        }});
                if (m_context->Resources() != nullptr)
                {
                    scene::ResolveSceneResources(*startScene, *m_context->Resources());
                }
            }
            EnsureCamera(*startScene);
            startScene->Start();
            startScene->SetSimulationEnabled(true);
            // The script launched above (before the scene); now make the freshly built scene this
            // instance's current scene (behaviors already bound to its run host via CreateScene).
            if (m_gameInstance != nullptr)
            {
                m_gameInstance->SetScene(startScene);
            }
            else
            {
                m_scene = startScene; // no instance to track a current scene (fallback path)
            }
        }

        m_running = true;
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->Start(); // the run's session: whatever follows the run's lifecycle hears it
        }
        if (m_pauseToggle != nullptr)
        {
            m_pauseToggle->SetChecked(false);
        }
        // Adopt the instance's current scene as this tab's borrowed render/input pointer: the default
        // we just started, a scene the game script loaded during launch(), or null (a scripted boot
        // that loads its first scene later). The follow-poll in OnUpdate tracks later run.loadScene
        // switches. Null is fine - the render + overlay paths early-return on a null scene.
        if (m_gameInstance != nullptr)
        {
            m_scene = m_gameInstance->GetScene();
        }
        m_sceneTitle = (m_scene != nullptr) ? String(m_scene->Name()) : String(u8"(no scene)");
        BindInput();
        BindBusLayout(*m_host);
        if (m_app != nullptr && m_app->Runs() != nullptr && m_gameInstance != nullptr)
        {
            m_app->Runs()->SetFocused(m_gameInstance); // the tab Play was pressed in is the one played (and heard)
        }
        LOG_INFO(u8"Editor", u8"Game: running scene '{}'", m_sceneTitle);
        RefreshToolbar();
    }

    void GameEditorPage::Stop()
    {
        m_pendingPlay = false; // Stop while waiting for the cook cancels the deferred start
        if (!m_running && m_scene == nullptr)
        {
            return;
        }
        DropScriptedInput(); // a stopped run scripts nothing
        // The map clears (no actions bound between runs); the SOURCE stays - it is
        // the runtime input's permanent provider (v3) - but its SCENE BINDING drops
        // with the run, returning the editor to inert (ScreenTierOnly) game UI.
        if (m_input != nullptr)
        {
            m_input->SetMap(foundation::input::InputMap{});
            m_input->SetSourceProvider(&m_viewportSource, nullptr);
        }
        // Drop the debugger wiring BEFORE the run tears the debugger down (the panel
        // holds a borrowed pointer; the run host owns + destroys it in OnExit).
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->RunHost().SetExternalDebugListener(nullptr);
        }
        m_debuggerPanel.SetIdle();
        m_simPausedByDebugger = false;
        m_context->ClearScriptExecutionPoint(); // no run = no paused location
        m_context->ScriptValueProbe = {};       // hover-values die with the run
        m_context->OnBreakpointsChanged = {};   // live sync dies with the run
        m_appliedBreakpoints.Clear();
        if (m_running && m_gameInstance != nullptr)
        {
            m_stoppedRunTime = m_gameInstance->RunTime(); // pie_state still reads where it ended
        }
        // Script exits first (it may still observe the world), then the scenes.
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->StopScript();
            m_gameInstance->SetScriptErrorHandler(nullptr);
        }
        // The instance's SceneManager owns EVERY scene this run created - the default scene AND any the
        // game script loaded via run.loadScene (LoadScene/SetScene never destroy a replaced scene, so
        // they accumulate on the group; the follow-poll retires outgoing ones during play, but this
        // final sweep catches whatever is current at stop plus any straggler). Stop each active scene's
        // behaviors while the aware subsystems are alive, then tear the whole group down. ClearScenes
        // also drops any in-flight tracked async load first, so a post-stop PumpScriptLoads can never
        // activate a just-freed scene. m_scene is a borrowed pointer into this group - just drop it.
        {
            Array<scene::Scene*> active;
            for (scene::Scene* s : SceneGroup().ActiveScenes())
            {
                active.PushBack(s);
            }
            for (scene::Scene* s : active)
            {
                s->Stop();
            }
            if (m_gameInstance != nullptr)
            {
                m_gameInstance->ClearScenes();
            }
            else
            {
                m_fallbackScenes.Clear(); // no instance: the page's own placeholder group
            }
            m_scene = nullptr;
        }
        // The run's session ends: its sound fades out and its screens go with it, as everything
        // following the run's lifecycle hears (another tab's run and the editor's own play on).
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->Stop();
        }
        m_running = false;
        m_viewport->ClearPicture(); // the run's last frame does not linger
        RefreshToolbar();
    }

    void GameEditorPage::FollowInstanceScene()
    {
        if (!m_running || m_gameInstance == nullptr)
        {
            return;
        }
        scene::Scene* current = m_gameInstance->GetScene();
        if (current == m_scene)
        {
            return;
        }
        // A scripted scene switch (run.loadScene, or an async load that just activated) repointed the
        // instance's current scene. The instance's SceneManager owns EVERY scene (default + loaded)
        // and never destroys a replaced one, so the tab retires the scenes it is leaving here. Adopt
        // the new current scene as our borrowed render/input pointer FIRST, then stop + destroy every
        // OTHER active scene in the group - exactly once, the manager being the sole owner (no double-
        // free: the instance already repointed its current pointer + net replication off the old one).
        m_scene = current; // may be null (a script that unloaded without loading; paths are null-safe)

        // Collect first: DestroyScene mutates the active set, so we cannot destroy mid-iteration. An
        // in-flight async-load scene is INACTIVE (not in ActiveScenes), so this never destroys one.
        Array<scene::Scene*> outgoing;
        for (scene::Scene* s : SceneGroup().ActiveScenes())
        {
            if (s != m_scene)
            {
                outgoing.PushBack(s);
            }
        }
        for (scene::Scene* s : outgoing)
        {
            s->Stop();
            m_gameInstance->DestroyScene(s); // drops any matching tracked load too, then frees the scene
        }

        // The adopted scene arrives already Started + simulation-enabled (run.loadScene's activation
        // policy) - do NOT re-Start it. Re-point the per-run input scene binding at it, honor an active
        // pause, and reflect the switch in the readout + log so they name the actually-rendered scene.
        if (m_input != nullptr)
        {
            m_input->SetSourceProvider(ActiveSource(), m_scene);
        }
        if (m_scene != nullptr)
        {
            // A scene loaded via run.loadScene may ship no camera; give it a default one, like Play
            // does for the first scene - otherwise RenderScene draws nothing and the old frame lingers.
            EnsureCamera(*m_scene);
            const bool paused = m_simPausedByDebugger ||
                                (m_pauseToggle != nullptr && m_pauseToggle->IsChecked());
            if (paused)
            {
                m_scene->SetSimulationEnabled(false);
            }
            m_sceneTitle = String(m_scene->Name());
        }
        else
        {
            m_sceneTitle = String(u8"(no scene)");
        }
        LOG_INFO(u8"Editor", u8"Game: running scene '{}'", m_sceneTitle);
        RefreshToolbar();
    }

    void GameEditorPage::SyncRunAudio()
    {
        if (m_audio == nullptr || m_gameInstance == nullptr)
        {
            return;
        }
        const bool hearAll = HearAllGameInstances(*m_context);
        if (m_audio->HearAllRuns() != hearAll)
        {
            m_audio->SetHearAllRuns(hearAll);
        }
        if (!m_running)
        {
            return;
        }
        // The toolbar's pause and a debugger break freeze the run's sound with the game (a game's
        // own pause menu is time scale 0, the game's business: its menu music plays on).
        const bool paused =
            m_simPausedByDebugger || (m_pauseToggle != nullptr && m_pauseToggle->IsChecked());
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->SetPaused(paused); // the host's pause: the run (its sound with it) hears it once per change
        }
        // Clicking into this tab's viewport makes its run the one played, and the one heard.
        foundation::run::IRunManager* runs = m_app != nullptr ? m_app->Runs() : nullptr;
        if (m_viewport->IsFocused() && runs != nullptr && m_gameInstance != nullptr && runs->Focused() != m_gameInstance)
        {
            runs->SetFocused(m_gameInstance);
        }
    }

    void GameEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        // A capture recorded last frame: the GPU has to finish the copy.
        m_capture.Complete(host.Graphics() != nullptr ? host.Graphics()->Raw() : nullptr,
                           Allocator());
        if (m_exitRequested)
        {
            m_exitRequested = false;
            Stop();
        }
        // A Play pressed while the cook ran (or just kicked one) starts here, on the first
        // frame the cook service reports idle - never against a half-written cooked DB.
        if (m_pendingPlay && !m_context->IsCookBusy())
        {
            m_pendingPlay = false;
            StartRunNow();
        }
        // Follow the instance's current scene BEFORE anything renders this frame: a scripted switch
        // (run.loadScene) repointed it, so adopt the new scene + retire the outgoing one here.
        FollowInstanceScene();
        AdvanceScriptedInput();
        TickResolution(dt);
        SyncGameFit(); // the target may have changed shape: the turn, and so the pointer's map
        // A running game owns every key while its view has the focus (the editor's focus moves and
        // its bindings stay out of it); Shift+F1 gives the keyboard back, the focus with it, until
        // a click in the view. Armed only while focused: armed whenever it ran, the release was
        // undone the next frame and the game kept the keys.
        m_viewport->SetCapturesAllKeys(m_running && m_viewport->IsFocused());
        SyncRunAudio();
        RefreshToolbar();
        // The game UI's input (one pump for every Game tab) goes to the tab the keyboard is in, or
        // to the one a playtest is scripting; with neither, it stays with the tab that took it last
        // (a lone tab keeps it from Play on). Each run's screens are its own (run screens), so the
        // claim decides only whose menus the keys and clicks reach.
        if (m_running && m_input != nullptr &&
            (m_scripted.Get() != nullptr ||
             (m_viewport->IsFocused() && m_viewport->HostedSurface() != nullptr && m_viewport->HostedSurface()->Focused())))
        {
            m_input->SetSourceProvider(ActiveSource(), m_scene);
        }
        // The window's text input follows the game GUI's focus: the viewport (the editor GUI's focused
        // node while playing) says the game's GUI wants text, and the editor GUI's bridge starts it.
        m_viewport->SetHostedTextInputWanted(m_app != nullptr && m_app->GUI() != nullptr && m_app->GUI()->WantsTextInput());
        // The embedded app's OnUpdate (ticking EVERY instance's game script) is driven ONCE by
        // the editor app - not per game tab, or N tabs would
        // tick every instance N times. This tab only drains its own debugger state.
        (void)host;
        (void)dt;
        DrainDebuggerState();
    }

    void GameEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                        foundation::graphics::FrameContext& frame)
    {
        if (!m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        // Idle (no run): the viewport keeps what it was cleared to (Stop asks for the clear).
        if (m_scene == nullptr)
        {
            return;
        }
        if (m_render == nullptr || !m_render->IsReady())
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0)
        {
            return;
        }
        if (!m_viewport->IsTreeVisible())
        {
            return;
        }

        // RenderTexture canvases draw before the scene (the same host seam the
        // player runs in DefaultApplication::OnRenderWindow).
        if (m_app != nullptr && m_app->GUI() != nullptr)
        {
            m_app->GUI()->RenderTextures(*frame.encoder, frame.frameIndex); // and the engine GUI's
        }

        // No camera override: the SCENE's primary camera drives the view (its clear
        // color included) - the player's presentation, not the editor's.
        render::TargetState targetState;
        targetState.texture = m_viewport->ColorTexture();
        targetState.currentState = m_viewport->ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;
        m_captureX = 0;
        m_captureY = 0;
        m_captureWidth = w;
        m_captureHeight = h;
        const ContentFit fit = GameFitIn(w, h);
        if (GameFitted(fit))
        {
            // At the render resolution (or turned), fitted into the panel as the player fits its
            // window; the capture is the game's image, without the bars.
            const Rectangle dst = fit.DstRect();
            const render::ViewportRect viewport{static_cast<i32>(dst.x), static_cast<i32>(dst.y),
                                                static_cast<u32>(Max(dst.width, 1.0f)),
                                                static_cast<u32>(Max(dst.height, 1.0f))};
            m_render->RenderScene(*m_scene, m_viewport->ColorView(), m_viewport->ColorFormat(), w, h,
                                  viewport, nullptr, targetState, nullptr, nullptr, nullptr,
                                  render::SceneSize::FromFit(fit));
            m_captureX = static_cast<u32>(Max(viewport.x, 0));
            m_captureY = static_cast<u32>(Max(viewport.y, 0));
            m_captureWidth = Min(viewport.width, w - m_captureX);
            m_captureHeight = Min(viewport.height, h - m_captureY);
        }
        else
        {
            m_render->RenderScene(*m_scene, m_viewport->ColorView(), m_viewport->ColorFormat(), w, h,
                                  render::ViewportRect{0, 0, w, h}, nullptr, targetState);
        }
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
        if (m_running)
        {
            ++m_frameCount;
        }
        m_renderedThisFrame = true; // OnAfterSceneRender captures this frame, at the above
    }

    void GameEditorPage::OnAfterSceneRender(runtime::IApplicationHost& host,
                                            foundation::graphics::FrameContext& frame)
    {
        // Scene-tier UI (HUD canvases/billboards) is already drawn in the viewport
        // inside the compose. This composites the game's WINDOW-SPACE overlays
        // (screen-tier UI, diagnostics) onto the viewport through the generic
        // registry - the tab shows the same full output as the player's window.
        const bool rendered = m_renderedThisFrame;
        m_renderedThisFrame = false;
        if (!m_running || m_scene == nullptr || !m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        // The screen-tier overlay is registered against the EMBEDDED RUNTIME context's
        // RenderSubsystem (the one the embedded DefaultApplication configured). The
        // `host` handed to this callback is the editor's OWN outer host, whose context
        // carries no gameplay subsystems - resolving the RenderSubsystem through it
        // returns null and the game's screen-tier UI never composites. Use m_render:
        // the runtime RenderSubsystem the ctor cached from the embedded host, the same
        // one the game UISubsystem self-registered its IScreenOverlay with.
        (void)host;
        engine::render::RenderSubsystem* render = m_render;
        if (render == nullptr)
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0)
        {
            return;
        }
        frame.encoder->TransitionTexture(m_viewport->ColorTexture(), m_viewport->ColorState(),
                                         rhi::ResourceState::RenderTarget);
        // The GUI is shared between the Game tabs; each draws its own run's screen tier, laid out at
        // its own render resolution (the shared tier's global overlays draw above it).
        if (m_app != nullptr && m_app->GUI() != nullptr)
        {
            m_app->GUI()->SetScreenResolution(m_gameInstance, m_renderWidth, m_renderHeight, m_renderFit);
            m_app->GUI()->SetScreenOrientation(m_gameInstance, m_orientation);
            m_app->GUI()->SetRenderRun(m_gameInstance);
        }
        render->RenderOverlays(*frame.encoder, m_viewport->ColorView(),
                               m_viewport->ColorFormat(), w, h, frame.frameIndex,
                               m_viewport->ColorAlternateView()); // the screen UI blends encoded
        if (m_app != nullptr && m_app->GUI() != nullptr)
        {
            m_app->GUI()->SetRenderRun(nullptr);
        }
        frame.encoder->TransitionTexture(m_viewport->ColorTexture(),
                                         rhi::ResourceState::RenderTarget,
                                         rhi::ResourceState::ShaderRead);
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
        // The capture, once the frame is whole: the scene, then the game's overlays over it.
        // The device is the editor's own (the outer host's), the one the viewport lives on.
        if (rendered && m_capture.Armed() && host.Graphics() != nullptr)
        {
            m_capture.Record(host.Graphics()->Raw(), frame.encoder, m_viewport->ColorTexture(),
                             m_viewport->ColorFormat(), m_captureWidth, m_captureHeight,
                             rhi::ResourceState::ShaderRead, m_captureX, m_captureY, m_renderWidth,
                             m_renderHeight);
        }
    }

    void GameEditorPage::OnClose()
    {
        DropScriptedInput(); // before the sources it replaced are torn down
        Stop();
        // Stop leaves this page's viewport source as the input override (by design, so a STOPPED-
        // but-open tab keeps viewport input). On CLOSE the source is about to be freed, so clear it
        // or ActiveSource() dangles and the next PumpInput crashes (guarded: only if it's ours).
        if (m_input != nullptr)
        {
            m_input->ClearSourceProviderIf(&m_viewportSource);
        }
        // The PER-INSTANCE input source (BindInput set it to this tab's viewport) also dangles once
        // m_viewportSource is freed - and ReleaseInstance below is a NO-OP for the PRIMARY instance,
        // so its DriveInput would dereference the freed source next frame. Revert it to the shell.
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->SetInputSource(m_input != nullptr ? &m_input->ShellSource() : nullptr);
            m_gameInstance->RunBinding().requestExit = Function<void(i32)>{}; // it points here
        }
        // Destroy THIS tab's extra instance (unregisters its scene manager + tears down its run
        // host) so nothing dangling is ticked/rendered after the tab closes. No-op for the primary.
        if (m_app != nullptr && m_gameInstance != nullptr)
        {
            m_app->ReleaseInstance(m_gameInstance);
        }
        m_gameInstance = nullptr;
        if (m_host->Graphics() != nullptr)
        {
            // A copy recorded last frame completes first: freeing its buffer in flight is the
            // device's error (the scene viewport's, 2026-10-25).
            m_capture.Complete(m_host->Graphics()->Raw(), Allocator());
            m_capture.Release(m_host->Graphics()->Raw()); // the readback buffer, while the device lives
        }
    }

    // ---- scripted input (pie_run) ----

    foundation::input::IInputSourceProvider* GameEditorPage::ActiveSource() noexcept
    {
        if (m_scripted.Get() != nullptr)
        {
            return m_scripted.Get();
        }
        return &m_viewportSource;
    }

    void GameEditorPage::BeginScriptedInput(
        UniquePtr<foundation::input::ScriptedInputSource> source)
    {
        DropScriptedInput();
        m_scripted = Move(source);
        m_scriptedStart = RunTime();
        m_scriptedEnding = false;
        m_scriptedReleased = false;
        // The subsystem's source only while this tab holds it: another tab's stays its own.
        if (m_input != nullptr && &m_input->ActiveSource() == &m_viewportSource)
        {
            m_input->SetSourceProvider(m_scripted.Get(), m_scene);
        }
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->SetInputSource(m_scripted.Get());
        }
    }

    void GameEditorPage::EndScriptedInput()
    {
        if (m_scripted.Get() != nullptr)
        {
            m_scriptedEnding = true;
        }
    }

    void GameEditorPage::AdvanceScriptedInput()
    {
        if (m_scripted.Get() == nullptr)
        {
            return;
        }
        if (!m_scriptedEnding)
        {
            m_scripted->Advance(RunTime() - m_scriptedStart);
            return;
        }
        if (!m_scriptedReleased)
        {
            m_scripted->ReleaseAll(); // the game reads the let-go next frame
            m_scriptedReleased = true;
            return;
        }
        DropScriptedInput();
    }

    void GameEditorPage::DropScriptedInput()
    {
        if (m_scripted.Get() == nullptr)
        {
            return;
        }
        if (m_input != nullptr && &m_input->ActiveSource() == m_scripted.Get())
        {
            m_input->SetSourceProvider(&m_viewportSource, m_scene);
        }
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->SetInputSource(&m_viewportSource);
        }
        m_scripted.Reset();
        m_scriptedEnding = false;
        m_scriptedReleased = false;
    }

    void GameEditorPage::EnsureCamera(scene::Scene& scene)
    {
        auto* cameras = scene.GetSystem<engine::render::CameraComponentManager>();
        if (cameras == nullptr)
        {
            return;
        }
        bool hasCamera = false;
        cameras->ForEach([&](engine::render::CameraComponent&, scene::EntityHandle) { hasCamera = true; });
        if (hasCamera)
        {
            return;
        }

        LOG_WARNING(u8"Editor", u8"Game: scene has no camera - adding a default one");
        const scene::EntityHandle e = scene.CreateEntity(u8"PlayerCamera");
        Transform t;
        t.position = Float3{8.0f, 6.0f, 10.0f};
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.675f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.42f);
        scene.SetLocalTransform(e, t);
        cameras->Add(e);
    }

    void GameEditorPage::BindInput()
    {
        if (m_input == nullptr)
        {
            return;
        }
        m_viewportSource.viewport = m_viewport.Get();
        m_viewportSource.shellInput = m_shellInput;
        // Per-surface scene binding: the viewport source represents
        // THIS run's scene, so game-UI routing + consumption confine to it - open
        // editing pages' HUDs can no longer catch the run's clicks/keys, and the
        // run's UI never reacts to another scene's coordinates.
        m_input->SetSourceProvider(ActiveSource(),
                                   m_scene); // UI-pump active source (game-UI routing)
        // Per-instance INPUT: this tab's game reads its OWN viewport source through its OWN action
        // runtime, so two Game tabs never cross-feed keys (only the FOCUSED tab's surface reports
        // them). The shared subsystem runtime is no longer the game's input.
        if (m_gameInstance != nullptr)
        {
            m_gameInstance->SetInputSource(ActiveSource());
        }
        const Guid mapId = m_context->Project()->Settings().defaultInputMapId;
        if (mapId.IsNil() || m_context->Resources() == nullptr)
        {
            return;
        }
        auto proxy = m_context->Resources()->Bind<foundation::input::InputMapResource>(mapId);
        if (proxy)
        {
            if (m_gameInstance != nullptr)
            {
                m_gameInstance->SetInputMap(proxy->Map());
            }
            LOG_INFO(u8"Editor", u8"Game: input map bound ({} set(s))",
                              proxy->Map().sets.Size());
        }
        else
        {
            m_context->Notify(NoticeKind::Warning, u8"Game: default input map is not cooked yet.");
        }
    }

    void GameEditorPage::BindBusLayout(runtime::IApplicationHost& host)
    {
        auto* audio = host.Subsystems().GetSubsystem<engine::audio::AudioSubsystem>();
        if (audio == nullptr || audio->Engine() == nullptr)
        {
            return;
        }
        const Guid layoutId = m_context->Project()->Settings().defaultBusLayoutId;
        if (layoutId.IsNil() || m_context->Resources() == nullptr)
        {
            return;
        }
        auto proxy =
            m_context->Resources()->Bind<foundation::audio::AudioBusLayoutResource>(layoutId);
        if (proxy)
        {
            audio->Engine()->ApplyBusLayout(proxy->layout);
            LOG_INFO(u8"Editor", u8"Game: audio bus layout applied");
        }
        else
        {
            m_context->Notify(NoticeKind::Warning, u8"Game: default bus layout is not cooked yet.");
        }
    }

    void GameEditorPage::StartGameScriptFromProject()
    {
        // The startup script is a cooked ScriptClass asset (guid-authoritative), bound from the
        // content DB like every other asset - no raw source path.
        const Guid scriptId = m_context->Project()->Settings().startupScriptId;
        if (scriptId.IsNil() || m_context->Resources() == nullptr)
        {
            return;
        }
        auto proxy = m_context->Resources()->Bind<foundation::script::ScriptClass>(scriptId);
        if (!proxy || proxy->source.IsEmpty())
        {
            m_context->Notify(NoticeKind::Warning, u8"Game: startup script asset not found.");
            return;
        }
        if (m_gameInstance != nullptr)
        {
            // Play in editor keeps its own save in the project's ignored Editor/ folder, so testing
            // never touches a player's save (Documentation/Specs/save-data.md).
            const editor::EditorProject& project = *m_context->Project();
            m_gameInstance->SetSaveFile(
                PathJoin(project.EditorStateRoot().AsView(),
                         engine::project::SaveFileName(project.Settings().name.AsView(), Allocator())
                             .AsView(),
                         Allocator())
                    .AsView());
        }
        if (m_gameInstance == nullptr ||
            !m_gameInstance->StartScript(*proxy))
        {
            m_context->Notify(NoticeKind::Error,
                              u8"Game: startup script failed to start (see Console).");
        }
    }

    void GameEditorPage::EnableDebugging()
    {
        if (m_gameInstance == nullptr)
        {
            return;
        }
        m_gameInstance->RunHost().SetExternalDebugListener(&m_debugListener);
        EditorContext* context = m_context;
        GameEditorPage* self = this;
        m_gameInstance->RunHost().RequestDebugger(Function<void(script::IScriptDebugger&)>{
            [context, self](script::IScriptDebugger& debugger)
            {
                for (const EditorContext::ScriptBreakpoint& breakpoint : context->Breakpoints())
                {
                    debugger.SetBreakpoint(breakpoint.file.AsView(), breakpoint.line);
                    self->m_appliedBreakpoints.PushBack(breakpoint);
                }
                self->m_debuggerPanel.SetDebugger(&debugger);
            }});

        // LIVE breakpoint sync: gutter toggles during the run diff-apply onto the debugger
        // (removals take effect on the next executed line; additions arm immediately).
        m_context->OnBreakpointsChanged = [self] { self->SyncBreakpointsToDebugger(); };

        // Hover-value probe for ScriptPages: while THIS run is paused at a breakpoint, an
        // identifier resolves against the innermost frame's locals. Cleared on Stop.
        m_context->ScriptValueProbe = [self](StringView identifier) -> String
        {
            if (self->m_gameInstance == nullptr || !self->m_running ||
                !self->m_gameInstance->RunHost().IsDebugPaused())
            {
                return String();
            }
            script::IScriptDebugger* debugger = self->m_debuggerPanel.Debugger();
            if (debugger == nullptr)
            {
                return String();
            }
            const Array<script::ScriptVariable> locals = debugger->CaptureLocals(0);
            for (const script::ScriptVariable& local : locals)
            {
                if (local.name.AsView() != identifier)
                {
                    continue;
                }
                String text(local.value.AsView());
                if (!local.typeName.IsEmpty())
                {
                    text.Append(u8" : ");
                    text.Append(local.typeName.AsView());
                }
                return text;
            }
            return String();
        };
    }

    void GameEditorPage::DrainDebuggerState()
    {
        if (!m_running)
        {
            return;
        }
        if (m_debugListener.changed)
        {
            m_debugListener.changed = false;
            const bool paused = m_debugListener.state == script::ScriptDebuggerState::Breakpoint ||
                                m_debugListener.state == script::ScriptDebuggerState::Stepped;
            if (paused)
            {
                if (m_scene != nullptr && !m_simPausedByDebugger)
                {
                    m_scene->SetSimulationEnabled(false);
                    m_simPausedByDebugger = true;
                }
                // A paused run shows why: the panel opens if it was hidden.
                if (m_debuggerToggle != nullptr && !m_debuggerToggle->IsChecked())
                {
                    m_debuggerToggle->SetChecked(true);
                    ShowDebugger(true);
                }
                m_debuggerPanel.Refresh();
                // Publish the paused location (innermost frame) - the ScriptPage editing
                // that file shows it as the ExecutionLine marker.
                if (script::IScriptDebugger* debugger = m_debuggerPanel.Debugger())
                {
                    const Array<script::ScriptStackFrame> frames =
                        debugger->CaptureStackFrames();
                    if (!frames.IsEmpty())
                    {
                        m_context->SetScriptExecutionPoint(frames[0].file.AsView(),
                                                           frames[0].line);
                    }
                }
            }
            else
            {
                if (m_scene != nullptr && m_simPausedByDebugger)
                {
                    m_scene->SetSimulationEnabled(true);
                    m_simPausedByDebugger = false;
                }
                m_debuggerPanel.Clear();
                m_context->ClearScriptExecutionPoint();
            }
        }
    }

    void GameEditorPage::SyncBreakpointsToDebugger()
    {
        script::IScriptDebugger* debugger = m_debuggerPanel.Debugger();
        if (debugger == nullptr || !m_running)
        {
            return;
        }
        const Span<const EditorContext::ScriptBreakpoint> store = m_context->Breakpoints();
        const auto contains = [](Span<const EditorContext::ScriptBreakpoint> set,
                                 const EditorContext::ScriptBreakpoint& breakpoint)
        {
            for (const EditorContext::ScriptBreakpoint& entry : set)
            {
                if (entry.line == breakpoint.line &&
                    entry.file.AsView() == breakpoint.file.AsView())
                {
                    return true;
                }
            }
            return false;
        };
        for (const EditorContext::ScriptBreakpoint& applied : m_appliedBreakpoints)
        {
            if (!contains(store, applied))
            {
                debugger->RemoveBreakpoint(applied.file.AsView(), applied.line);
            }
        }
        const Span<const EditorContext::ScriptBreakpoint> appliedView(
            m_appliedBreakpoints.Data(), m_appliedBreakpoints.Size());
        for (const EditorContext::ScriptBreakpoint& breakpoint : store)
        {
            if (!contains(appliedView, breakpoint))
            {
                debugger->SetBreakpoint(breakpoint.file.AsView(), breakpoint.line);
            }
        }
        m_appliedBreakpoints.Clear();
        for (const EditorContext::ScriptBreakpoint& breakpoint : store)
        {
            m_appliedBreakpoints.PushBack(breakpoint);
        }
    }

    void GameEditorPage::BindResolution()
    {
        GameEditorPage* self = this;
        m_resolutionCombo = Find<gui::ComboBox>(u8"game-resolution");
        if (m_resolutionCombo != nullptr)
            m_resolutionCombo->SetOnSelectionChanged([self](i32 index)
                                                     {
                                                         if (self->m_resolutionRebuilding || index < 0 ||
                                                             static_cast<usize>(index) >= self->m_resolutionChoices.Size())
                                                             return;
                                                         self->m_resolutionKey = self->m_resolutionChoices[static_cast<usize>(index)].key;
                                                         self->SaveResolutionKey();
                                                         self->ApplyResolution();
                                                     });
        // A phone held upright: the game as a phone would show it, turned when it is held the
        // other way (mouse and touch map through the same turn).
        m_phoneToggle = Find<gui::ButtonBase>(u8"game-phone");
        if (m_phoneToggle != nullptr)
            m_phoneToggle->SetOnToggled([self](bool on)
                                        {
                                            self->SetPhonePreview(on);
                                            self->SaveResolutionKey();
                                        });
        LoadResolutionKey();
        if (m_phoneToggle != nullptr)
            m_phoneToggle->SetChecked(m_phonePreview);
        RefreshResolutionChoices(true);
    }

    i32 GameEditorPage::SelectedResolution() const
    {
        if (m_resolutionCombo != nullptr)
            return m_resolutionCombo->GetSelectedIndex();
        // No dropdown (no markup): the chosen key's entry.
        const usize found = FindGameResolution(Span<const GameResolutionChoice>(m_resolutionChoices.Data(), m_resolutionChoices.Size()), m_resolutionKey.AsView());
        return m_resolutionChoices.IsEmpty() ? -1 : static_cast<i32>(found);
    }

    Span<const GameResolutionChoice> GameEditorPage::ResolutionChoices()
    {
        RefreshResolutionChoices(false);
        return Span<const GameResolutionChoice>(m_resolutionChoices.Data(), m_resolutionChoices.Size());
    }

    bool GameEditorPage::SelectResolution(StringView key)
    {
        RefreshResolutionChoices(false);
        for (usize i = 0; i < m_resolutionChoices.Size(); ++i)
        {
            if (m_resolutionChoices[i].key.AsView() == key)
            {
                m_resolutionKey = m_resolutionChoices[i].key;
                m_resolutionRebuilding = true; // the dropdown shows it; its handler stays out
                if (m_resolutionCombo != nullptr)
                    m_resolutionCombo->SetSelectedIndex(static_cast<i32>(i));
                m_resolutionRebuilding = false;
                SaveResolutionKey();
                ApplyResolution();
                return true;
            }
        }
        return false;
    }

    void GameEditorPage::SelectPhonePreview(bool on)
    {
        if (m_phoneToggle != nullptr)
            m_phoneToggle->SetChecked(on); // code's change: its handler stays out
        SetPhonePreview(on);
        SaveResolutionKey();
    }

    void GameEditorPage::SetPhonePreview(bool on)
    {
        m_phonePreview = on;
        ApplyResolution();
    }

    void GameEditorPage::SyncGameFit()
    {
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0)
        {
            m_viewport->SetContentResolution(m_renderWidth, m_renderHeight, m_renderFit);
            return;
        }
        const ContentFit fit = GameFitIn(w, h);
        if (GameFitted(fit))
        {
            m_viewport->SetContentResolution(static_cast<u32>(fit.contentSize.x), static_cast<u32>(fit.contentSize.y),
                                             fit.mode, fit.rotation);
        }
        else
        {
            m_viewport->SetContentResolution(0, 0, m_renderFit);
        }
    }

    void GameEditorPage::TickResolution(f32 dt)
    {
        m_resolutionRefresh -= dt;
        if (m_resolutionRefresh > 0.0f)
        {
            return;
        }
        m_resolutionRefresh = 1.0f; // cheap, and a reopened dropdown is fresh within a second
        RefreshResolutionChoices(false);
    }

    void GameEditorPage::RefreshResolutionChoices(bool force)
    {
        EditorProject* project = m_context->Project();
        const engine::project::ProjectSettings* settings = project != nullptr ? &project->Settings() : nullptr;
        ExportPresetSet presets;
        bool hasPresets = false;
        if (project != nullptr)
        {
            vfs::NativeFileSystem root(project->Directory(), Allocator());
            hasPresets = LoadExportPresets(root, presets).IsOk();
        }
        Array<GameResolutionChoice> fresh = CollectGameResolutions(
            settings, hasPresets ? &presets : nullptr, GamePreviewSettings::From(m_context->UserEditorSettings()));
        const String signature =
            GameResolutionSignature(Span<const GameResolutionChoice>(fresh.Data(), fresh.Size()),
                                    settings != nullptr ? settings->renderFit : FitMode::Letterbox);
        if (!force && signature == m_resolutionSignature)
        {
            return;
        }
        m_resolutionSignature = signature;
        m_resolutionChoices = Move(fresh);

        // The chosen entry survives a rebuild by its key; gone, the project's comes back.
        const usize selected = FindGameResolution(
            Span<const GameResolutionChoice>(m_resolutionChoices.Data(), m_resolutionChoices.Size()),
            m_resolutionKey.AsView());
        m_resolutionKey = m_resolutionChoices[selected].key;
        if (m_resolutionCombo != nullptr)
        {
            m_resolutionRebuilding = true;
            m_resolutionCombo->SetItemsText(String{});
            for (const GameResolutionChoice& choice : m_resolutionChoices)
                m_resolutionCombo->AddItem(choice.label.AsView());
            m_resolutionCombo->SetSelectedIndex(static_cast<i32>(selected));
            m_resolutionRebuilding = false;
        }
        ApplyResolution();
    }

    void GameEditorPage::ApplyResolution()
    {
        const i32 index = SelectedResolution();
        if (index < 0 || static_cast<usize>(index) >= m_resolutionChoices.Size())
        {
            return;
        }
        // Fitted into the panel by the project's render fit, as the player fits it into its window:
        // the scene draws at it into the fitted rectangle of a panel-sized target, the screen UI
        // lays out at it and draws crisp, and the pointer maps into it.
        const GameResolutionChoice& choice = m_resolutionChoices[static_cast<usize>(index)];
        EditorProject* project = m_context->Project();
        m_renderWidth = choice.width;
        m_renderHeight = choice.height;
        m_renderFit = project != nullptr ? project->Settings().renderFit : FitMode::Letterbox;
        m_orientation = choice.overridesOrientation ? choice.orientation
                        : project != nullptr      ? project->Settings().orientation
                                                  : ScreenOrientation::Any;
        // The previewed phone's screen is the target, letterboxed in the panel; else the panel is.
        m_viewport->SetFixedResolution(m_phonePreview ? kPhonePreviewWidth : 0,
                                       m_phonePreview ? kPhonePreviewHeight : 0);
        m_viewport->SetFitMode(m_phonePreview ? FitMode::Letterbox : FitMode::Stretch);
        SyncGameFit();
    }

    void GameEditorPage::LoadResolutionKey()
    {
        m_resolutionKey = String(kProjectResolutionKey);
        if (foundation::settings::Settings* store = m_context->ProjectEditorSettings())
        {
            if (const GamePageSettings* section = store->Find<GamePageSettings>())
            {
                if (!section->resolutionKey.IsEmpty())
                {
                    m_resolutionKey = section->resolutionKey;
                }
                m_phonePreview = section->phonePreview;
            }
        }
    }

    void GameEditorPage::SaveResolutionKey()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        store->Section<GamePageSettings>().resolutionKey = m_resolutionKey;
        store->Section<GamePageSettings>().phonePreview = m_phonePreview;
        store->MarkChanged<GamePageSettings>();
        m_context->RequestProjectEditorSettingsSave();
    }

    void GameEditorPage::RefreshToolbar()
    {
        // The kit's transport: Play while stopped, Pause and Stop while it runs, Restart either way;
        // its state named beside it (Playing, Paused, a script's fault) and the scene it runs.
        const bool faulted = m_running && ScriptState() == PieScriptState::Faulted;
        const bool paused = m_running && (m_simPausedByDebugger || (m_pauseToggle != nullptr && m_pauseToggle->IsChecked()));
        if (m_playButton != nullptr)
            m_playButton->SetEnabled(!m_running && !m_pendingPlay);
        if (m_pauseToggle != nullptr)
            m_pauseToggle->SetEnabled(m_running);
        if (m_stopButton != nullptr)
            m_stopButton->SetEnabled(m_running || m_pendingPlay);
        if (m_cookBadge != nullptr)
            m_cookBadge->SetDisplayed(m_pendingPlay);
        if (m_stateLabel != nullptr)
        {
            const StringView state = !m_running ? StringView(u8"Stopped") : faulted ? StringView(u8"Script error") : paused ? StringView(u8"Paused") : StringView(u8"Playing");
            const StringView kind = !m_running ? StringView(u8"stopped") : faulted ? StringView(u8"faulted") : paused ? StringView(u8"paused") : StringView(u8"playing");
            if (m_stateLabel->GetText() != state)
            {
                m_stateLabel->SetText(state);
                const StringView kinds[] = {u8"stopped", u8"faulted", u8"paused", u8"playing"};
                for (const StringView old : kinds)
                    m_stateLabel->RemoveClass(old);
                m_stateLabel->AddClass(kind);
                m_stateLabel->SetTooltip(faulted ? ScriptFault() : StringView{});
            }
        }
        if (m_sceneLabel != nullptr)
            m_sceneLabel->SetText(m_running ? m_sceneTitle.AsView() : StringView{});
    }

    void GameScriptErrorSink::OnError(const foundation::script::ScriptError& error)
    {
        if (context == nullptr)
        {
            return;
        }
        String message(u8"Game script error: ");
        message += error.message;
        context->Notify(NoticeKind::Error, message.AsView());
    }
    // The keys only while the viewport is the GUI's focused node: a field elsewhere in the editor
    // types without the game reading it (one app keyboard).
    foundation::shell::IKeyboard* GameViewportInputSource::Keyboard()
    {
        return viewport != nullptr && viewport->IsFocused() ? viewport->Keyboard() : nullptr;
    }

    foundation::shell::IMouse* GameViewportInputSource::Mouse()
    {
        return viewport != nullptr ? viewport->Mouse() : nullptr;
    }

    i32 GameViewportInputSource::GamepadCount() const
    {
        // Count is structural; the surface's per-pad facades gate the actual reads.
        return shellInput != nullptr ? Min(shellInput->GamepadCount(), 8) : 0;
    }

    foundation::shell::IGamepad* GameViewportInputSource::Gamepad(i32 index)
    {
        auto* surface = viewport != nullptr && viewport->IsFocused() ? viewport->HostedSurface() : nullptr;
        return surface != nullptr ? surface->Gamepad(index) : nullptr;
    }

    foundation::shell::ITouch* GameViewportInputSource::Touch()
    {
        return viewport != nullptr ? viewport->Touch() : nullptr;
    }

    Span<const foundation::shell::InputEvent> GameViewportInputSource::Events()
    {
        // Key/text events stream only while the viewport owns keyboard focus - the
        // same gate SurfaceKeyboard applies to the polled reads.
        auto* surface = viewport != nullptr && viewport->IsFocused() ? viewport->HostedSurface() : nullptr;
        if (surface == nullptr || !surface->Focused() || shellInput == nullptr)
        {
            return {};
        }
        return shellInput->Events();
    }

    void RegisterGameAudioEditorSettingsTypes()
    {
        GlobalTypeRegistry().Register(GameAudioEditorSettings::StaticType());
        RegisterSerializable<GameAudioEditorSettings>();
    }

    bool HearAllGameInstances(const EditorContext& context)
    {
        foundation::settings::Settings* store = context.UserEditorSettings();
        const GameAudioEditorSettings* section =
            store != nullptr ? store->Find<GameAudioEditorSettings>() : nullptr;
        return section != nullptr && section->hearAllInstances;
    }

    void RegisterGameAudioEditorSettings(EditorContext& context)
    {
        RegisterGameAudioEditorSettingsTypes(); // idempotent belt for odd boot orders
        EditorContext* ctx = &context;
        EditorContext::EditorSettingsContribution contribution;
        contribution.category = String(u8"Game audio");
        EditorContext::EditorSettingsBoolField all;
        all.label = String(u8"Hear every Game tab");
        all.description = String(u8"With several Game tabs running, mix every instance's sound. Off "
                                 u8"(the default): only the focused tab is heard, the others run "
                                 u8"muted.");
        all.get = [ctx]() { return HearAllGameInstances(*ctx); };
        all.set = [ctx](bool value)
        {
            if (foundation::settings::Settings* store = ctx->UserEditorSettings())
            {
                store->Section<GameAudioEditorSettings>().hearAllInstances = value;
                store->MarkChanged<GameAudioEditorSettings>();
            }
        };
        contribution.bools.PushBack(static_cast<EditorContext::EditorSettingsBoolField&&>(all));
        context.RegisterEditorSettingsContribution(
            static_cast<EditorContext::EditorSettingsContribution&&>(contribution));
    }
}

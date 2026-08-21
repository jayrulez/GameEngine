// Engine::GameInstance - the script-bracket bodies (moved verbatim from DefaultApplication's
// former StartGameScript/StopGameScript/TickGameScript, now per-instance).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // the run facade reflection body

module engine.gameinstance;

import foundation.core;
import foundation.scene;
import foundation.scene.resource; // LoadScene / ResolveSceneResources / ResolveScenePrefabs
import foundation.content;        // content::Instance
import foundation.resource;       // ResourceManager + AsyncBindScope (async level load, task #123)
import foundation.script;
import engine.script;
import foundation.script.facades; // RegisterExtraFacadeName (run behavior-prelude hook)
import foundation.net.manager; // NetworkManager factories + InstallNetScriptService
import foundation.input;       // kInputScriptService (install the per-instance runtime)

using namespace foundation::core;
namespace content = foundation::content;
namespace core = foundation::core;
namespace input = foundation::input;
namespace net = foundation::net;
namespace resource = foundation::resource;
namespace scene = foundation::scene;
namespace script = foundation::script;

namespace engine::runtime
{
    // ---- run.* facade (task #123 + game-ready-scripting2 P2-2): reflection bodies + registration (kept
    // out of the interface unit per the GCC gcm-cluster rule). Bound to scripts LOWERCASE as `run` via
    // ScriptName; includes the running instance's level-load control.
    void RunEvents::emit(core::String name) const
    {
        if (bus != nullptr)
        {
            bus->Publish(core::StringHash(name.AsView()), core::Variant{});
        }
    }
    void RunEvents::emit(core::String name, core::Variant payload) const
    {
        if (bus != nullptr)
        {
            bus->Publish(core::StringHash(name.AsView()), static_cast<core::Variant&&>(payload));
        }
    }

    REFLECT_VALUE(RunEvents, "rtti::engine::runtime")
    {
        // emit as an ARITY FAMILY (mirrors SceneEvents): emit(name) + emit(name, payload:Variant).
        builder.Method<static_cast<void (RunEvents::*)(core::String) const>(&RunEvents::emit)>("emit");
        builder.Method<static_cast<void (RunEvents::*)(core::String, core::Variant) const>(
            &RunEvents::emit)>("emit", {"name", "payload"});
        builder.Constructor();
    }

    REFLECT_MEMBERS(Run, "rtti::engine::runtime")
    {
        builder.Attribute("scriptName", "run"); // the reserved lowercase `run` (ScriptName alias)
        builder.Method<&Run::events>("events");  // run.events() -> the run-bus handle
        builder.Method<&Run::loadSceneAsync>("loadSceneAsync");
        builder.Method<&Run::loadProgress>("loadProgress");
        builder.Method<&Run::loadComplete>("loadComplete");
        builder.Method<&Run::loadFailed>("loadFailed");
        builder.Method<&Run::loadScene>("loadScene");
        builder.Method<&Run::sceneReady>("sceneReady");
        builder.Method<&Run::currentScene>("currentScene"); // -> bound Scene (orchestrator)
        // requestExit as an ARITY FAMILY (mirrors emit): requestExit() + requestExit(code:int).
        builder.Method<static_cast<void (*)()>(&Run::requestExit)>("requestExit");
        builder.Method<static_cast<void (*)(i32)>(&Run::requestExit)>("requestExit", {"code"});
        builder.Constructor();
    }

    void RegisterRunScriptFacade()
    {
        static const bool once = []()
        {
            RttiRegisterValue_RunEvents(); // the run.events() handle (a bound value type)
            GlobalTypeRegistry().Register(TypeOf<RunEvents>());
            GlobalTypeRegistry().Register(Run::StaticType()); // binds as `run` (its scriptName alias)
            foundation::script::RegisterExtraFacadeName(u8"run"); // prelude imports the ALIAS, not Run
            return true;
        }();
        (void)once;
    }

    bool GameInstance::StartScript(core::StringView source, core::StringView name,
                                   core::Span<const core::String> gameHandlers)
    {
        StopScript();
        // THIS instance's run host is the game's context (game-instance.md §11.10). The host was
        // configured by the app (ConfigureRunHost) with the facades + Scene.spawn + entity.send routing.
        m_runHost.SetExternalErrorSink(m_errorHandler); // before the context is created
        script::IScriptContext* context = m_runHost.EnsureContextForFile(name);
        if (context == nullptr)
        {
            LOG_ERROR(u8"App", u8"no script backend for '{}'", name);
            return false;
        }
        m_scriptContext = core::RefPtr<script::IScriptContext>(context);
        m_runHost.SetGameScriptHold(true);
        InstallNetBinding(); // the game script (its menu) can now call Net.startServer()/connect()
        m_runBinding.runEvents = &m_runEvents; // run.events() -> THIS run's bus (P2-2, §1a/§1c)
        InstallRunScriptService(
            *m_scriptContext, m_runBinding); // run.* -> this instance's registry
        // Install THIS instance's input runtime as the context's Input service (overriding the shared
        // editor runtime the run-host configurator installed), so the game reads only ITS own source.
        context->SetService(input::kInputScriptService, &m_inputRuntime);

        const bool loaded = m_scriptContext->Load(source, name).IsOk();
        m_runHost
            .NoteExternalLoad(); // the game script loaded its own module (behaviors reload target)
        if (!loaded)
        {
            LOG_ERROR(u8"App", u8"game script '{}' failed to compile", name);
            StopScript();
            return false;
        }
        m_game = m_scriptContext->CreateInstance(u8"Game", core::Span<core::Variant>{});
        if (m_game.Get() == nullptr)
        {
            LOG_ERROR(u8"App", u8"game script '{}' has no `Game` class (construct new())",
                               name);
            StopScript();
            return false;
        }
        // Wire the Game tier's on<Event> inbox to THIS run's bus (game-ready-scripting2 §1a): the shared
        // bridge subscribes the run bus to the Game class's declared handlers (threaded in from the cooked
        // ScriptClass), fanning each to DispatchGameEvent -> the Game object. No implicit scene->run relay.
        m_gameEventSubs.Bind(&m_runEvents,
                             core::Function<void(core::StringView, const core::Variant&)>{
                                 [this](core::StringView eventName, const core::Variant& payload)
                                 { DispatchGameEvent(eventName, payload); }});
        m_gameEventSubs.EnsureFor(gameHandlers);

        (void)m_game->Invoke(u8"launch", core::Span<core::Variant>{});
        LOG_INFO(u8"App", u8"game script '{}' launched", name);
        return true;
    }

    void GameInstance::StopScript()
    {
        m_gameEventSubs.Clear(); // unsubscribe the Game inbox from the run bus (bus outlives the game)
        if (m_game.Get() != nullptr)
        {
            (void)m_game->Invoke(u8"exit", core::Span<core::Variant>{});
            m_game = nullptr;
        }
        m_scriptContext = nullptr; // drop the game script's ref; the run host owns the context
        m_runHost.SetGameScriptHold(false);
        m_runHost.SetExternalErrorSink(nullptr);
        StopNetworking(); // networking belongs to the run - the endpoint drops with it
        // The run host tears down when nothing else pins it - driven by the scene-stop observer
        // (ScriptSubsystem::MaybeTeardownRunHost). A bare instance (no scenes) keeps it until destruction.
    }

    void GameInstance::InstallNetBinding()
    {
        m_netBinding.controller = this; // stable; the endpoint m_net points at may come and go
        if (m_scriptContext.Get() != nullptr)
        {
            net::InstallNetScriptService(*m_scriptContext, m_netBinding);
        }
    }

    bool GameInstance::StartServer(u16 port, bool dedicated)
    {
        m_net = net::NetworkManager::HostServer(port, dedicated);
        if (!m_net)
        {
            LOG_ERROR(u8"App", u8"failed to open a server socket on port {}", port);
            return false;
        }
        m_net->SetReplicatedScene(m_scene);
        if (m_onEndpointOnline)
        {
            m_onEndpointOnline(*m_net);
        } // app wires per-endpoint setup (spawn resolver)
        LOG_INFO(u8"App", u8"server listening on port {}", m_net->BoundPort());
        return true;
    }

    bool GameInstance::Connect(core::StringView host, u16 port)
    {
        m_net = net::NetworkManager::JoinServer(host, port);
        if (!m_net)
        {
            LOG_ERROR(u8"App", u8"failed to open a client socket");
            return false;
        }
        m_net->SetReplicatedScene(m_scene);
        if (m_onEndpointOnline)
        {
            m_onEndpointOnline(*m_net);
        }
        LOG_INFO(u8"App", u8"connecting to {}:{}", host, port);
        return true;
    }

    void GameInstance::StopNetworking()
    {
        if (m_net)
        {
            LOG_INFO(u8"App", u8"networking stopped");
        }
        m_net = nullptr; // closes the session (drops peers) + the owned socket
    }

    void GameInstance::DriveNetwork(f32 fixedDeltaMs)
    {
        if (m_net)
        {
            m_net->Update(fixedDeltaMs);
        }
    }

    void GameInstance::DriveInput(f32 deltaTime, f32 contextTimeScale)
    {
        if (m_inputSource == nullptr)
        {
            return;
        }
        m_inputRuntime.SetTimeScale(contextTimeScale);
        m_inputRuntime.Update(*m_inputSource, deltaTime);
    }

    scene::Scene* GameInstance::CreateScene(core::StringView name, bool activate)
    {
        scene::Scene* scene = m_sceneManager.CreateScene(name, activate);
        if (scene != nullptr)
        {
            // OnSceneCreated (the ScriptSubsystem) added the behavior + level script systems bound to
            // the DEFAULT host; re-bind BOTH to THIS instance's host so they share the game's context.
            if (auto* system = scene->GetSystem<engine::script::ScriptSceneSystem>())
            {
                system->SetRunHost(&m_runHost);
            }
            if (auto* level = scene->GetSystem<engine::script::SceneScriptSystem>())
            {
                level->SetRunHost(&m_runHost);
            }
        }
        return scene;
    }

    scene::Scene* GameInstance::LoadScene(
        content::Instance& sceneInstance, resource::ResourceManager& resources,
        Function<UniquePtr<IStream>(const Guid&)> prefabProvider)
    {
        scene::Scene* scene = CreateScene(sceneInstance.Name()); // active (sync path unchanged)
        if (scene == nullptr || !scene::LoadScene(sceneInstance, *scene).IsOk())
        {
            if (scene != nullptr)
            {
                DestroyScene(scene);
            }
            return nullptr;
        }
        scene::ResolveSceneResources(*scene, resources);
        if (scene->PendingPrefabInstanceCount() > 0)
        {
            scene::ResolveScenePrefabs(*scene, prefabProvider);
            scene::ResolveSceneResources(*scene, resources); // bind the spawned prefabs' refs
        }
        return scene;
    }

    SceneLoadHandle GameInstance::LoadSceneAsync(
        content::Instance& sceneInstance, resource::ResourceManager& resources,
        Function<UniquePtr<IStream>(const Guid&)> prefabProvider)
    {
        SceneLoadHandle handle;
        handle.m_resources = &resources;

        scene::Scene* scene = CreateScene(sceneInstance.Name(), /*activate*/ false); // inactive
        if (scene == nullptr || !scene::LoadScene(sceneInstance, *scene).IsOk())
        {
            if (scene != nullptr)
            {
                DestroyScene(scene);
            }
            handle.m_failed = true;
            return handle;
        }

        {
            // Under the scope, the scene's component Refs bind via BindAsync (decode on workers).
            resource::AsyncBindScope scope(resources);
            scene::ResolveSceneResources(*scene, resources);
        }
        if (scene->PendingPrefabInstanceCount() > 0)
        {
            // Prefab spawn (no resource binds itself), then bind the spawned refs async.
            scene::ResolveScenePrefabs(*scene, prefabProvider);
            resource::AsyncBindScope scope(resources);
            scene::ResolveSceneResources(*scene, resources);
        }

        handle.m_scene = scene;
        handle.m_total = resources.PendingCount(); // snapshot AFTER all async binds are issued
        return handle;
    }

    scene::Scene* GameInstance::ActivateLoadedScene(SceneLoadHandle& handle)
    {
        if (handle.m_failed || handle.m_scene == nullptr || !handle.IsComplete())
        {
            return nullptr;
        }
        m_sceneManager.ActivateScene(handle.m_scene);
        return handle.m_scene;
    }

    i32 GameInstance::TrackScriptLoad(SceneLoadHandle handle)
    {
        const i32 ticket = ++m_nextScriptTicket; // 1-based; 0 stays reserved for "did not start"
        TrackedScriptLoad tracked;
        tracked.ticket = ticket;
        tracked.handle = handle; // SceneLoadHandle is a value type (safe to copy/store)
        m_scriptLoads.PushBack(Move(tracked));
        return ticket;
    }

    void GameInstance::PumpScriptLoads()
    {
        // Index walk (not range-for): a successful activation RETIRES its entry in place, so the
        // array shrinks mid-loop (Fable review: was monotonic growth). Failed / in-flight entries
        // stay and we advance past them.
        for (core::usize i = 0; i < m_scriptLoads.Size();)
        {
            TrackedScriptLoad& load = m_scriptLoads[i];
            if (load.handle.Failed() || !load.handle.IsComplete())
            {
                ++i; // terminally failed (lingers so ScriptLoadFailed stays truthful) or still streaming
                continue;
            }
            scene::Scene* activated = ActivateLoadedScene(load.handle);
            if (activated == nullptr)
            {
                load.handle.m_failed = true; // complete-but-unactivatable: terminal (defensive)
                ++i;
                continue;
            }
            SetScene(activated);              // instance bookkeeping: current scene + net replication
            if (m_activatePolicy)
            {
                m_activatePolicy(activated); // app render/sim policy (EnsureCamera, Start, ...)
            }
            m_scriptLoads.RemoveAt(i); // retire: the ticket now reads terminal-safe via the fallback
        }
    }

    f32 GameInstance::ScriptLoadProgress(i32 ticket) const
    {
        for (const TrackedScriptLoad& load : m_scriptLoads)
        {
            if (load.ticket == ticket)
            {
                return load.handle.Progress();
            }
        }
        return 1.0f; // unknown/expired ticket (incl. a retired-on-activation load: complete)
    }

    bool GameInstance::ScriptLoadComplete(i32 ticket) const
    {
        for (const TrackedScriptLoad& load : m_scriptLoads)
        {
            if (load.ticket == ticket)
            {
                return load.handle.Failed(); // a retained entry is terminal only when it FAILED
            }
        }
        return true; // retired-on-activation (success) OR a bad/expired ticket -> never hang the yield loop
    }

    bool GameInstance::ScriptLoadFailed(i32 ticket) const
    {
        for (const TrackedScriptLoad& load : m_scriptLoads)
        {
            if (load.ticket == ticket)
            {
                return load.handle.Failed();
            }
        }
        return false;
    }

    void GameInstance::DriveRunHost(f32 deltaTime)
    {
        auto& binding = m_runHost.Binding();
        binding.timeSeconds += static_cast<core::f64>(deltaTime);
        binding.deltaSeconds = deltaTime;
        if (m_runHost.Manager() != nullptr)
        {
            m_runHost.Manager()->CollectGarbage();
        }
    }

    void GameInstance::TickScript(f32 hostDeltaTime, f32 contextTimeScale)
    {
        if (m_game.Get() == nullptr)
        {
            return;
        }
        // Debug-paused: the debugger holds a suspended update mid-call. Starting a NEW
        // update each frame would re-hit the breakpoint per frame (pause-event spam, locals
        // flicker) and orphan the held context. Hold script time still, like the scene sim.
        if (m_runHost.IsDebugPaused())
        {
            return;
        }
        const f32 sceneScale = m_scene != nullptr ? m_scene->TimeScale() : 1.0f;
        const scene::FrameTime frame(hostDeltaTime, contextTimeScale, m_instanceTimeScale,
                                     sceneScale);
        core::Variant dt = core::Variant::From(frame.SceneDt());
        if (auto result = m_game->Invoke(u8"update", core::Span<core::Variant>{&dt, 1});
            !result.HasValue())
        {
            // A DEBUGGER SUSPENSION surfaces as an error result too (the suspended call
            // unwinds to here) - same distinction the behavior path's InvokeHandler makes:
            // paused is not a fault. The debugger completes the held call on Continue.
            if (m_runHost.IsDebugPaused())
            {
                return;
            }
            LOG_ERROR(u8"App", u8"game script update() faulted - stopping script");
            m_game = nullptr;
        }
    }

    void GameInstance::DispatchGameEvent(core::StringView eventName, const core::Variant& payload)
    {
        if (m_game.Get() == nullptr)
        {
            return;
        }
        core::String handler(u8"on");
        handler += eventName; // "on" + "Delivered" reconstructs the declared handler
        core::Variant arg = payload;
        const bool hasArg = !payload.IsEmpty();
        const core::Span<core::Variant> args =
            hasArg ? core::Span<core::Variant>{&arg, 1} : core::Span<core::Variant>{};
        if (auto result = m_game->Invoke(handler.AsView(), args); !result.HasValue())
        {
            if (m_runHost.IsDebugPaused())
            {
                return; // suspended at a breakpoint, not a fault (same as update())
            }
            LOG_ERROR(u8"App", u8"game script event handler {} faulted - stopping script", handler);
            m_game = nullptr;
        }
    }

}

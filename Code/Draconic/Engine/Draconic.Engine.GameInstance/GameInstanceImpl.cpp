// Draconic::RuntimeGameInstance - the script-bracket bodies (moved verbatim from DefaultApplication's
// former StartGameScript/StopGameScript/TickGameScript, now per-instance).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h" // the SceneLoader facade reflection body

module draconic.engine.gameinstance;

import draconic.foundation;
import draconic.scene;
import draconic.scene.resource; // LoadScene / ResolveSceneResources / ResolveScenePrefabs
import draconic.content;        // content::Instance
import draconic.resource;       // ResourceManager + AsyncBindScope (async level load, task #123)
import draconic.script;
import draconic.engine.script;
import draconic.script.facades; // RegisterExtraFacadeName (SceneLoader behavior-prelude hook)
import draconic.net.manager; // NetworkManager factories + InstallNetScriptService
import draconic.input;       // kInputScriptService (install the per-instance runtime)

using namespace draconic::foundation;

namespace draconic::runtime
{
    // The SceneLoader.* facade reflection body + registration (kept out of the interface unit per the
    // GCC gcm-cluster rule). Owned by the game-instance project (the out-of-tree facade pattern).
    DRACONIC_REFLECT(SceneLoader, "draconic::runtime")
    {
        builder.Method<&SceneLoader::loadSceneAsync>("loadSceneAsync");
        builder.Method<&SceneLoader::loadProgress>("loadProgress");
        builder.Method<&SceneLoader::loadComplete>("loadComplete");
        builder.Method<&SceneLoader::loadFailed>("loadFailed");
        builder.Method<&SceneLoader::loadScene>("loadScene");
        builder.Method<&SceneLoader::sceneReady>("sceneReady");
        builder.Method<&SceneLoader::currentScene>("currentScene"); // -> bound Scene (orchestrator)
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    void RegisterSceneLoaderScriptFacade()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(SceneLoader::StaticType());
            draconic::script::RegisterExtraFacadeName(
                u8"SceneLoader"); // Wren behavior prelude imports it (AngelScript binds by registry)
            return true;
        }();
        (void)once;
    }

    bool GameInstance::StartScript(foundation::StringView source, foundation::StringView name)
    {
        StopScript();
        // THIS instance's run host is the game's context (game-instance.md §11.10). The host was
        // configured by the app (ConfigureRunHost) with the facades + Scene.spawn + entity.send routing.
        m_runHost.SetExternalErrorSink(m_errorHandler); // before the context is created
        script::IScriptContext* context = m_runHost.EnsureContextForFile(name);
        if (context == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"App", u8"no script backend for '{}'", name);
            return false;
        }
        m_scriptContext = foundation::RefPtr<script::IScriptContext>(context);
        m_runHost.SetGameScriptHold(true);
        InstallNetBinding(); // the game script (its menu) can now call Net.startServer()/connect()
        InstallSceneLoaderScriptService(
            *m_scriptContext, m_sceneLoaderBinding); // SceneLoader.* -> this instance's load registry
        // Install THIS instance's input runtime as the context's Input service (overriding the shared
        // editor runtime the run-host configurator installed), so the game reads only ITS own source.
        context->SetService(input::kInputScriptService, &m_inputRuntime);

        const bool loaded = m_scriptContext->Load(source, name).IsOk();
        m_runHost
            .NoteExternalLoad(); // the game script loaded its own module (behaviors reload target)
        if (!loaded)
        {
            DRACONIC_LOG_ERROR(u8"App", u8"game script '{}' failed to compile", name);
            StopScript();
            return false;
        }
        m_game = m_scriptContext->CreateInstance(u8"Game", foundation::Span<foundation::Variant>{});
        if (m_game.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"App", u8"game script '{}' has no `Game` class (construct new())",
                               name);
            StopScript();
            return false;
        }
        (void)m_game->Invoke(u8"launch", foundation::Span<foundation::Variant>{});
        DRACONIC_LOG_INFO(u8"App", u8"game script '{}' launched", name);
        return true;
    }

    void GameInstance::StopScript()
    {
        if (m_game.Get() != nullptr)
        {
            (void)m_game->Invoke(u8"exit", foundation::Span<foundation::Variant>{});
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
            DRACONIC_LOG_ERROR(u8"App", u8"failed to open a server socket on port {}", port);
            return false;
        }
        m_net->SetReplicatedScene(m_scene);
        if (m_onEndpointOnline)
        {
            m_onEndpointOnline(*m_net);
        } // app wires per-endpoint setup (spawn resolver)
        DRACONIC_LOG_INFO(u8"App", u8"server listening on port {}", m_net->BoundPort());
        return true;
    }

    bool GameInstance::Connect(foundation::StringView host, u16 port)
    {
        m_net = net::NetworkManager::JoinServer(host, port);
        if (!m_net)
        {
            DRACONIC_LOG_ERROR(u8"App", u8"failed to open a client socket");
            return false;
        }
        m_net->SetReplicatedScene(m_scene);
        if (m_onEndpointOnline)
        {
            m_onEndpointOnline(*m_net);
        }
        DRACONIC_LOG_INFO(u8"App", u8"connecting to {}:{}", host, port);
        return true;
    }

    void GameInstance::StopNetworking()
    {
        if (m_net)
        {
            DRACONIC_LOG_INFO(u8"App", u8"networking stopped");
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

    scene::Scene* GameInstance::CreateScene(foundation::StringView name, bool activate)
    {
        scene::Scene* scene = m_sceneManager.CreateScene(name, activate);
        if (scene != nullptr)
        {
            // OnSceneCreated (the ScriptSubsystem) added the behavior + level script systems bound to
            // the DEFAULT host; re-bind BOTH to THIS instance's host so they share the game's context.
            if (auto* system = scene->GetSystem<script::ScriptSceneSystem>())
            {
                system->SetRunHost(&m_runHost);
            }
            if (auto* level = scene->GetSystem<script::SceneScriptSystem>())
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
        for (foundation::usize i = 0; i < m_scriptLoads.Size();)
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
        binding.timeSeconds += static_cast<foundation::f64>(deltaTime);
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
        foundation::Variant dt = foundation::Variant::From(hostDeltaTime * contextTimeScale *
                                               m_instanceTimeScale * sceneScale);
        if (auto result = m_game->Invoke(u8"update", foundation::Span<foundation::Variant>{&dt, 1});
            !result.HasValue())
        {
            // A DEBUGGER SUSPENSION surfaces as an error result too (the suspended call
            // unwinds to here) - same distinction the behavior path's InvokeHandler makes:
            // paused is not a fault. The debugger completes the held call on Continue.
            if (m_runHost.IsDebugPaused())
            {
                return;
            }
            DRACONIC_LOG_ERROR(u8"App", u8"game script update() faulted - stopping script");
            m_game = nullptr;
        }
    }

}

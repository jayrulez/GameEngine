// Draconic Runtime - draconic.engine.defaultapp implementation unit.
//
// Out-of-line definitions for DefaultApplication's member functions (sec 3.2 / sec 10.6).
// The class declaration + trivial inline accessors stay in DefaultApplication.cppm.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.engine.defaultapp;

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
namespace net = draconic::net;
using namespace draconic::shell;
using namespace draconic::graphics;

namespace draconic::runtime
{
    void DefaultApplication::OnUpdate(IApplicationHost& host, foundation::f32 deltaTime)
    {
        // Finalize any async resource loads first, so this frame's spawns/ticks see ready
        // resources (task #123). Pump ONLY the manager this app OWNS: when this DefaultApplication
        // is embedded in the editor it BORROWS the editor's manager (m_ownedResources stays null),
        // and the editor pumps that manager itself - pumping it here too would double-pump.
        // Inert until a factory migrates to the async path.
        if (m_ownedResources)
        {
            m_ownedResources->Pump();
        }

        // Drive + tick EVERY instance (primary + any extras - multi-instance PIE / headless server).
        // Input FIRST (so the game script sees this frame's keys), then the run host clock, then tick.
        const foundation::f32 contextScale = host.Ctx().TimeScale();
        ForEachInstance(
            [&](GameInstance& gi)
            {
                gi.PumpScriptLoads(); // activate any script-initiated load that finished (after Pump)
                gi.DriveInput(deltaTime, contextScale);
                gi.DriveRunHost(deltaTime);
                gi.TickScript(deltaTime, contextScale);
            });
        IShell* plat = host.Shell();
        IInputManager* input = (plat != nullptr) ? plat->Input() : nullptr;
        IKeyboard* kb = (input != nullptr) ? input->Keyboard() : nullptr;
        if (kb == nullptr || !kb->IsKeyPressed(KeyCode::P))
        {
            return;
        }

        foundation::ConsoleWrite(draconic::profiler::Profiler::Get().BuildReport().AsView());
        if (auto* renderer = host.Ctx().GetSubsystem<draconic::render::RenderSubsystem>())
        {
            foundation::String gpu;
            renderer->BuildGpuProfileReport(gpu);
            foundation::ConsoleWrite(gpu.AsView());
        }
    }

    void DefaultApplication::TickGameScript(IApplicationHost& host, foundation::f32 deltaTime)
    {
        m_instance.TickScript(deltaTime, host.Ctx().TimeScale());
    }

    void DefaultApplication::Configure(IApplicationHost& host)
    {
        m_scenes = host.Ctx().AddSubsystem<draconic::scene::SceneSubsystem>();
        // The run's scene group lives on the GameInstance (game-instance.md §11): wire it to the
        // app-wide aware registry and register it so it ticks on the Context lane beside the default
        // (editor/loose) group. WireInstance centralizes this so extra instances wire the same way.
        m_scenes->RegisterManager(&m_instance.Scenes());
        m_instance.Scenes().SetAwareRegistry(&m_scenes->AwareRegistry());
        if (GraphicsDevice* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            host.Ctx().AddSubsystem<draconic::render::RenderSubsystem>(*gfx->Raw(),
                                                                       gfx->FramesInFlight());
            // Drives skeletal animation from the scene tick (injects the SkeletalAnimation manager,
            // ticks players, feeds bone matrices to mesh components). Needs the render managers.
            host.Ctx().AddSubsystem<draconic::animation::AnimationSubsystem>();
            host.Ctx().AddSubsystem<draconic::particles::ParticleSubsystem>();
        }
        m_physics = host.Ctx().AddSubsystem<draconic::physics::PhysicsSubsystem>();
        // Networking scene integration: injects the NetworkComponentManager into every scene so
        // authored NetworkComponents work (the per-instance endpoint replicates over it).
        host.Ctx().AddSubsystem<draconic::net::NetworkSubsystem>();
        m_audio = host.Ctx().AddSubsystem<draconic::audio::AudioSubsystem>(m_audioEngineSettings);
        m_input = host.Ctx().AddSubsystem<draconic::input::InputSubsystem>(
            host.Shell() != nullptr ? host.Shell()->Input() : nullptr);
        // The primary instance's per-instance input reads the shell devices by default (the player
        // path); the editor Game tab overrides this to its gated viewport source per tab.
        m_instance.SetInputSource(&m_input->ShellSource());
        m_ui = host.Ctx().AddSubsystem<draconic::ui::UISubsystem>();
        if (!m_uiFontPath.IsEmpty())
        {
            m_ui->SetFontPath(m_uiFontPath.AsView());
        }

        // Entity behaviors (scripting.md P1). Facade/backend registration is
        // batteries-included here (idempotent - entry points may register more);
        // the run context itself is created lazily by the subsystem and SHARED
        // with the game script (one gameplay context per run, the locked rule).
        m_scripts = host.Ctx().AddSubsystem<draconic::script::ScriptSubsystem>();
        // The instance owns its run host (game-instance.md §11.10); wire it with the app's facades
        // + Scene.spawn + entity.send routing so its context has them when the game script starts.
        // (The subsystem's own default host - for editor scenes - is wired in its OnReady.)
        m_scripts->ConfigureRunHost(m_instance.RunHost());
        InstallInstanceLoadFacade(m_instance); // SceneLoader.* level-load facade for the primary instance
        draconic::input::RegisterInputScriptFacade();
        draconic::physics::RegisterPhysicsScriptFacade();
        draconic::render::RegisterRenderScriptFacade();
        draconic::animation::RegisterAnimationScriptFacade();
        draconic::particles::RegisterParticleScriptFacade();
        draconic::audio::RegisterAudioScriptFacade();
        RegisterSceneLoaderScriptFacade();     // SceneLoader.* (owned by the game-instance project)
        draconic::ui::RegisterUiScriptFacade(); // Ui.* (owned by the UISubsystem)
        // Every built backend registers (batteries-included); a run resolves by the game script's
        // LANGUAGE - one gameplay context per run stays the locked rule. Each backend is independently
        // toggleable (DRACONIC_ENABLE_WREN / _ANGELSCRIPT); both build on every platform, web included.
#ifdef DRACONIC_HAS_WREN
        draconic::script::wren::RegisterWrenScriptBackend();
#endif
#ifdef DRACONIC_HAS_ANGELSCRIPT
        draconic::script::angelscript::RegisterAngelScriptBackend();
#endif
        // Networking (net.md §6): the Net facade type is registered here; each GameInstance owns
        // its OWN endpoint and goes online at RUNTIME via the facade (Net.startServer/connect from
        // the game's menu) - no app-owned socket. The primary instance carries the online hook (the
        // prefab net-spawn resolver) + the optional startup preset below; extras get the hook in
        // CreateInstance. The per-instance net binding is installed by GameInstance itself.
        net::RegisterNetScriptFacade();
        draconic::net::RegisterNetworkComponentScriptFacade(); // NetworkComponent.of(entity).authority
        m_instance.SetEndpointOnlineHook(MakeEndpointOnlineHook());
        ApplyNetworkStartup(
            m_instance); // enter a preset server/client role at startup (None = offline)

        DefaultApplication* self = this;

        m_scripts->SetContextConfigurator(foundation::Function<void(draconic::script::IScriptContext&)>{
            [self](draconic::script::IScriptContext& context)
            {
                if (self->m_input != nullptr)
                {
                    self->m_input->ExposeToScript(context);
                }
                if (self->m_physics != nullptr)
                {
                    self->m_physics->ExposeToScript(context);
                }
                if (self->m_audio != nullptr)
                {
                    self->m_audio->ExposeToScript(context, self->Resources());
                }
                // Ui.* -> the app-wide screen-tier host (backs pushOverlay/setters/onClick).
                draconic::ui::InstallUiScriptService(context, self->m_uiScriptBinding);
            }});

        // Back the Ui.* facade with the live screen tier: attach the UISubsystem, resolve cooked
        // UIDocuments by guid from the run's resource manager (read live - it may attach later in
        // the editor), and route the binding into the host.
        if (m_ui != nullptr)
        {
            m_uiScriptHost.Attach(*m_ui);
            m_uiScriptHost.SetDocumentResolver(
                foundation::Function<foundation::RefPtr<draconic::ui::UIDocument>(const foundation::Guid&)>{
                    [self](const foundation::Guid& id) -> foundation::RefPtr<draconic::ui::UIDocument>
                    {
                        if (self->Resources() == nullptr || id.IsNil())
                        {
                            return {};
                        }
                        auto proxy = self->Resources()->Bind<draconic::ui::UIDocument>(id);
                        draconic::ui::UIDocument* document = proxy.Get();
                        return document != nullptr
                                   ? foundation::RefPtr<draconic::ui::UIDocument>(document)
                                   : foundation::RefPtr<draconic::ui::UIDocument>{};
                    }});
            m_uiScriptHost.Install(m_uiScriptBinding);
        }
        // Scene.spawn: resolve the prefab payload from the content DB the entry point
        // preset, spawn it, place the root at the requested world position, and bind
        // the freshly spawned entities' resources.
        m_scripts->SetPrefabSpawner(
            foundation::Function<draconic::scene::EntityHandle(draconic::scene::Scene*, const foundation::Guid&,
                                                         const foundation::Float3&)>{
                [self](draconic::scene::Scene* scene, const foundation::Guid& prefabId,
                       const foundation::Float3& position) -> draconic::scene::EntityHandle
                {
                    if (scene == nullptr || self->m_contentDatabase == nullptr)
                    {
                        return draconic::scene::EntityHandle::Invalid();
                    }
                    draconic::content::Instance* prefab =
                        self->m_contentDatabase->GetInstance(prefabId);
                    foundation::UniquePtr<foundation::IStream> payload = (prefab != nullptr)
                                                                 ? prefab->ReadData(u8"scene")
                                                                 : foundation::UniquePtr<foundation::IStream>{};
                    if (!payload)
                    {
                        return draconic::scene::EntityHandle::Invalid();
                    }
                    const draconic::scene::EntityHandle root =
                        draconic::scene::SpawnPrefab(*scene, *payload, prefabId);
                    if (root.IsAssigned())
                    {
                        foundation::Transform transform = scene->GetLocalTransform(root);
                        transform.position = position;
                        scene->SetLocalTransform(root, transform);
                        if (self->Resources() != nullptr)
                        {
                            draconic::scene::ResolveSceneResources(*scene, *self->Resources());
                        }
                    }
                    return root;
                }});

        // Track A resource swaps (SceneRender.setMesh, ...): give the run a GETTER for the app's
        // resource manager (created later, in OnStartup), so a behavior can bind a resource id onto
        // a component's Ref. Borrowed - the app owns it.
        m_scripts->SetResourceManager(foundation::Function<draconic::resource::ResourceManager*()>{
            [self]() -> draconic::resource::ResourceManager* { return self->Resources(); }});

        // Composition-root bridge: forward physics contacts to the script subsystem's
        // neutral ingress. Keeps the two subsystems independent - neither depends on the
        // other for scripting; the wiring lives here, where integration belongs.
        if (m_physics != nullptr && m_scripts != nullptr)
        {
            m_contactBridge.Install(*m_physics, *m_scripts);
        }
    }

    draconic::script::ScriptSubsystem* DefaultApplication::Scripts() const noexcept
    {
        return m_scripts;
    }

    draconic::scene::SceneManager& DefaultApplication::PrimaryScenes() noexcept
    {
        return m_instance.Scenes();
    }

    GameInstance* DefaultApplication::CreateInstance(bool headless)
    {
        if (m_scenes == nullptr || m_scripts == nullptr)
        {
            return nullptr;
        }
        foundation::UniquePtr<GameInstance> owned =
            foundation::MakeUnique<GameInstance>(foundation::DefaultAllocator());
        GameInstance* gi = owned.Get();
        gi->SetHeadless(headless);
        gi->Scenes().SetAwareRegistry(&m_scenes->AwareRegistry());
        m_scenes->RegisterManager(&gi->Scenes());
        m_scripts->ConfigureRunHost(gi->RunHost());
        InstallInstanceLoadFacade(*gi); // SceneLoader.* level-load facade for this extra instance
        gi->SetEndpointOnlineHook(
            MakeEndpointOnlineHook()); // its own endpoint, wired like the primary
        if (m_input != nullptr)
        {
            gi->SetInputSource(&m_input->ShellSource());
        } // editor tabs override to their viewport
        m_extraInstances.PushBack(Move(owned));
        return gi;
    }

    void DefaultApplication::ReleaseInstance(GameInstance* instance)
    {
        if (instance == nullptr || instance == &m_instance)
        {
            return;
        }
        for (foundation::usize i = 0; i < m_extraInstances.Size(); ++i)
        {
            if (m_extraInstances[i].Get() != instance)
            {
                continue;
            }
            if (m_scenes != nullptr)
            {
                m_scenes->UnregisterManager(&instance->Scenes());
            }
            instance->Scenes().Clear(); // destroy any remaining scenes (aware subsystems notified)
            instance->RunHost().Teardown();
            m_extraInstances.RemoveAt(i); // frees the GameInstance
            return;
        }
    }

    void DefaultApplication::ApplyLoadedSceneActivation(draconic::scene::Scene* scene)
    {
        if (scene == nullptr)
        {
            return;
        }
        scene->Start();
        scene->SetSimulationEnabled(true);
    }

    void DefaultApplication::InstallInstanceLoadFacade(GameInstance& gi)
    {
        DefaultApplication* self = this;
        GameInstance* instance = &gi;

        // The render/sim policy PumpScriptLoads runs when a tracked load finishes (SetScene already
        // done by then). Virtual, so the player seeds a camera; the base just starts + simulates.
        gi.SetSceneActivationPolicy(foundation::Function<void(draconic::scene::Scene*)>{
            [self](draconic::scene::Scene* scene) { self->ApplyLoadedSceneActivation(scene); }});

        // SceneLoader.loadSceneAsync(id) -> resolve the cooked scene instance, kick an async load into THIS
        // instance, register it under a ticket. 0 = could not start (bad id / no DB). The prefab
        // provider reads a nested-prefab payload by guid - the same source the sync path uses.
        gi.SceneLoaderBinding().loadSceneAsync =
            foundation::Function<foundation::i32(const foundation::Guid&)>{[self, instance](const foundation::Guid& sceneId) -> foundation::i32
            {
                if (self->m_contentDatabase == nullptr || self->Resources() == nullptr)
                {
                    return 0;
                }
                draconic::content::Instance* sceneInst = self->m_contentDatabase->GetInstance(sceneId);
                if (sceneInst == nullptr)
                {
                    return 0;
                }
                draconic::content::IContentDatabase* db = self->m_contentDatabase;
                runtime::SceneLoadHandle handle = instance->LoadSceneAsync(
                    *sceneInst, *self->Resources(),
                    foundation::Function<foundation::UniquePtr<foundation::IStream>(const foundation::Guid&)>{
                        [db](const foundation::Guid& prefabId) -> foundation::UniquePtr<foundation::IStream>
                        {
                            draconic::content::Instance* prefab = db->GetInstance(prefabId);
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : foundation::UniquePtr<foundation::IStream>{};
                        }});
                return instance->TrackScriptLoad(foundation::Move(handle));
            }};

        gi.SceneLoaderBinding().loadProgress = foundation::Function<foundation::f64(foundation::i32)>{
            [instance](foundation::i32 ticket) -> foundation::f64
            { return static_cast<foundation::f64>(instance->ScriptLoadProgress(ticket)); }};
        gi.SceneLoaderBinding().loadComplete = foundation::Function<bool(foundation::i32)>{
            [instance](foundation::i32 ticket) -> bool { return instance->ScriptLoadComplete(ticket); }};
        gi.SceneLoaderBinding().loadFailed = foundation::Function<bool(foundation::i32)>{
            [instance](foundation::i32 ticket) -> bool { return instance->ScriptLoadFailed(ticket); }};

        // Game.loadScene(id): synchronous convenience for tiny scenes - load, make current, apply the
        // same activation policy, all before the call returns. false on a resolve/load failure.
        gi.SceneLoaderBinding().loadScene =
            foundation::Function<bool(const foundation::Guid&)>{[self, instance](const foundation::Guid& sceneId) -> bool
            {
                if (self->m_contentDatabase == nullptr || self->Resources() == nullptr)
                {
                    return false;
                }
                draconic::content::Instance* sceneInst = self->m_contentDatabase->GetInstance(sceneId);
                if (sceneInst == nullptr)
                {
                    return false;
                }
                draconic::content::IContentDatabase* db = self->m_contentDatabase;
                draconic::scene::Scene* scene = instance->LoadScene(
                    *sceneInst, *self->Resources(),
                    foundation::Function<foundation::UniquePtr<foundation::IStream>(const foundation::Guid&)>{
                        [db](const foundation::Guid& prefabId) -> foundation::UniquePtr<foundation::IStream>
                        {
                            draconic::content::Instance* prefab = db->GetInstance(prefabId);
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : foundation::UniquePtr<foundation::IStream>{};
                        }});
                if (scene == nullptr)
                {
                    return false;
                }
                instance->SetScene(scene); // current-scene bookkeeping (async path does this in Pump)
                self->ApplyLoadedSceneActivation(scene);
                return true;
            }};

        gi.SceneLoaderBinding().sceneReady =
            foundation::Function<bool()>{[instance]() -> bool { return instance->SceneReady(); }};
        gi.SceneLoaderBinding().currentScene = foundation::Function<scene::Scene*()>{
            [instance]() -> scene::Scene* { return instance->GetScene(); }};
    }

    draconic::physics::PhysicsSubsystem* DefaultApplication::Physics() const noexcept
    {
        return m_physics;
    }

    void DefaultApplication::OnFixedUpdate(IApplicationHost& host, foundation::f32 fixedDeltaTime)
    {
        (void)host;
        const foundation::f32 fixedMs = fixedDeltaTime * 1000.0f; // seconds -> ms
        ForEachInstance([fixedMs](GameInstance& gi) { gi.DriveNetwork(fixedMs); });
    }

    void
    DefaultApplication::SetAudioEngineSettings(const draconic::audio::AudioEngineSettings& settings)
    {
        m_audioEngineSettings = settings;
    }

    void
    DefaultApplication::SetResourceManager(draconic::resource::ResourceManager* borrowed) noexcept
    {
        m_borrowedResources = borrowed;
    }

    void
    DefaultApplication::SetContentDatabase(draconic::content::IContentDatabase* database) noexcept
    {
        m_contentDatabase = database;
    }

    draconic::resource::ResourceManager* DefaultApplication::Resources() const noexcept
    {
        return m_borrowedResources != nullptr ? m_borrowedResources : m_ownedResources.Get();
    }

    void DefaultApplication::OnStartup(IApplicationHost& host)
    {
        // Product/runtime types: factories construct cooked products BY TYPE NAME.
        draconic::model::RegisterModelResourceTypes();
        draconic::image::RegisterImageResource();
        draconic::particles::RegisterParticleEffectResource();
        draconic::input::RegisterInputMapResource();
        draconic::physics::RegisterPhysicsResource();
        draconic::audio::RegisterAudioResource();
        draconic::script::RegisterScriptResource();
        draconic::ui::RegisterUIResource();
        foundation::GlobalTypeRegistry().Register(draconic::scene::SceneDocument::StaticType());
        foundation::RegisterSerializable<draconic::scene::SceneDocument>();
        draconic::ui::RegisterUIComponentReflection();
        if (GraphicsDevice* gfx = host.Graphics();
            gfx != nullptr && gfx->Raw() != nullptr && m_ui != nullptr)
        {
            m_ui->EnsureRenderReady(*gfx->Raw(), gfx->FramesInFlight());
        }

        if (m_borrowedResources == nullptr && m_contentDatabase != nullptr)
        {
            // Share the global JobSystem so migrated factories can decode off the main thread
            // (async resource loading, task #123); null when there is no pool = synchronous loads.
            m_ownedResources = foundation::MakeUnique<draconic::resource::ResourceManager>(
                foundation::DefaultAllocator(), *m_contentDatabase,
                foundation::HasGlobalJobSystem() ? &foundation::GlobalJobs() : nullptr);
        }
        draconic::resource::ResourceManager* resources = Resources();
        if (resources == nullptr)
        {
            return;
        } // headless/no-content apps (a project-manager editor attaches one later)
        RegisterStandardFactories(*resources, host);
    }

    void DefaultApplication::RegisterStandardFactories(draconic::resource::ResourceManager& resources,
                                                       IApplicationHost& host)
    {
        resources.AddFactory(&m_meshFactory);
        resources.AddFactory(&m_skinnedMeshFactory);
        resources.AddFactory(&m_materialFactory);
        resources.AddFactory(&m_skeletonFactory);
        resources.AddFactory(&m_animationClipFactory);
        resources.AddFactory(&m_animationGraphFactory);
        resources.AddFactory(&m_particleEffectFactory);
        resources.AddFactory(&m_inputMapFactory);
        resources.AddFactory(&m_collisionShapeFactory);
        resources.AddFactory(&m_physicalMaterialFactory);
        resources.AddFactory(&m_audioClipFactory);
        resources.AddFactory(&m_busLayoutFactory);
        resources.AddFactory(&m_soundCueFactory);
        resources.AddFactory(&m_scriptClassFactory);
        resources.AddFactory(&m_modelFactory);
        resources.AddFactory(&m_uiDocumentFactory);
        resources.AddFactory(&m_uiThemeFactory);
        if (GraphicsDevice* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            if (!m_textureFactory)
            {
                m_textureFactory = foundation::MakeUnique<draconic::texture::TextureFactory>(
                    foundation::DefaultAllocator(), *gfx->Raw());
            }
            resources.AddFactory(m_textureFactory.Get());
        }
    }

    void DefaultApplication::AttachResourceManager(draconic::resource::ResourceManager* borrowed,
                                                   IApplicationHost& host)
    {
        m_borrowedResources = borrowed;
        if (borrowed != nullptr)
        {
            RegisterStandardFactories(*borrowed, host);
        }
    }

    void DefaultApplication::OnShutdown(IApplicationHost&)
    {
        // Destroy the run's scenes while the aware subsystems are still alive (they get
        // OnSceneDestroyed). The editor's GamePage already cleared them per Stop; this covers the
        // player + any leftover. Do it FIRST, before subsystem teardown, for EVERY instance.
        ForEachInstance(
            [](GameInstance& gi)
            {
                gi.StopNetworking();
                gi.Scenes().Clear();
                gi.RunHost().Teardown();
            });
        m_contactBridge.Uninstall();
        m_ownedResources = nullptr; // release products while the device is alive
        m_textureFactory = nullptr;
    }

    void DefaultApplication::SetPrimaryScene(draconic::scene::Scene* scene) noexcept
    {
        m_instance.SetScene(scene); // also repoints the instance's replicated scene when online
    }

    draconic::scene::Scene* DefaultApplication::PrimaryScene() const noexcept
    {
        return m_instance.GetScene();
    }

    void DefaultApplication::SetGameScriptErrorHandler(
        draconic::script::IScriptErrorHandler* handler) noexcept
    {
        m_instance.SetScriptErrorHandler(handler);
    }

    bool DefaultApplication::StartGameScript(foundation::StringView source, foundation::StringView name)
    {
        return m_instance.StartScript(source, name);
    }

    void DefaultApplication::OnRenderWindow(IApplicationHost& host, FrameContext& frame)
    {
        auto* render = host.Ctx().GetSubsystem<draconic::render::RenderSubsystem>();
        auto* scenes = host.Ctx().GetSubsystem<draconic::scene::SceneSubsystem>();
        if (render == nullptr || !render->IsReady() || scenes == nullptr ||
            frame.encoder == nullptr || frame.backbufferView == nullptr || frame.window == nullptr)
        {
            frame.Clear(0.08f, 0.09f, 0.12f, 1.0f); // no renderer - present a clear
            return;
        }

        const rhi::TextureFormat colorFormat = frame.window->Swap()->Format();
        // RenderTexture canvases draw BEFORE the scene so materials sampling them
        // see this frame's UI (the RenderCanvasTextures host seam).
        if (m_ui != nullptr)
        {
            m_ui->RenderCanvasTextures(*frame.encoder, static_cast<foundation::i32>(frame.frameIndex));
        }
        render->BeginRendering(*frame.encoder, frame.frameIndex);
        // Render every NON-headless instance's scenes (game-instance.md §11 - a headless dedicated
        // server simulates but isn't drawn) + the default group's loose scenes. Clear comes from
        // the scene's camera.
        ForEachInstance(
            [&](GameInstance& gi)
            {
                if (gi.IsHeadless())
                {
                    return;
                }
                for (draconic::scene::Scene* scene : gi.Scenes().ActiveScenes())
                {
                    render->RenderScene(*scene, frame.backbufferView, colorFormat, frame.width,
                                        frame.height);
                }
            });
        render->EndRendering(); // scene-tier overlays (HUD/billboards) draw inside the compose

        // Window-space overlays (screen-tier UI, diagnostics, ...) composite over the
        // finished frame through the generic registry - the host names no source.
        render->RenderOverlays(*frame.encoder, frame.backbufferView, colorFormat, frame.width,
                               frame.height, frame.frameIndex);
    }

    EndpointOnlineHook DefaultApplication::MakeEndpointOnlineHook()
    {
        DefaultApplication* self = this;
        return EndpointOnlineHook{
            [self](net::NetworkManager& endpoint)
            {
                endpoint.Replication().SetSpawnHandler(
                    foundation::Function<draconic::scene::EntityHandle(
                        draconic::scene::Scene&, const foundation::Guid&, net::NetworkId)>{
                        [self](draconic::scene::Scene& scene, const foundation::Guid& prefabId,
                               net::NetworkId) -> draconic::scene::EntityHandle
                        {
                            if (self->m_contentDatabase == nullptr)
                            {
                                return draconic::scene::EntityHandle::Invalid();
                            }
                            draconic::content::Instance* prefab =
                                self->m_contentDatabase->GetInstance(prefabId);
                            foundation::UniquePtr<foundation::IStream> payload =
                                (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                    : foundation::UniquePtr<foundation::IStream>{};
                            if (!payload)
                            {
                                return draconic::scene::EntityHandle::Invalid();
                            }
                            const draconic::scene::EntityHandle root =
                                draconic::scene::SpawnPrefab(scene, *payload, prefabId);
                            if (root.IsAssigned() && self->Resources() != nullptr)
                            {
                                draconic::scene::ResolveSceneResources(scene, *self->Resources());
                            }
                            return root;
                        }});
            }};
    }

    void DefaultApplication::ApplyNetworkStartup(GameInstance& instance)
    {
        switch (m_netStartup.role)
        {
        case net::NetworkRole::Server:
            (void)instance.StartServer(m_netStartup.listenPort, m_netStartup.dedicated);
            break;
        case net::NetworkRole::Client:
            (void)instance.Connect(m_netStartup.serverHost.AsView(), m_netStartup.serverPort);
            break;
        case net::NetworkRole::None:
        default:
            break;
        }
    }
}

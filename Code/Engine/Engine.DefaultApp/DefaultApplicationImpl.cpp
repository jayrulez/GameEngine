// Runtime - engine.defaultapp implementation unit.
//
// Out-of-line definitions for DefaultApplication's member functions (sec 3.2 / sec 10.6).
// The class declaration + trivial inline accessors stay in DefaultApplication.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module engine.defaultapp;

import foundation.core;
import foundation.rhi;
import foundation.runtime.client;       // IApplication, IApplicationHost
import engine.gameinstance; // GameInstance - this app's running game (scene + script bracket)
import foundation.shell;                // IShell, IKeyboard, KeyCode (the profile-dump hotkey)
import foundation.graphics;             // GraphicsDevice, FrameContext
import foundation.scene;                // Scene
import engine.scene;      // SceneSubsystem (the standard scene driver)
import engine.scenesurface; // FullSceneComposition (the single source of truth for scene assembly)
import engine.render;     // RenderSubsystem (the standard renderer)
import engine.animation; // AnimationSubsystem (drives skeletal animation from the scene)
import engine.particles; // ParticleSubsystem (scene-driven CPU sim)
import foundation.physics;             // ContactKind/EntityContact (the contact bridge)
import engine.physics;   // PhysicsSubsystem (Jolt worlds + interpolation)
import engine.navigation; // NavigationSubsystem + script facade
import foundation.input;               // the action model/runtime
import engine.input;     // InputSubsystem + the Input facade
import foundation.script;              // IScriptManager/Context (the game script)
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript; // the AngelScript backend (second backend; OPTION_ENABLE_ANGELSCRIPT)
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;         // the Luau backend (OPTION_ENABLE_LUAU)
#endif
import foundation.script.resource;     // cooked script classes + factory (entity behaviors)
import engine.script;    // ScriptSubsystem (behaviors + the run's shared context)
import foundation.resource;            // ResourceManager (owned or borrowed - see the preset seam)
import foundation.content;             // IContentDatabase (preset by the entry point)
import foundation.scene.resource;      // SceneDocument (product-type registration)
import foundation.geometry.resource;   // mesh factories
import foundation.materials.resource;  // material factory
import foundation.animation.resource;  // skeleton/clip/graph factories
import foundation.propertyanimation.resource; // property-animation clip factory
import foundation.particles.resource;  // particle-effect factory
import foundation.input.resource;      // input-map factory
import foundation.fonts.resource;      // FontResource + FontFactory (default UI font)
import foundation.physics.resource;    // collision-shape/physical-material factories
import foundation.navigation.resource; // navmesh-zone factory
import foundation.texture.resource;    // texture factory (device-backed)
import foundation.image.resource;      // image resource registration
import foundation.model.resource;      // cooked-model family types + registration
import foundation.ui;                  // View (the `ui` binding's instantiate return type)
import foundation.ui.resource;         // cooked UI documents/themes (game-ui)
import engine.ui;        // the game screen tier (canvases + overlay + consumption)
import engine.ui.script;   // UiScreenScriptBinding + InstallUiScreenScriptService + RegisterUiScriptSurface
import foundation.audio;               // AudioEngine (owned by the audio subsystem)
import foundation.audio.resource;      // cooked audio clips + factory
import engine.audio;     // AudioSubsystem (voices/buses/one-shots + scene sync)
import foundation.net;                 // UdpSocket / DatagramEndpoint (the transport)
import foundation.net.replication;     // NetworkId / StateReplication (the spawn-handler seam)
import foundation.net.manager;   // NetworkManager + NetworkStartup/StartNetworking + the Net facade
import engine.net; // NetworkSubsystem (injects the NetworkComponentManager into scenes)
import foundation.profiler;      // the CPU scope profiler (P-key dump)

namespace rhi = foundation::rhi;
namespace core = foundation::core;
namespace net = foundation::net;
using namespace foundation::runtime; // foundation runtime: IApplication/IApplicationHost/Subsystem/Context
using namespace foundation::shell;
using namespace foundation::graphics;
namespace scene = foundation::scene;

namespace engine::runtime
{
    void DefaultApplication::OnUpdate(IApplicationHost& host, core::f32 deltaTime)
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
        const core::f32 contextScale = host.Ctx().TimeScale();
        ForEachInstance(
            [&](GameInstance& gi)
            {
                gi.PumpScriptLoads(); // activate any script-initiated load that finished (after Pump)
                gi.DriveInput(deltaTime, contextScale);
                gi.DriveRunHost(deltaTime);
                gi.TickScript(deltaTime, contextScale);
                gi.DrainRunEvents(); // deliver this frame's run-bus events (after the game script ticked)
            });
        IShell* plat = host.Shell();
        IInputManager* input = (plat != nullptr) ? plat->Input() : nullptr;
        IKeyboard* kb = (input != nullptr) ? input->Keyboard() : nullptr;
        if (kb == nullptr || !kb->IsKeyPressed(KeyCode::P))
        {
            return;
        }

        core::ConsoleWrite(foundation::profiler::Profiler::Get().BuildReport().AsView());
        if (auto* renderer = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
        {
            core::String gpu;
            renderer->BuildGpuProfileReport(gpu);
            core::ConsoleWrite(gpu.AsView());
        }
    }

    void DefaultApplication::TickGameScript(IApplicationHost& host, core::f32 deltaTime)
    {
        m_instance.TickScript(deltaTime, host.Ctx().TimeScale());
    }

    void DefaultApplication::Configure(IApplicationHost& host)
    {
        m_host = &host; // stable for the app's lifetime; extra instances route run.requestExit through it
        m_scenes = host.Ctx().AddSubsystem<engine::scene::SceneSubsystem>();
        // The scene-assembly blueprint (scene-composition.md): every registered manager's CreateScene
        // now assembles from the full composition (with ISceneAware injection still layered on top for
        // custom/plugin subsystems) instead of the per-subsystem ISceneAware two-pass alone.
        m_scenes->SetComposition(engine::FullSceneComposition());
        // The run's scene group lives on the GameInstance (game-instance.md §11): wire it to the
        // app-wide aware registry and register it so it ticks on the Context lane beside the default
        // (editor/loose) group. WireInstance centralizes this so extra instances wire the same way.
        m_scenes->RegisterManager(&m_instance.Scenes());
        m_instance.Scenes().SetAwareRegistry(&m_scenes->AwareRegistry());
        if (GraphicsDevice* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            host.Ctx().AddSubsystem<engine::render::RenderSubsystem>(*gfx->Raw(),
                                                                       gfx->FramesInFlight());
            // Drives skeletal animation from the scene tick (injects the SkeletalAnimation manager,
            // ticks players, feeds bone matrices to mesh components). Needs the render managers.
            host.Ctx().AddSubsystem<engine::animation::AnimationSubsystem>();
            host.Ctx().AddSubsystem<engine::particles::ParticleSubsystem>();
        }
        m_physics = host.Ctx().AddSubsystem<engine::physics::PhysicsSubsystem>();
        host.Ctx().AddSubsystem<engine::navigation::NavigationSubsystem>();
        // Networking scene integration: injects the NetworkComponentManager into every scene so
        // authored NetworkComponents work (the per-instance endpoint replicates over it).
        host.Ctx().AddSubsystem<engine::net::NetworkSubsystem>();
        m_audio = host.Ctx().AddSubsystem<engine::audio::AudioSubsystem>(m_audioEngineSettings);
        m_input = host.Ctx().AddSubsystem<engine::input::InputSubsystem>(
            host.Shell() != nullptr ? host.Shell()->Input() : nullptr);
        // The primary instance's per-instance input reads the shell devices by default (the player
        // path); the editor Game tab overrides this to its gated viewport source per tab.
        m_instance.SetInputSource(&m_input->ShellSource());
        m_ui = host.Ctx().AddSubsystem<engine::ui::UISubsystem>();
        if (!m_uiFontPath.IsEmpty())
        {
            m_ui->SetFontPath(m_uiFontPath.AsView());
        }

        // Entity behaviors (scripting.md P1). Facade/backend registration is
        // batteries-included here (idempotent - entry points may register more);
        // the run context itself is created lazily by the subsystem and SHARED
        // with the game script (one gameplay context per run, the locked rule).
        m_scripts = host.Ctx().AddSubsystem<engine::script::ScriptSubsystem>();
        // The instance owns its run host (game-instance.md §11.10); wire it with the app's facades
        // + Scene.spawn + entity.send routing so its context has them when the game script starts.
        // (The subsystem's own default host - for editor scenes - is wired in its OnReady.)
        m_scripts->ConfigureRunHost(m_instance.RunHost());
        InstallInstanceLoadFacade(m_instance, host); // run.* level-load facade for the primary instance
        engine::input::RegisterInputScriptFacade();
        engine::physics::RegisterPhysicsScriptFacade();
        engine::navigation::RegisterNavigationScriptFacade();
        engine::render::RegisterRenderScriptFacade();
        engine::animation::RegisterAnimationScriptFacade();
        engine::particles::RegisterParticleScriptFacade();
        engine::audio::RegisterAudioScriptFacade();
        engine::uiscript::RegisterUiScriptSurface();    // screen-tier `ui` facade + reflected view handles
        engine::ui::RegisterUiComponentScriptFacades(); // world-space UI components' `.of` surface
        // Every built backend registers (batteries-included); a run resolves by the game script's
        // LANGUAGE - one gameplay context per run stays the locked rule. Each backend is independently
        // toggleable (OPTION_ENABLE_ANGELSCRIPT / _LUAU); all build on every platform, web included.
#ifdef OPTION_HAS_ANGELSCRIPT
        foundation::script::angelscript::RegisterAngelScriptBackend();
#endif
#ifdef OPTION_HAS_LUAU
        foundation::script::RegisterLuauScriptBackend();
#endif
        // Networking (net.md §6): the Net facade type is registered here; each GameInstance owns
        // its OWN endpoint and goes online at RUNTIME via the facade (Net.startServer/connect from
        // the game's menu) - no app-owned socket. The primary instance carries the online hook (the
        // prefab net-spawn resolver) + the optional startup preset below; extras get the hook in
        // CreateInstance. The per-instance net binding is installed by GameInstance itself.
        net::RegisterNetScriptFacade();
        foundation::net::RegisterNetworkComponentScriptFacade(); // NetworkComponent.of(entity).authority
        m_instance.SetEndpointOnlineHook(MakeEndpointOnlineHook());
        ApplyNetworkStartup(
            m_instance); // enter a preset server/client role at startup (None = offline)

        DefaultApplication* self = this;

        m_scripts->SetContextConfigurator(core::Function<void(foundation::script::IScriptContext&)>{
            [self](foundation::script::IScriptContext& context)
            {
                if (self->m_input != nullptr)
                {
                    self->m_input->ExposeToScript(context);
                }
                if (self->m_audio != nullptr)
                {
                    self->m_audio->ExposeToScript(context, self->Resources());
                }
                // `ui` -> the app-wide screen tier (finders + push/pop over the ScreenStack).
                engine::uiscript::InstallUiScreenScriptService(context, self->m_uiScreenBinding);
            }});

        // Back the `ui` facade with the live screen tier: point the binding at the UISubsystem's
        // screen root + ScreenStack, and supply a cooked-UIDocument instantiator (resolve by guid
        // from the run's resource manager - read live, as it may attach later in the editor - then
        // instantiate the markup into a view tree).
        if (m_ui != nullptr)
        {
            m_uiScreenBinding.screenRoot = m_ui->ScreenRoot();
            m_uiScreenBinding.stack = &m_ui->Screens();
            m_uiScreenBinding.instantiate =
                core::Function<core::RefPtr<foundation::ui::View>(const core::Guid&)>{
                    [self](const core::Guid& id) -> core::RefPtr<foundation::ui::View>
                    {
                        if (self->m_ui == nullptr || self->Resources() == nullptr || id.IsNil())
                        {
                            return {};
                        }
                        auto proxy = self->Resources()->Bind<foundation::ui::UIDocument>(id);
                        foundation::ui::UIDocument* document = proxy.Get();
                        return document != nullptr ? self->m_ui->InstantiateScreenOverlay(*document)
                                                   : core::RefPtr<foundation::ui::View>{};
                    }};
        }
        // Scene.spawn: resolve the prefab payload from the content DB the entry point
        // preset, spawn it, place the root at the requested world position, and bind
        // the freshly spawned entities' resources.
        m_scripts->SetPrefabSpawner(
            core::Function<foundation::scene::EntityHandle(foundation::scene::Scene*, const core::Guid&,
                                                         const core::Float3&)>{
                [self](foundation::scene::Scene* scene, const core::Guid& prefabId,
                       const core::Float3& position) -> foundation::scene::EntityHandle
                {
                    if (scene == nullptr || self->m_contentDatabase == nullptr)
                    {
                        return foundation::scene::EntityHandle::Invalid();
                    }
                    foundation::content::Instance* prefab =
                        self->m_contentDatabase->GetInstance(prefabId);
                    core::UniquePtr<core::IStream> payload = (prefab != nullptr)
                                                                 ? prefab->ReadData(u8"scene")
                                                                 : core::UniquePtr<core::IStream>{};
                    if (!payload)
                    {
                        return foundation::scene::EntityHandle::Invalid();
                    }
                    const foundation::scene::EntityHandle root =
                        foundation::scene::SpawnPrefab(*scene, *payload, prefabId);
                    if (root.IsAssigned())
                    {
                        core::Transform transform = scene->GetLocalTransform(root);
                        transform.position = position;
                        scene->SetLocalTransform(root, transform);
                        if (self->Resources() != nullptr)
                        {
                            foundation::scene::ResolveSceneResources(*scene, *self->Resources());
                        }
                    }
                    return root;
                }});

        // Track A resource swaps (SceneRender.setMesh, ...): give the run a GETTER for the app's
        // resource manager (created later, in OnStartup), so a behavior can bind a resource id onto
        // a component's Ref. Borrowed - the app owns it.
        m_scripts->SetResourceManager(core::Function<foundation::resource::ResourceManager*()>{
            [self]() -> foundation::resource::ResourceManager* { return self->Resources(); }});

        // Composition-root bridge: forward physics contacts to the script subsystem's
        // neutral ingress. Keeps the two subsystems independent - neither depends on the
        // other for scripting; the wiring lives here, where integration belongs.
        if (m_physics != nullptr && m_scripts != nullptr)
        {
            m_contactBridge.Install(*m_physics, *m_scripts);
        }
    }

    engine::script::ScriptSubsystem* DefaultApplication::Scripts() const noexcept
    {
        return m_scripts;
    }

    foundation::scene::SceneManager& DefaultApplication::PrimaryScenes() noexcept
    {
        return m_instance.Scenes();
    }

    GameInstance* DefaultApplication::CreateInstance(bool headless)
    {
        if (m_scenes == nullptr || m_scripts == nullptr || m_host == nullptr)
        {
            return nullptr;
        }
        core::UniquePtr<GameInstance> owned =
            core::MakeUnique<GameInstance>(core::DefaultAllocator());
        GameInstance* gi = owned.Get();
        gi->SetHeadless(headless);
        gi->Scenes().SetAwareRegistry(&m_scenes->AwareRegistry());
        m_scenes->RegisterManager(&gi->Scenes());
        m_scripts->ConfigureRunHost(gi->RunHost());
        InstallInstanceLoadFacade(*gi, *m_host); // run.* level-load facade for this extra instance
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
        for (core::usize i = 0; i < m_extraInstances.Size(); ++i)
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

    void DefaultApplication::ApplyLoadedSceneActivation(foundation::scene::Scene* scene)
    {
        if (scene == nullptr)
        {
            return;
        }
        scene->Start();
        scene->SetSimulationEnabled(true);
    }

    void DefaultApplication::InstallInstanceLoadFacade(GameInstance& gi, IApplicationHost& host)
    {
        DefaultApplication* self = this;
        GameInstance* instance = &gi;

        // The render/sim policy PumpScriptLoads runs when a tracked load finishes (SetScene already
        // done by then). Virtual, so the player seeds a camera; the base just starts + simulates.
        gi.SetSceneActivationPolicy(core::Function<void(foundation::scene::Scene*)>{
            [self](foundation::scene::Scene* scene) { self->ApplyLoadedSceneActivation(scene); }});

        // run.loadSceneAsync(id) -> resolve the cooked scene instance, kick an async load into THIS
        // instance, register it under a ticket. 0 = could not start (bad id / no DB). The prefab
        // provider reads a nested-prefab payload by guid - the same source the sync path uses.
        gi.RunBinding().loadSceneAsync =
            core::Function<core::i32(const core::Guid&)>{[self, instance](const core::Guid& sceneId) -> core::i32
            {
                if (self->m_contentDatabase == nullptr || self->Resources() == nullptr)
                {
                    return 0;
                }
                foundation::content::Instance* sceneInst = self->m_contentDatabase->GetInstance(sceneId);
                if (sceneInst == nullptr)
                {
                    return 0;
                }
                foundation::content::IContentDatabase* db = self->m_contentDatabase;
                engine::runtime::SceneLoadHandle handle = instance->LoadSceneAsync(
                    *sceneInst, *self->Resources(),
                    core::Function<core::UniquePtr<core::IStream>(const core::Guid&)>{
                        [db](const core::Guid& prefabId) -> core::UniquePtr<core::IStream>
                        {
                            foundation::content::Instance* prefab = db->GetInstance(prefabId);
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : core::UniquePtr<core::IStream>{};
                        }});
                return instance->TrackScriptLoad(core::Move(handle));
            }};

        gi.RunBinding().loadProgress = core::Function<core::f64(core::i32)>{
            [instance](core::i32 ticket) -> core::f64
            { return static_cast<core::f64>(instance->ScriptLoadProgress(ticket)); }};
        gi.RunBinding().loadComplete = core::Function<bool(core::i32)>{
            [instance](core::i32 ticket) -> bool { return instance->ScriptLoadComplete(ticket); }};
        gi.RunBinding().loadFailed = core::Function<bool(core::i32)>{
            [instance](core::i32 ticket) -> bool { return instance->ScriptLoadFailed(ticket); }};

        // run.loadScene(id): synchronous convenience for tiny scenes - load, make current, apply the
        // same activation policy, all before the call returns. false on a resolve/load failure.
        gi.RunBinding().loadScene =
            core::Function<bool(const core::Guid&)>{[self, instance](const core::Guid& sceneId) -> bool
            {
                if (self->m_contentDatabase == nullptr || self->Resources() == nullptr)
                {
                    return false;
                }
                foundation::content::Instance* sceneInst = self->m_contentDatabase->GetInstance(sceneId);
                if (sceneInst == nullptr)
                {
                    return false;
                }
                foundation::content::IContentDatabase* db = self->m_contentDatabase;
                foundation::scene::Scene* scene = instance->LoadScene(
                    *sceneInst, *self->Resources(),
                    core::Function<core::UniquePtr<core::IStream>(const core::Guid&)>{
                        [db](const core::Guid& prefabId) -> core::UniquePtr<core::IStream>
                        {
                            foundation::content::Instance* prefab = db->GetInstance(prefabId);
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : core::UniquePtr<core::IStream>{};
                        }});
                if (scene == nullptr)
                {
                    return false;
                }
                instance->SetScene(scene); // current-scene bookkeeping (async path does this in Pump)
                self->ApplyLoadedSceneActivation(scene);
                return true;
            }};

        gi.RunBinding().sceneReady =
            core::Function<bool()>{[instance]() -> bool { return instance->SceneReady(); }};
        gi.RunBinding().currentScene = core::Function<scene::Scene*()>{
            [instance]() -> scene::Scene* { return instance->GetScene(); }};

        // run.requestExit(code): end the run through the app host. Standalone (ApplicationHost) stops
        // the loop; the editor's EmbeddedApplicationHost routes to the exit handler the editor set,
        // which stops the Game tab's play session. The host outlives the binding (app/session lifetime),
        // so a borrowed pointer is safe.
        gi.RunBinding().requestExit = core::Function<void(core::i32)>{
            [hostPtr = &host](core::i32 code) { hostPtr->RequestExit(code); }};
    }

    engine::physics::PhysicsSubsystem* DefaultApplication::Physics() const noexcept
    {
        return m_physics;
    }

    void DefaultApplication::OnFixedUpdate(IApplicationHost& host, core::f32 fixedDeltaTime)
    {
        (void)host;
        const core::f32 fixedMs = fixedDeltaTime * 1000.0f; // seconds -> ms
        ForEachInstance([fixedMs](GameInstance& gi) { gi.DriveNetwork(fixedMs); });
    }

    void
    DefaultApplication::SetAudioEngineSettings(const foundation::audio::AudioEngineSettings& settings)
    {
        m_audioEngineSettings = settings;
    }

    void
    DefaultApplication::SetResourceManager(foundation::resource::ResourceManager* borrowed) noexcept
    {
        m_borrowedResources = borrowed;
    }

    void
    DefaultApplication::SetContentDatabase(foundation::content::IContentDatabase* database) noexcept
    {
        m_contentDatabase = database;
    }

    foundation::resource::ResourceManager* DefaultApplication::Resources() const noexcept
    {
        return m_borrowedResources != nullptr ? m_borrowedResources : m_ownedResources.Get();
    }

    void DefaultApplication::OnStartup(IApplicationHost& host)
    {
        // Product/runtime types: factories construct cooked products BY TYPE NAME.
        foundation::model::RegisterModelResourceTypes();
        foundation::image::RegisterImageResource();
        foundation::particles::RegisterParticleEffectResource();
        foundation::input::RegisterInputMapResource();
        foundation::physics::RegisterPhysicsResource();
        foundation::navigation::RegisterNavigationResource();
        foundation::audio::RegisterAudioResource();
        foundation::script::RegisterScriptResource();
        foundation::ui::RegisterUIResource();
        foundation::fonts::RegisterFontResource();
        core::GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
        core::RegisterSerializable<foundation::scene::SceneDocument>();
        engine::ui::RegisterUIComponentReflection();
        if (GraphicsDevice* gfx = host.Graphics();
            gfx != nullptr && gfx->Raw() != nullptr && m_ui != nullptr)
        {
            m_ui->EnsureRenderReady(*gfx->Raw(), gfx->FramesInFlight());
        }

        if (m_borrowedResources == nullptr && m_contentDatabase != nullptr)
        {
            // Share the global JobSystem so migrated factories can decode off the main thread
            // (async resource loading, task #123); null when there is no pool = synchronous loads.
            m_ownedResources = core::MakeUnique<foundation::resource::ResourceManager>(
                core::DefaultAllocator(), *m_contentDatabase,
                core::HasGlobalJobSystem() ? &core::GlobalJobs() : nullptr);
        }
        foundation::resource::ResourceManager* resources = Resources();
        if (resources == nullptr)
        {
            return;
        } // headless/no-content apps (a project-manager editor attaches one later)
        RegisterStandardFactories(*resources, host);
    }

    void DefaultApplication::RegisterStandardFactories(foundation::resource::ResourceManager& resources,
                                                       IApplicationHost& host)
    {
        resources.AddFactory(&m_meshFactory);
        resources.AddFactory(&m_skinnedMeshFactory);
        resources.AddFactory(&m_materialFactory);
        resources.AddFactory(&m_skeletonFactory);
        resources.AddFactory(&m_animationClipFactory);
        resources.AddFactory(&m_animationGraphFactory);
        resources.AddFactory(&m_propertyAnimationClipFactory);
        resources.AddFactory(&m_particleEffectFactory);
        resources.AddFactory(&m_inputMapFactory);
        resources.AddFactory(&m_collisionShapeFactory);
        resources.AddFactory(&m_navigationZoneFactory);
        resources.AddFactory(&m_physicalMaterialFactory);
        resources.AddFactory(&m_audioClipFactory);
        resources.AddFactory(&m_busLayoutFactory);
        resources.AddFactory(&m_soundCueFactory);
        resources.AddFactory(&m_scriptClassFactory);
        resources.AddFactory(&m_modelFactory);
        resources.AddFactory(&m_uiDocumentFactory);
        resources.AddFactory(&m_uiThemeFactory);
        resources.AddFactory(&m_fontFactory);
        if (GraphicsDevice* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            if (!m_textureFactory)
            {
                m_textureFactory = core::MakeUnique<foundation::texture::TextureFactory>(
                    core::DefaultAllocator(), *gfx->Raw());
            }
            resources.AddFactory(m_textureFactory.Get());
        }
    }

    void DefaultApplication::AttachResourceManager(foundation::resource::ResourceManager* borrowed,
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

    void DefaultApplication::SetPrimaryScene(foundation::scene::Scene* scene) noexcept
    {
        m_instance.SetScene(scene); // also repoints the instance's replicated scene when online
    }

    foundation::scene::Scene* DefaultApplication::PrimaryScene() const noexcept
    {
        return m_instance.GetScene();
    }

    void DefaultApplication::SetGameScriptErrorHandler(
        foundation::script::IScriptErrorHandler* handler) noexcept
    {
        m_instance.SetScriptErrorHandler(handler);
    }

    bool DefaultApplication::StartGameScript(core::StringView source, core::StringView name)
    {
        return m_instance.StartScript(source, name);
    }

    void DefaultApplication::OnRenderWindow(IApplicationHost& host, FrameContext& frame)
    {
        auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();
        auto* scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
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
            m_ui->RenderCanvasTextures(*frame.encoder, static_cast<core::i32>(frame.frameIndex));
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
                for (foundation::scene::Scene* scene : gi.Scenes().ActiveScenes())
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
                    core::Function<foundation::scene::EntityHandle(
                        foundation::scene::Scene&, const core::Guid&, net::NetworkId)>{
                        [self](foundation::scene::Scene& scene, const core::Guid& prefabId,
                               net::NetworkId) -> foundation::scene::EntityHandle
                        {
                            if (self->m_contentDatabase == nullptr)
                            {
                                return foundation::scene::EntityHandle::Invalid();
                            }
                            foundation::content::Instance* prefab =
                                self->m_contentDatabase->GetInstance(prefabId);
                            core::UniquePtr<core::IStream> payload =
                                (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                    : core::UniquePtr<core::IStream>{};
                            if (!payload)
                            {
                                return foundation::scene::EntityHandle::Invalid();
                            }
                            const foundation::scene::EntityHandle root =
                                foundation::scene::SpawnPrefab(scene, *payload, prefabId);
                            if (root.IsAssigned() && self->Resources() != nullptr)
                            {
                                foundation::scene::ResolveSceneResources(scene, *self->Resources());
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

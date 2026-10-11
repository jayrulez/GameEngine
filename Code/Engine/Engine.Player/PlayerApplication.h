// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PlayerApplication - the generic game runner's IApplication, shared by the desktop and web
// entry points (Main.cpp / WebMain.cpp). Runs a project with ZERO native game code: engine
// subsystems + the project's content + the default scene, and the project's game SCRIPT (a
// scripted IApplication counterpart - resolved by the script's LANGUAGE, so AngelScript or Luau
// both work). See Main.cpp's header comment for the two layouts (PROJECT vs DIST).
//
// Classic header (the APP_MAIN pattern): it carries no `import` of its own and uses names
// the INCLUDING TU must bring in first. Include it AFTER these imports:
//   foundation.core, foundation.vfs, foundation.vfs.pak, foundation.content, foundation.resource,
//   foundation.runtime, foundation.runtime.client, engine.defaultapp,
//   foundation.scene, engine.scene, foundation.scene.resource,
//   foundation.render, foundation.script, foundation.script.resource,
//   foundation.input, foundation.audio, foundation.audio.resource, foundation.ui.resource,
//   engine.ui, foundation.settings, foundation.project, foundation.xml.serialization
// The two entry points add only the PLATFORM trio (shell + runner + graphics) on top.

#ifndef ENGINE_PLAYER_PLAYERAPPLICATION_H
#define ENGINE_PLAYER_PLAYERAPPLICATION_H

#include "Core/Prelude.h"
#include "Core/Log/Log.h"

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}


namespace engine::player
{
    using namespace foundation::core;
    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;
    namespace resource = foundation::resource;

    struct PlayerOptions
    {
        String projectDir;
        String sceneOverride; // source-DB path; empty = the manifest's defaultScene
        f32 exitAfterSeconds = 0.0f;
        // The project's statically-linked native game plugin (SHIP stubs pass it through
        // PlayerMain; dev builds leave it null and load any native module dynamically).
        // Consumed at the host seam in N2 (game-native-code.md); borrowed, ship-stub-owned.
        foundation::runtime::IRuntimePlugin* nativeGame = nullptr;
    };

    class PlayerApplication : public engine::runtime::DefaultApplication
    {
    public:
        explicit PlayerApplication(PlayerOptions options) : m_options(Move(options)) {}

        void OnStartup(runtime::IApplicationHost& host) override
        {
            namespace project = engine::project;


            m_root = MakeUnique<foundation::vfs::NativeFileSystem>(
                AppRoot(), m_options.projectDir.AsView(), AppRoot());

            // Dist layout wins when present (a staged dist can sit inside a project tree).
            if (m_root->Exists(project::kDistContentPak))
            {
                m_pak = MakeUnique<foundation::vfs::PakFileSystem>(
                    AppRoot(),
                    PathJoin(m_options.projectDir.AsView(), project::kDistContentPak).AsView());
                if (!m_pak->IsValid() ||
                    !project::LoadProjectSettings(*m_root, m_settings, project::kDistManifestFile)
                         .IsOk())
                {
                    LOG_ERROR(u8"Player", u8"dist at '{}' is unreadable",
                                       m_options.projectDir);
                    host.RequestExit(1);
                    return;
                }
                m_contentDb = MakeUnique<foundation::content::ContentDatabase>(
                    AppRoot(), AppRoot(), *m_pak, BinarySerializerFactory(),
                    project::kCookedAssetExtension);
                m_sceneDb = m_contentDb.Get(); // scenes live IN the pak, binary like products
                LOG_INFO(u8"Player", u8"dist mode ({} pak entries)", m_pak->EntryCount());
            }
            else if (m_root->Exists(project::kProjectManifestFile))
            {
                if (!project::LoadProjectSettings(*m_root, m_settings).IsOk())
                {
                    LOG_ERROR(u8"Player", u8"project manifest at '{}' is unreadable",
                                       m_options.projectDir);
                    host.RequestExit(1);
                    return;
                }
                m_contentMount = MakeUnique<foundation::vfs::NativeFileSystem>(
                    AppRoot(),
                    PathJoin(m_options.projectDir.AsView(), project::kProjectContentDir).AsView(),
                    AppRoot());
                m_cookedMount = MakeUnique<foundation::vfs::NativeFileSystem>(
                    AppRoot(),
                    PathJoin(m_options.projectDir.AsView(), project::kProjectCookedDir).AsView(),
                    AppRoot());
                m_sourceDb = MakeUnique<foundation::content::ContentDatabase>(
                    AppRoot(), AppRoot(), *m_contentMount,
                    foundation::xml::XmlSerializerFactory(), project::kSourceAssetExtension);
                m_contentDb = MakeUnique<foundation::content::ContentDatabase>(
                    AppRoot(), AppRoot(), *m_cookedMount,
                    BinarySerializerFactory(), project::kCookedAssetExtension);
                m_sceneDb = m_sourceDb.Get(); // authored scenes; products from the cooked DB
            }
            else
            {
                LOG_ERROR(
                    u8"Player",
                    u8"'{}' is neither a project (Project.xml) nor a dist (Content.pak)",
                    m_options.projectDir);
                host.RequestExit(1);
                return;
            }

            // The app builds the resource manager + standard factories over this DB
            // (product types registered there too - the infra preset).
            SetContentDatabase(m_contentDb.Get());
            engine::runtime::DefaultApplication::OnStartup(host);

            // Game-UI IME lifecycle: the player owns its window, so the UI subsystem
            // drives StartTextInput/StopTextInput on it as game EditText focus moves.
            // (The editor leaves this null - its UIHost bridge owns the IME there.)
            if (UI() != nullptr && host.Shell() != nullptr)
            {
                UI()->SetTextInputTarget(host.Shell()->MainWindow());
            }

            // Native game plugin (game-native-code.md N2). Placed AFTER the base startup so
            // OnLoad can resolve engine subsystems from Ctx(), and BEFORE the initial scene
            // resolves so plugin-registered types deserialize. Two convergent paths:
            // a SHIP stub hands the statically-linked plugin through PlayerOptions; a dev
            // build loads the manifest's nativeModule (a project-relative path to the
            // built module - shared-engine builds only, the ruled plugin model). A missing
            // or unloadable module is reported and the run continues (scripts still work).
            m_plugins = MakeUnique<runtime::PluginHost>(AppRoot(), host.Ctx());
            // Record the managers/systems the game contributes to scenes so unload reverses
            // them (game-native-code.md S1).
            m_plugins->AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
            if (m_options.nativeGame != nullptr)
            {
                m_plugins->Add(m_options.nativeGame);
                LOG_INFO(u8"Player", u8"native game plugin '{}' (static)",
                         m_options.nativeGame->Name());
            }
            else if (!m_settings.nativeModule.IsEmpty())
            {
                const String modulePath = PathJoin(m_options.projectDir.AsView(),
                                                   m_settings.nativeModule.AsView());
                auto loaded = m_plugins->Load(modulePath.AsView());
                if (loaded.HasValue())
                {
                    LOG_INFO(u8"Player", u8"native game module '{}' loaded ('{}')",
                             m_settings.nativeModule, loaded.Value()->Name());
                }
                else
                {
                    LOG_ERROR(u8"Player",
                              u8"native game module '{}' failed to load - continuing "
                              u8"without it (a static engine build cannot load native "
                              u8"modules; use a shared build, or re-export)",
                              modulePath);
                }
            }
        }


        void OnLaunch(runtime::IApplicationHost& host) override
        {
            if (m_sceneDb == nullptr)
            {
                return;
            }
            auto* scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }

            // Resolution order: --scene path override, the manifest's guid (authoritative,
            // rename-proof), then the path mirror (v2 manifests).
            foundation::content::Instance* instance = nullptr;
            if (!m_options.sceneOverride.IsEmpty())
            {
                instance = m_sceneDb->GetInstance(m_options.sceneOverride.AsView());
            }
            else
            {
                if (!m_settings.defaultSceneId.IsNil())
                {
                    instance = m_sceneDb->GetInstance(m_settings.defaultSceneId);
                }
                if (instance == nullptr && !m_settings.defaultScene.IsEmpty())
                {
                    instance = m_sceneDb->GetInstance(m_settings.defaultScene.AsView());
                }
            }
            // defaultSceneId presence is the boot switch now (task #123 boot reorder): a resolved
            // startup scene loads AFTER the game script launches (below); none = the script owns boot
            // entirely. EXCEPTION: an EXPLICIT --scene that did not resolve is still a user error.
            if (instance == nullptr && !m_options.sceneOverride.IsEmpty())
            {
                LOG_ERROR(u8"Player", u8"--scene '{}' did not resolve",
                                   m_options.sceneOverride.AsView());
                host.RequestExit(1);
                return;
            }

            // The project's default input map: cooked resource -> the PRIMARY instance's ActionRuntime
            // (per-instance input; the game reads its own runtime). Nil/unresolved = no actions bound.
            if (Input() != nullptr && !m_settings.defaultInputMapId.IsNil())
            {
                auto mapProxy = Resources()->Bind<foundation::input::InputMapResource>(
                    m_settings.defaultInputMapId);
                if (mapProxy)
                {
                    Instance().SetInputMap(mapProxy->Map());
                    LOG_INFO(u8"Player", u8"input map bound ({} set(s))",
                                      mapProxy->Map().sets.Size());
                }
                else
                {
                    LOG_WARNING(u8"Player", u8"default input map did not resolve");
                }
            }

            // The project's scene-pass MSAA sample count: drive the render subsystem's
            // global path. The subsystem capability-clamps per view (an unsupported count degrades,
            // e.g. 2x -> 1x on WebGPU), so we pass the authored value straight through. Only when > 1
            // (1 = off = the default, which also avoids forcing the global-post path on).
            if (m_settings.renderMsaaSamples > 1)
            {
                if (auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
                {
                    render->SetMsaaSamples(m_settings.renderMsaaSamples);
                    LOG_INFO(u8"Player", u8"scene-pass MSAA requested: {}x",
                                      m_settings.renderMsaaSamples);
                }
            }

            // The project's default audio bus layout: cooked mixer data -> the engine.
            // Nil/unresolved = the built-in neutral four-bus layout.
            if (Audio() != nullptr && Audio()->Engine() != nullptr &&
                !m_settings.defaultBusLayoutId.IsNil())
            {
                auto layoutProxy = Resources()->Bind<foundation::audio::AudioBusLayoutResource>(
                    m_settings.defaultBusLayoutId);
                if (layoutProxy)
                {
                    Audio()->Engine()->ApplyBusLayout(layoutProxy->layout);
                    LOG_INFO(u8"Player", u8"audio bus layout applied");
                }
                else
                {
                    LOG_WARNING(u8"Player", u8"default bus layout did not resolve");
                }
            }

            // Per-user audio volumes: <userdata>/<project>.user.settings.xml, applied
            // ON TOP of the layout (the user's slider is absolute - options-menu law).
            // Absent on first run; captured back at shutdown so script-driven changes
            // (Audio.setBusVolume) persist for free.
            if (Audio() != nullptr && Audio()->Engine() != nullptr)
            {
                engine::audio::RegisterAudioSettingsTypes();
                foundation::vfs::NativeFileSystem userFs(
                    foundation::core::GetUserDataDirectory().AsView(),
                    AppRoot());
                UniquePtr<IStream> stream =
                    userFs.Open(UserSettingsFileName().AsView(), FileMode::Read);
                if (stream)
                {
                    foundation::settings::Settings store(AppRoot());
                    if (store.Load(*stream, foundation::xml::XmlSerializerFactory()).IsOk())
                    {
                        if (const auto* audio = store.Find<engine::audio::AudioUserSettings>())
                        {
                            engine::audio::ApplyAudioUserSettings(*Audio()->Engine(), *audio);
                            LOG_INFO(u8"Player", u8"user audio settings applied");
                        }
                    }
                }
            }

            // The project's default UI font: cooked FontResource -> the game context's
            // font service (nil/unresolved = the dev TTF fallback, which does not exist
            // in a dist - a distributed game NEEDS this binding for any text at all).
            if (UI() != nullptr && !m_settings.defaultUiFontId.IsNil())
            {
                auto fontProxy =
                    Resources()->Bind<foundation::fonts::Font>(m_settings.defaultUiFontId);
                if (fontProxy)
                {
                    UI()->SetDefaultFont(fontProxy.Get());
                    LOG_INFO(u8"Player", u8"default UI font bound");
                }
                else
                {
                    LOG_WARNING(u8"Player", u8"default UI font did not resolve");
                }
            }
            else if (UI() != nullptr)
            {
                // A distributed game with no default font renders no text (no dev fallback here).
                LOG_WARNING(u8"Player",
                                     u8"no default UI font set in project settings - game UI text "
                                     u8"will not render (set Project Settings > Default UI font)");
            }

            // The project's other UI fonts, each a family beside the default one (the subsystem
            // holds the bound products).
            if (UI() != nullptr && !m_settings.uiFontIds.IsEmpty())
            {
                Array<const foundation::fonts::Font*> extras;
                for (const Guid& id : m_settings.uiFontIds)
                {
                    auto extra = Resources()->Bind<foundation::fonts::Font>(id);
                    if (extra)
                    {
                        extras.PushBack(extra.Get());
                    }
                    else
                    {
                        LOG_WARNING(u8"Player", u8"UI font {} did not resolve", id);
                    }
                }
                UI()->SetExtraFonts(Span<const foundation::fonts::Font* const>(extras.Data(), extras.Size()));
            }

            // The project's default UI theme: cooked UITheme -> the game context's
            // stylesheet (nil/unresolved = the built-in GameTheme stays).
            if (UI() != nullptr && !m_settings.defaultUiThemeId.IsNil())
            {
                auto themeProxy =
                    Resources()->Bind<foundation::ui::UITheme>(m_settings.defaultUiThemeId);
                if (themeProxy)
                {
                    UI()->SetDefaultTheme(themeProxy.Get());
                    LOG_INFO(u8"Player", u8"default UI theme bound");
                }
                else
                {
                    LOG_WARNING(u8"Player", u8"default UI theme did not resolve");
                }
            }

            // The resolution the game draws at, fitted into whatever window it got.
            SetRenderResolution(m_settings.renderWidth, m_settings.renderHeight, m_settings.renderFit);

            // Game script launches FIRST (task #123 boot reorder): the orchestrator's launch()/
            // update(dt) run from frame 1, scene or none, with the engine-service bindings above
            // already in place. A game whose launch() needs a scene waits `while (!Game.sceneReady())`.
            LoadAndStartGameScript();

            // Then, if a startup scene resolved, load it ASYNC behind a boot splash (task #123
            // step 4): push the splash overlay first (sync - a small resident document / the engine's
            // default), kick the async load, and let OnUpdate drive the splash + activate the
            // scene on completion. A NAMED-but-broken scene is still fatal.
            if (instance != nullptr)
            {
                m_bootScenePath = String(instance->Path());
                m_splashView = PushSplash();
                foundation::content::ContentDatabase* sceneDb = m_sceneDb;
                m_bootLoad = Instance().LoadSceneAsync(
                    *instance, *Resources(),
                    Function<UniquePtr<IStream>(const Guid&)>{
                        [sceneDb](const Guid& prefabId) -> UniquePtr<IStream>
                        {
                            foundation::content::Instance* prefab =
                                (sceneDb != nullptr) ? sceneDb->GetInstance(prefabId) : nullptr;
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : UniquePtr<IStream>{};
                        }});
                if (m_bootLoad.Failed())
                {
                    PopSplash();
                    LOG_ERROR(u8"Player", u8"scene '{}' failed to load", m_bootScenePath);
                    host.RequestExit(1);
                    return;
                }
                m_booting = true;
                DriveSplash(0.0f); // paint the initial state before the first pump
            }
            else
            {
                LOG_INFO(u8"Player", u8"no startup scene - the game script owns boot");
            }
        }

        [[nodiscard]] String UserSettingsFileName() const
        {
            String name =
                m_settings.name.IsEmpty() ? String(u8"project") : String(m_settings.name.AsView());
            name.Append(u8".user.settings.xml");
            return name;
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host,
                                                  deltaTime); // pumps resources + ticks the game script
            if (m_booting)
            {
                DriveBoot(host); // advance the splash + activate the scene when the load completes
            }
            if (m_options.exitAfterSeconds > 0.0f)
            {
                m_elapsed += deltaTime;
                if (m_elapsed >= m_options.exitAfterSeconds)
                {
                    host.RequestExit(0);
                }
            }
        }

        void OnExit(runtime::IApplicationHost&) override
        {
            StopGameScript();
            SetPrimaryScene(nullptr);
            if (m_scene != nullptr)
            {
                m_scene->Stop();
            }
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            // Native game teardown FIRST: OnUnload deregisters the plugin's subsystems
            // while Ctx() and the engine are still alive (game-native-code.md N2).
            if (m_plugins.Get() != nullptr)
            {
                m_plugins->UnloadAll();
                m_plugins = nullptr;
            }

            // Persist the user's mixer state (see the startup load).
            if (Audio() != nullptr && Audio()->Engine() != nullptr)
            {
                foundation::settings::Settings store(AppRoot());
                engine::audio::CaptureAudioUserSettings(
                    *Audio()->Engine(), store.Section<engine::audio::AudioUserSettings>());
                const String dir = foundation::core::GetUserDataDirectory();
                (void)foundation::core::CreateDirectory(dir.AsView());
                foundation::vfs::NativeFileSystem userFs(dir.AsView(), AppRoot());
                MemoryStream buffer;
                if (store.Save(buffer, foundation::xml::XmlSerializerFactory()).IsOk())
                {
                    (void)userFs.AsWritable()->Save(UserSettingsFileName().AsView(),
                                                    buffer.Bytes());
                    foundation::core::PersistUserData();
                }
            }
            engine::runtime::DefaultApplication::OnShutdown(host); // releases products device-alive
        }

    private:
        // A renderable scene needs a primary camera; authored game scenes should carry one, but
        // a bare editor scene shouldn't ship a black screen - frame the origin like the editor does.
        void EnsureCamera() { EnsureCameraOn(m_scene); }

        // Seed a default camera on `scene` if it has none, so a level renders. Scene-agnostic (the
        // boot path passes m_scene; a script-loaded level (Game.loadSceneAsync) passes the freshly
        // activated scene via ApplyLoadedSceneActivation below).
        void EnsureCameraOn(scene::Scene* scene)
        {
            if (scene == nullptr)
            {
                return;
            }
            auto* cameras = scene->GetSystem<engine::render::CameraComponentManager>();
            if (cameras == nullptr)
            {
                return;
            }
            bool hasCamera = false;
            cameras->ForEach([&](engine::render::CameraComponent&, scene::EntityHandle)
                             { hasCamera = true; });
            if (hasCamera)
            {
                return;
            }

            LOG_WARNING(u8"Player", u8"scene has no camera - adding a default one");
            const scene::EntityHandle e = scene->CreateEntity(u8"PlayerCamera");
            Transform t;
            t.position = Float3{8.0f, 6.0f, 10.0f};
            // Yaw toward the origin, then pitch down (same convention as the seeded Sun).
            t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.675f) *
                         Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.42f);
            scene->SetLocalTransform(e, t);
            cameras->Add(e);
        }

        // A script-loaded level (Game.loadSceneAsync/loadScene) gets the player's full activation:
        // a default camera so it renders, then the base Start + SetSimulationEnabled. The load's
        // scene is the run's current one from here: the instance destroys the scene it lands over,
        // so the boot scene the player held is gone (and stopping it at exit would read freed memory).
        void ApplyLoadedSceneActivation(scene::Scene* scene) override
        {
            EnsureCameraOn(scene);
            DefaultApplication::ApplyLoadedSceneActivation(scene);
            m_scene = scene;
        }

        // Resolves the startup-script SOURCE (project file / pak entry); the lifecycle
        // (facades, services, launch/update/exit) lives in DefaultApplication.
        void LoadAndStartGameScript()
        {
            // The startup game script is a cooked ScriptClass asset, bound from the content DB by guid.
            const Guid scriptId = m_settings.startupScriptId;
            if (scriptId.IsNil() || Resources() == nullptr)
            {
                return;
            }
            auto proxy = Resources()->Bind<foundation::script::ScriptClass>(scriptId);
            if (!proxy || proxy->source.IsEmpty())
            {
                LOG_ERROR(u8"Player", u8"startup script asset not found");
                return;
            }
            // The game's save lives beside the user's settings, read before the game launches.
            Instance().SetSaveFile(
                PathJoin(foundation::core::GetUserDataDirectory().AsView(),
                         project::SaveFileName(m_settings.name.AsView(), AppRoot()).AsView(), AppRoot())
                    .AsView());
            (void)StartGameScript(*proxy); // with its handlers: the game's on<Event> inbox
        }

        // ---- boot splash (task #123 step 4): a SCREEN overlay shown while the default scene streams
        // async. loadingDocumentId names a cooked UIDocument; nil = the engine's default loading
        // screen (engine::ui::BuildDefaultLoadingScreen: its logo, a bar). Conventional control ids
        // driven each frame: `progress` (ProgressBar). ----

        void DriveBoot(runtime::IApplicationHost& host)
        {
            DriveSplash(m_bootLoad.Progress());
            if (!m_bootLoad.IsComplete())
            {
                return;
            }
            m_booting = false;
            scene::Scene* activated = Instance().ActivateLoadedScene(m_bootLoad);
            PopSplash();
            if (activated == nullptr)
            {
                LOG_ERROR(u8"Player", u8"scene '{}' failed to activate", m_bootScenePath);
                host.RequestExit(1);
                return;
            }
            m_scene = activated;
            EnsureCamera();
            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            SetPrimaryScene(m_scene);
            LOG_INFO(u8"Player", u8"running scene '{}'", m_bootScenePath);
        }

        [[nodiscard]] RefPtr<foundation::ui::View> PushSplash()
        {
            if (UI() == nullptr)
            {
                return {};
            }
            RefPtr<foundation::ui::UIDocument> doc;
            if (!m_settings.loadingDocumentId.IsNil() && Resources() != nullptr)
            {
                auto proxy =
                    Resources()->Bind<foundation::ui::UIDocument>(m_settings.loadingDocumentId);
                if (proxy.Get() != nullptr)
                {
                    doc = RefPtr<foundation::ui::UIDocument>(proxy.Get());
                }
            }
            if (doc)
            {
                return UI()->PushScreenOverlay(*doc);
            }
            // None authored: the engine's own, with its logo.
            RefPtr<foundation::ui::View> view = engine::ui::BuildDefaultLoadingScreen(
                AppRoot(), StringView(reinterpret_cast<const utf8char*>(g_embeddedEngineLogo),
                                      static_cast<usize>(g_embeddedEngineLogoSize)));
            UI()->PushScreenOverlay(view);
            return view;
        }

        void PopSplash()
        {
            if (m_splashView && UI() != nullptr)
            {
                UI()->RemoveScreenOverlay(m_splashView.Get());
            }
            m_splashView = nullptr;
        }

        void DriveSplash(f32 progress)
        {
            foundation::ui::ViewGroup* root = Cast<foundation::ui::ViewGroup>(m_splashView.Get());
            if (root == nullptr)
            {
                return;
            }
            if (auto* bar = root->FindByName<foundation::ui::ProgressBar>(u8"progress"))
            {
                bar->Value.SetValue(progress);
            }
        }

        PlayerOptions m_options;
        f32 m_elapsed = 0.0f;
        engine::project::ProjectSettings m_settings;
        UniquePtr<runtime::PluginHost> m_plugins; // the native game (static or dev-loaded)
        UniquePtr<foundation::vfs::NativeFileSystem> m_root;
        UniquePtr<foundation::vfs::PakFileSystem> m_pak;             // dist mode only
        UniquePtr<foundation::vfs::NativeFileSystem> m_contentMount; // project mode only
        UniquePtr<foundation::vfs::NativeFileSystem> m_cookedMount;
        UniquePtr<foundation::content::ContentDatabase> m_sourceDb;  // project mode: authored scenes
        UniquePtr<foundation::content::ContentDatabase> m_contentDb; // products (and dist scenes)
        foundation::content::ContentDatabase* m_sceneDb = nullptr;   // where scenes come from
        scene::Scene* m_scene = nullptr;                           // owned by the SceneSubsystem

        // Boot splash + async default-scene load (task #123 step 4).
        engine::runtime::SceneLoadHandle m_bootLoad;     // the in-flight async boot load
        RefPtr<foundation::ui::View> m_splashView; // the pushed splash overlay (null = none)
        String m_bootScenePath;
        bool m_booting = false;
    };
}

#endif // ENGINE_PLAYER_PLAYERAPPLICATION_H

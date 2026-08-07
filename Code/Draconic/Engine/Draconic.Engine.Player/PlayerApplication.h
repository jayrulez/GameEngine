// PlayerApplication - the generic game runner's IApplication, shared by the desktop and web
// entry points (Main.cpp / WebMain.cpp). Runs a project with ZERO native game code: engine
// subsystems + the project's content + the default scene, and the project's game SCRIPT (a
// scripted IApplication counterpart - resolved by the script's LANGUAGE, so Wren or AngelScript
// both work). See Main.cpp's header comment for the two layouts (PROJECT vs DIST).
//
// Classic header (the DRACONIC_APP_MAIN pattern): it carries no `import` of its own and uses names
// the INCLUDING TU must bring in first. Include it AFTER these imports:
//   Draconic.Foundation, draconic.vfs, draconic.vfs.pak, draconic.content, draconic.resource,
//   draconic.runtime, draconic.runtime.client, draconic.engine.defaultapp,
//   draconic.scene, draconic.engine.scene, draconic.scene.resource,
//   draconic.render, draconic.script, draconic.script.resource,
//   draconic.input, draconic.audio, draconic.audio.resource, draconic.ui.resource,
//   draconic.engine.ui, draconic.settings, draconic.project, draconic.xml.serialization
// The two entry points add only the PLATFORM trio (shell + runner + graphics) on top.

#ifndef DRACONIC_TOOLS_PLAYER_PLAYERAPPLICATION_H
#define DRACONIC_TOOLS_PLAYER_PLAYERAPPLICATION_H

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

namespace draconic::player
{
    using namespace draconic::foundation;
    namespace runtime = draconic::runtime;
    namespace scene = draconic::scene;
    namespace resource = draconic::resource;

    struct PlayerOptions
    {
        String projectDir;
        String sceneOverride; // source-DB path; empty = the manifest's defaultScene
        f32 exitAfterSeconds = 0.0f;
    };

    class PlayerApplication : public runtime::DefaultApplication
    {
    public:
        explicit PlayerApplication(PlayerOptions options) : m_options(Move(options)) {}

        void OnStartup(runtime::IApplicationHost& host) override
        {
            namespace project = draconic::project;
            m_root = MakeUnique<draconic::vfs::NativeFileSystem>(DefaultAllocator(),
                                                                 m_options.projectDir.AsView());

            // Dist layout wins when present (a staged dist can sit inside a project tree).
            if (m_root->Exists(project::kDistContentPak))
            {
                m_pak = MakeUnique<draconic::vfs::PakFileSystem>(
                    DefaultAllocator(),
                    PathJoin(m_options.projectDir.AsView(), project::kDistContentPak).AsView());
                if (!m_pak->IsValid() ||
                    !project::LoadProjectSettings(*m_root, m_settings, project::kDistManifestFile)
                         .IsOk())
                {
                    DRACONIC_LOG_ERROR(u8"Player", u8"dist at '{}' is unreadable",
                                       m_options.projectDir);
                    host.RequestExit(1);
                    return;
                }
                m_contentDb = MakeUnique<draconic::content::ContentDatabase>(
                    DefaultAllocator(), *m_pak, BinarySerializerFactory(),
                    project::kCookedAssetExtension);
                m_sceneDb = m_contentDb.Get(); // scenes live IN the pak, binary like products
                DRACONIC_LOG_INFO(u8"Player", u8"dist mode ({} pak entries)", m_pak->EntryCount());
            }
            else if (m_root->Exists(project::kProjectManifestFile))
            {
                if (!project::LoadProjectSettings(*m_root, m_settings).IsOk())
                {
                    DRACONIC_LOG_ERROR(u8"Player", u8"project manifest at '{}' is unreadable",
                                       m_options.projectDir);
                    host.RequestExit(1);
                    return;
                }
                m_contentMount = MakeUnique<draconic::vfs::NativeFileSystem>(
                    DefaultAllocator(),
                    PathJoin(m_options.projectDir.AsView(), project::kProjectContentDir).AsView());
                m_cookedMount = MakeUnique<draconic::vfs::NativeFileSystem>(
                    DefaultAllocator(),
                    PathJoin(m_options.projectDir.AsView(), project::kProjectCookedDir).AsView());
                m_sourceDb = MakeUnique<draconic::content::ContentDatabase>(
                    DefaultAllocator(), *m_contentMount, draconic::xml::XmlSerializerFactory(),
                    project::kSourceAssetExtension);
                m_contentDb = MakeUnique<draconic::content::ContentDatabase>(
                    DefaultAllocator(), *m_cookedMount, BinarySerializerFactory(),
                    project::kCookedAssetExtension);
                m_sceneDb = m_sourceDb.Get(); // authored scenes; products from the cooked DB
            }
            else
            {
                DRACONIC_LOG_ERROR(
                    u8"Player",
                    u8"'{}' is neither a project (Project.xml) nor a dist (Content.pak)",
                    m_options.projectDir);
                host.RequestExit(1);
                return;
            }

            // The app builds the resource manager + standard factories over this DB
            // (product types registered there too - runtime-host.md v3 infra preset).
            SetContentDatabase(m_contentDb.Get());
            runtime::DefaultApplication::OnStartup(host);

            // Game-UI IME lifecycle: the player owns its window, so the UI subsystem
            // drives StartTextInput/StopTextInput on it as game EditText focus moves.
            // (The editor leaves this null - its UIHost bridge owns the IME there.)
            if (UI() != nullptr && host.Shell() != nullptr)
            {
                UI()->SetTextInputTarget(host.Shell()->MainWindow());
            }
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            if (m_sceneDb == nullptr)
            {
                return;
            }
            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }

            // Resolution order: --scene path override, the manifest's guid (authoritative,
            // rename-proof), then the path mirror (v2 manifests).
            draconic::content::Instance* instance = nullptr;
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
                DRACONIC_LOG_ERROR(u8"Player", u8"--scene '{}' did not resolve",
                                   m_options.sceneOverride.AsView());
                host.RequestExit(1);
                return;
            }

            // The project's default input map: cooked resource -> the PRIMARY instance's ActionRuntime
            // (per-instance input; the game reads its own runtime). Nil/unresolved = no actions bound.
            if (Input() != nullptr && !m_settings.defaultInputMapId.IsNil())
            {
                auto mapProxy = Resources()->Bind<draconic::input::InputMapResource>(
                    m_settings.defaultInputMapId);
                if (mapProxy)
                {
                    Instance().SetInputMap(mapProxy->Map());
                    DRACONIC_LOG_INFO(u8"Player", u8"input map bound ({} set(s))",
                                      mapProxy->Map().sets.Size());
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Player", u8"default input map did not resolve");
                }
            }

            // The project's default audio bus layout: cooked mixer data -> the engine.
            // Nil/unresolved = the built-in neutral four-bus layout.
            if (Audio() != nullptr && Audio()->Engine() != nullptr &&
                !m_settings.defaultBusLayoutId.IsNil())
            {
                auto layoutProxy = Resources()->Bind<draconic::audio::AudioBusLayoutResource>(
                    m_settings.defaultBusLayoutId);
                if (layoutProxy)
                {
                    Audio()->Engine()->ApplyBusLayout(layoutProxy->layout);
                    DRACONIC_LOG_INFO(u8"Player", u8"audio bus layout applied");
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Player", u8"default bus layout did not resolve");
                }
            }

            // Per-user audio volumes: <userdata>/<project>.user.settings.xml, applied
            // ON TOP of the layout (the user's slider is absolute - options-menu law).
            // Absent on first run; captured back at shutdown so script-driven changes
            // (Audio.setBusVolume) persist for free.
            if (Audio() != nullptr && Audio()->Engine() != nullptr)
            {
                draconic::audio::RegisterAudioSettingsTypes();
                draconic::vfs::NativeFileSystem userFs(
                    draconic::foundation::GetUserDataDirectory(u8"draconic").AsView());
                UniquePtr<IStream> stream =
                    userFs.Open(UserSettingsFileName().AsView(), FileMode::Read);
                if (stream)
                {
                    draconic::settings::Settings store;
                    if (store.Load(*stream, draconic::xml::XmlSerializerFactory()).IsOk())
                    {
                        if (const auto* audio = store.Find<draconic::audio::AudioUserSettings>())
                        {
                            draconic::audio::ApplyAudioUserSettings(*Audio()->Engine(), *audio);
                            DRACONIC_LOG_INFO(u8"Player", u8"user audio settings applied");
                        }
                    }
                }
            }

            // The project's default UI font: cooked FontResource -> the game context's
            // font service (nil/unresolved = the dev TTF fallback, which does not exist
            // in a dist - a shipped game NEEDS this binding for any text at all).
            if (UI() != nullptr && !m_settings.defaultUiFontId.IsNil())
            {
                auto fontProxy =
                    Resources()->Bind<draconic::fonts::Font>(m_settings.defaultUiFontId);
                if (fontProxy)
                {
                    UI()->SetDefaultFont(fontProxy.Get());
                    DRACONIC_LOG_INFO(u8"Player", u8"default UI font bound");
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Player", u8"default UI font did not resolve");
                }
            }

            // The project's default UI theme: cooked UITheme -> the game context's
            // stylesheet (nil/unresolved = the built-in GameTheme stays).
            if (UI() != nullptr && !m_settings.defaultUiThemeId.IsNil())
            {
                auto themeProxy =
                    Resources()->Bind<draconic::ui::UITheme>(m_settings.defaultUiThemeId);
                if (themeProxy)
                {
                    UI()->SetDefaultTheme(themeProxy.Get());
                    DRACONIC_LOG_INFO(u8"Player", u8"default UI theme bound");
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Player", u8"default UI theme did not resolve");
                }
            }

            // Game script launches FIRST (task #123 boot reorder): the orchestrator's launch()/
            // update(dt) run from frame 1, scene or none, with the engine-service bindings above
            // already in place. A game whose launch() needs a scene waits `while (!Game.sceneReady())`.
            LoadAndStartGameScript();

            // Then, if a startup scene resolved, load it ASYNC behind a boot splash (task #123
            // step 4): push the splash overlay first (sync - a small resident document / the built-
            // in default), kick the async load, and let OnUpdate drive the splash + activate the
            // scene on completion. A NAMED-but-broken scene is still fatal.
            if (instance != nullptr)
            {
                m_bootScenePath = String(instance->Path());
                m_splashView = PushSplash();
                draconic::content::ContentDatabase* sceneDb = m_sceneDb;
                m_bootLoad = Instance().LoadSceneAsync(
                    *instance, *Resources(),
                    Function<UniquePtr<IStream>(const Guid&)>{
                        [sceneDb](const Guid& prefabId) -> UniquePtr<IStream>
                        {
                            draconic::content::Instance* prefab =
                                (sceneDb != nullptr) ? sceneDb->GetInstance(prefabId) : nullptr;
                            return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                       : UniquePtr<IStream>{};
                        }});
                if (m_bootLoad.Failed())
                {
                    PopSplash();
                    DRACONIC_LOG_ERROR(u8"Player", u8"scene '{}' failed to load", m_bootScenePath);
                    host.RequestExit(1);
                    return;
                }
                m_booting = true;
                DriveSplash(0.0f); // paint the initial state before the first pump
            }
            else
            {
                DRACONIC_LOG_INFO(u8"Player", u8"no startup scene - the game script owns boot");
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
            runtime::DefaultApplication::OnUpdate(host,
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
            // Persist the user's mixer state (see the startup load).
            if (Audio() != nullptr && Audio()->Engine() != nullptr)
            {
                draconic::settings::Settings store;
                draconic::audio::CaptureAudioUserSettings(
                    *Audio()->Engine(), store.Section<draconic::audio::AudioUserSettings>());
                const String dir = draconic::foundation::GetUserDataDirectory(u8"draconic");
                (void)draconic::foundation::CreateDirectory(dir.AsView());
                draconic::vfs::NativeFileSystem userFs(dir.AsView());
                MemoryStream buffer;
                if (store.Save(buffer, draconic::xml::XmlSerializerFactory()).IsOk())
                {
                    (void)userFs.AsWritable()->Save(UserSettingsFileName().AsView(),
                                                    buffer.Bytes());
                }
            }
            runtime::DefaultApplication::OnShutdown(host); // releases products device-alive
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
            auto* cameras = scene->GetSystem<draconic::render::CameraComponentManager>();
            if (cameras == nullptr)
            {
                return;
            }
            bool hasCamera = false;
            cameras->ForEach([&](draconic::render::CameraComponent&, scene::EntityHandle)
                             { hasCamera = true; });
            if (hasCamera)
            {
                return;
            }

            DRACONIC_LOG_WARNING(u8"Player", u8"scene has no camera - adding a default one");
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
        // a default camera so it renders, then the base Start + SetSimulationEnabled.
        void ApplyLoadedSceneActivation(scene::Scene* scene) override
        {
            EnsureCameraOn(scene);
            DefaultApplication::ApplyLoadedSceneActivation(scene);
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
            auto proxy = Resources()->Bind<draconic::script::ScriptClass>(scriptId);
            if (!proxy || proxy->source.IsEmpty())
            {
                DRACONIC_LOG_ERROR(u8"Player", u8"startup script asset not found");
                return;
            }
            (void)StartGameScript(proxy->source.AsView(), proxy->sourceName.AsView());
        }

        // ---- boot splash (task #123 step 4): a SCREEN overlay shown while the default scene streams
        // async. loadingDocumentId names a cooked UIDocument; nil = the built-in default. Conventional
        // control ids driven each frame: `progress` (ProgressBar). ----

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
                DRACONIC_LOG_ERROR(u8"Player", u8"scene '{}' failed to activate", m_bootScenePath);
                host.RequestExit(1);
                return;
            }
            m_scene = activated;
            EnsureCamera();
            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            SetPrimaryScene(m_scene);
            DRACONIC_LOG_INFO(u8"Player", u8"running scene '{}'", m_bootScenePath);
        }

        [[nodiscard]] RefPtr<draconic::ui::View> PushSplash()
        {
            if (UI() == nullptr)
            {
                return {};
            }
            RefPtr<draconic::ui::UIDocument> doc;
            if (!m_settings.loadingDocumentId.IsNil() && Resources() != nullptr)
            {
                auto proxy =
                    Resources()->Bind<draconic::ui::UIDocument>(m_settings.loadingDocumentId);
                if (proxy.Get() != nullptr)
                {
                    doc = RefPtr<draconic::ui::UIDocument>(proxy.Get());
                }
            }
            if (!doc)
            {
                doc = DefaultSplashDocument();
            }
            return UI()->PushScreenOverlay(*doc);
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
            draconic::ui::ViewGroup* root = Cast<draconic::ui::ViewGroup>(m_splashView.Get());
            if (root == nullptr)
            {
                return;
            }
            if (auto* bar = root->FindByName<draconic::ui::ProgressBar>(u8"progress"))
            {
                bar->Value.SetValue(progress);
            }
        }

        // The built-in default splash (bare-bones - a status label + progress bar). A shipped game
        // authors its own UIDocument and sets loadingDocumentId; this just proves the flow works with
        // zero authoring. The `status`/`progress` ids are the app<->document contract.
        [[nodiscard]] static RefPtr<draconic::ui::UIDocument> DefaultSplashDocument()
        {
            RefPtr<draconic::ui::UIDocument> doc =
                MakeRef<draconic::ui::UIDocument>(DefaultAllocator());
            doc->markup = String(u8"<FlexLayout>"
                                 u8"<Label id=\"status\" text=\"Loading...\"/>"
                                 u8"<ProgressBar id=\"progress\"/>"
                                 u8"</FlexLayout>");
            return doc;
        }

        PlayerOptions m_options;
        f32 m_elapsed = 0.0f;
        draconic::project::ProjectSettings m_settings;
        UniquePtr<draconic::vfs::NativeFileSystem> m_root;
        UniquePtr<draconic::vfs::PakFileSystem> m_pak;             // dist mode only
        UniquePtr<draconic::vfs::NativeFileSystem> m_contentMount; // project mode only
        UniquePtr<draconic::vfs::NativeFileSystem> m_cookedMount;
        UniquePtr<draconic::content::ContentDatabase> m_sourceDb;  // project mode: authored scenes
        UniquePtr<draconic::content::ContentDatabase> m_contentDb; // products (and dist scenes)
        draconic::content::ContentDatabase* m_sceneDb = nullptr;   // where scenes come from
        scene::Scene* m_scene = nullptr;                           // owned by the SceneSubsystem

        // Boot splash + async default-scene load (task #123 step 4).
        runtime::SceneLoadHandle m_bootLoad;     // the in-flight async boot load
        RefPtr<draconic::ui::View> m_splashView; // the pushed splash overlay (null = none)
        String m_bootScenePath;
        bool m_booting = false;
    };
}

#endif // DRACONIC_TOOLS_PLAYER_PLAYERAPPLICATION_H

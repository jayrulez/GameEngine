// Draconic::EditorScene - :page partition.
//
// SceneEditorPage: the scene document editor (design doc §3.6, phase 2). Each page owns its OWN
// live Scene (multi-scene rule - several pages open at once; everything scene-scoped is
// per-page): a ViewportView renders the scene through the REAL renderer via CameraOverride into
// the viewport's offscreen color target, an EditorCamera flies on the viewport's gated devices
// (hover/focus-gated, so occluded/inactive-tab input can't leak), and open/save round-trip the
// content DB through LoadScene/SaveScene. An empty scene shows a debug-draw ground grid + origin
// axes so navigation reads immediately.
//
// RegisterSceneEditor is the module's RegisterEditor entry point (§3.1): the EXECUTABLE calls it
// (editor core/app never link this module); it registers the SceneDocument type, the page
// factory, and the "Scene" asset creator (File > New Scene).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.scene:page;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.engine.defaultapp;
import draconic.engine.gameinstance; // GameInstance (the Game tab's run; multi-instance factory)
import draconic.scene;
import draconic.engine.scene;
import draconic.scene.resource;
import draconic.scene.editor;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.engine.ui; // game-UI RenderTexture canvases (live in editing viewports)
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;
import :edit;
import :model_prefab;
import :game_page;
import :gizmo;
import :component_gizmos;
import :hierarchy;
import :inspector;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace vg = draconic::vg;
    namespace scene = draconic::scene;
    namespace render = draconic::render;

    class SceneEditorPage final : public app::UIEditorPage
    {
    public:
        SceneEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                        ui::runtime::UIHost& uiHost, draconic::content::Instance& instance)
            : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
        {
            m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();
            m_gameUI = host.Ctx().GetSubsystem<draconic::ui::UISubsystem>();

            // Own live Scene per page in this page's OWN SceneManager (registered with the subsystem
            // so it ticks on the Context lane; there is no shared default manager).
            if (m_scenes != nullptr)
            {
                m_sceneManager.SetAwareRegistry(&m_scenes->AwareRegistry());
                m_scenes->RegisterManager(&m_sceneManager);
                m_scene = m_sceneManager.CreateScene(instance.Name());
                m_scene->SetSimulationEnabled(false); // edit mode is frozen; Simulate un-freezes
                const Status loaded = scene::LoadScene(instance, *m_scene);
                if (loaded.IsOk())
                {
                    // Bind the scene's resource refs to cooked products (no-op refs stay null;
                    // a later cook + reopen picks them up - live hot reload is the 6d pass).
                    if (context.Resources() != nullptr)
                    {
                        scene::ResolveSceneResources(*m_scene, *context.Resources());
                    }
                    // Prefab instances load as ref+deltas - respawn them from the SOURCE DB
                    // (payloads are edited assets, not cooked products), then bind the
                    // spawned components' refs too.
                    if (m_scene->PendingPrefabInstanceCount() > 0 && context.Project() != nullptr)
                    {
                        EditorContext* editorContext = &context;
                        scene::ResolveScenePrefabs(
                            *m_scene,
                            Function<UniquePtr<IStream>(const Guid&)>{
                                [editorContext](const Guid& prefabId) -> UniquePtr<IStream>
                                {
                                    draconic::content::Instance* prefab =
                                        editorContext->Project()->SourceDb().GetInstance(prefabId);
                                    return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                               : UniquePtr<IStream>{};
                                }});
                        if (context.Resources() != nullptr)
                        {
                            scene::ResolveSceneResources(*m_scene, *context.Resources());
                        }
                    }
                    DRACONIC_LOG_INFO(u8"Editor", u8"opened scene '{}'", m_title);
                }
                else if (loaded.Code() == ErrorCode::NotFound)
                {
                    DRACONIC_LOG_INFO(u8"Editor", u8"new scene '{}' (no scene stream yet)",
                                      m_title);
                }
                else
                {
                    DRACONIC_LOG_ERROR(u8"Editor", u8"scene '{}' failed to load", m_title);
                }
            }

            // No OnRender/RenderContent: the frame graph renders the scene into the color target
            // and manages its transitions via TargetState (ColorState()/SetColorState tracking),
            // inside the app's single per-frame bracket.
            m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
            m_viewport->ClearColor = rhi::ClearColor{0.10f, 0.11f, 0.13f, 1.0f};

            // Everything scene-scoped is PER PAGE (multi-scene): mutation mediator (all edits
            // are commands on THIS page's stack), selection, hierarchy + inspector views.
            if (m_scene != nullptr)
            {
                m_editContext =
                    MakeUnique<SceneEditContext>(DefaultAllocator(), *m_scene, Commands());
                m_editContext->SetResources(context.Resources());
                EditorContext* resolverContext = &context;
                m_editContext->SetPrefabResolver(scene::PrefabPayloadResolver{
                    [resolverContext](const Guid& prefabId) -> UniquePtr<IStream>
                    {
                        if (resolverContext->Project() == nullptr)
                        {
                            return UniquePtr<IStream>{};
                        }
                        draconic::content::Instance* prefab =
                            resolverContext->Project()->SourceDb().GetInstance(prefabId);
                        return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                   : UniquePtr<IStream>{};
                    }});
                m_hierarchy = MakeRef<SceneHierarchyView>(DefaultAllocator(), *m_editContext);
                m_hierarchy->SetEditorContext(&context);
                {
                    SceneEditorPage* page = this;
                    m_hierarchy->OnCreatePrefab = [page](const Guid& entity)
                    { page->CreatePrefabFromEntity(entity); };
                    m_hierarchy->OnSpawnPrefab = [page](const Guid& parent)
                    { page->PickAndSpawnPrefab(parent); };
                    m_hierarchy->OnApplyPrefab = [page](const Guid& root)
                    { page->ApplyInstanceToPrefab(root); };
                    m_hierarchy->OnRevertPrefab = [page](const Guid& root)
                    { page->RevertInstance(root); };
                }
                m_inspector =
                    MakeRef<SceneInspectorView>(DefaultAllocator(), context, *m_editContext);
                m_gizmos = MakeUnique<GizmoController>(DefaultAllocator(), *m_editContext);
                RegisterBuiltinGizmoRenderers(m_componentGizmos);
            }

            // Viewport pane: [toolbar strip | viewport]. The toolbar mirrors and drives the
            // gizmo state the W/E/R/X keys already control - the on-screen answer to "which
            // space am I in" (the recorded gap: X toggled with no visible state anywhere).
            BuildViewportToolbar();
            auto viewportPane = MakeRef<draconic::ui::FlexLayout>(DefaultAllocator());
            viewportPane->Direction = draconic::ui::Orientation::Vertical;
            {
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = draconic::ui::SizeSpec::Match();
                lp->Height = draconic::ui::SizeSpec::Fixed(draconic::ui::Unit::Px(30));
                viewportPane->AddView(m_toolbar.Get(), lp);
            }
            {
                // Wrap the viewport in a FrameLayout so the camera preview (task #118) overlays it
                // in the bottom-right corner. The viewport fills the frame; the preview floats over.
                BuildCameraPreview();
                auto viewportFrame = MakeRef<draconic::ui::FrameLayout>(DefaultAllocator());
                {
                    auto vfp = MakeRef<draconic::ui::FrameLayoutParams>(DefaultAllocator());
                    vfp->Gravity = draconic::ui::Gravity::Fill;
                    viewportFrame->AddView(m_viewport.Get(), vfp);
                }
                {
                    auto pfp = MakeRef<draconic::ui::FrameLayoutParams>(DefaultAllocator());
                    pfp->Gravity = static_cast<draconic::ui::Gravity>(
                        static_cast<u32>(draconic::ui::Gravity::Bottom) |
                        static_cast<u32>(draconic::ui::Gravity::Right));
                    pfp->Margin = draconic::ui::Thickness{12.0f, 12.0f, 12.0f, 12.0f};
                    viewportFrame->AddView(m_previewContainer.Get(), pfp);
                }
                auto lp = MakeRef<draconic::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = draconic::ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                viewportPane->AddView(viewportFrame.Get(), lp);
            }

            // Page layout: hierarchy | (viewport | inspector).
            auto inner = MakeRef<draconic::ui::toolkit::SplitView>(DefaultAllocator());
            inner->SetSplitRatio(0.72f);
            inner->SetPanes(viewportPane.Get(), m_inspector.Get());
            m_content = MakeRef<draconic::ui::toolkit::SplitView>(DefaultAllocator());
            m_content->SetSplitRatio(0.2f);
            m_content->SetPanes(m_hierarchy.Get(), inner.Get());

            m_router =
                MakeUnique<draconic::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());

            // Start framed on the origin (grid center), orbit pivot there, horizon level.
            m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});
        }

        // === UIEditorPage ===

        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;

        // Called only for the MAIN window's frame, inside the app-level scene-renderer bracket
        // (Sedulous structure): the offscreen target is window-agnostic, so this renders no
        // matter which OS window hosts the panel; that window's UI samples the result.
        void OnRenderWindow(runtime::IApplicationHost&,
                            draconic::graphics::FrameContext& frame) override;

        // Create-from-selection: capture the subtree as a prefab asset (under "Prefabs/",
        // named after the entity) and replace the original with an instance of it (one undo
        // group). The payload keeps the captured guids as its stable source ids.
        void CreatePrefabFromEntity(const Guid& entityId);

        // Apply-to-prefab entry point: rewrites the asset and rebuilds every instance, with
        // no undo - so it confirms first (mirror of RevertInstance).
        void ApplyInstanceToPrefab(const Guid& rootId);

        // Apply-to-prefab: the instance's CURRENT state becomes the template (source-id
        // keyed, so other instances' deltas stay valid), then every instance everywhere
        // rebuilds from it - including this one, which re-baselines to clean.
        void ApplyInstanceToPrefabNow(const Guid& rootId);

        // Revert-instance entry point: destructive + not undoable, so it confirms first.
        // Non-member children under the instance are destroyed too - the dialog says so.
        void RevertInstance(const Guid& rootId);

        // Revert-instance: discard this instance's deltas (respawn from the current template,
        // placement kept). Not undoable.
        void RevertInstanceNow(const Guid& rootId);

        // Spawn an instance under `parent` (nil = scene root) via the asset picker.
        void PickAndSpawnPrefab(const Guid& parent);

        // The asset changed under this page (apply-to-prefab from another page): wipe and
        // reload so the split-view prefab editor shows the new template. Unsaved edits are
        // never clobbered - the page just warns instead.
        void OnAssetExternallyModified() override;

        void OnSavedAs(draconic::content::Instance& instance) override;

        [[nodiscard]] Status Save() override;

        void OnClose() override;

        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }
        [[nodiscard]] EditorCamera& Camera() noexcept { return m_camera; }
        [[nodiscard]] SceneEditContext* EditContext() const noexcept { return m_editContext.Get(); }

    private:
        static constexpr f32 kFovY = 1.0472f; // must match OnRenderWindow's projection

        // Camera ray through the mouse position, built from the camera basis (no matrix inverse).
        [[nodiscard]] bool MakeMouseRay(GizmoRay& out) const;

        // Feed the gizmo controller a frame of viewport input. Runs EVERY frame so the gizmo
        // pose tracks tree selections and undo/redo even while the mouse is elsewhere; when the
        // viewport isn't hovered/focused the pointer is flagged invalid (pose-sync only). The
        // camera owns the mouse while Alt (orbit) or RMB (fly) is down, so gizmo buttons are
        // masked then. Returns true when the gizmo consumed the mouse (hot handle or active
        // drag) - click-picking must skip.
        [[nodiscard]] bool UpdateGizmos(bool viewportActive);

        void DrawGizmos(render::debug::DebugDraw& dd);

        // The viewport's ephemeral post "show flags" popup (checkable). Toggling a flag re-renders
        // this page's viewport with that effect stripped; the scene asset is never touched.
        void ShowPostFlagsMenu(draconic::ui::View* anchor);

        // === Viewport toolbar (gizmo mode/space/grid) ===

        void BuildViewportToolbar();

        // === Simulation lifecycle (Sedulous SceneEditorPage port) ===

        /// Snapshot the scene and flip it live: Scene::Start() fires OnSceneStarted on every
        /// system, SimulationEnabled un-freezes simulation-only work, and the command stack
        /// LOCKS (runtime mutations don't belong on the edit history; undoing into entities
        /// the restore recreates is a guid minefield). No-op if already simulating.
        ///
        /// Game UI stays NOT interactive during Simulate (deliberate): the scene's HUD
        /// renders in the viewport (WYSIWYG), but no per-surface scene binding is made
        /// (input-subsystem SetSourceProvider), so under the editor's ScreenTierOnly
        /// policy the canvases never take editor clicks/keys. Simulate is a physics/
        /// systems preview whose pointer must keep serving SELECTION and camera flight;
        /// the Game tab is the interactive-run surface and binds its scene on Play.
        void StartSimulation();

        /// Freeze/resume the running simulation (SimulationEnabled only - the Start/Stop
        /// system callbacks are for the big transitions, not the per-frame pause).
        void PauseSimulation(bool paused);

        /// Scene::Stop(), then restore the snapshot INTO THE SAME Scene instance (borrowed
        /// scene pointers stay valid; the guid-keyed selection re-resolves against restored
        /// entities - runtime-spawned ones drop out naturally). No-op if not simulating.
        void StopSimulation();

        void RefreshSimToolbar();

        // (split out so the lambda below can live next to its state)
        void ScenePage_GridToggleInit();

        // Reflect externally-driven state (the W/E/R keys, X space toggle) back into the
        // toolbar. SetIsChecked no-ops when unchanged, and the mode handlers only act on
        // true, so this settles without feedback loops.
        void SyncToolbar();

        // Position markers for every entity (small cross; selected = brighter + boxed) - empty
        // entities have no renderable, so the editor gives them a visual anchor. The selection
        // box hugs the entity's REAL renderable bounds when it has any (mesh AABB in the
        // entity's oriented frame; instanced sets use their merged world bounds); the small
        // fixed cube remains the meshless fallback.
        void DrawEntityMarkers(render::debug::DebugDraw& dd);

        // Click-to-select in the viewport: a camera ray through the clicked pixel against small
        // pick spheres at entity positions (CPU picking v1; component bounds and marquee later).
        // Left click only, and not while Alt-orbiting; Ctrl toggles; empty space clears.
        void PickOnClick();

        // Bind (and re-bind after dock/float moves) the viewport to the window that hosts it -
        // the UISandbox UpdateViewportHostWindow dance: RendererFor is only valid once the
        // window is attached, and a floated panel lives in a different OS window.
        void EnsureViewportBound();

        EditorContext* m_context;                  // borrowed
        runtime::IApplicationHost* m_host;         // borrowed
        ui::runtime::UIHost* m_uiHost;             // borrowed
        scene::SceneSubsystem* m_scenes = nullptr; // borrowed (context subsystem: registry + tick)
        scene::SceneManager
            m_sceneManager; // this page's OWN scene group (registered with m_scenes)
        render::RenderSubsystem* m_render = nullptr;
        draconic::ui::UISubsystem* m_gameUI = nullptr; // RT-canvas host seam (borrowed)

        String m_title;
        scene::Scene* m_scene = nullptr;           // owned by the SceneSubsystem
        UniquePtr<SceneEditContext> m_editContext; // per-page mutation mediator + selection
        RefPtr<draconic::ui::toolkit::SplitView> m_content; // hierarchy | viewport
        RefPtr<SceneHierarchyView> m_hierarchy;
        RefPtr<ui::toolkit::Toolbar> m_toolbar;
        render::ViewPostOverride
            m_postOverride; // ephemeral viewport post show-flags (not serialized)
        ui::toolkit::ToolbarButton* m_playButton = nullptr; // borrowed (toolbar-owned)
        ui::toolkit::ToolbarToggle* m_pauseToggle = nullptr;
        ui::toolkit::ToolbarButton* m_stopButton = nullptr;
        RefPtr<draconic::ui::Label> m_simLabel;
        UniquePtr<scene::SceneSnapshot> m_simSnapshot;
        bool m_isSimulating = false;
        bool m_isPaused = false;
        ui::toolkit::ToolbarToggle* m_translateToggle = nullptr; // borrowed (toolbar-owned)
        ui::toolkit::ToolbarToggle* m_rotateToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_scaleToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_spaceToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_gridToggle = nullptr;
        bool m_showGrid = true;
        RefPtr<SceneInspectorView> m_inspector;
        UniquePtr<GizmoController> m_gizmos;
        GizmoRendererRegistry m_componentGizmos;
        RefPtr<ui::viewport::ViewportView> m_viewport;

        // Camera preview (task #118): a small bottom-right overlay showing a selected/pinned camera's
        // live view. Editor-session state only - NEVER persisted into a runtime/wire struct.
        RefPtr<ui::viewport::ViewportView> m_previewViewport;
        RefPtr<draconic::ui::View> m_previewContainer; // the overlay (Visibility::Gone when idle)
        RefPtr<draconic::ui::Button> m_previewPin;
        scene::EntityHandle m_pinnedCamera;  // the PINNED camera entity (unassigned = not pinned)
        scene::EntityHandle m_previewTarget; // the camera previewed this frame (unassigned = none)
        u32 m_previewHeight = 180;           // panel + render-target height (from the target aspect)
        void BuildCameraPreview();
        void UpdateCameraPreview();
        void RenderCameraPreview();
        void ToggleCameraPin();
        [[nodiscard]] scene::EntityHandle SelectedCameraEntity() const;
        [[nodiscard]] bool IsLiveCamera(scene::EntityHandle entity) const;
        UniquePtr<draconic::shell::InputRouter> m_router;
        EditorCamera m_camera;
        draconic::graphics::RenderWindow* m_hostWindow =
            nullptr;                 // borrowed; tracks dock/float moves
        bool m_renderedOnce = false; // first-frame debug log
    };

    // === Factory + registration (the module's RegisterEditor entry point, §3.1) ===

    class SceneEditorPageFactory final : public IEditorPageFactory
    {
    public:
        SceneEditorPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override;

        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Prefab assets open on the SAME editor page - a prefab payload IS a scene stream (the
    // page's Save branches to SavePrefab + rebuilds open instances).
    class PrefabEditorPageFactory final : public IEditorPageFactory
    {
    public:
        PrefabEditorPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override;

        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Create a fresh empty prefab instance under "Prefabs/", named uniquely (Prefab,
    // Prefab2, ...). Content arrives when the user saves the opened page (an unsaved prefab
    // has no payload; spawning one warns).
    inline draconic::content::Instance*
    CreatePrefabInstance(EditorContext& context, draconic::content::Group* target = nullptr)
    {
        EditorProject* project = context.Project();
        if (project == nullptr)
        {
            return nullptr;
        }

        draconic::content::Group* prefabs = target;
        if (prefabs == nullptr)
        {
            draconic::content::Group* root = project->SourceDb().RootGroup();
            prefabs = root->GetGroup(u8"Prefabs");
            if (prefabs == nullptr)
            {
                prefabs = root->CreateGroup(u8"Prefabs");
            }
        }
        if (prefabs == nullptr)
        {
            return nullptr;
        }

        const String name = prefabs->UniqueInstanceName(u8"Prefab");

        draconic::content::Instance* instance =
            prefabs->CreateInstance(name.AsView(), scene::PrefabDocument::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        scene::PrefabDocument doc;
        doc.name = name;
        if (!instance->WriteObject(doc).IsOk())
        {
            return nullptr;
        }

        // Seed one root entity so the prefab opens in the enforced single-root shape and is
        // spawnable immediately (an empty payload can't spawn).
        scene::Scene seed(u8"seed");
        scene::EntityHandle root = seed.CreateEntity(name.AsView());
        MemoryStream buffer;
        if (scene::CapturePrefab(seed, root, buffer).IsOk())
        {
            (void)instance->WriteData(u8"scene", buffer.Bytes());
        }
        return instance;
    }

    // Create a fresh scene instance in the project's source DB under "Scenes/", named uniquely
    // (Scene, Scene2, ...). Writes the SceneDocument primary so the instance materializes; the
    // page treats the missing "scene" stream as an empty scene.
    inline draconic::content::Instance*
    CreateSceneInstance(EditorContext& context, draconic::content::Group* target = nullptr)
    {
        EditorProject* project = context.Project();
        if (project == nullptr)
        {
            return nullptr;
        }

        draconic::content::Group* scenes = target;
        if (scenes == nullptr)
        {
            draconic::content::Group* root = project->SourceDb().RootGroup();
            scenes = root->GetGroup(u8"Scenes");
            if (scenes == nullptr)
            {
                scenes = root->CreateGroup(u8"Scenes");
            }
        }
        if (scenes == nullptr)
        {
            return nullptr;
        }

        const String name = scenes->UniqueInstanceName(u8"Scene");

        draconic::content::Instance* instance =
            scenes->CreateInstance(name.AsView(), scene::SceneDocument::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }

        scene::SceneDocument doc;
        doc.name = name;
        if (!instance->WriteObject(doc).IsOk())
        {
            return nullptr;
        }

        // Seed default content: a directional Sun so a fresh scene is LIT out of the box
        // (with no light, meshes render in the dim flat ambient fallback and read as broken -
        // the classic "why is my duck untextured"). An authored entity, not editor magic: it
        // saves with the scene, shows in the hierarchy, and is free to edit or delete.
        {
            scene::Scene seeded(name.AsView());
            seeded.AddSystem<draconic::render::LightComponentManager>();
            const scene::EntityHandle sun = seeded.CreateEntity(u8"Sun");
            Transform t;
            // Shines along the entity's forward (-Z): tilt ~60 deg down, a slight compass yaw
            // (the Sandbox key-light default) so shading has direction.
            t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                         Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
            seeded.SetLocalTransform(sun, t);
            draconic::render::LightComponent& light =
                seeded.GetSystem<draconic::render::LightComponentManager>()->Add(sun);
            light.castsShadows = true; // intensity stays the component default (the value the
                                       // duck-scene fix was verified with)
            (void)scene::SaveScene(seeded, *instance);
        }
        return instance;
    }

    inline void RegisterSceneEditor(EditorContext& context, runtime::IApplicationHost& host,
                                    ui::runtime::UIHost& uiHost,
                                    draconic::runtime::DefaultApplication* embeddedApp = nullptr)
    {
        GlobalTypeRegistry().Register(scene::SceneDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::SceneDocument>();
        GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::PrefabDocument>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<SceneEditorPageFactory>(host, uiHost), DefaultAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<PrefabEditorPageFactory>(host, uiHost), DefaultAllocator()));

        EditorContext::AssetCreator creator;
        creator.label = String(u8"Scene");
        creator.create = [](EditorContext& ctx, draconic::content::Group* group)
        { return CreateSceneInstance(ctx, group); };
        creator.setsDefaultScene = true;
        context.RegisterCreator(Move(creator));

        EditorContext::AssetCreator prefabCreator;
        prefabCreator.label = String(u8"Prefab");
        prefabCreator.create = [](EditorContext& ctx, draconic::content::Group* group)
        { return CreatePrefabInstance(ctx, group); };
        context.RegisterCreator(Move(prefabCreator));

        // Model imports: generate/refresh the hierarchy prefab beside the manifest (the
        // "Generate prefab" import option). Lives here - not in the importer - because it
        // needs scene machinery the importer library (linked by the headless cooker) never
        // links. Re-import reuses the prefab's guid, so placed instances rebuild in every
        // open scene and an open prefab page refreshes like any external asset change.
        EditorContext* editorContext = &context;
        runtime::IApplicationHost* appHost = &host;

        // Play-in-editor: the singleton Game tab (player behavior in-process).
        context.GamePageFactory = [editorContext, appHost, appUiHost = &uiHost,
                                   embeddedApp](bool newInstance) -> UniquePtr<EditorPage>
        {
            // Reuse the primary instance for the normal Play; spin up an extra for "Play New Instance".
            runtime::GameInstance* instance =
                newInstance ? embeddedApp->CreateInstance() : &embeddedApp->Instance();
            return UniquePtr<EditorPage>(
                DefaultAllocator().New<GameEditorPage>(*editorContext, *appHost, *appUiHost,
                                                       embeddedApp, instance),
                DefaultAllocator());
        };

        // Export seam: scene/prefab TEXT sources transcode to the binary wire on the main
        // thread before the pack job. The scratch scene comes from the SceneSubsystem so
        // ISceneAware injection gives it the app's FULL manager set - a hand-listed set
        // would silently drop component types.
        context.SceneStreamStager = [appHost](draconic::content::Instance& instance,
                                              Array<byte>& out) -> bool
        {
            const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = instance.TypeName() == StringView(u8"PrefabDocument");
            if (!isScene && !isPrefab)
            {
                return false;
            }
            UniquePtr<IStream> stream = instance.ReadData(u8"scene");
            if (stream.Get() == nullptr)
            {
                return false;
            }
            auto* scenes = appHost->Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return false;
            }
            // A transient scratch group over the app's aware registry, so OnSceneCreated injects the
            // FULL component-manager set (no type silently skipped). Destructs at scope end.
            scene::SceneManager scratchMgr(&scenes->AwareRegistry());
            scene::Scene* scratch = scratchMgr.CreateScene(u8"__export_transcode");
            if (scratch == nullptr)
            {
                return false;
            }
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*stream, *scratch, /*includeSettings=*/isScene);
            scratchMgr.DestroyScene(scratch);
            if (!bytes.HasValue())
            {
                return false;
            }
            out = Move(bytes.Value());
            return true;
        };

        // Export reachability seam (docs/design/export-reachability.md): collect the assets a
        // scene/prefab references so the export closure can chase the scene->asset edges the cook's
        // read-dep graph can't see. Same SceneSubsystem-scratch pattern as the stager above (the full
        // manager set via ISceneAware, so no component type is silently skipped); resolves the scene's
        // Refs through a factory-less ResourceManager (nothing builds, so every bound id lands in
        // CollectUnresolved) and reads back the parked prefab instances. MAIN-THREAD only.
        context.SceneRefScanner =
            [appHost](draconic::content::Instance& instance, draconic::content::ContentDatabase& db,
                      Array<Guid>& outResources, Array<Guid>& outPrefabs) -> bool
        {
            const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = instance.TypeName() == StringView(u8"PrefabDocument");
            if (!isScene && !isPrefab)
            {
                return false;
            }
            auto* scenes = appHost->Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return false;
            }
            scene::SceneManager scratchMgr(
                &scenes->AwareRegistry()); // full manager set via ISceneAware
            scene::Scene* scratch = scratchMgr.CreateScene(u8"__export_scan");
            if (scratch == nullptr)
            {
                return false;
            }
            const bool loaded = scene::LoadScene(instance, *scratch).IsOk();
            if (loaded)
            {
                draconic::resource::ResourceManager collector(
                    db); // no factories -> all binds unresolved
                scene::ResolveSceneResources(*scratch, collector);
                collector.CollectUnresolved(outResources);
                scratch->ForEachPendingPrefabInstance(
                    [&outPrefabs](scene::Scene::PendingPrefabInstance& pending)
                    { outPrefabs.PushBack(pending.prefabId); });
            }
            scratchMgr.DestroyScene(scratch);
            return loaded;
        };

        context.AddImportListener(
            [editorContext, appHost](draconic::content::Instance& instance,
                                     const ImportOptions* options)
            {
                if (instance.TypeName() != StringView(u8"ModelManifestAsset"))
                {
                    return;
                }
                if (options != nullptr)
                {
                    auto* modelOptions = Cast<draconic::modelimporter::ModelImportOptions>(
                        const_cast<ImportOptions*>(options));
                    if (modelOptions != nullptr && !modelOptions->generatePrefab)
                    {
                        return;
                    }
                }
                ModelPrefabResult generated = GenerateModelPrefab(instance);
                if (generated.instance == nullptr)
                {
                    editorContext->Notify(NoticeKind::Error, u8"Model prefab generation failed.");
                    return;
                }
                if (generated.regenerated)
                {
                    UniquePtr<IStream> payload = generated.instance->ReadData(u8"scene");
                    if (payload.Get() != nullptr)
                    {
                        Array<byte> bytes;
                        bytes.Resize(static_cast<usize>(payload->Size()));
                        (void)payload->Read(bytes.Data(), bytes.Size());
                        const Guid prefabId = generated.instance->Id();
                        scene::PrefabPayloadResolver resolver{
                            [editorContext](const Guid& id) -> UniquePtr<IStream>
                            {
                                if (editorContext->Project() == nullptr)
                                {
                                    return UniquePtr<IStream>{};
                                }
                                draconic::content::Instance* prefab =
                                    editorContext->Project()->SourceDb().GetInstance(id);
                                return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                           : UniquePtr<IStream>{};
                            }};
                        if (auto* scenes = appHost->Ctx().GetSubsystem<scene::SceneSubsystem>())
                        {
                            scenes->ForEachScene(
                                [&](scene::Scene& scene)
                                {
                                    const u32 rebuilt = scene::RebuildPrefabInstances(
                                        scene, prefabId,
                                        Span<const byte>{bytes.Data(), bytes.Size()}, &resolver);
                                    if (rebuilt > 0 && editorContext->Resources() != nullptr)
                                    {
                                        scene::ResolveSceneResources(scene,
                                                                     *editorContext->Resources());
                                    }
                                });
                        }
                        for (const UniquePtr<EditorPage>& open : editorContext->OpenPages())
                        {
                            if (open->InstanceId() == prefabId)
                            {
                                open->OnAssetExternallyModified();
                            }
                        }
                    }
                }
                String message(u8"Prefab '");
                message += generated.instance->Name();
                message += generated.regenerated
                               ? StringView(u8"' regenerated (placed instances updated).")
                               : StringView(u8"' generated.");
                editorContext->Notify(NoticeKind::Success, message.AsView());
            });
    }
}

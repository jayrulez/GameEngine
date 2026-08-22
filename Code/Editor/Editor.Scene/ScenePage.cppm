// Editor::Scene - :page partition.
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
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.scene:page;

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import engine.scenesurface; // AddAllSceneManagers (the full composition for headless scratch scenes)
import engine.gameinstance; // GameInstance (the Game tab's run; multi-instance factory)
import foundation.scene;
import engine.scene;
import foundation.scene.resource;
import foundation.resource; // AsyncBindScope (pop-in page loads)
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import engine.ui; // game-UI RenderTexture canvases (live in editing viewports)
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.propertyanimation; // the persistent in-scene property-animation editor panel
import editor.camera;
import :edit;
import :model_prefab;
import :game_page;
import :gizmo;
import :tools;
import editor.viewporttools;
import :component_gizmos;
import :hierarchy;
import :inspector;
import :entity_picker_dialog; // the animation panel's Bind... target

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace vg = foundation::vg;
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    class SceneEditorPage final : public app::UIEditorPage
    {
    public:
        SceneEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                        ui::runtime::UIHost& uiHost, foundation::content::Instance& instance)
            : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
        {
            m_scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();
            m_gameUI = host.Ctx().GetSubsystem<engine::ui::UISubsystem>();

            // Own live Scene per page in this page's OWN SceneManager (registered with the subsystem
            // so it ticks on the Context lane; there is no shared default manager).
            if (m_scenes != nullptr)
            {
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
                        // Async binds: decodes go to workers, proxies settle over the next
                        // frames (the app pumps) and content POPS IN - a Sponza-sized page
                        // must never stall the UI thread (the player's LoadSceneAsync model,
                        // adopted editor-side).
                        foundation::resource::AsyncBindScope asyncScope(*context.Resources());
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
                                    foundation::content::Instance* prefab =
                                        editorContext->Project()->SourceDb().GetInstance(prefabId);
                                    return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                               : UniquePtr<IStream>{};
                                }});
                        if (context.Resources() != nullptr)
                        {
                            foundation::resource::AsyncBindScope asyncScope(*context.Resources());
                            scene::ResolveSceneResources(*m_scene, *context.Resources());
                        }
                    }
                    LOG_INFO(u8"Editor", u8"opened scene '{}'", m_title);
                }
                else if (loaded.Code() == ErrorCode::NotFound)
                {
                    LOG_INFO(u8"Editor", u8"new scene '{}' (no scene stream yet)",
                                      m_title);
                }
                else
                {
                    LOG_ERROR(u8"Editor", u8"scene '{}' failed to load", m_title);
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
                        foundation::content::Instance* prefab =
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
                {
                    auto selectTool =
                        MakeUnique<SelectTransformTool>(DefaultAllocator(), *m_editContext);
                    m_selectTool = selectTool.Get();
                    m_viewportTools.Add(Move(selectTool)); // first added = the default tool
                    ViewportToolHostContext toolHost;
                    toolHost.scene = &m_editContext->Scene();
                    toolHost.commands = &m_editContext->Commands();
                    toolHost.entitySelection = &m_editContext->EntitySelection();
                    ViewportToolProviderRegistry::Get().CreateAll(m_viewportTools, toolHost);
                }
                RegisterBuiltinGizmoRenderers(m_componentGizmos);
            }

            // Viewport pane: [toolbar strip | viewport]. The toolbar mirrors and drives the
            // gizmo state the W/E/R/X keys already control - the on-screen answer to "which
            // space am I in" (the recorded gap: X toggled with no visible state anywhere).
            BuildViewportToolbar();
            auto viewportPane = MakeRef<foundation::ui::FlexLayout>(DefaultAllocator());
            viewportPane->Direction = foundation::ui::Orientation::Vertical;
            {
                auto lp = MakeRef<foundation::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = foundation::ui::SizeSpec::Match();
                lp->Height = foundation::ui::SizeSpec::Fixed(foundation::ui::Unit::Px(30));
                viewportPane->AddView(m_toolbar.Get(), lp);
            }
            {
                // Wrap the viewport in a FrameLayout so the camera preview (task #118) overlays it
                // in the bottom-right corner. The viewport fills the frame; the preview floats over.
                BuildCameraPreview();
                auto viewportFrame = MakeRef<foundation::ui::FrameLayout>(DefaultAllocator());
                {
                    auto vfp = MakeRef<foundation::ui::FrameLayoutParams>(DefaultAllocator());
                    vfp->Gravity = foundation::ui::Gravity::Fill;
                    viewportFrame->AddView(m_viewport.Get(), vfp);
                }
                {
                    auto pfp = MakeRef<foundation::ui::FrameLayoutParams>(DefaultAllocator());
                    pfp->Gravity = static_cast<foundation::ui::Gravity>(
                        static_cast<u32>(foundation::ui::Gravity::Bottom) |
                        static_cast<u32>(foundation::ui::Gravity::Right));
                    pfp->Margin = foundation::ui::Thickness{12.0f, 12.0f, 12.0f, 12.0f};
                    viewportFrame->AddView(m_previewContainer.Get(), pfp);
                }
                auto lp = MakeRef<foundation::ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = foundation::ui::SizeSpec::Match();
                lp->Grow = 1.0f;
                viewportPane->AddView(viewportFrame.Get(), lp);
            }

            // The persistent property-animation editor, docked BELOW THE VIEWPORT ONLY (not under the
            // hierarchy/inspector) in a resizable vertical split, so a timeline has the horizontal room
            // it needs. Scene-page-owned for the page's whole life (the fix for the tool-mode's undo-UAF
            // + stale-snapshot); its header carries a collapse toggle. Starts collapsed so the viewport
            // opens full-height; the user expands it (or drags the splitter) when authoring animation.
            m_propAnimPanel = MakeRef<PropertyAnimationPanel>(
                DefaultAllocator(), *m_context, m_editContext->Scene(), m_editContext->Commands(),
                m_editContext->EntitySelection());

            // Godot-style bottom dock (property-animation-editor.md A2 REVISED): a persistent tab bar
            // under the viewport; the "Animation" tab expands the property-animation panel above it
            // (draggable splitter between the two) and collapses back to just the bar. The panel view
            // lives for the page's whole life either way (the F1 invariant). Per-scene-page - editor
            // singletons (Console/Output) stay in the shell docking and never migrate here.
            m_bottomDock = MakeRef<foundation::ui::toolkit::BottomDock>(DefaultAllocator());
            m_bottomDock->AddTab(u8"animation", u8"Animation", m_propAnimPanel.Get());

            // Viewport column: [ viewport (toolbar + 3D) / bottom dock ] as a vertical split. The dock
            // drives the split's pane-collapse: the divider vanishes when collapsed and the stored ratio
            // survives expand/collapse. Starts collapsed (bar only; viewport full-height).
            m_viewportColumn = MakeRef<foundation::ui::toolkit::SplitView>(DefaultAllocator());
            m_viewportColumn->Orientation = foundation::ui::Orientation::Vertical;
            m_viewportColumn->SetSplitRatio(0.72f);
            m_viewportColumn->SetPanes(viewportPane.Get(), m_bottomDock.Get());
            m_viewportColumn->SetPaneCollapsed(foundation::ui::toolkit::SplitPane::Second, true);
            {
                SceneEditorPage* page = this;
                m_bottomDock->OnExpandedChanged.Add(
                    [page](bool expanded)
                    {
                        page->m_viewportColumn->SetPaneCollapsed(
                            foundation::ui::toolkit::SplitPane::Second, !expanded);
                    });
            }

            // Clip-editing workflow wiring (property-animation-editor.md, 2026-08-17):
            // 1) the panel's Bind... button opens THIS page's entity picker (the panel cannot
            //    depend on editor.scene, so the page injects it);
            // 2) opening a PropertyAnimationClipAsset while this page is on screen is CLAIMED
            //    into the animation bar (the animator slot's pencil routes through the shared
            //    open-asset path), auto-binding the current primary selection; any other page
            //    state falls through to the generic asset page.
            {
                SceneEditorPage* page = this;
                m_propAnimPanel->RequestEntityPick =
                    [page](const Guid& current,
                           Function<void(const Guid&)> onPicked)
                {
                    foundation::ui::UIContext* ctx = page->m_propAnimPanel->Context;
                    if (ctx == nullptr)
                    {
                        return;
                    }
                    auto dialog = MakeRef<EntityPickerDialog>(DefaultAllocator(),
                                                              page->m_editContext->Scene(),
                                                              current);
                    dialog->OnPicked = [cb = Move(onPicked)](const Guid& picked)
                    {
                        if (cb && !picked.IsNil()) // [Clear] means "keep the binding"
                        {
                            cb(picked);
                        }
                    };
                    dialog->Show(ctx);
                };
                m_openAssetInterceptorId = m_context->AddOpenAssetInterceptor(
                    [page](foundation::content::Instance& instance) -> bool
                    {
                        if (instance.TypeName() !=
                            StringView(u8"PropertyAnimationClipAsset"))
                        {
                            return false; // not ours
                        }
                        // Only the page ON SCREEN claims - a background scene page must not
                        // swallow an open meant for the visible one (or the generic page).
                        if (page->m_content.Get() == nullptr ||
                            !page->m_content->IsEffectivelyVisible())
                        {
                            return false;
                        }
                        page->m_bottomDock->ActivateTab(u8"animation"); // expand the bar
                        const Guid* primary =
                            page->m_editContext->EntitySelection().Primary();
                        page->m_propAnimPanel->RequestEditClip(
                            instance.Id(), (primary != nullptr) ? *primary : Guid{});
                        return true;
                    });
            }

            // Page layout: [ hierarchy | (viewport-column | inspector) ].
            auto inner = MakeRef<foundation::ui::toolkit::SplitView>(DefaultAllocator());
            inner->SetSplitRatio(0.72f);
            inner->SetPanes(m_viewportColumn.Get(), m_inspector.Get());
            auto topContent = MakeRef<foundation::ui::toolkit::SplitView>(DefaultAllocator());
            topContent->SetSplitRatio(0.2f);
            topContent->SetPanes(m_hierarchy.Get(), inner.Get());
            m_content = topContent;

            m_router =
                MakeUnique<foundation::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());

            // Start framed on the origin (grid center), orbit pivot there, horizon level.
            m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});
        }

        ~SceneEditorPage() override
        {
            // OnClose removes it on the normal close path; this is the backstop - the
            // interceptor captures a raw `this` and must never outlive the page.
            if (m_openAssetInterceptorId != 0)
            {
                m_context->RemoveOpenAssetInterceptor(m_openAssetInterceptorId);
                m_openAssetInterceptorId = 0;
            }
        }

        // === UIEditorPage ===

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;

        // Called only for the MAIN window's frame, inside the app-level scene-renderer bracket
        // (Sedulous structure): the offscreen target is window-agnostic, so this renders no
        // matter which OS window hosts the panel; that window's UI samples the result.
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;

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

        void OnSavedAs(foundation::content::Instance& instance) override;

        [[nodiscard]] Status Save() override;

        void OnClose() override;

        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }
        [[nodiscard]] EditorCamera& Camera() noexcept { return m_camera; }
        [[nodiscard]] SceneEditContext* EditContext() const noexcept { return m_editContext.Get(); }

    private:
        static constexpr f32 kFovY = 1.0472f; // must match OnRenderWindow's projection

        // Camera ray through the mouse position, built from the camera basis (no matrix inverse).
        [[nodiscard]] bool MakeMouseRay(GizmoRay& out) const;

        // Feed the viewport tool manager a frame of input (editor.viewporttools). Runs EVERY
        // frame so the active tool's visuals track tree selections and undo/redo even while the
        // mouse is elsewhere (pointer flagged invalid then - pose-sync only). The page keeps
        // CAMERA POLICY: buttons are masked and the keyboard nulled while the camera owns the
        // mouse (Alt orbit / RMB fly / Tab-captured). Selection picking lives INSIDE the default
        // SelectTransformTool now; Simulate maps to input.editingLocked.
        [[nodiscard]] bool UpdateViewportTools(bool viewportActive);

        void DrawGizmos(render::debug::DebugDraw& dd);

        // The viewport's ephemeral post "show flags" popup (checkable). Toggling a flag re-renders
        // this page's viewport with that effect stripped; the scene asset is never touched.
        void ShowPostFlagsMenu(foundation::ui::View* anchor);

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

        // Bind (and re-bind after dock/float moves) the viewport to the window that hosts it -
        // the UISandbox UpdateViewportHostWindow dance: RendererFor is only valid once the
        // window is attached, and a floated panel lives in a different OS window.
        void EnsureViewportBound();

        EditorContext* m_context;                  // borrowed
        runtime::IApplicationHost* m_host;         // borrowed
        ui::runtime::UIHost* m_uiHost;             // borrowed
        engine::scene::SceneSubsystem* m_scenes = nullptr; // borrowed (context subsystem: registry + tick)
        scene::SceneManager
            m_sceneManager; // this page's OWN scene group (registered with m_scenes)
        engine::render::RenderSubsystem* m_render = nullptr;
        engine::ui::UISubsystem* m_gameUI = nullptr; // RT-canvas host seam (borrowed)

        String m_title;
        scene::Scene* m_scene = nullptr;           // owned by the SceneSubsystem
        UniquePtr<SceneEditContext> m_editContext; // per-page mutation mediator + selection
        RefPtr<foundation::ui::View> m_content; // [hierarchy | viewport | inspector] over bottom panel
        RefPtr<SceneHierarchyView> m_hierarchy;
        RefPtr<ui::toolkit::Toolbar> m_toolbar;
        render::ViewPostOverride
            m_postOverride; // ephemeral viewport post show-flags (not serialized)
        ui::toolkit::ToolbarButton* m_playButton = nullptr; // borrowed (toolbar-owned)
        ui::toolkit::ToolbarToggle* m_pauseToggle = nullptr;
        ui::toolkit::ToolbarButton* m_stopButton = nullptr;
        RefPtr<foundation::ui::Label> m_simLabel;
        UniquePtr<scene::SceneSnapshot> m_simSnapshot;
        bool m_isSimulating = false;
        bool m_isPaused = false;
        ui::toolkit::ToolbarToggle* m_translateToggle = nullptr; // borrowed (toolbar-owned)
        ui::toolkit::ToolbarToggle* m_rotateToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_scaleToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_spaceToggle = nullptr;
        ui::toolkit::ToolbarToggle* m_gridToggle = nullptr;
        bool m_showGrid = true;

        // Viewport tool palette: one toggle per non-default registered tool (Property Animation, and
        // future terrain/nav-mesh) that activates it - the affordance that docks the tool's panel.
        struct ToolToggle
        {
            ui::toolkit::ToolbarToggle* toggle = nullptr;
            String id;
        };
        Array<ToolToggle> m_toolToggles;
        RefPtr<SceneInspectorView> m_inspector;
        ViewportToolManager m_viewportTools;             // declared after m_editContext (tools borrow it)
        SelectTransformTool* m_selectTool = nullptr;     // borrowed (manager-owned default tool)
        GizmoRendererRegistry m_componentGizmos;

        // The persistent in-scene property-animation editor, hosted as the "Animation" tab of the
        // bottom dock (a resizable vertical split below the viewport). Scene-page-owned for its whole
        // life, so an undo command can never outlive it. Ticked + overlay-drawn each frame from OnUpdate.
        RefPtr<PropertyAnimationPanel> m_propAnimPanel;
        RefPtr<foundation::ui::toolkit::BottomDock> m_bottomDock;   // the collapsible bottom strip
        RefPtr<foundation::ui::toolkit::SplitView> m_viewportColumn; // [viewport / bottom dock] vsplit
        u64 m_openAssetInterceptorId = 0; // the clip-open claim (removed in the destructor)
        RefPtr<ui::viewport::ViewportView> m_viewport;

        // Camera preview (task #118): a small bottom-right overlay showing a selected/pinned camera's
        // live view. Editor-session state only - NEVER persisted into a runtime/wire struct.
        RefPtr<ui::viewport::ViewportView> m_previewViewport;
        RefPtr<foundation::ui::View> m_previewContainer; // the overlay (Visibility::Gone when idle)
        RefPtr<foundation::ui::Button> m_previewPin;
        scene::EntityHandle m_pinnedCamera;  // the PINNED camera entity (unassigned = not pinned)
        scene::EntityHandle m_previewTarget; // the camera previewed this frame (unassigned = none)
        u32 m_previewHeight = 180;           // panel + render-target height (from the target aspect)
        void BuildCameraPreview();
        void UpdateCameraPreview();
        void RenderCameraPreview();
        void ToggleCameraPin();
        [[nodiscard]] scene::EntityHandle SelectedCameraEntity() const;
        [[nodiscard]] bool IsLiveCamera(scene::EntityHandle entity) const;
        UniquePtr<foundation::shell::InputRouter> m_router;
        EditorCamera m_camera;
        foundation::graphics::RenderWindow* m_hostWindow =
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
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

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
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Create a fresh empty prefab instance under "Prefabs/", named uniquely (Prefab,
    // Prefab2, ...). Content arrives when the user saves the opened page (an unsaved prefab
    // has no payload; spawning one warns).
    inline foundation::content::Instance*
    CreatePrefabInstance(EditorContext& context, foundation::content::Group* target = nullptr)
    {
        EditorProject* project = context.Project();
        if (project == nullptr)
        {
            return nullptr;
        }

        foundation::content::Group* prefabs = target;
        if (prefabs == nullptr)
        {
            foundation::content::Group* root = project->SourceDb().RootGroup();
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

        foundation::content::Instance* instance =
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
    inline foundation::content::Instance*
    CreateSceneInstance(EditorContext& context, foundation::content::Group* target = nullptr)
    {
        EditorProject* project = context.Project();
        if (project == nullptr)
        {
            return nullptr;
        }

        foundation::content::Group* scenes = target;
        if (scenes == nullptr)
        {
            foundation::content::Group* root = project->SourceDb().RootGroup();
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

        foundation::content::Instance* instance =
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
            seeded.AddSystem<engine::render::LightComponentManager>();
            const scene::EntityHandle sun = seeded.CreateEntity(u8"Sun");
            Transform t;
            // Shines along the entity's forward (-Z): tilt ~60 deg down, a slight compass yaw
            // (the Sandbox key-light default) so shading has direction.
            t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                         Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
            seeded.SetLocalTransform(sun, t);
            engine::render::LightComponent& light =
                seeded.GetSystem<engine::render::LightComponentManager>()->Add(sun);
            light.castsShadows = true; // intensity stays the component default (the value the
                                       // duck-scene fix was verified with)
            (void)scene::SaveScene(seeded, *instance);
        }
        return instance;
    }

    inline void RegisterSceneEditor(EditorContext& context, runtime::IApplicationHost& host,
                                    ui::runtime::UIHost& uiHost,
                                    engine::runtime::DefaultApplication* embeddedApp = nullptr)
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
        creator.create = [](EditorContext& ctx, foundation::content::Group* group)
        { return CreateSceneInstance(ctx, group); };
        creator.setsDefaultScene = true;
        context.RegisterCreator(Move(creator));

        EditorContext::AssetCreator prefabCreator;
        prefabCreator.label = String(u8"Prefab");
        prefabCreator.create = [](EditorContext& ctx, foundation::content::Group* group)
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
            engine::runtime::GameInstance* instance =
                newInstance ? embeddedApp->CreateInstance() : &embeddedApp->Instance();
            return UniquePtr<EditorPage>(
                DefaultAllocator().New<GameEditorPage>(*editorContext, *appHost, *appUiHost,
                                                       embeddedApp, instance),
                DefaultAllocator());
        };

        // Export seam: scene/prefab TEXT sources transcode to the binary wire on the main
        // thread before the pack job. The scratch scene is assembled from the full
        // composition - a hand-listed set would silently drop component types.
        context.SceneStreamStager = [](foundation::content::Instance& instance,
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
            // A transient scratch scene assembled from the full composition (no component type
            // silently skipped by a hand-listed set).
            scene::Scene scratch(u8"__export_transcode");
            engine::AddAllSceneManagers(scratch);
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*stream, scratch, /*includeSettings=*/isScene);
            if (!bytes.HasValue())
            {
                return false;
            }
            out = Move(bytes.Value());
            return true;
        };

        // Export reachability seam (docs/design/export-reachability.md): collect the assets a
        // scene/prefab references so the export closure can chase the scene->asset edges the cook's
        // read-dep graph can't see. Same full-composition scratch pattern as the stager above (the full
        // manager set, so no component type is silently skipped); resolves the scene's Refs through a
        // factory-less ResourceManager (nothing builds, so every bound id lands in CollectUnresolved)
        // and reads back the parked prefab instances. MAIN-THREAD only.
        context.SceneRefScanner =
            [](foundation::content::Instance& instance, foundation::content::ContentDatabase& db,
               Array<Guid>& outResources, Array<Guid>& outPrefabs) -> bool
        {
            const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = instance.TypeName() == StringView(u8"PrefabDocument");
            if (!isScene && !isPrefab)
            {
                return false;
            }
            // A transient scratch scene assembled from the full composition (no component type
            // silently skipped by a hand-listed set).
            scene::Scene scratch(u8"__export_scan");
            engine::AddAllSceneManagers(scratch);
            const bool loaded = scene::LoadScene(instance, scratch).IsOk();
            if (loaded)
            {
                foundation::resource::ResourceManager collector(
                    db); // no factories -> all binds unresolved
                scene::ResolveSceneResources(scratch, collector);
                collector.CollectUnresolved(outResources);
                scratch.ForEachPendingPrefabInstance(
                    [&outPrefabs](scene::Scene::PendingPrefabInstance& pending)
                    { outPrefabs.PushBack(pending.prefabId); });
            }
            return loaded;
        };

        context.AddImportListener(
            [editorContext, appHost](foundation::content::Instance& instance,
                                     const pipeline::ImportOptions* options)
            {
                if (instance.TypeName() != StringView(u8"ModelManifestAsset"))
                {
                    return;
                }
                // Prefab + scene are independent toggles (either, both, or neither). No options
                // object = the default fresh import, which generates the prefab only.
                bool wantPrefab = true;
                bool wantScene = false;
                if (options != nullptr)
                {
                    auto* modelOptions = Cast<pipeline::ModelImportOptions>(
                        const_cast<pipeline::ImportOptions*>(options));
                    if (modelOptions != nullptr)
                    {
                        wantPrefab = modelOptions->generatePrefab;
                        wantScene = modelOptions->generateScene;
                    }
                }

                if (wantPrefab)
                {
                    ModelPrefabResult generated = GenerateModelPrefab(instance);
                    if (generated.instance == nullptr)
                    {
                        editorContext->Notify(NoticeKind::Error,
                                              u8"Model prefab generation failed.");
                    }
                    else
                    {
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
                                        foundation::content::Instance* prefab =
                                            editorContext->Project()->SourceDb().GetInstance(id);
                                        return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                                   : UniquePtr<IStream>{};
                                    }};
                                if (auto* scenes =
                                        appHost->Ctx().GetSubsystem<engine::scene::SceneSubsystem>())
                                {
                                    scenes->ForEachScene(
                                        [&](scene::Scene& scene)
                                        {
                                            const u32 rebuilt = scene::RebuildPrefabInstances(
                                                scene, prefabId,
                                                Span<const byte>{bytes.Data(), bytes.Size()},
                                                &resolver);
                                            if (rebuilt > 0 &&
                                                editorContext->Resources() != nullptr)
                                            {
                                                scene::ResolveSceneResources(
                                                    scene, *editorContext->Resources());
                                            }
                                        });
                                }
                                for (const UniquePtr<EditorPage>& open :
                                     editorContext->OpenPages())
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
                    }
                }

                if (wantScene)
                {
                    ModelPrefabResult generatedScene = GenerateModelScene(instance);
                    if (generatedScene.instance == nullptr)
                    {
                        editorContext->Notify(NoticeKind::Error,
                                              u8"Model scene generation failed.");
                    }
                    else
                    {
                        // A regenerated scene refreshes any open page editing it (scenes are
                        // referenced/opened, not spawned as instances, so there is nothing to
                        // rebuild in other scenes).
                        if (generatedScene.regenerated)
                        {
                            const Guid sceneId = generatedScene.instance->Id();
                            for (const UniquePtr<EditorPage>& open : editorContext->OpenPages())
                            {
                                if (open->InstanceId() == sceneId)
                                {
                                    open->OnAssetExternallyModified();
                                }
                            }
                        }
                        String message(u8"Scene '");
                        message += generatedScene.instance->Name();
                        message += generatedScene.regenerated ? StringView(u8"' regenerated.")
                                                              : StringView(u8"' generated.");
                        editorContext->Notify(NoticeKind::Success, message.AsView());
                    }
                }
            });
    }
}

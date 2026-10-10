// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :page partition.
//
// SceneEditorPage: the scene document editor. Each page owns its OWN
// live Scene (multi-scene rule - several pages open at once; everything scene-scoped is
// per-page): a ViewportView renders the scene through the REAL renderer via CameraOverride into
// the viewport's offscreen color target, an EditorCamera flies on the viewport's gated devices
// (hover/focus-gated, so occluded/inactive-tab input can't leak), and open/save round-trip the
// content DB through LoadScene/SaveScene. An empty scene shows a debug-draw ground grid + origin
// axes so navigation reads immediately.
//
// RegisterSceneEditor is the module's RegisterEditor entry point: the EXECUTABLE calls it
// (editor core/app never link this module); it registers the SceneDocument type, the page
// factory, and the "Scene" asset creator (File > New Scene).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.scene:page;

import :asset_thumbnails; // RegisterSceneThumbnailGenerators (called from RegisterSceneEditor)

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import engine.composition; // AddAllSceneManagers (the full composition for headless scratch scenes)
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
import pipeline.importer; // the import framework (a consumer imports it itself)
import scene.pipeline;    // a model import's prefab and scene (GenerateForImport)
import modelimporter;     // ModelImportOptions (the import's prefab and scene toggles)
import editor.app;
import editor.propertyanimation; // the persistent in-scene property-animation editor panel
import editor.camera;
import foundation.mcp; // McpServer (the MCP tool contribution)
import :view_settings; // RegisterSceneViewSettingsType (per-scene grid pref)
import :zoom_readout;  // the measurement overlay while zooming
import :view_gizmo;    // the orientation gizmo in the viewport's top-right
import :edit;
import :settings_profiles; // QueueSettingsProfileEdit (a profile-mode settings edit)
import :scene_page_interface; // ISceneEditorPage (published on the page)
import :viewport_capture;     // ViewportCaptureRecorder (viewport_screenshot)
import :pie_tools;            // RegisterPieTools (the contribution)
import :actions; // the scene editor's action declarations
import :mcp_tools;            // RegisterSceneLiveTools (the contribution)
import :game_page;
import :gizmo;
import :tools;
import editor.viewporttools;
import :component_gizmos;
import :hierarchy;
import :inspector;
import :entity_picker_dialog; // the animation panel's Bind... target

using namespace foundation::core;

namespace core = foundation::core;
namespace rhi = foundation::rhi;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace vg = foundation::vg;
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    class SceneEditorPage final : public app::UIEditorPage, public ISceneEditorPage
    {
        static constexpr StringView kAnimationTab = u8"animation"; // the bottom dock's tab

    public:
        SceneEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                        ui::runtime::UIHost& uiHost, foundation::content::Instance& instance)
            : app::UIEditorPage(context.Allocator()),
              m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
        {
            // Set the instance id NOW, before the viewport toolbar's ScenePage_OverlaysInit ->
            // LoadViewPrefs runs: the per-scene grid/LOD prefs are keyed by this guid, and the ctor
            // builds the toolbar before the context's later SetInstanceId, so loading with a nil guid
            // would always fall back to the default (grid on) and never restore the saved toggle. Same
            // early-bind MeshPage does; the context's SetInstanceId is then the identical value.
            SetInstanceId(instance.Id());
            Provide<ISceneEditorPage>(*this); // what this page lets others act through

            m_scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            m_render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();
            m_gameUI = host.Ctx().GetSubsystem<engine::ui::UISubsystem>();

            // Own live Scene per page in this page's OWN SceneManager (registered with the subsystem
            // so it ticks on the Context lane; there is no shared default manager).
            if (m_scenes != nullptr)
            {
                m_scenes->RegisterManager(&m_sceneManager);
                // The page IS the run scope for edit-mode Simulate:
                // it owns the bus and injects it BEFORE CreateScene so systems
                // binding at assembly see it; OnUpdate drains it once per frame.
                m_sceneManager.SetSceneEventBus(&m_pageEvents);
                m_scene = m_sceneManager.CreateScene(instance.Name());
                m_scene->SetSimulationEnabled(false); // edit mode is frozen; Simulate un-freezes
                const Stopwatch loadClock = Stopwatch::StartNew();
                const Status loaded = scene::LoadScene(instance, *m_scene);
                const i64 parseMs = static_cast<i64>(loadClock.Elapsed().AsMilliseconds());
                i64 bindMs = 0;
                i64 prefabMs = 0;
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
                    bindMs = static_cast<i64>(loadClock.Elapsed().AsMilliseconds()) - parseMs;
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
                        prefabMs = static_cast<i64>(loadClock.Elapsed().AsMilliseconds()) -
                                   parseMs - bindMs;
                    }
                    // The UI-thread cost of opening this scene: the document parse, the bind
                    // pass (async: it queues decodes, the products pop in over later frames),
                    // the prefab respawn. The async decodes and GPU finalizes are not in here.
                    LOG_INFO(u8"Editor",
                             u8"opened scene '{}': parse {} ms, bind {} ms, prefabs {} ms "
                             u8"(UI thread)",
                             m_title, parseMs, bindMs, prefabMs);
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
            m_viewport = MakeRef<ui::viewport::ViewportView>(Allocator());
            m_viewport->ClearColor = rhi::ClearColor{0.349f, 0.366f, 0.396f, 1.0f};

            // Everything scene-scoped is PER PAGE (multi-scene): mutation mediator (all edits
            // are commands on THIS page's stack), selection, hierarchy + inspector views.
            if (m_scene != nullptr)
            {
                m_editContext =
                    MakeUnique<SceneEditContext>(Allocator(), *m_scene, Commands());
                m_editContext->SetResources(context.Resources());
                // A settings edit that lands in a profile (the block's source is a profile) is
                // written to the profile's asset by the save flow, like any live asset edit.
                {
                    EditorContext* profileContext = &context;
                    SceneEditContext* profileEdit = m_editContext.Get();
                    m_editContext->OnSettingsProfileEdited =
                        [profileContext, profileEdit](const TypeInfo* type, const Guid& profile)
                    {
                        if (scene::SceneSystem* system = profileEdit->FindSystemBySettingsType(type))
                        {
                            QueueSettingsProfileEdit(*profileContext, *system, profile);
                        }
                    };
                }
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
                m_hierarchy = MakeRef<SceneHierarchyView>(Allocator(), *m_editContext);
                m_hierarchy->SetEditorContext(&context);
                // Its context menus are the scene editor's actions over THIS page.
                m_hierarchy->SetActions(&context.Actions(), this);
                m_hierarchy->OnFrameEntity = [this](const Guid& entity)
                { (void)FrameEntities(Span<const Guid>(&entity, 1), true); };
                m_inspector =
                    MakeRef<SceneInspectorView>(Allocator(), context, *m_editContext);
                {
                    auto selectTool =
                        MakeUnique<SelectTransformTool>(Allocator(), *m_editContext);
                    m_selectTool = selectTool.Get();
                    m_selectTool->SetPicker(&m_picker); // GPU pick over this page's viewport
                    m_viewportTools.Add(Move(selectTool)); // first added = the default tool
                    ViewportToolHostContext toolHost;
                    toolHost.scene = &m_editContext->Scene();
                    toolHost.commands = &m_editContext->Commands();
                    toolHost.entitySelection = &m_editContext->EntitySelection();
                    toolHost.picker = &m_picker;
                    // Asset-edit persistence transport: a tool (terrain sculpt) that mutates a
                    // cooked product live registers a write-back-to-source closure on the context;
                    // the editor save flow drains it (see EditorContext::DrainAssetEdits).
                    toolHost.assetEdits = &context;
                    ViewportToolProviderRegistry::Get().CreateAll(m_viewportTools, toolHost);
                }
                RegisterBuiltinGizmoRenderers(m_componentGizmos);
            }

            // Viewport pane: [toolbar strip | viewport]. The toolbar mirrors and drives the
            // gizmo state the W/E/R/X keys already control - the on-screen answer to "which
            // space am I in" (the recorded gap: X toggled with no visible state anywhere).
            BuildViewportToolbar();
            auto viewportPane = MakeRef<foundation::ui::FlexLayout>(Allocator());
            viewportPane->Direction = foundation::ui::Orientation::Vertical;
            {
                foundation::ui::LayoutStyle lp;
                lp.Width = foundation::ui::SizeSpec::Match();
                lp.Height = foundation::ui::SizeSpec::Fixed(foundation::ui::Unit::Dp(30));
                viewportPane->AddView(m_toolbar.Get(), lp);
            }
            {
                // Wrap the viewport in a FrameLayout so the camera preview (task #118) overlays it
                // in the bottom-right corner. The viewport fills the frame; the preview floats over.
                BuildCameraPreview();
                auto viewportFrame = MakeRef<foundation::ui::FrameLayout>(Allocator());
                {
                    foundation::ui::LayoutStyle vfp;
                    vfp.Gravity = foundation::ui::Gravity::Fill;
                    viewportFrame->AddView(m_viewport.Get(), vfp);
                }
                {
                    foundation::ui::LayoutStyle pfp;
                    pfp.Gravity = static_cast<foundation::ui::Gravity>(
                        static_cast<u32>(foundation::ui::Gravity::Bottom) |
                        static_cast<u32>(foundation::ui::Gravity::Right));
                    pfp.Margin = foundation::ui::Thickness{12.0f, 12.0f, 12.0f, 12.0f};
                    viewportFrame->AddView(m_previewContainer.Get(), pfp);
                }
                {
                    // The ViewportOverlay tool-panel target: a themed HUD panel floating top-right over
                    // the viewport. Idle (Gone) until a ViewportOverlay-placed tool panel mounts into it.
                    m_toolOverlay = MakeRef<foundation::ui::Panel>(Allocator());
                    m_toolOverlay->AddClass(u8"panel"); // resolve the theme's panel background
                    m_toolOverlay->Visibility = foundation::ui::Visibility::Gone;
                    m_toolOverlay->Padding = foundation::ui::Thickness{8.0f, 8.0f, 8.0f, 8.0f};
                    foundation::ui::LayoutStyle ofp;
                    ofp.Gravity = static_cast<foundation::ui::Gravity>(
                        static_cast<u32>(foundation::ui::Gravity::Top) |
                        static_cast<u32>(foundation::ui::Gravity::Right));
                    ofp.Margin = foundation::ui::Thickness{12.0f, 12.0f, 12.0f, 12.0f};
                    // Constrain the width so the HUD does NOT stretch to fill the viewport (the panel
                    // content sizes to Match otherwise). Height wraps to the controls.
                    ofp.Width = foundation::ui::SizeSpec::Fixed(foundation::ui::Unit::Dp(260.0f));
                    viewportFrame->AddView(m_toolOverlay.Get(), ofp);
                }
                {
                    // The Float tool-panel target: a FloatingPanel (draggable / resizable / collapsible /
                    // closable) hosted in an AbsoluteLayout layer that fills the viewport. The layer is
                    // NOT hit-test-visible, so empty areas fall through to the 3D viewport; the panel
                    // positions itself by AbsoluteLayoutParams X/Y so its Bounds stay exact for input.
                    BuildToolFloat();
                    m_toolFloatLayer = MakeRef<foundation::ui::AbsoluteLayout>(Allocator());
                    m_toolFloatLayer->IsHitTestVisible = false;
                    foundation::ui::LayoutStyle layerLp;
                    layerLp.Gravity = foundation::ui::Gravity::Fill;
                    layerLp.Width = foundation::ui::SizeSpec::Match();
                    layerLp.Height = foundation::ui::SizeSpec::Match();
                    viewportFrame->AddView(m_toolFloatLayer.Get(), layerLp);

                    foundation::ui::LayoutStyle alp;
                    alp.Left = 16.0f;
                    alp.Top = 16.0f;
                    m_toolFloatLayer->AddView(m_toolFloat.Get(), alp);
                }
                foundation::ui::LayoutStyle lp;
                lp.Width = foundation::ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                viewportPane->AddView(viewportFrame.Get(), lp);
            }

            // The persistent property-animation editor, docked BELOW THE VIEWPORT ONLY (not under the
            // hierarchy/inspector) in a resizable vertical split, so a timeline has the horizontal room
            // it needs. Scene-page-owned for the page's whole life (the fix for the tool-mode's undo-UAF
            // + stale-snapshot); its header carries a collapse toggle. Starts collapsed so the viewport
            // opens full-height; the user expands it (or drags the splitter) when authoring animation.
            m_propAnimPanel = MakeRef<PropertyAnimationPanel>(
                Allocator(), *m_context, m_editContext->Scene(), m_editContext->Commands(),
                m_editContext->EntitySelection());

            // Godot-style bottom dock under the viewport. It opens from elsewhere (the toolbar's
            // Animation toggle, a viewport tool that docks its panel under its domain's name), and
            // with nothing open it takes no space, tab bar included. The animation panel view lives
            // for the page's whole life either way (the F1 invariant). Per-scene-page - editor
            // singletons (Console/Output) stay in the shell docking and never migrate here.
            m_bottomDock = MakeRef<foundation::ui::toolkit::BottomDock>(Allocator());
            m_bottomDock->SetHideWhenCollapsed(true);
            m_bottomDock->AddTab(kAnimationTab, u8"Animation", m_propAnimPanel.Get());

            // Viewport-tool PANEL seam: the ViewportToolPanelHost watches the active viewport tool
            // and mounts its registered panel (or unmounts it); a docked panel is a bottom dock tab
            // of its own (MountToolPanel).
            {
                SceneEditorPage* page = this;
                ViewportToolHostContext panelCtx;
                panelCtx.scene = &m_editContext->Scene();
                panelCtx.commands = &m_editContext->Commands();
                panelCtx.entitySelection = &m_editContext->EntitySelection();
                panelCtx.assetEdits = &context;
                panelCtx.editorContext = &context;
                m_toolPanelHost = MakeUnique<ViewportToolPanelHost>(
                    Allocator(), m_viewportTools, ViewportToolPanelRegistry::Get(), panelCtx,
                    core::Function<void(foundation::ui::View*, ToolPanelPlacement)>{
                        [page](foundation::ui::View* view, ToolPanelPlacement placement)
                        { page->MountToolPanel(view, placement); }},
                    core::Function<void(ToolPanelPlacement)>{
                        [page](ToolPanelPlacement placement)
                        { page->MountToolPanel(nullptr, placement); }});
            }

            // Viewport column: [ viewport (toolbar + 3D) / bottom dock ] as a vertical split. The dock
            // drives the split's pane-collapse: the divider vanishes when collapsed and the stored ratio
            // survives expand/collapse. Starts collapsed (bar only; viewport full-height).
            m_viewportColumn = MakeRef<foundation::ui::toolkit::SplitView>(Allocator());
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

            // Clip-editing workflow wiring:
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
                    auto dialog = MakeRef<EntityPickerDialog>(editor::EditorRootAllocator(),
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
                        page->m_bottomDock->ActivateTab(kAnimationTab); // open the panel
                        const Guid* primary =
                            page->m_editContext->EntitySelection().Primary();
                        page->m_propAnimPanel->RequestEditClip(
                            instance.Id(), (primary != nullptr) ? *primary : Guid{});
                        return true;
                    });
            }

            // Page layout: [ hierarchy | (viewport-column | inspector) ].
            m_inspectorSplit = MakeRef<foundation::ui::toolkit::SplitView>(Allocator());
            m_inspectorSplit->SetSplitRatio(0.72f);
            m_inspectorSplit->SetPanes(m_viewportColumn.Get(), m_inspector.Get());
            m_hierarchySplit = MakeRef<foundation::ui::toolkit::SplitView>(Allocator());
            m_hierarchySplit->SetSplitRatio(0.2f);
            m_hierarchySplit->SetPanes(m_hierarchy.Get(), m_inspectorSplit.Get());
            m_content = m_hierarchySplit;

            m_router =
                MakeUnique<foundation::shell::InputRouter>(Allocator(), host.Shell()->Input());

            // Start framed on the origin (grid center), orbit pivot there, horizon level -
            // then where this scene was last left, if a page saved that (camera + selection +
            // the split positions).
            m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});
            RestoreViewState();
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
        // The requested viewport capture is recorded here, AFTER the scene renderer composed
        // the frame: RenderScene only adds the view, EndRendering writes the image.
        void OnAfterSceneRender(runtime::IApplicationHost& host,
                                foundation::graphics::FrameContext& frame) override;

        // Create-from-selection: capture the subtree as a prefab asset (under "Prefabs/",
        // named after the entity) and replace the original with an instance of it (one undo
        // group). The payload keeps the captured guids as its stable source ids.
        void CreatePrefabFromEntity(const Guid& entityId) override;

        // Apply-to-prefab entry point: rewrites the asset and rebuilds every instance, with
        // no undo - so it confirms first (mirror of RevertInstance).
        void ApplyInstanceToPrefab(const Guid& rootId) override;

        // Apply-to-prefab: the instance's CURRENT state becomes the template (source-id
        // keyed, so other instances' deltas stay valid), then every instance everywhere
        // rebuilds from it - including this one, which re-baselines to clean.
        void ApplyInstanceToPrefabNow(const Guid& rootId);

        // Revert-instance entry point: destructive + not undoable, so it confirms first.
        // Non-member children under the instance are destroyed too - the dialog says so.
        void RevertInstance(const Guid& rootId) override;

        // Revert-instance: discard this instance's deltas (respawn from the current template,
        // placement kept). Not undoable.
        void RevertInstanceNow(const Guid& rootId);

        // Spawn an instance under `parent` (nil = scene root) via the asset picker.
        void PickAndSpawnPrefab(const Guid& parent) override;

        // The asset changed under this page (apply-to-prefab from another page): wipe and
        // reload so the split-view prefab editor shows the new template. Unsaved edits are
        // never clobbered - the page just warns instead.
        void OnAssetExternallyModified() override;

        void OnSavedAs(foundation::content::Instance& instance) override;

        [[nodiscard]] Status Save() override;

        void OnClose() override;

        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }
        [[nodiscard]] EditorCamera& Camera() noexcept { return m_camera; }
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return *m_editContext; }
        [[nodiscard]] bool IsSimulating() const noexcept override { return m_isSimulating; }
        [[nodiscard]] bool IsPaused() const noexcept override { return m_isPaused; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override
        {
            return m_selectTool != nullptr ? &m_selectTool->Gizmos() : nullptr;
        }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override;
        [[nodiscard]] bool MarkersShown() const noexcept override { return m_showMarkers; }
        void SetMarkersShown(bool shown) override { m_showMarkers = shown; }
        [[nodiscard]] bool AnimationPanelShown() const noexcept override
        {
            return m_bottomDock.Get() != nullptr && m_bottomDock->IsExpanded() &&
                   m_bottomDock->ActiveTabId() == kAnimationTab;
        }
        void SetAnimationPanelShown(bool shown) override;
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return &m_camera; }
        bool FrameEntities(Span<const Guid> entities, bool ease) override;
        [[nodiscard]] Status RequestViewportCapture(StringView path) override
        {
            if (m_viewport.Get() == nullptr)
            {
                return Status{ErrorCode::NotSupported};
            }
            m_capture.Request(path);
            return Status{};
        }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override
        {
            return m_capture.State();
        }

    private:
        static constexpr f32 kFovY = 1.0472f; // must match OnRenderWindow's projection
        // The ground grid: a square this many metres wide in this many cells (one cell is what
        // the zoom readout reports).
        static constexpr f32 kGridSize = 20.0f;
        static constexpr i32 kGridDivisions = 20;

        // The measurement overlay while zooming, bottom-left: the focus distance, the grid cell
        // and a scale bar, at `opacity`.
        void DrawZoomReadout(render::debug::DebugDraw& dd, f32 opacity) const;

        // Camera ray through the mouse position, built from the camera basis (no matrix inverse).
        [[nodiscard]] bool MakeMouseRay(GizmoRay& out) const;

        // Feed the viewport tool manager a frame of input (editor.viewporttools). Runs EVERY
        // frame so the active tool's visuals track tree selections and undo/redo even while the
        // mouse is elsewhere (pointer flagged invalid then - pose-sync only). The page keeps
        // CAMERA POLICY: buttons are masked and the keyboard nulled while the camera owns the
        // mouse (Alt orbit / RMB fly / Tab-captured). Selection picking lives INSIDE the default
        // SelectTransformTool now; Simulate maps to input.editingLocked.
        [[nodiscard]] bool UpdateViewportTools(bool viewportActive, f32 deltaSeconds);
        // Present (view != null) or tear down (view == null) the active tool's panel at `placement`.
        // The ViewportToolPanelHost drives this on a tool change; the page owns the actual targets.
        void MountToolPanel(foundation::ui::View* view, ToolPanelPlacement placement);

        void DrawGizmos(render::debug::DebugDraw& dd);

        // The viewport's ephemeral post "show flags" popup (checkable). Toggling a flag re-renders
        // this page's viewport with that effect stripped; the scene asset is never touched.
        void ShowPostFlagsMenu(foundation::ui::View* anchor);

        // The viewport's debug-view popup: pick any render-graph texture to visualize in
        // this viewport ("Final" = off). Built from the renderer's last-frame inventory.
        void ShowDebugViewMenu(foundation::ui::View* anchor);

        // The viewport's overlays popup (checkable): the editor's own debug draws - grid,
        // entity markers, LOD overlay, colliders. Per-scene persisted (SceneViewPref).
        void ShowOverlaysMenu(foundation::ui::View* anchor);

        // A tool dropdown's popup: one checkable item per tool of the category; picking
        // activates it (or, for the active one, returns to the default tool).
        struct ToolMenu;
        void ShowToolMenu(const ToolMenu& toolMenu, foundation::ui::View* anchor);
        // Activate (on) or release (off) a palette tool, saying a refusal as a notice.
        void ToggleViewportTool(StringView id, bool on);

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
        void StartSimulation() override;

        /// Freeze/resume the running simulation (SimulationEnabled only - the Start/Stop
        /// system callbacks are for the big transitions, not the per-frame pause).
        void PauseSimulation(bool paused) override;

        /// Scene::Stop(), then restore the snapshot INTO THE SAME Scene instance (borrowed
        /// scene pointers stay valid; the guid-keyed selection re-resolves against restored
        /// entities - runtime-spawned ones drop out naturally). No-op if not simulating.
        void StopSimulation() override;

        void RefreshSimToolbar();

        // (split out so the lambda below can live next to its state)
        void ScenePage_OverlaysInit();
        void LoadViewPrefs(); // read this scene's saved overlay toggles (per-project, by guid)
        void SaveViewPrefs(); // persist the whole pref: toggles + camera + selection (toggle / close)
        void RestoreViewState(); // camera, selection, splits from the saved pref (after the scene loads)

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
        scene::SceneManager m_sceneManager{
            Allocator()}; // this page's OWN scene group (registered with m_scenes)
        foundation::messaging::EventBus m_pageEvents; // the page's run-scope bus (edit-mode Simulate)
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
        render::ViewDebugView
            m_debugView; // ephemeral viewport debug-view selection (not serialized)
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
        ui::toolkit::ToolbarToggle* m_animationToggle = nullptr; // the bottom dock's Animation tab
        // The Overlays dropdown's state (per-scene persisted): grid, LOD overlay, edit-time
        // physics collider gizmos, the origin cross on every entity.
        ui::toolkit::ToolbarMenuButton* m_overlaysButton = nullptr; // borrowed (toolbar-owned)
        bool m_showGrid = true;
        bool m_showLodOverlay = false;
        bool m_showColliders = false;
        bool m_showMarkers = true;
        bool m_showFps = false;
        // The FPS overlay samples half-second windows so the readout is legible, not a blur.
        f64 m_fpsWindowSeconds = 0.0;
        u32 m_fpsWindowFrames = 0;
        String m_fpsText;
        ZoomReadout m_zoomReadout; // shown for a moment after each wheel zoom
        bool m_viewGizmoPressed = false; // a press that began on the orientation gizmo, until release
        // The orientation gizmo: the axis tripod turning with the camera, the knob under the pointer lit.
        void DrawViewGizmo(render::debug::DebugDraw& dd) const;

        // Viewport tool palette: a toggle per lone non-default tool, a dropdown per category
        // with two or more (GroupViewportTools). Checking one activates it - the affordance
        // that docks the tool's panel.
        struct ToolToggle
        {
            ui::toolkit::ToolbarToggle* toggle = nullptr;
            String id;
        };
        Array<ToolToggle> m_toolToggles;
        struct ToolMenu
        {
            ui::toolkit::ToolbarMenuButton* button = nullptr; // borrowed (toolbar-owned)
            String category;
            Array<String> ids;
            String label; // what the button shows now ("Terrain" / "Terrain: Sculpt")
        };
        Array<ToolMenu> m_toolMenus;
        RefPtr<SceneInspectorView> m_inspector;
        // The GPU pick seam the tools see: RenderSubsystem::RequestPick keyed by THIS page's
        // viewport (the same key its RenderScene call carries), hits decoded to entity handles.
        class ViewportPicker final : public IViewportPicker
        {
        public:
            explicit ViewportPicker(SceneEditorPage& page) noexcept : m_page(&page) {}
            [[nodiscard]] u32 RequestPick(i32 x, i32 y, u32 width, u32 height) override;
            [[nodiscard]] bool TryTakePick(u32 request,
                                           Array<foundation::scene::EntityHandle>& hits) override;

        private:
            SceneEditorPage* m_page;
        };
        ViewportPicker m_picker{*this};
        ViewportToolManager m_viewportTools;             // declared after m_editContext (tools borrow it)
        SelectTransformTool* m_selectTool = nullptr;     // borrowed (manager-owned default tool)
        GizmoRendererRegistry m_componentGizmos;

        // The persistent in-scene property-animation editor, hosted as the "Animation" tab of the
        // bottom dock (a resizable vertical split below the viewport). Scene-page-owned for its whole
        // life, so an undo command can never outlive it. Ticked + overlay-drawn each frame from OnUpdate.
        RefPtr<PropertyAnimationPanel> m_propAnimPanel;
        // The viewport-tool PANEL seam (first consumer: terrain brushes). A persistent "Brush" tab
        // whose content the host swaps to the active tool's settings panel; empty when the active
        // tool has no panel (Select). Synced from OnUpdate.
        // The bottom dock tab a docked tool panel has, named for the tool's domain; empty when no
        // tool panel is docked.
        String m_dockedToolTab;
        // ViewportOverlay placement (experiment): a themed panel floating over the viewport (top-right),
        // holding the active tool's settings as a HUD. Visibility::Gone unless a ViewportOverlay panel
        // is mounted. The dock slot above is the Dock placement; MountToolPanel routes between them.
        RefPtr<foundation::ui::Panel> m_toolOverlay;
        // Float placement: a FloatingPanel (drag / resize / collapse / close) floating over the
        // viewport, holding the active tool's settings. Built in BuildToolFloat; close deactivates
        // the tool. Visibility::Gone unless a Float-placed panel is mounted.
        RefPtr<foundation::ui::AbsoluteLayout> m_toolFloatLayer; // fills the viewport; hosts m_toolFloat
        RefPtr<foundation::ui::toolkit::FloatingPanel> m_toolFloat;
        UniquePtr<ViewportToolPanelHost> m_toolPanelHost;
        RefPtr<foundation::ui::toolkit::BottomDock> m_bottomDock;   // the collapsible bottom strip
        RefPtr<foundation::ui::toolkit::SplitView> m_viewportColumn; // [viewport / bottom dock] vsplit
        RefPtr<foundation::ui::toolkit::SplitView> m_hierarchySplit; // [hierarchy | the rest]
        RefPtr<foundation::ui::toolkit::SplitView> m_inspectorSplit; // [viewport column | inspector]
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
        void BuildToolFloat(); // the Float placement: builds m_toolFloat (FloatingPanel)
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
        // The viewport capture (viewport_screenshot): armed by RequestViewportCapture, recorded
        // in OnAfterSceneRender off the composed colour target, completed in the next OnUpdate.
        ViewportCaptureRecorder m_capture;
        bool m_renderedThisFrame = false; // OnRenderWindow added the view this frame, at:
        u32 m_captureWidth = 0;
        u32 m_captureHeight = 0;
    };

    // === Factory + registration (the module's RegisterEditor entry point) ===

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

    inline void RegisterSceneEditor(EditorContext& context, runtime::IApplicationHost& host,
                                    ui::runtime::UIHost& uiHost,
                                    engine::runtime::DefaultApplication* embeddedApp = nullptr)
    {
        GlobalTypeRegistry().Register(scene::SceneDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::SceneDocument>();
        RegisterSceneViewSettingsType();
        GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::PrefabDocument>();

        if (context.Thumbnails() != nullptr)
        {
            RegisterSceneThumbnailGenerators(*context.Thumbnails(), context);
        }

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<SceneEditorPageFactory>(host, uiHost), editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<PrefabEditorPageFactory>(host, uiHost), editor::EditorRootAllocator()));


        // The scene editor's actions: the Scene menu, the chords, the page toolbar and the
        // hierarchy's menus are served from these; the palette and the MCP bridge read them.
        RegisterSceneEditorActions(context);

        // The scene editor's MCP tools (selection, simulate) - served by the editor's MCP host
        // over whichever scene page a call addresses.
        {
            EditorContext* ctx = &context;
            context.RegisterMcpToolContribution([ctx](foundation::mcp::McpServer& server)
                                                { RegisterSceneLiveTools(server, *ctx); });
            // Play in editor for agents: start, stop, state and capture, per Game tab.
            context.RegisterMcpToolContribution([ctx](foundation::mcp::McpServer& server)
                                                { RegisterPieTools(server, *ctx); });
        }

        // Model imports: generate/refresh the hierarchy prefab beside the manifest (the
        // "Generate prefab" import option). Lives here - not in the importer - because it
        // needs scene machinery the importer library (linked by the headless cooker) never
        // links. Re-import reuses the prefab's guid, so placed instances rebuild in every
        // open scene and an open prefab page refreshes like any external asset change.
        EditorContext* editorContext = &context;
        runtime::IApplicationHost* appHost = &host;

        // Play-in-editor: the singleton Game tab (player behavior in-process).
        context.GamePageFactory = [editorContext, appHost, appUiHost = &uiHost, embeddedApp](
                                      bool newInstance, StringView pieId) -> UniquePtr<EditorPage>
        {
            // Reuse the primary instance for the normal Play; spin up an extra for "Play New Instance".
            engine::runtime::GameInstance* instance =
                newInstance ? embeddedApp->CreateInstance() : &embeddedApp->Instance();
            return UniquePtr<EditorPage>(
                editor::EditorRootAllocator().New<GameEditorPage>(*editorContext, *appHost, *appUiHost,
                                                       embeddedApp, instance, pieId),
                editor::EditorRootAllocator());
        };

        // Export seam: scene/prefab TEXT sources transcode to the binary wire on the main
        // thread before the pack job. The scratch scene is assembled from the full
        // composition - a hand-listed set would silently drop component types.
        context.SceneStreamStager = [](foundation::content::Instance& instance,
                                       Array<byte>& out) -> bool
        {
            if (!engine::IsSceneLike(instance))
            {
                return false;
            }
            UniquePtr<IStream> stream = instance.ReadData(engine::kSceneStream);
            if (stream.Get() == nullptr)
            {
                return false;
            }
            // A transient scratch scene assembled from the full composition (no component type
            // silently skipped by a hand-listed set).
            scene::Scene scratch(editor::EditorRootAllocator(), u8"__export_transcode");
            engine::AddAllSceneManagers(scratch);
            const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*stream, scratch, /*includeSettings=*/isScene);
            if (!bytes.HasValue())
            {
                return false;
            }
            out = Move(bytes.Value());
            return true;
        };

        // Export reachability seam: the assets a scene or prefab references, so the export
        // closure can chase the scene->asset edges the cook's read-dep graph can't see. MAIN-THREAD
        // only.
        context.SceneRefScanner =
            [](foundation::content::Instance& instance, foundation::content::ContentDatabase& db,
               Array<Guid>& outResources, Array<Guid>& outPrefabs) -> bool
        {
            if (!engine::IsSceneLike(instance))
            {
                return false;
            }
            return engine::ScanSceneReferences(editor::EditorRootAllocator(), instance, db,
                                               outResources, outPrefabs);
        };

        context.AddImportListener(
            [editorContext, appHost](foundation::content::Instance& instance,
                                     const pipeline::ImportOptions* options)
            {
                // A model's prefab and scene, the pipeline's generation every host runs, then
                // what only the editor does with them.
                pipeline::ModelPrefabResult generated;
                pipeline::ModelPrefabResult generatedScene;
                if (!pipeline::GenerateForImport(editor::EditorRootAllocator(), instance, options,
                                                 generated, generatedScene))
                {
                    return;
                }
                // No options object = the default fresh import, which generates the prefab only.
                const auto* modelOptions =
                    Cast<pipeline::ModelImportOptions>(const_cast<pipeline::ImportOptions*>(options));
                const bool wantPrefab = modelOptions == nullptr || modelOptions->generatePrefab;
                const bool wantScene = modelOptions != nullptr && modelOptions->generateScene;

                if (wantPrefab)
                {
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
                                (void)editorContext->NotifyAssetExternallyModified(prefabId);
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
                            (void)editorContext->NotifyAssetExternallyModified(
                                generatedScene.instance->Id());
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

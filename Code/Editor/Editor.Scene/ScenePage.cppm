// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :page partition.
//
// SceneEditorPage: the scene (and prefab) document editor, on the GUI from markup
// (Data/Editor/Scene.xml; Documentation/Specs/editor-gui.md, 5.3, layer 2). Each page owns its own
// live scene, in its own scene manager (several pages are open at once; everything scene-scoped is
// per page), and is made of parts: its viewport (the renderer drawing the scene into it, the
// camera, picking, the capture), its overlays, Simulate, the prefab flows and the view state kept
// per scene, each a partition of its own. The page holds them together: it loads the scene, edits
// it through its edit context and its tools, saves it, follows a change made elsewhere, and says
// on the page when the scene did not load (and then refuses to save over it). Its bar's Save, Undo
// and Redo act on this page; its hierarchy (layer 3) is the scene's tree in its left pane, its
// inspector (layer 4) the selection's components in its right.

module;
#include "Core/Prelude.h"

export module editor.scene:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.scene.resource;
import foundation.render;
import engine.render;
import engine.ui;
import engine.gui;
import foundation.messaging;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import editor.core;
import editor.app;
import editor.camera;
import editor.viewporttools;
import editor.propertyanimation; // the animation panel under the viewport
import :edit;
import :gizmo;
import :tools;
import :component_gizmos;
import :scene_page_interface;
import :scene_viewport;
import :scene_overlays;
import :scene_view_state;
import :scene_simulation;
import :prefab_flows;
import :hierarchy;
import :inspector;
import :entity_picker_window;
import :scene_bar;
import :scene_hud;
import :scene_camera_preview;
import :scene_tools;

using namespace foundation::core;

export namespace editor
{
    class SceneEditorPage final : public app::UIEditorPage, public ISceneEditorPage
    {
    public:
        static constexpr const char8_t* kPath = u8"Editor/Scene.xml";

        /// What a page asks of the editor.
        struct Services
        {
            foundation::runtime::IApplicationHost* host = nullptr;
            foundation::gui::GuiHost* gui = nullptr; // what draws its viewport (null: none, a test)
            /// The page's markup by its path under the data root.
            Function<RefPtr<foundation::gui::Node>(StringView path)> loadMarkup;
            /// The asset picker and the rest an asset slot asks (null: no picking, a test).
            const app::ResourceRefServices* refs = nullptr;
            /// The type icons the hierarchy and the inspector show by (null: none).
            EditorIcons* icons = nullptr;
            /// Opens the picker for a request (the inspector's Add Component).
            Function<void(app::PickerRequest request)> pick;
            /// Opens Create Asset (a title, a suggested name, a group to start; null: the top),
            /// told the group and the name chosen (the animation panel's New Clip). Null: none.
            Function<void(StringView title, StringView suggested, foundation::content::Group* group,
                          Function<void(foundation::content::Group&, StringView)> create)>
                createAsset;
        };

        SceneEditorPage(EditorContext& context, Services services, foundation::content::Instance& instance);
        ~SceneEditorPage() override = default;

        // === UIEditorPage ===
        [[nodiscard]] RefPtr<foundation::gui::Node> Content() override { return m_content; }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        // Inside the app's one renderer bracket, before the GUI draws.
        void OnRenderWindow(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) override;
        // A requested capture, recorded once the renderer composed the frame.
        void OnAfterSceneRender(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // === EditorPage ===
        [[nodiscard]] Status Save() override;
        /// The clip its animation panel edits, while unsaved: asked about beside the scene.
        void ListUnsavedBesides(Array<String>& out) const override;
        Status SaveBesides() override;
        void OnSavedAs(foundation::content::Instance& instance) override;
        /// The asset changed under this page (apply-to-prefab from another page): read again, its
        /// history gone; unsaved edits are never clobbered, the page warns instead.
        void OnAssetExternallyModified() override;

        // === ISceneEditorPage ===
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return *m_editContext; }
        void StartSimulation() override;
        void StopSimulation() override;
        void PauseSimulation(bool paused) override;
        [[nodiscard]] bool IsSimulating() const noexcept override { return m_simulation.IsRunning(); }
        [[nodiscard]] bool IsPaused() const noexcept override { return m_simulation.IsPaused(); }
        [[nodiscard]] ViewportToolManager* ViewportTools() noexcept override { return &m_viewportTools; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override { return m_selectTool != nullptr ? &m_selectTool->Gizmos() : nullptr; }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override { return m_viewport->CameraOwnsInput(); }
        [[nodiscard]] bool MarkersShown() const noexcept override { return m_overlays.settings.showMarkers; }
        void SetMarkersShown(bool shown) override;
        /// The property animation panel under the viewport (X6 layer 9): a clip of its own, never
        /// the scene's history or its unsaved mark.
        [[nodiscard]] bool AnimationPanelShown() const noexcept override { return m_animationShown; }
        void SetAnimationPanelShown(bool shown) override;
        void EditAnimationClip(const Guid& clip, const Guid& entity) override;
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return &m_viewport->Camera(); }
        bool FrameEntities(Span<const Guid> entities, bool ease) override;
        [[nodiscard]] Status RequestViewportCapture(StringView path) override { return m_viewport->RequestCapture(path, false); }
        [[nodiscard]] Status RequestProjectThumbnail() override;
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override { return m_viewport->LastCapture(); }
        bool BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource> source) override
        {
            m_viewport->BeginScripted(Move(source));
            return true;
        }
        void EndScriptedInput() override { m_viewport->EndScripted(); }
        [[nodiscard]] f64 ScriptedInputTime() const noexcept override { return m_viewport->ScriptedTime(); }
        [[nodiscard]] Float2 ViewportPixels() const noexcept override
        {
            return Float2{static_cast<f32>(m_viewport->View().RenderWidth()), static_cast<f32>(m_viewport->View().RenderHeight())};
        }
        void CreatePrefabFromEntity(const Guid& entity) override { m_prefabs->CreateFromEntity(entity); }
        void PickAndSpawnPrefab(const Guid& parent) override { m_prefabs->PickAndSpawn(parent); }
        void ApplyInstanceToPrefab(const Guid& root) override { m_prefabs->Apply(root); }
        void RevertInstance(const Guid& root) override { m_prefabs->Revert(root); }

        // === For tests ===
        [[nodiscard]] foundation::scene::Scene* ScenePtr() const noexcept { return m_scene; }
        /// Whether the scene failed to load (and what the page says).
        [[nodiscard]] bool LoadFailed() const noexcept { return m_loadFailed; }
        [[nodiscard]] bool IsPrefab() const noexcept { return m_isPrefab; }
        [[nodiscard]] SceneOverlaySettings& Overlays() noexcept { return m_overlays.settings; }
        [[nodiscard]] SceneBar* Bar() const noexcept { return m_bar.Get(); }
        [[nodiscard]] SceneHud* Hud() const noexcept { return m_hud.Get(); }
        [[nodiscard]] SceneCameraPreview* CameraPreview() const noexcept { return m_cameraPreview.Get(); }
        [[nodiscard]] SceneTools* ToolsView() const noexcept { return m_tools.Get(); }
        [[nodiscard]] SceneViewport& Viewport() noexcept { return *m_viewport; }
        [[nodiscard]] SceneHierarchyView* Hierarchy() noexcept { return m_hierarchy.Get(); }
        [[nodiscard]] SceneInspectorView* Inspector() noexcept { return m_inspector.Get(); }
        [[nodiscard]] PropertyAnimationPanel* AnimationPanel() const noexcept { return m_animation.Get(); }
        /// Opens the entity picker over the page's GUI for `request` (made the first time); false
        /// with no markup, no edit context, or the page in no GUI.
        bool PickEntity(EntityPickRequest request);
        /// Whether its content is shown in the window now (its tab the one in front).
        [[nodiscard]] bool IsOnScreen() const;
        [[nodiscard]] EntityPickerWindow* EntityPicker() const noexcept { return m_entityPicker.Get(); }
        [[nodiscard]] foundation::gui::MessageBox* OpenConfirm() const noexcept { return m_confirm && m_confirm->IsOpen() ? m_confirm.Get() : nullptr; }

    private:
        void Build(foundation::content::Instance& instance);
        void BuildTools(EditorContext& context);
        // The viewport tools' frame of input (picking lives in the select tool). Runs every frame so
        // the active tool follows selections and undo while the mouse is elsewhere (the pointer then
        // flagged invalid). The page keeps the camera's policy: no buttons and no keyboard for the
        // tools while the camera has the mouse; Simulate is editing locked.
        void UpdateTools(bool viewportActive, f32 deltaSeconds);
        void SaveViewState();
        void RestoreViewState();
        [[nodiscard]] bool IsProjectDefaultScene() const;
        void Confirm(StringView title, StringView text, StringView verb, Function<void()> yes);
        void Later(Function<void()> work);
        void BuildAnimationPanel(EditorContext& context);
        // The page's prompt for the panel's unsaved clip: Save, Discard (as saved) or Cancel.
        void AskAboutClip(Function<void()> proceed);
        // New Clip: named and placed (Create Asset), opened, given to the entity's animator when it
        // has none.
        void NewClip();
        void GiveClipToAnimator(const Guid& entity, const Guid& clip);
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        EditorContext* m_context;
        Services m_services;
        engine::scene::SceneSubsystem* m_scenes = nullptr; // the context's (registry and tick)
        foundation::scene::SceneManager m_sceneManager{Allocator()}; // this page's own scene group
        foundation::messaging::EventBus m_pageEvents; // the page's run scope (edit-mode Simulate)
        String m_title;
        foundation::scene::Scene* m_scene = nullptr; // the scene manager's
        bool m_loadFailed = false;
        bool m_isPrefab = false;
        UniquePtr<SceneEditContext> m_editContext; // every edit, as this page's commands
        UniquePtr<SceneViewport> m_viewport;
        SceneOverlays m_overlays;
        SceneSimulation m_simulation;
        UniquePtr<PrefabFlows> m_prefabs;
        UniquePtr<SceneHierarchyView> m_hierarchy;
        UniquePtr<SceneInspectorView> m_inspector;
        UniquePtr<EntityPickerWindow> m_entityPicker; // an entity slot's pick, made the first time
        ViewportToolHostContext m_toolHost; // what the tools and their panels were made with
        ViewportToolManager m_viewportTools;         // after m_editContext (the tools borrow it)
        SelectTransformTool* m_selectTool = nullptr; // the manager's default tool
        GizmoRendererRegistry m_componentGizmos;
        foundation::render::ViewPostOverride m_postOverride; // the viewport's post show flags (not saved)
        foundation::render::ViewDebugView m_debugView;       // the viewport's debug view (not saved)
        RefPtr<foundation::gui::Node> m_content;
        UniquePtr<SceneBar> m_bar;
        UniquePtr<SceneHud> m_hud; // over the viewport
        UniquePtr<SceneCameraPreview> m_cameraPreview; // the HUD's bottom end: what a camera sees
        UniquePtr<SceneTools> m_tools; // the viewport tools on the bar, the active one's panel
        UniquePtr<PropertyAnimationPanel> m_animation; // under the viewport (null: no markup)
        RefPtr<foundation::gui::Node> m_animationPane; // held: out of the splitter while hidden, nothing else holds it
        bool m_animationShown = false;
        u64 m_openClipInterceptor = 0; // takes a clip's open into the panel while on screen
        RefPtr<foundation::gui::MessageBox> m_confirm;
        // The launcher's picture of the project: the default scene's view, taken once it settled
        // when there is none yet, and on every save of it.
        bool m_thumbnailChecked = false;
        f32 m_thumbnailWait = 0.0f;
    };
}

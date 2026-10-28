// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :page partition, the page's body (Documentation/Specs/editor-gui.md, 5.3,
// layer 2). See ScenePage.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.scene;

import foundation.core;
import foundation.content;
import foundation.shell;
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
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import editor.core;
import editor.app;
import editor.camera;
import editor.viewporttools;
import editor.propertyanimation; // the animation panel under the viewport
import propertyanimation.pipeline; // CreatePropertyAnimationClip (the panel's New)
import engine.animation;          // PropertyAnimatorComponent (a new clip given to it)
import foundation.propertyanimation.resource;
import :view_settings;
import :view_gizmo;
import :edit;
import :gizmo;
import :tools;
import :component_gizmos;
import :scene_page_interface;
import :scene_loading;
import :settings_profiles;
import :actions;
import :scene_viewport;
import :scene_overlays;
import :scene_view_state;
import :scene_simulation;
import :prefab_flows;
import :hierarchy;
import :inspector;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;
namespace runtime = foundation::runtime;

namespace editor
{
    SceneEditorPage::SceneEditorPage(EditorContext& context, Services services, foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()), m_context(&context), m_services(Move(services)), m_title(instance.Name())
    {
        // The id now, before the view state is read: the per-scene prefs are keyed by it.
        SetInstanceId(instance.Id());
        Provide<ISceneEditorPage>(*this); // what this page lets others act through
        m_isPrefab = instance.TypeName() == StringView(u8"PrefabDocument");
        Build(instance);
    }

    void SceneEditorPage::Build(foundation::content::Instance& instance)
    {
        EditorContext& context = *m_context;
        runtime::IApplicationHost* host = m_services.host;
        SceneViewport::Services view;
        if (host != nullptr)
        {
            m_scenes = host->Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            view.render = host->Ctx().GetSubsystem<engine::render::RenderSubsystem>();
            view.gameUI = host->Ctx().GetSubsystem<engine::ui::UISubsystem>();
            view.gameGui = host->Ctx().GetSubsystem<engine::gui::GuiSubsystem>();
        }
        m_viewport = MakeUnique<SceneViewport>(Allocator(), Allocator(), view);

        // Its own live scene in its own scene manager, registered with the subsystem so it ticks.
        if (m_scenes != nullptr)
        {
            m_scenes->RegisterManager(&m_sceneManager);
            // The page is edit-mode Simulate's run scope: its bus is set before the scene is made,
            // so systems binding as it is assembled see it; OnUpdate drains it once a frame.
            m_sceneManager.SetSceneEventBus(&m_pageEvents);
            m_scene = m_sceneManager.CreateScene(instance.Name());
            m_scene->SetSimulationEnabled(false); // edit mode is frozen; Simulate un-freezes it
            SceneLoadTimes times;
            const Status loaded = LoadEditorScene(context, instance, *m_scene, &times);
            if (loaded.IsOk())
                LOG_INFO(u8"Editor", u8"opened scene '{}': parse {} ms, bind {} ms, prefabs {} ms (UI thread)", m_title, times.parseMs, times.bindMs,
                         times.prefabMs);
            else if (loaded.Code() == ErrorCode::NotFound)
                LOG_INFO(u8"Editor", u8"new scene '{}' (no scene stream yet)", m_title);
            else
            {
                LOG_ERROR(u8"Editor", u8"scene '{}' failed to load", m_title);
                m_loadFailed = true;
            }
        }

        if (m_scene != nullptr)
        {
            m_editContext = MakeUnique<SceneEditContext>(Allocator(), *m_scene, Commands());
            m_editContext->SetResources(context.Resources());
            // A settings edit that lands in a profile (the block's source is a profile) is written
            // to the profile's asset by the save flow, like any live asset edit.
            {
                EditorContext* profileContext = &context;
                SceneEditContext* profileEdit = m_editContext.Get();
                m_editContext->OnSettingsProfileEdited = [profileContext, profileEdit](const TypeInfo* type, const Guid& profile)
                {
                    if (scene::SceneSystem* system = profileEdit->FindSystemBySettingsType(type))
                        QueueSettingsProfileEdit(*profileContext, *system, profile);
                };
            }
            EditorContext* resolverContext = &context;
            m_editContext->SetPrefabResolver(scene::PrefabPayloadResolver{[resolverContext](const Guid& prefabId) -> UniquePtr<IStream>
                                                                          {
                                                                              if (resolverContext->Project() == nullptr)
                                                                                  return UniquePtr<IStream>{};
                                                                              foundation::content::Instance* prefab =
                                                                                  resolverContext->Project()->SourceDb().GetInstance(prefabId);
                                                                              return prefab != nullptr ? prefab->ReadData(u8"scene") : UniquePtr<IStream>{};
                                                                          }});
            BuildTools(context);
            RegisterBuiltinGizmoRenderers(m_componentGizmos);

            PrefabFlows::Services flows;
            flows.context = &context;
            flows.edit = m_editContext.Get();
            flows.scenes = m_scenes;
            SceneEditorPage* self = this;
            flows.pageAsset = [self]() { return self->InstanceId(); };
            flows.confirm = [self](StringView title, StringView text, StringView verb, Function<void()> yes) { self->Confirm(title, text, verb, Move(yes)); };
            flows.later = [self](Function<void()> work) { self->Later(Move(work)); };
            if (m_services.refs != nullptr && m_services.refs->Pick)
                flows.pickAsset = [self](Span<const String> types, Function<void(const Guid&)> chosen)
                { self->m_services.refs->Pick(types, Guid{}, Move(chosen)); };
            m_prefabs = MakeUnique<PrefabFlows>(Allocator(), Move(flows));
        }

        // The page from its markup: the bar over the hierarchy, the viewport and the inspector.
        m_content = m_services.loadMarkup ? m_services.loadMarkup(StringView(kPath)) : RefPtr<gui::Node>{};
        if (!m_content)
            m_content = MakeRef<gui::Panel>(Allocator()); // no markup: still a page, with its viewport
        gui::Node* pane = Find<gui::Node>(u8"scene-viewport");
        (void)(pane != nullptr ? pane : m_content.Get())->AddChildAt(m_viewport->ViewNode().Get(), 0);
        m_viewport->HostIn(m_services.gui);
        if (m_editContext)
        {
            // The HUD over the viewport: the chips, the stats and the view gizmo, the tool's status.
            SceneHud::Services hud;
            hud.context = &context;
            hud.edit = m_editContext.Get();
            hud.camera = &m_viewport->Camera();
            hud.overlays = &m_overlays;
            hud.post = &m_postOverride;
            hud.debugView = &m_debugView;
            hud.tools = &m_viewportTools;
            hud.selectTool = m_selectTool;
            SceneEditorPage* self = this;
            hud.gizmos = [self]() { return self->Gizmos(); };
            if (engine::render::RenderSubsystem* renderer = m_viewport->Renderer())
                hud.debugResources = [renderer](Array<foundation::render::DebugResourceInfo>& out) { renderer->GetDebugResources(out); };
            if (m_services.pick)
                hud.pick = [self](app::PickerRequest request) { self->m_services.pick(Move(request)); };
            hud.overlaysChanged = [self]() { self->SaveViewState(); };
            m_hud = MakeUnique<SceneHud>(Allocator(), Allocator(), Move(hud), [self](StringView id) { return self->Find<gui::Node>(id); });
        }
        if (m_editContext)
            if (gui::Node* viewportPane = Find<gui::Node>(u8"scene-viewport"))
            {
                // The camera preview at the HUD's bottom end: the selected or pinned camera's view.
                SceneCameraPreview::Services preview;
                preview.edit = m_editContext.Get();
                preview.icons = m_services.icons;
                preview.render = m_viewport->Renderer();
                m_cameraPreview = MakeUnique<SceneCameraPreview>(Allocator(), Allocator(), preview, *viewportPane);
                m_cameraPreview->HostIn(m_services.gui);
            }
        if (m_editContext)
        {
            // The viewport tools on the bar, and the active one's panel over the view.
            SceneTools::Services tools;
            tools.context = &context;
            tools.page = this;
            tools.tools = &m_viewportTools;
            tools.hostContext = m_toolHost;
            tools.barEnd = Find<gui::Node>(u8"scene-tools-end");
            tools.pane = Find<gui::Node>(u8"scene-viewport");
            tools.zone = Find<gui::Node>(u8"hud-top-start");
            SceneEditorPage* self = this;
            tools.placeChanged = [self]() { self->SaveViewState(); };
            m_tools = MakeUnique<SceneTools>(Allocator(), Allocator(), Move(tools));
        }
        {
            SceneEditorPage* self = this;
            m_bar = MakeUnique<SceneBar>(Allocator(), Allocator(), context.Actions(), *this,
                                         [self](StringView id) { return self->Find<gui::ButtonBase>(id); },
                                         Find<gui::Label>(u8"scene-simulate-state"), [self]() { return self->Gizmos(); });
        }
        // A prefab is marked as one: the same page edits both.
        if (gui::Node* kind = Find<gui::Node>(u8"scene-kind"))
            kind->SetDisplayed(m_isPrefab);
        // A scene that did not load says so where it would show, not only in the log.
        if (gui::Node* state = Find<gui::Node>(u8"scene-state"))
            state->SetDisplayed(m_loadFailed || m_scene == nullptr);
        if (auto* text = Find<gui::Label>(u8"scene-state-text"); text != nullptr && m_scene == nullptr)
            text->SetText(u8"The editor has no scenes to show it in.");

        // The hierarchy in its pane: the scene's tree, the page's selection.
        if (m_editContext)
            if (gui::Node* hierarchyPane = Find<gui::Node>(u8"scene-hierarchy"))
            {
                SceneHierarchyView::Services tree;
                tree.context = &context;
                tree.page = this;
                tree.icons = m_services.icons;
                tree.sceneIcon = m_isPrefab ? StringView(u8"Editor/Icons/Types/PrefabDocument.svg") : StringView(u8"Editor/Icons/Types/SceneDocument.svg");
                SceneEditorPage* self = this;
                tree.sceneName = [self]() { return self->m_title.AsView(); };
                tree.frame = [self](const Guid& entity) { (void)self->FrameEntities(Span<const Guid>(&entity, 1u), true); };
                tree.spawnPrefab = [self](const Guid& prefab, const Guid& parent) { return self->m_prefabs && self->m_prefabs->Spawn(prefab, parent); };
                m_hierarchy = MakeUnique<SceneHierarchyView>(Allocator(), Allocator(), *m_editContext, Move(tree), *hierarchyPane);
            }

        // The inspector in its pane: the selection's entities and components.
        if (m_editContext)
            if (gui::Node* inspectorPane = Find<gui::Node>(u8"scene-inspector"))
            {
                SceneInspectorView::Services inspector;
                inspector.context = &context;
                inspector.page = this;
                inspector.icons = m_services.icons;
                inspector.refs = m_services.refs;
                inspector.pick = m_services.pick ? [self = this](app::PickerRequest request) { self->m_services.pick(Move(request)); }
                                                 : Function<void(app::PickerRequest)>{};
                SceneEditorPage* self = this;
                inspector.pickEntity = [self](EntityPickRequest request) { (void)self->PickEntity(Move(request)); };
                inspector.simulating = [self]() { return self->m_simulation.IsRunning(); };
                inspector.sceneSelected = [self]() { return self->m_hierarchy && self->m_hierarchy->SceneSelected(); };
                inspector.sceneIcon = m_isPrefab ? StringView(u8"Editor/Icons/Types/PrefabDocument.svg") : StringView(u8"Editor/Icons/Types/SceneDocument.svg");
                inspector.sceneName = [self]() { return self->m_title.AsView(); };
                m_inspector = MakeUnique<SceneInspectorView>(Allocator(), Allocator(), *m_editContext, Move(inspector), *inspectorPane);
            }

        BuildAnimationPanel(context);
        m_overlays.settings = SceneViewState::LoadOverlays(context.ProjectEditorSettings(), InstanceId(), m_overlays.settings);
        RestoreViewState();
    }

    void SceneEditorPage::BuildAnimationPanel(EditorContext& context)
    {
        // Under the viewport, its pane out of the splitter until shown (a pane that goes gives its
        // share to the viewport).
        m_animationPane = RefPtr<gui::Node>(Find<gui::Node>(u8"scene-animation"));
        if (!m_animationPane || m_scene == nullptr || !m_editContext || !m_services.loadMarkup)
            return;
        RefPtr<gui::Node> content = m_services.loadMarkup(PropertyAnimationPanel::kPath);
        if (!content)
            return;
        m_animationPane->AddChild(content.Get());
        m_animation = MakeUnique<PropertyAnimationPanel>(Allocator(), context, *m_scene, content);
        SceneEditorPage* self = this;
        m_animation->SelectedEntity = [self]() -> Guid
        {
            const Guid* primary = self->m_editContext->EntitySelection().Primary();
            return primary != nullptr ? *primary : Guid{};
        };
        m_animation->GuardUnsaved = [self](Function<void()> proceed) { self->AskAboutClip(Move(proceed)); };
        // Hidden: what it posed goes back, its clip stays open in it (unsaved or not) for next time.
        m_animation->RequestClose = [self]() { self->Later([self]() { self->SetAnimationPanelShown(false); }); };
        if (m_services.refs != nullptr && m_services.refs->Pick)
            m_animation->RequestOpenClip = [self]()
            {
                const String types[] = {String(u8"PropertyAnimationClipAsset")};
                self->m_services.refs->Pick(Span<const String>(types, 1u), self->m_animation->Document().ClipId(),
                                            [self](const Guid& picked)
                                            {
                                                if (!picked.IsNil())
                                                    self->m_animation->RequestEditClip(picked, Guid{});
                                            });
            };
        // New: the clip named and placed in Create Asset (Animations and the entity's name to
        // start), then opened; an animator on the entity with no clip yet plays it.
        m_animation->RequestNewClip = [self]() { self->AskAboutClip([self]() { self->NewClip(); }); };
        // Pick: the page's entity picker, the bound entity lit.
        m_animation->RequestPickEntity = [self](const Guid& current, Function<void(const Guid&)> chosen)
        {
            EntityPickRequest request;
            request.current = current;
            request.chosen = Move(chosen);
            (void)self->PickEntity(Move(request));
        };
        // A clip opened from the browser or a slot while this page is on screen: into the panel,
        // shown, bound to the selected entity (the old page's), not a page of its own.
        m_openClipInterceptor = context.AddOpenAssetInterceptor([self](foundation::content::Instance& instance) -> bool
                                                                {
                                                                    if (instance.TypeName() != StringView(u8"PropertyAnimationClipAsset") || !self->IsOnScreen())
                                                                        return false;
                                                                    self->SetAnimationPanelShown(true);
                                                                    const Guid* primary = self->m_editContext->EntitySelection().Primary();
                                                                    self->m_animation->RequestEditClip(instance.Id(), primary != nullptr ? *primary : Guid{});
                                                                    return true;
                                                                });
        m_animationPane->GetParent()->RemoveChild(m_animationPane.Get()); // hidden until shown
    }

    void SceneEditorPage::NewClip()
    {
        foundation::content::Group* top = m_context->Project() != nullptr ? m_context->Project()->SourceDb().RootGroup() : nullptr;
        if (top == nullptr)
            return;
        foundation::content::Group* animations = nullptr;
        for (foundation::content::Group* child : top->Groups())
            if (child->Name() == StringView(u8"Animations"))
                animations = child;
        const Guid entity = m_animation->BoundEntity();
        const scene::EntityHandle handle = entity.IsNil() ? scene::EntityHandle{} : m_scene->FindEntity(entity);
        // A name not taken where it starts.
        const String base = handle.IsAssigned() ? Format(u8"{} Clip", m_scene->GetEntityName(handle)) : String(u8"Clip");
        const String suggested = (animations != nullptr ? animations : top)->UniqueInstanceName(base.AsView());
        SceneEditorPage* self = this;
        const auto make = [self, entity](foundation::content::Group& group, StringView name)
        {
            foundation::content::Instance* made = pipeline::CreatePropertyAnimationClip(&group, name);
            if (made == nullptr)
            {
                self->m_context->Notify(NoticeKind::Warning, u8"The clip could not be made; the Console says why.");
                return;
            }
            (void)self->m_animation->LoadClip(made->Id());
            self->GiveClipToAnimator(entity, made->Id());
        };
        if (m_services.createAsset)
        {
            m_services.createAsset(u8"New Clip", suggested.AsView(), animations, make);
            return;
        }
        // No dialog (a test): Animations, as suggested.
        if (animations == nullptr)
            animations = top->CreateGroup(u8"Animations");
        if (animations != nullptr)
            make(*animations, suggested.AsView()); // made unique there
    }

    void SceneEditorPage::GiveClipToAnimator(const Guid& entity, const Guid& clip)
    {
        // Only an animator with no clip: one that plays another keeps it (this is another clip).
        auto* animators = m_scene->GetSystem<engine::animation::PropertyAnimatorComponentManager>();
        const scene::EntityHandle handle = entity.IsNil() ? scene::EntityHandle{} : m_scene->FindEntity(entity);
        engine::animation::PropertyAnimatorComponent* animator = animators != nullptr && handle.IsAssigned() ? animators->Get(handle) : nullptr;
        if (animator == nullptr || !animator->clip.id.IsNil())
            return;
        m_editContext->SetComponentResourceRef<foundation::propertyanimation::PropertyAnimationClipResource>(
            entity, &TypeOf<engine::animation::PropertyAnimatorComponent>(), "clip", clip, m_context->Resources());
    }

    void SceneEditorPage::AskAboutClip(Function<void()> proceed)
    {
        if (!m_animation || !m_animation->Document().IsDirty())
        {
            proceed();
            return;
        }
        gui::Node* root = m_content ? m_content->GetRootNode() : nullptr;
        if (root == nullptr)
            return; // nothing to ask over: the clip stays
        auto box = MakeRef<gui::MessageBox>(Allocator());
        box->Configure(u8"Unsaved clip", Format(u8"The clip '{}' has unsaved changes.", m_animation->Document().Name()).AsView(), gui::MessageBox::Buttons::Ok);
        box->ClearButtons();
        (void)box->AddButton(u8"Discard", u8"discard", gui::DialogRole::Destructive);
        (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
        (void)box->AddButton(u8"Save", u8"save", gui::DialogRole::Primary);
        SceneEditorPage* self = this;
        gui::MessageBox* raw = box.Get();
        (void)box->AddEventListener(gui::EventType::Changed, [self, raw, onProceed = Move(proceed)](const gui::Event&)
                                    {
                                        const StringView answer = raw->GetResultName().AsView();
                                        if (answer == StringView(u8"save") && !self->m_animation->Save().IsOk())
                                            return; // it said why
                                        if (answer == StringView(u8"save") || answer == StringView(u8"discard"))
                                            onProceed();
                                    });
        box->OpenModal(*root);
        m_confirm = box;
    }

    void SceneEditorPage::SetAnimationPanelShown(bool shown)
    {
        if (!m_animation || !m_animationPane || shown == m_animationShown)
            return;
        gui::Splitter* center = Find<gui::Splitter>(u8"scene-center");
        if (center == nullptr)
            return;
        m_animationShown = shown;
        if (shown)
        {
            center->AddChild(m_animationPane.Get());
            const f32 weights[] = {0.68f, 0.32f};
            center->SetWeights(Span<const f32>(weights, 2u));
        }
        else
        {
            m_animation->StopPreview(); // what it posed goes back with it
            center->RemoveChild(m_animationPane.Get());
        }
    }

    void SceneEditorPage::EditAnimationClip(const Guid& clip, const Guid& entity)
    {
        if (!m_animation)
            return;
        SetAnimationPanelShown(true);
        if (!clip.IsNil())
            m_animation->RequestEditClip(clip, entity);
        else if (!entity.IsNil())
            m_animation->BindEntity(entity);
    }

    void SceneEditorPage::ListUnsavedBesides(Array<String>& out) const
    {
        if (m_animation && m_animation->Document().IsDirty())
            out.PushBack(Format(u8"The clip '{}'", m_animation->Document().Name()));
    }

    Status SceneEditorPage::SaveBesides()
    {
        return m_animation && m_animation->Document().IsDirty() ? m_animation->Save() : Status{};
    }

    void SceneEditorPage::BuildTools(EditorContext& context)
    {
        auto selectTool = MakeUnique<SelectTransformTool>(Allocator(), *m_editContext);
        m_selectTool = selectTool.Get();
        m_selectTool->SetPicker(&m_viewport->Picker()); // the GPU pick over this page's viewport
        m_viewportTools.Add(Move(selectTool));        // the first added is the default tool
        m_toolHost.scene = &m_editContext->Scene();
        m_toolHost.commands = &m_editContext->Commands();
        m_toolHost.entitySelection = &m_editContext->EntitySelection();
        m_toolHost.picker = &m_viewport->Picker();
        // A tool that edits a cooked product live (terrain sculpt) registers its write-back to the
        // source on the context; the save flow drains it.
        m_toolHost.assetEdits = &context;
        m_toolHost.editorContext = &context; // a panel's asset-backed choices (thumbnails, names)
        ViewportToolProviderRegistry::Get().CreateAll(m_viewportTools, m_toolHost);
    }

    void SceneEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        m_viewport->CompleteCapture(host); // a capture recorded last frame: the GPU finished the copy
        // A project with no launcher picture gets one from its default scene, once the view has been
        // up a moment with nothing left loading (content pops in asynchronously).
        if (!m_thumbnailChecked && IsProjectDefaultScene())
        {
            m_thumbnailWait += dt;
            const bool settled = m_context->Resources() == nullptr || m_context->Resources()->PendingCount() == 0;
            if (m_thumbnailWait > 1.5f && settled && m_viewport->IsShown())
            {
                m_thumbnailChecked = true;
                if (!FileExists(ProjectThumbnailPath(m_context->Project()->Directory()).AsView()))
                    (void)RequestProjectThumbnail();
            }
        }

        if (m_animation && m_animationShown)
            m_animation->Update(dt, m_simulation.IsRunning());
        m_viewport->AdvanceScripted(dt); // a playtest's frame (viewport_run), before the view's input is read
        // A modal tool (a brush) owns Shift + wheel to size itself, so the camera leaves that wheel
        // to it; the bare wheel stays the camera's under a brush too.
        const bool modalTool = m_viewportTools.ActiveTool() != nullptr && m_viewportTools.ActiveTool() != m_selectTool;
        // Its Shift read with the pointer: the brush's panel, just used, may have the keys.
        const bool active = m_viewport->UpdateCamera(dt, !(modalTool && m_viewport->ShiftHeld()));
        if (active && m_viewport->ZoomedThisFrame() && m_hud)
            m_hud->Zoomed();
        if (m_editContext)
            UpdateTools(active, dt);
        // The page is this scene's run scope: its bus drains once a frame (events a Simulate tick
        // emitted last frame arrive now).
        m_pageEvents.Drain();

        // The overlays into this viewport's own debug list (the lists clear each frame, so they are
        // drawn again every frame); none on the frame a project picture is captured.
        if (foundation::render::debug::DebugDraw* dd = m_viewport->Overlay(); dd != nullptr && m_scene != nullptr && !m_viewport->CleanCaptureFrame())
        {
            SceneOverlayFrame frame;
            frame.scene = m_scene;
            frame.selection = m_editContext ? &m_editContext->EntitySelection() : nullptr;
            frame.camera = &m_viewport->Camera();
            frame.width = m_viewport->View().RenderWidth();
            frame.height = m_viewport->View().RenderHeight();
            frame.tools = &m_viewportTools;
            frame.componentGizmos = &m_componentGizmos;
            if (m_animation && m_animationShown && m_animation->Preview().IsPosing())
                frame.previewed = m_animation->Preview().PosedEntity();
            m_overlays.Draw(*dd, frame);
        }
        if (m_tools)
            m_tools->Sync();
        if (m_hud)
            m_hud->Update(dt, m_viewport->View().RenderHeight());
        if (m_cameraPreview)
            m_cameraPreview->Update();
        if (m_hierarchy)
            m_hierarchy->Refresh(); // the scene changed: built again
        if (m_inspector)
            m_inspector->Refresh(); // the selection or its components changed: built again; else its values
        // A move snapping by the grid's cell takes the cell where the camera is now.
        if (GizmoController* gizmos = Gizmos(); gizmos != nullptr && gizmos->MoveSnapsToGrid())
            gizmos->Gizmo().translateSnap = m_overlays.CurrentGridSpacing(m_viewport->Camera()).spacing;
        if (m_bar)
            m_bar->Sync();
    }

    void SceneEditorPage::UpdateTools(bool viewportActive, f32 deltaSeconds)
    {
        const EditorCamera& camera = m_viewport->Camera();
        ViewportToolInput in;
        in.deltaSeconds = deltaSeconds;
        // The draw eye: under orthographic what lies behind the eye's own plane is in view too, and
        // the gizmo's size and the marquee take a depth from here (the eye in perspective).
        in.cameraPosition = camera.DrawEye();
        in.cameraForward = camera.Forward();
        in.cameraUp = camera.Up();
        in.fovY = camera.fovY;
        in.orthoHeight = camera.orthographic ? camera.OrthoHeight() : 0.0f;
        // Simulate: edits are refused (the select tool's read-only gizmo); selecting still works.
        in.editingLocked = m_simulation.IsRunning();
        GizmoRay ray;
        in.pointerValid = viewportActive && m_viewport->MouseRay(ray);
        in.ray.origin = ray.origin;
        in.ray.direction = ray.direction;
        in.pointerOver = m_viewport->IsHovered();
        if (!in.pointerValid)
        {
            (void)m_viewportTools.Update(in);
            return;
        }
        foundation::shell::IMouse* mouse = m_viewport->Mouse();
        foundation::shell::IKeyboard* keyboard = m_viewport->Keyboard();
        // The pointer's pixel and the view's size: the GPU pick's input, in the render target's space.
        in.pointerX = static_cast<i32>(mouse->X());
        in.pointerY = static_cast<i32>(mouse->Y());
        in.viewportWidth = m_viewport->View().RenderWidth();
        in.viewportHeight = m_viewport->View().RenderHeight();
        const bool cameraOwnsMouse = m_viewport->CameraOwnsInput();
        if (!cameraOwnsMouse)
        {
            in.leftPressed = mouse->IsButtonPressed(foundation::shell::MouseButton::Left);
            in.leftDown = mouse->IsButtonDown(foundation::shell::MouseButton::Left);
        }
        // A release always reaches the tool, so a gesture can finish when a modifier goes down mid-drag.
        in.leftReleased = mouse->IsButtonReleased(foundation::shell::MouseButton::Left) || !mouse->IsButtonDown(foundation::shell::MouseButton::Left);
        if (keyboard != nullptr)
        {
            in.ctrl = keyboard->IsKeyDown(foundation::shell::KeyCode::LeftCtrl) || keyboard->IsKeyDown(foundation::shell::KeyCode::RightCtrl);
        }
        in.shift = m_viewport->ShiftHeld(); // with the pointer, focused or not (a brush's Shift + wheel)
        in.wheelDelta = mouse->ScrollY();
        // The tools' keys (the select tool's W/E/R/X) belong to the camera while it owns the input.
        in.keyboard = cameraOwnsMouse ? nullptr : keyboard;
        (void)m_viewportTools.Update(in);
    }

    void SceneEditorPage::OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_scene == nullptr)
            return;
        m_viewport->Render(frame, *m_scene, &m_postOverride, m_viewport->CleanCaptureFrame() ? nullptr : &m_debugView);
        if (m_cameraPreview)
            m_cameraPreview->Render(frame);
    }

    void SceneEditorPage::OnAfterSceneRender(runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame)
    {
        m_viewport->AfterRender(host, frame);
    }

    bool SceneEditorPage::PickEntity(EntityPickRequest request)
    {
        gui::SceneRoot* root = m_content ? m_content->GetSceneRoot() : nullptr;
        if (!m_editContext || root == nullptr || !m_services.loadMarkup)
            return false;
        if (!m_entityPicker)
        {
            EntityPickerWindow::Services services;
            services.icons = m_services.icons;
            services.sceneIcon = m_isPrefab ? StringView(u8"Editor/Icons/Types/PrefabDocument.svg") : StringView(u8"Editor/Icons/Types/SceneDocument.svg");
            SceneEditorPage* self = this;
            services.sceneName = [self]() { return self->m_title.AsView(); };
            m_entityPicker = MakeUnique<EntityPickerWindow>(Allocator(), Allocator(), *m_editContext, Move(services),
                                                            m_services.loadMarkup(StringView(EntityPickerWindow::kPath)));
        }
        return m_entityPicker->Open(*root, Move(request));
    }

    bool SceneEditorPage::IsOnScreen() const
    {
        return m_content && m_content->GetSceneRoot() != nullptr && m_content->IsTreeVisible();
    }

    void SceneEditorPage::OnClose()
    {
        if (m_openClipInterceptor != 0)
        {
            m_context->RemoveOpenAssetInterceptor(m_openClipInterceptor);
            m_openClipInterceptor = 0;
        }
        m_animation.Reset(); // what its preview posed goes back while the scene is here
        m_entityPicker.Reset(); // it lets the scene's tree go before the scene does
        m_cameraPreview.Reset(); // its picture's targets go with the viewports, before the scene
        m_inspector.Reset(); // the grid lets its sources go before the scene does
        m_hierarchy.Reset(); // it lets the selection and the tree go before the scene does
        // Where the scene was left: the camera, the selection, the panes, for the next open.
        if (m_scene != nullptr)
            SaveViewState();
        if (m_services.host != nullptr)
            m_viewport->Close(*m_services.host);
        if (m_scene != nullptr)
        {
            m_sceneManager.DestroyScene(m_scene); // aware subsystems are told OnSceneDestroyed
            m_scene = nullptr;
        }
        if (m_scenes != nullptr)
            m_scenes->UnregisterManager(&m_sceneManager);
    }

    Status SceneEditorPage::Save()
    {
        if (m_scene == nullptr || m_context->Project() == nullptr)
            return Status{ErrorCode::NotFound};
        foundation::content::Instance* instance = m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
            return Status{ErrorCode::NotFound};
        // A scene that did not load is empty here: saving would write that over its source.
        if (m_loadFailed)
        {
            m_context->Notify(NoticeKind::Warning, Format(u8"'{}' did not load, so it is not saved over: the Console says why.", m_title).AsView());
            return Status{ErrorCode::NotSupported};
        }
        const bool isPrefab = instance->TypeName() == StringView(u8"PrefabDocument");
        if (isPrefab)
        {
            if (const String refused = PrefabFlows::SaveRefusal(*m_scene, InstanceId()); !refused.IsEmpty())
            {
                m_context->Notify(NoticeKind::Warning, refused.AsView());
                return Status{ErrorCode::InvalidArgument};
            }
        }
        // What the animation panel posed is not the scene's: saved as it is, posed again after.
        if (m_animation)
            m_animation->StopPreview();
        const Status saved = isPrefab ? scene::SavePrefab(*m_scene, *instance) : scene::SaveScene(*m_scene, *instance);
        if (m_animation)
            m_animation->Repose();
        if (!saved.IsOk())
            return saved;
        ClearDirty();
        LOG_INFO(u8"Editor", u8"saved {} '{}'", isPrefab ? StringView(u8"prefab") : StringView(u8"scene"), m_title);
        // The launcher's picture of the project follows its default scene as it is saved.
        if (!isPrefab && IsProjectDefaultScene())
            (void)RequestProjectThumbnail();
        // The template changed: its instances in every other open scene rebuild, their overrides kept.
        if (isPrefab && m_prefabs)
            m_prefabs->RebuildOtherInstances(*instance);
        return saved;
    }

    void SceneEditorPage::OnSavedAs(foundation::content::Instance& instance)
    {
        EditorPage::OnSavedAs(instance);
        m_title = String(instance.Name());
        // SavePrefab stamps the document's name from the scene, so the fork gets its own.
        if (m_scene != nullptr)
            m_scene->SetName(instance.Name());
    }

    void SceneEditorPage::OnAssetExternallyModified()
    {
        if (m_scene == nullptr || m_context->Project() == nullptr)
            return;
        foundation::content::Instance* instance = m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
            return;
        if (IsDirty())
        {
            m_context->Notify(NoticeKind::Warning, Format(u8"'{}' changed on disk but has unsaved edits here - not refreshed.", m_title).AsView());
            return;
        }
        // The same entities come back from the stream; the history predates the reload and would
        // replay onto the old content, so it goes.
        while (m_scene->GetFirstRoot().IsAssigned())
            m_scene->DestroyEntity(m_scene->GetFirstRoot());
        m_scene->ClearPrefabInstances();
        m_editContext->EntitySelection().Clear();
        Commands().Clear();
        m_loadFailed = !LoadEditorScene(*m_context, *instance, *m_scene).IsOk() && instance->ReadData(u8"scene").Get() != nullptr;
        if (gui::Node* state = Find<gui::Node>(u8"scene-state"))
            state->SetDisplayed(m_loadFailed);
        ClearDirty(); // Commands().Clear() told OnChanged, which marks it dirty
    }

    void SceneEditorPage::StartSimulation()
    {
        // The scene as it is, not as the animation panel posed it, is what the run starts from and
        // what its end puts back.
        if (m_animation)
            m_animation->StopPreview();
        if (m_scene == nullptr || !m_simulation.Start(*m_scene, Commands()))
            return;
        // Modal edit tools (terrain sculpt) share runtime state with the simulation (the collider):
        // back to the default select tool for the run.
        m_viewportTools.ActivateDefault();
    }
    void SceneEditorPage::PauseSimulation(bool paused)
    {
        if (m_scene != nullptr)
            m_simulation.Pause(*m_scene, paused);
    }
    void SceneEditorPage::StopSimulation()
    {
        if (m_scene != nullptr)
            m_simulation.Stop(*m_scene, Commands(), m_context->Resources());
    }

    void SceneEditorPage::SetMarkersShown(bool shown)
    {
        m_overlays.settings.showMarkers = shown;
        SaveViewState();
    }


    bool SceneEditorPage::FrameEntities(Span<const Guid> entities, bool ease)
    {
        return m_scene != nullptr && m_viewport->Frame(*m_scene, entities, ease, Allocator());
    }

    Status SceneEditorPage::RequestProjectThumbnail()
    {
        if (m_context->Project() == nullptr)
            return Status{ErrorCode::NotSupported};
        const String path = ProjectThumbnailPath(m_context->Project()->Directory());
        (void)CreateDirectories(PathParent(path.AsView()));
        return m_viewport->RequestCapture(path.AsView(), true);
    }

    bool SceneEditorPage::IsProjectDefaultScene() const
    {
        const EditorProject* project = m_context->Project();
        return project != nullptr && !InstanceId().IsNil() && project->Settings().defaultSceneId == InstanceId();
    }

    void SceneEditorPage::SaveViewState()
    {
        // The whole pref (overlays, camera, selection, panes), so saving one never brings back
        // another's default.
        ScenePaneWeights panes;
        ScenePaneWeights* shown = nullptr;
        if (auto* body = Find<gui::Splitter>(u8"scene-body"))
        {
            const Array<f32> weights = body->GetWeights();
            if (weights.Size() == 3u)
            {
                panes = ScenePaneWeights{weights[0], weights[1], weights[2]};
                shown = &panes;
            }
        }
        Array<Guid> selection;
        if (m_editContext)
            for (const Guid& id : m_editContext->EntitySelection().Items())
                selection.PushBack(id);
        const SceneViewPref pref =
            SceneViewState::Capture(InstanceId(), m_overlays.settings, m_viewport->Camera(), shown, Span<const Guid>(selection.Data(), selection.Size()),
                                    m_tools ? m_tools->Place() : nullptr);
        if (SaveSceneViewPref(m_context->ProjectEditorSettings(), pref))
            m_context->RequestProjectEditorSettingsSave();
    }

    void SceneEditorPage::RestoreViewState()
    {
        const SceneViewPref pref = LoadSceneViewPref(m_context->ProjectEditorSettings(), InstanceId(), SceneViewPref{InstanceId()});
        if (pref.hasSplits)
            if (auto* body = Find<gui::Splitter>(u8"scene-body"))
            {
                const ScenePaneWeights panes = SceneViewState::WeightsFromSplits(pref.hierarchySplit, pref.inspectorSplit);
                const f32 weights[3] = {panes.hierarchy, panes.viewport, panes.inspector};
                body->SetWeights(Span<const f32>(weights, 3u));
            }
        if (m_tools)
            m_tools->SetPlace(pref.hasToolPanelPlace ? &pref.toolPanelPlace : nullptr);
        if (!SceneViewState::RestoreCamera(pref, m_viewport->Camera()))
            return; // never saved by a page: the origin's framing stands
        if (m_editContext && m_scene != nullptr)
        {
            const Array<Guid> live = SceneViewState::LiveSelection(pref, *m_scene);
            if (!live.IsEmpty())
                m_editContext->EntitySelection().Set(Span<const Guid>{live.Data(), live.Size()});
        }
    }

    void SceneEditorPage::Confirm(StringView title, StringView text, StringView verb, Function<void()> yes)
    {
        gui::Node* root = m_content ? m_content->GetRootNode() : nullptr;
        if (root == nullptr)
            return;
        auto box = MakeRef<gui::MessageBox>(Allocator());
        box->Configure(title, text, gui::MessageBox::Buttons::Ok);
        box->ClearButtons();
        (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
        (void)box->AddButton(verb, u8"yes", gui::DialogRole::Destructive);
        gui::MessageBox* raw = box.Get();
        (void)box->AddEventListener(gui::EventType::Changed, [raw, onYes = Move(yes)](const gui::Event&)
                                    {
                                        if (raw->GetResultName().AsView() == StringView(u8"yes") && onYes)
                                            onYes();
                                    });
        box->OpenModal(*root);
        m_confirm = box;
    }

    void SceneEditorPage::Later(Function<void()> work)
    {
        gui::SceneRoot* root = m_content ? m_content->GetSceneRoot() : nullptr;
        gui::MutationQueue* queue = root != nullptr ? root->GetMutationQueue() : nullptr;
        if (queue == nullptr)
            work();
        else
            queue->Enqueue(Move(work));
    }

    template <typename T>
    T* SceneEditorPage::Find(StringView id) const
    {
        if (!m_content)
            return nullptr;
        Array<gui::Node*> pending(Allocator());
        pending.PushBack(m_content.Get());
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    }
}

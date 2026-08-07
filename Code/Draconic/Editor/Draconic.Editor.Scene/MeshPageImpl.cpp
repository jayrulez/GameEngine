// Draconic::EditorScene - :mesh_page partition (implementation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.scene;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.geometry;
import draconic.geometry.editor; // StaticMeshAsset / SkinnedMeshAsset (factory PrimaryType)
import draconic.materials;
import draconic.resource;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit; // SplitView
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;

using namespace draconic::foundation;

namespace draconic::editor
{
    MeshEditorPage::MeshEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                   ui::runtime::UIHost& uiHost,
                                   draconic::content::Instance& instance)
        : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        m_router =
            MakeUnique<draconic::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());
        m_camera.position = Float3{4.0f, 3.0f, 6.0f};
        m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});

        // The context assigns the instance id AFTER construction (OpenPage), but BindMesh keys the
        // product bind on it - set it from the instance now (the later SetInstanceId is the same value).
        SetInstanceId(instance.Id());

        m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
        m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();
        m_defaultMaterial = materials::CreatePBR(u8"MeshPreview");

        BuildPreviewScene();

        m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
        m_viewport->ClearColor = rhi::ClearColor{0.10f, 0.11f, 0.13f, 1.0f};

        m_statsColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        m_statsColumn->Direction = ui::Orientation::Vertical;
        m_statsColumn->Spacing = 4.0f;

        auto scroll = MakeRef<ui::ScrollView>(DefaultAllocator());
        scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            auto lp = MakeRef<ui::LayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            scroll->AddView(m_statsColumn.Get(), lp);
        }

        auto statsColumnOuter = MakeRef<ui::FlexLayout>(DefaultAllocator());
        statsColumnOuter->Direction = ui::Orientation::Vertical;
        statsColumnOuter->Padding = ui::Thickness{8, 6};
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            statsColumnOuter->AddView(scroll.Get(), lp);
        }

        auto split = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        split->SetSplitRatio(0.66f);
        split->SetPanes(m_viewport.Get(), statsColumnOuter.Get());
        m_content = split;

        // Bind the cooked product (if already cooked) + populate the stats; the OnUpdate
        // watchdog catches a later cook / hot-reload.
        BindMesh();
    }

    void MeshEditorPage::BuildPreviewScene()
    {
        if (m_scenes == nullptr)
        {
            return;
        }
        m_sceneManager.SetAwareRegistry(&m_scenes->AwareRegistry());
        m_scenes->RegisterManager(&m_sceneManager);
        m_scene = m_sceneManager.CreateScene(u8"mesh.preview");
        m_scene->SetSimulationEnabled(false);

        m_entity = m_scene->CreateEntity(u8"PreviewMesh");
        if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
        {
            meshes->Add(m_entity); // mesh bound in BindMesh once the product resolves
        }

        const scene::EntityHandle sun = m_scene->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        m_scene->SetLocalTransform(sun, t);
        if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
        {
            render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false;
        }
    }

    void MeshEditorPage::BindMesh()
    {
        if (m_context->Resources() != nullptr)
        {
            m_meshProxy = m_context->Resources()->Bind<geometry::StaticMesh>(InstanceId());
        }
        geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
        PointComponentAtMesh(mesh);
        m_lastUid = mesh != nullptr ? mesh->uid : 0;
        FramePreview(mesh);
        RefreshStats();
    }

    void MeshEditorPage::RefreshStats()
    {
        if (m_statsColumn.Get() == nullptr)
        {
            return;
        }
        while (m_statsColumn->ChildCount() > 0)
        {
            m_statsColumn->RemoveView(m_statsColumn->GetChildAt(0), true);
        }

        geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
        if (mesh == nullptr)
        {
            AddStatLine(u8"Not cooked yet - the preview appears once the asset cooks.");
            return;
        }
        for (const String& line : MeshStatLines(*mesh))
        {
            AddStatLine(line.AsView());
        }
    }

    Array<String> MeshStatLines(const geometry::StaticMesh& mesh)
    {
        Array<String> lines;
        lines.PushBack(Format(u8"Name: {}", mesh.name));
        lines.PushBack(Format(u8"Vertices: {}", mesh.VertexCount()));
        lines.PushBack(Format(u8"Indices: {}", mesh.IndexCount()));
        lines.PushBack(Format(u8"Submeshes: {}", mesh.subMeshes.Size()));

        const Float3 size = mesh.bounds.Size();
        lines.PushBack(Format(u8"Bounds: {} x {} x {}", FormatFixed(size.x, 3),
                              FormatFixed(size.y, 3), FormatFixed(size.z, 3)));
        lines.PushBack(
            Format(u8"Skinned: {}", mesh.IsSkinned() ? StringView(u8"yes") : StringView(u8"no")));

        for (usize i = 0; i < mesh.subMeshes.Size(); ++i)
        {
            const geometry::SubMesh& sm = mesh.subMeshes[i];
            lines.PushBack(
                Format(u8"  [{}] material {}  |  {} indices", i, sm.materialIndex, sm.indexCount));
        }
        return lines;
    }

    void MeshEditorPage::AddStatLine(StringView text)
    {
        auto label = MakeRef<ui::Label>(DefaultAllocator(), text);
        label->FontSize.SetValue(12.0f);
        auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
        lp->Width = ui::SizeSpec::Match();
        m_statsColumn->AddView(label.Get(), lp);
    }

    void MeshEditorPage::FramePreview(const geometry::StaticMesh* mesh)
    {
        f32 radius = 1.0f;
        Float3 center{0.0f, 0.0f, 0.0f};
        if (mesh != nullptr && mesh->VertexCount() > 0)
        {
            center = mesh->bounds.Center();
            radius = foundation::Max(0.25f, Length(mesh->bounds.Extents()));
        }
        m_camera.position = center + Float3{0.0f, 0.4f, 1.0f} * (radius * 2.6f);
        m_camera.LookAt(center);
    }

    void MeshEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        EnsureViewportBound();
        if (m_hostWindow == nullptr)
        {
            return;
        }
        m_viewport->SyncInputRegion();
        if (m_router)
        {
            m_router->Update();
        }
        if (m_viewport->IsHovered() || m_viewport->IsFocused())
        {
            m_camera.Update(m_viewport->Keyboard(), m_viewport->Mouse(), dt);
        }

        // Product resolve / hot-reload watchdog: when the bound product first appears (cook) or
        // its identity changes (re-cook), re-point the component + reframe + refresh stats.
        const u64 uid = m_meshProxy ? m_meshProxy->uid : 0;
        if (uid != m_lastUid)
        {
            geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
            PointComponentAtMesh(mesh);
            m_lastUid = uid;
            FramePreview(mesh);
            RefreshStats();
        }
    }

    void MeshEditorPage::PointComponentAtMesh(geometry::StaticMesh* mesh)
    {
        auto* meshes = m_scene ? m_scene->GetSystem<render::MeshComponentManager>() : nullptr;
        render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_entity) : nullptr;
        if (mc == nullptr)
        {
            return;
        }
        mc->mesh.SetId(Guid{});
        if (mesh != nullptr)
        {
            mc->mesh = mesh; // direct override to the cooked product (Ref raw-assign AddRefs)
            mc->SetMaterial(m_defaultMaterial);
        }
        else
        {
            mc->mesh.SetDirect(RefPtr<geometry::StaticMesh>{}); // not cooked yet - clear
        }
    }

    void MeshEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                        draconic::graphics::FrameContext& frame)
    {
        if (!m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        if (m_render == nullptr || !m_render->IsReady() || m_scene == nullptr)
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0 || !m_viewport->IsEffectivelyVisible())
        {
            return;
        }

        render::ViewCamera camera;
        camera.view = Float4x4::LookAtRH(m_camera.position, m_camera.position + m_camera.Forward(),
                                         m_camera.Up());
        camera.projection = Float4x4::PerspectiveFovRH(
            1.0472f, static_cast<f32>(w) / static_cast<f32>(h), 0.05f, 500.0f);
        camera.position = m_camera.position;
        camera.farZ = 500.0f;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        cameraOverride.clearColor = Color{m_viewport->ClearColor.r, m_viewport->ClearColor.g,
                                          m_viewport->ClearColor.b, m_viewport->ClearColor.a};

        render::TargetState targetState;
        targetState.texture = m_viewport->ColorTexture();
        targetState.currentState = m_viewport->ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;

        m_render->RenderScene(*m_scene, m_viewport->ColorTargetView(), m_viewport->ColorFormat(), w,
                              h, render::ViewportRect{0, 0, w, h}, &cameraOverride, targetState);
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
    }

    void MeshEditorPage::OnClose()
    {
        m_viewport->Shutdown();
        if (m_scene != nullptr)
        {
            m_sceneManager.DestroyScene(m_scene);
            m_scene = nullptr;
        }
        if (m_scenes != nullptr)
        {
            m_scenes->UnregisterManager(&m_sceneManager);
        }
    }

    void MeshEditorPage::EnsureViewportBound()
    {
        draconic::ui::RootView* root = m_viewport->Root();
        if (root == nullptr)
        {
            return;
        }
        draconic::graphics::RenderWindow* window = m_uiHost->WindowForRoot(root);
        if (window == nullptr || window == m_hostWindow)
        {
            return;
        }
        vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(window);
        if (renderer == nullptr)
        {
            return;
        }
        if (m_hostWindow == nullptr)
        {
            m_viewport->Initialize(m_host->Graphics()->Raw(), renderer, m_host->Shell()->Input(),
                                   window->Window().Id());
            if (m_viewport->Surface() != nullptr)
            {
                m_router->AddSurface(m_viewport->Surface());
            }
        }
        else
        {
            m_viewport->AttachToWindow(renderer, window->Window().Id());
        }
        m_hostWindow = window;
    }

    UniquePtr<EditorPage> MeshEditorPageFactory::CreatePage(EditorContext& context,
                                                            draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<MeshEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }

    void RegisterMeshEditor(EditorContext& context, runtime::IApplicationHost& host,
                            ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<MeshEditorPageFactory>(geometry::StaticMeshAsset::StaticType(),
                                                          host, uiHost),
            DefaultAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<MeshEditorPageFactory>(geometry::SkinnedMeshAsset::StaticType(),
                                                          host, uiHost),
            DefaultAllocator()));
    }
}

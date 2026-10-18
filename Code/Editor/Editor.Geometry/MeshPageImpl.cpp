// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :page partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.geometry;

import foundation.core;
import foundation.settings; // per-project editor-settings store (preview-material pref)
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.geometry;
import geometry.pipeline; // StaticMeshAsset / SkinnedMeshAsset (factory PrimaryType)
import foundation.materials;
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit; // SplitView
import foundation.ui.runtime;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace core = foundation::core;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace render = foundation::render;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace ui = foundation::ui;

namespace editor
{
    // Per-asset mesh-preview prefs: {assetGuid -> preview-material guid} - a section in the
    // per-project editor-settings store, so a reopened mesh viewer restores its preview material.
    struct MeshPreviewPref
    {
        Guid asset;
        Guid material;
        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            ar.Key("material");
            ar.GuidValue(material);
        }
    };
    inline void Serialize(ISerializer& ar, MeshPreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }
    class MeshPreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(MeshPreviewSettings, ISerializable)
    public:
        Array<MeshPreviewPref> prefs;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "prefs", prefs);
        }
    };

    MeshEditorPage::MeshEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                   ui::runtime::UIHost& uiHost,
                                   foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // The context assigns the instance id AFTER construction (OpenPage), but BindMesh keys the
        // product bind on it - set it from the instance now (the later SetInstanceId is the same value).
        SetInstanceId(instance.Id());

        m_defaultMaterial = materials::CreatePBR(u8"MeshPreview");

        // Shared preview substrate (viewport + preview scene + orbit camera + render loop).
        m_preview = MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"mesh.preview");
        m_preview->Camera().position = Float3{4.0f, 3.0f, 6.0f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});

        BuildPreviewScene();

        m_statsColumn = MakeRef<ui::FlexLayout>(Allocator());
        m_statsColumn->Direction = ui::Orientation::Vertical;
        m_statsColumn->Spacing = 4.0f;

        auto scroll = MakeRef<ui::ScrollView>(Allocator());
        scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            scroll->AddView(m_statsColumn.Get(), lp);
        }

        auto statsColumnOuter = MakeRef<ui::FlexLayout>(Allocator());
        statsColumnOuter->Direction = ui::Orientation::Vertical;
        statsColumnOuter->Padding = ui::Thickness{8, 6};
        {
            ui::LayoutStyle lp;
            lp.FlexGrow = 1.0f;
            statsColumnOuter->AddView(scroll.Get(), lp);
        }

        auto split = MakeRef<ui::toolkit::SplitView>(Allocator());
        split->SetSplitRatio(0.66f);
        split->SetPanes(m_preview->View(), statsColumnOuter.Get());
        m_content = split;

        // Restore the persisted preview material (binds m_previewMaterial) before BindMesh applies it.
        LoadPreviewPref();

        // Bind the cooked product (if already cooked) + populate the stats; the OnUpdate
        // watchdog catches a later cook / hot-reload.
        BindMesh();
    }

    void MeshEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        if (const MeshPreviewSettings* section = store->Find<MeshPreviewSettings>())
        {
            for (const MeshPreviewPref& p : section->prefs)
            {
                if (p.asset == InstanceId())
                {
                    m_previewMaterialId = p.material;
                    if (!p.material.IsNil() && m_context->Resources() != nullptr)
                    {
                        m_previewMaterial =
                            m_context->Resources()->Bind<materials::Material>(p.material);
                    }
                    return;
                }
            }
        }
    }

    void MeshEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        MeshPreviewSettings& section = store->Section<MeshPreviewSettings>();
        for (MeshPreviewPref& p : section.prefs)
        {
            if (p.asset == InstanceId())
            {
                p.material = m_previewMaterialId;
                return;
            }
        }
        section.prefs.PushBack(MeshPreviewPref{InstanceId(), m_previewMaterialId});
    }

    void MeshEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }

        m_entity = scenePtr->CreateEntity(u8"PreviewMesh");
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            meshes->Add(m_entity); // mesh bound in BindMesh once the product resolves
        }

        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
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

        // Preview material row (always shown): pick a MaterialAsset to render with, or reset to the
        // neutral default. Applied live to the preview MeshComponent.
        {
            auto row = MakeRef<ui::FlexLayout>(Allocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 4.0f;

            auto label = MakeRef<ui::Label>(Allocator(), StringView(u8"Material"));
            label->FontSize.SetValue(Optional<f32>{12.0f});
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(64.0f));
                lp.AlignSelf = ui::Align::Center;
                row->AddView(label.Get(), lp);
            }
            // Rebuilt with the stats column: the previous row is released here, never inside its
            // own write (the write defers the rebuild).
            MeshEditorPage* self = this;
            const StringView materialTypes[] = {u8"MaterialAsset"};
            m_previewMaterialRow = MakeRef<app::ResourceRefEditor>(
                Allocator(), StringView(u8"Material"), StringView(u8"Default"), StringView{},
                Span<const StringView>{materialTypes, 1});
            m_previewMaterialRow->SetEmptyText(u8"Default");
            m_previewMaterialRow->BindAsset(
                *m_context, [self]() { return self->m_previewMaterialId; },
                [self](const Guid& picked)
                {
                    self->SetPreviewMaterial(picked);
                    if (ui::UIContext* ctx =
                            self->m_content.Get() != nullptr ? self->m_content->Context : nullptr)
                    {
                        ctx->MutationQueueRef().QueueAction(
                            Function<void()>{[self]() { self->RefreshStats(); }});
                    }
                });
            {
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                row->AddView(m_previewMaterialRow->EditorView(), lp);
            }
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(24.0f));
            m_statsColumn->AddView(row.Get(), lp);
        }

        geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
        if (mesh == nullptr)
        {
            AddStatLine(u8"Not cooked yet - the preview appears once the asset cooks.");
            return;
        }

        // LOD preview row (chains only): Auto + one button per level, driving the preview
        // MeshComponent's forceLod knob - the same wire the game uses, so what the page
        // shows IS the selected level's real draw.
        if (mesh->lodCount > 1)
        {
            auto row = MakeRef<ui::FlexLayout>(Allocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 4.0f;
            MeshEditorPage* self = this;
            const auto addLodButton = [&](StringView text, i32 value)
            {
                auto button = MakeRef<ui::Button>(Allocator(), text);
                button->FontSize.SetValue(Optional<f32>{12.0f});
                if (value == m_previewForceLod)
                {
                    button->IsEnabled = false; // the active choice reads as pressed
                }
                button->OnClick.Add(
                    [self, value](ui::ButtonBase*)
                    {
                        self->m_previewForceLod = value;
                        self->ApplyPreviewLod();
                        self->RefreshStats();
                    });
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                row->AddView(button.Get(), lp);
            };
            addLodButton(u8"Auto", -1);
            for (u32 l = 0; l < mesh->lodCount; ++l)
            {
                addLodButton(Format(u8"LOD {}", l).AsView(), static_cast<i32>(l));
            }
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            lp.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(24.0f));
            m_statsColumn->AddView(row.Get(), lp);
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
        // The LOD chain: per-level triangle totals + switch thresholds.
        if (mesh.lodCount > 1)
        {
            lines.PushBack(Format(u8"LOD levels: {}", mesh.lodCount));
            for (u32 l = 0; l < mesh.lodCount; ++l)
            {
                u64 indexTotal = 0;
                for (const geometry::SubMesh& sm : mesh.SubMeshesForLod(l))
                {
                    indexTotal += static_cast<u64>(sm.indexCount);
                }
                const f32 threshold =
                    (l < static_cast<u32>(mesh.lodCoverage.Size())) ? mesh.lodCoverage[l] : 0.0f;
                lines.PushBack(
                    (l == 0) ? Format(u8"  LOD 0: {} triangles", indexTotal / 3)
                             : Format(u8"  LOD {}: {} triangles  |  below {} coverage", l,
                                      indexTotal / 3, FormatFixed(threshold, 3)));
            }
        }
        return lines;
    }

    void MeshEditorPage::AddStatLine(StringView text)
    {
        auto label = MakeRef<ui::Label>(Allocator(), text);
        label->FontSize.SetValue(12.0f);
        ui::LayoutStyle lp;
        lp.Width = ui::SizeSpec::Match();
        m_statsColumn->AddView(label.Get(), lp);
    }

    void MeshEditorPage::FramePreview(const geometry::StaticMesh* mesh)
    {
        f32 radius = 1.0f;
        Float3 center{0.0f, 0.0f, 0.0f};
        if (mesh != nullptr && mesh->VertexCount() > 0)
        {
            center = mesh->bounds.Center();
            radius = core::Max(0.25f, Length(mesh->bounds.Extents()));
        }
        if (m_preview)
        {
            m_preview->Camera().FrameBounds(center, radius);
        }
    }

    void MeshEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_preview)
        {
            m_preview->Update(dt);
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
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        auto* meshes =
            scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_entity) : nullptr;
        if (mc == nullptr)
        {
            return;
        }
        mc->mesh.SetId(Guid{});
        if (mesh != nullptr)
        {
            mc->mesh = mesh; // direct override to the cooked product (Ref raw-assign AddRefs)
        }
        else
        {
            mc->mesh.SetDirect(RefPtr<geometry::StaticMesh>{}); // not cooked yet - clear
        }
        mc->forceLod = m_previewForceLod; // the page's LOD row drives the real knob
        ApplyPreviewMaterial();
    }

    void MeshEditorPage::ApplyPreviewLod()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        auto* meshes =
            scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_entity) : nullptr;
        if (mc != nullptr)
        {
            mc->forceLod = m_previewForceLod;
        }
    }

    void MeshEditorPage::ApplyPreviewMaterial()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        auto* meshes =
            scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_entity) : nullptr;
        if (mc == nullptr)
        {
            return;
        }
        materials::Material* picked = m_previewMaterial ? m_previewMaterial.Get() : nullptr;
        mc->SetMaterial(picked != nullptr ? RefPtr<materials::Material>(picked) : m_defaultMaterial);
    }

    void MeshEditorPage::SetPreviewMaterial(const Guid& picked)
    {
        m_previewMaterialId = picked;
        if (m_context->Resources() != nullptr && !picked.IsNil())
        {
            m_previewMaterial = m_context->Resources()->Bind<materials::Material>(picked);
        }
        else
        {
            m_previewMaterial = resource::Proxy<materials::Material>{};
        }
        ApplyPreviewMaterial();
        SavePreviewPref();
    }

    void MeshEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                        foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    void MeshEditorPage::OnClose()
    {
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    UniquePtr<EditorPage> MeshEditorPageFactory::CreatePage(EditorContext& context,
                                                            foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<MeshEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterMeshEditor(EditorContext& context, runtime::IApplicationHost& host,
                            ui::runtime::UIHost& uiHost)
    {
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(MeshPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MeshPreviewSettings>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MeshEditorPageFactory>(pipeline::StaticMeshAsset::StaticType(),
                                                          host, uiHost),
            editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MeshEditorPageFactory>(pipeline::SkinnedMeshAsset::StaticType(),
                                                          host, uiHost),
            editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(MeshPreviewSettings, "rtti::editor::editor.mesh", 1)
}

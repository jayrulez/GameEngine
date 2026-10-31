// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :page partition, its body. See MeshPage.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.geometry;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.settings;
import foundation.scene;
import engine.scene;
import foundation.geometry;
import geometry.pipeline; // StaticMeshAsset / SkinnedMeshAsset (factory PrimaryType)
import foundation.materials;
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.gui;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace core = foundation::core;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace editor
{
    // Per-asset mesh-preview prefs (asset guid -> preview-material guid): a section in the
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

    namespace
    {
        // Splits a stat line into a row's name and value: "Vertices: 1200", a submesh's
        // "  [2] material 1 | 90 indices", or a level's "  LOD 1: 400 triangles | ...".
        void SplitStat(StringView line, String& name, String& value)
        {
            usize start = 0;
            while (start < line.Size() && line[start] == u8' ')
                ++start;
            const StringView text = line.SubStr(start, line.Size() - start);
            if (!text.IsEmpty() && text[0] == u8'[')
            {
                usize close = 1;
                while (close < text.Size() && text[close] != u8']')
                    ++close;
                name = Format(u8"Submesh {}", text.SubStr(1, close - 1));
                const usize rest = close + 2 < text.Size() ? close + 2 : text.Size();
                value = String(text.SubStr(rest, text.Size() - rest));
                return;
            }
            for (usize i = 0; i + 1 < text.Size(); ++i)
                if (text[i] == u8':' && text[i + 1] == u8' ')
                {
                    name = String(text.SubStr(0, i));
                    value = String(text.SubStr(i + 2, text.Size() - i - 2));
                    return;
                }
            name = String(text);
            value = String{};
        }
    }

    MeshEditorPage::MeshEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance, const TypeInfo& type)
        : app::AssetObjectPage(context, Move(services), instance, type, kPath)
    {
        m_defaultMaterial = materials::CreatePBR(u8"MeshPreview");
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"mesh.preview");
            MeshEditorPage* self = this;
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            if (PreviewViewport* preview = m_pane->Viewport())
            {
                preview->Camera().position = Float3{4.0f, 3.0f, 6.0f};
                preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});
            }

            // The material the preview shows the mesh in: a slot on the preview's bar.
            m_materialSlot = MakeRef<app::AssetPickerSlot>(Allocator());
            m_materialSlot->AddClass(u8"preview-slot");
            m_materialSlot->SetTooltip(u8"The material the preview shows the mesh in (a neutral one when none); kept for this mesh");
            const app::ResourceRefServices* refs = Services().refs;
            if (refs != nullptr && refs->AcceptedTypes) // what makes a Material, asked of the builders
                m_materialSlot->SetAcceptedTypes(refs->AcceptedTypes(materials::Material::StaticType()));
            if (refs != nullptr && refs->Pick)
                m_materialSlot->SetOnPick([self, refs]()
                                          {
                                              refs->Pick(self->m_materialSlot->AcceptedTypes(), self->m_previewMaterialId,
                                                         [self](const Guid& picked) { self->SetPreviewMaterial(picked); });
                                          });
            m_materialSlot->SetOnClear([self]() { self->SetPreviewMaterial(Guid{}); });
            m_materialSlot->SetOnDropped([self](const Guid& dropped) { self->SetPreviewMaterial(dropped); });
            m_pane->AddControl(*m_materialSlot);

            // A chain's level of detail: the one the game would choose, or a fixed one.
            m_lod = MakeRef<gui::ComboBox>(Allocator());
            m_lod->AddClass(u8"preview-lod");
            m_lod->SetTooltip(u8"The level of detail the preview draws: Auto is the one the game would choose");
            m_lod->SetOnSelectionChanged([self](i32 index) { self->SetPreviewLod(index - 1); });
            m_lod->SetDisplayed(false);
            m_pane->AddControl(*m_lod);
        }
        BuildPreviewScene();
        LoadPreviewPref(); // binds m_previewMaterial before BindMesh applies it
        ShowMaterial();
        BindMesh();
    }

    void MeshEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
            return;
        if (const MeshPreviewSettings* section = store->Find<MeshPreviewSettings>())
            for (const MeshPreviewPref& p : section->prefs)
                if (p.asset == InstanceId())
                {
                    m_previewMaterialId = p.material;
                    if (!p.material.IsNil() && Context().Resources() != nullptr)
                        m_previewMaterial = Context().Resources()->Bind<materials::Material>(p.material);
                    return;
                }
    }

    void MeshEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
            return;
        MeshPreviewSettings& section = store->Section<MeshPreviewSettings>();
        for (MeshPreviewPref& p : section.prefs)
            if (p.asset == InstanceId())
            {
                p.material = m_previewMaterialId;
                store->MarkChanged<MeshPreviewSettings>();
                Context().RequestProjectEditorSettingsSave();
                return;
            }
        section.prefs.PushBack(MeshPreviewPref{InstanceId(), m_previewMaterialId});
        store->MarkChanged<MeshPreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    void MeshEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr)
            return;
        m_entity = scenePtr->CreateEntity(u8"PreviewMesh");
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            meshes->Add(m_entity); // mesh bound in BindMesh once the product resolves
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) * Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false;
        }
    }

    void MeshEditorPage::BindMesh()
    {
        if (Context().Resources() != nullptr)
            m_meshProxy = Context().Resources()->Bind<geometry::StaticMesh>(InstanceId());
        geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
        PointComponentAtMesh(mesh);
        m_lastUid = mesh != nullptr ? mesh->uid : 0;
        FramePreview();
        ShowMesh(mesh);
    }

    void MeshEditorPage::ShowMesh(geometry::StaticMesh* mesh)
    {
        if (mesh == nullptr)
        {
            SetFact(u8"Status", u8"Not cooked yet");
            if (m_pane)
                m_pane->ShowEmpty(u8"Not cooked yet", u8"The preview and the stats show once the mesh has cooked.");
            if (m_lod)
                m_lod->SetDisplayed(false);
            return;
        }
        SetFact(u8"Status", u8"Cooked");
        if (m_pane)
            m_pane->Show();
        for (const String& line : MeshStatLines(*mesh))
        {
            String name;
            String value;
            SplitStat(line.AsView(), name, value);
            SetFact(name.AsView(), value.AsView());
        }
        if (m_lod)
        {
            m_lod->SetDisplayed(mesh->lodCount > 1);
            String items(u8"Auto");
            for (u32 l = 0; l < mesh->lodCount; ++l)
                items.Append(Format(u8"|LOD {}", l).AsView());
            m_lod->SetItemsText(items);
            m_lod->SetSelectedIndex(m_previewForceLod + 1);
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
        // The LOD chain: per-level triangle totals and switch thresholds.
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

    void MeshEditorPage::FramePreview()
    {
        const geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
        f32 radius = 1.0f;
        Float3 center{0.0f, 0.0f, 0.0f};
        if (mesh != nullptr && mesh->VertexCount() > 0)
        {
            center = mesh->bounds.Center();
            radius = core::Max(0.25f, Length(mesh->bounds.Extents()));
        }
        if (m_pane && m_pane->Viewport() != nullptr)
            m_pane->Viewport()->Frame(center, radius);
    }

    void MeshEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
        // Product resolve and hot-reload watch: when the bound product first appears (a cook) or
        // its identity changes (a re-cook), re-point the component, reframe and refresh the stats.
        const u64 uid = m_meshProxy ? m_meshProxy->uid : 0;
        if (uid != m_lastUid)
        {
            geometry::StaticMesh* mesh = m_meshProxy ? m_meshProxy.Get() : nullptr;
            PointComponentAtMesh(mesh);
            m_lastUid = uid;
            FramePreview();
            ShowMesh(mesh);
        }
    }

    void MeshEditorPage::PointComponentAtMesh(geometry::StaticMesh* mesh)
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* meshes = scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = meshes != nullptr ? meshes->Get(m_entity) : nullptr;
        if (mc == nullptr)
            return;
        mc->mesh.SetId(Guid{});
        if (mesh != nullptr)
            mc->mesh = mesh; // direct override to the cooked product (Ref raw-assign AddRefs)
        else
            mc->mesh.SetDirect(RefPtr<geometry::StaticMesh>{}); // not cooked yet: clear
        mc->forceLod = m_previewForceLod; // the LOD choice drives the real knob
        ApplyPreviewMaterial();
    }

    void MeshEditorPage::SetPreviewLod(i32 lod)
    {
        m_previewForceLod = lod;
        ApplyPreviewLod();
    }

    void MeshEditorPage::ApplyPreviewLod()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* meshes = scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        if (engine::render::MeshComponent* mc = meshes != nullptr ? meshes->Get(m_entity) : nullptr)
            mc->forceLod = m_previewForceLod;
    }

    void MeshEditorPage::ApplyPreviewMaterial()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* meshes = scenePtr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = meshes != nullptr ? meshes->Get(m_entity) : nullptr;
        if (mc == nullptr)
            return;
        materials::Material* picked = m_previewMaterial ? m_previewMaterial.Get() : nullptr;
        mc->SetMaterial(picked != nullptr ? RefPtr<materials::Material>(picked) : m_defaultMaterial);
    }

    void MeshEditorPage::SetPreviewMaterial(const Guid& picked)
    {
        m_previewMaterialId = picked;
        if (Context().Resources() != nullptr && !picked.IsNil())
            m_previewMaterial = Context().Resources()->Bind<materials::Material>(picked);
        else
            m_previewMaterial = resource::Proxy<materials::Material>{};
        ApplyPreviewMaterial();
        SavePreviewPref();
        ShowMaterial();
    }

    void MeshEditorPage::ShowMaterial()
    {
        if (!m_materialSlot)
            return;
        app::ResourceRefShown shown;
        const app::ResourceRefServices* refs = Services().refs;
        if (!m_previewMaterialId.IsNil() && refs != nullptr && refs->Describe && refs->Describe(m_previewMaterialId, shown))
        {
            m_materialSlot->SetValue(shown.Name.AsView());
            m_materialSlot->SetGlyph(shown.Glyph.AsView(), shown.GlyphClass.AsView());
            return;
        }
        m_materialSlot->SetValue(StringView{}); // the neutral default
    }

    void MeshEditorPage::OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void MeshEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    UniquePtr<EditorPage> MeshEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<MeshEditorPage>(context, m_services->Make(), instance, *m_type);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterMeshEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(MeshPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MeshPreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MeshEditorPageFactory>(pipeline::StaticMeshAsset::StaticType(), services), editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<MeshEditorPageFactory>(pipeline::SkinnedMeshAsset::StaticType(), services), editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(MeshPreviewSettings, "rtti::editor::editor.mesh", 1)
}

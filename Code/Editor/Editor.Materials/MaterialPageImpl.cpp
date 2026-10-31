// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Materials - :page partition, its body. See MaterialPage.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.materials;

import foundation.core;
import foundation.vfs;
import foundation.settings;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.geometry;
import foundation.materials;
import foundation.materials.resource;
import materials.pipeline;
import foundation.texture.resource;
import foundation.resource;
import foundation.shaders;
import foundation.shaders.resource;
import foundation.shaders.system;
import foundation.render;
import engine.render;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace core = foundation::core;
namespace materials = foundation::materials;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace editor
{
    using app::PrettifyPropertyName; // a row's label from its property's name

    namespace
    {
        constexpr StringView kBlendItems[] = {u8"Opaque", u8"Masked", u8"AlphaBlend", u8"Additive", u8"Multiply", u8"PremultipliedAlpha"};
        constexpr StringView kDepthItems[] = {u8"Disabled", u8"ReadWrite", u8"ReadOnly", u8"WriteOnly"};
        constexpr StringView kCullItems[] = {u8"None", u8"Back", u8"Front"};

        // The pipeline state rows: their path, label, choices and the byte each writes. The render
        // state fields are typed enums (each `: u8`), edited through that identical byte.
        struct PipelineRow
        {
            StringView path;
            StringView label;
            Span<const StringView> items;
            u8& (*field)(materials::MaterialSource&);
        };
        const PipelineRow kPipelineRows[] = {
            {u8"blend", u8"Blend", Span<const StringView>(kBlendItems, 6), [](materials::MaterialSource& s) -> u8& { return reinterpret_cast<u8&>(s.blendMode); }},
            {u8"depth", u8"Depth", Span<const StringView>(kDepthItems, 4), [](materials::MaterialSource& s) -> u8& { return reinterpret_cast<u8&>(s.depthMode); }},
            {u8"cull", u8"Cull", Span<const StringView>(kCullItems, 3), [](materials::MaterialSource& s) -> u8& { return reinterpret_cast<u8&>(s.cullMode); }},
        };

        String GuidText(const Guid& id)
        {
            if (id.IsNil())
                return String{};
            utf8char text[37];
            id.ToChars(text);
            return String(StringView(text, 36));
        }
        Guid GuidOf(const gui::Variant& value)
        {
            Guid id;
            const StringView text = Trim(value.AsString());
            if (!text.IsEmpty())
                (void)Guid::TryParse(text, id);
            return id;
        }
        void AddChoices(gui::PropertyInfo& info, Span<const StringView> items)
        {
            for (usize i = 0; i < items.Size(); ++i)
                info.Choices.PushBack(gui::PropertyChoice{String(items[i]), static_cast<i64>(i)});
        }
        // A row's value set only when it differs, so a row the user is on is not disturbed.
        void ShowIfChanged(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
        {
            if (rows.FindInfo(path) == nullptr)
                return;
            const gui::Variant now = rows.GetValue(path, 0);
            if (now.GetType() != value.GetType() || now.Compare(value) != 0)
                rows.SetValue(path, 0, value);
        }
        // What the rows are laid out from: the shader's property names, then each effect group's
        // `Effect.Property` names. A change of it (a shader switched, its undo) lays them out again.
        Array<String> RowLayoutOf(const materials::MaterialSource& s)
        {
            Array<String> names;
            for (const String& name : s.propertyNames)
                names.PushBack(String(name.AsView()));
            for (const materials::MaterialEffectGroupSource& group : s.effects)
                for (const String& name : group.propertyNames)
                    names.PushBack(Format(u8"{}.{}", group.name, name));
            return names;
        }
        bool SameLayout(const Array<String>& a, const Array<String>& b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
                if (a[i].AsView() != b[i].AsView())
                    return false;
            return true;
        }
        // A number's part by its name in a group (x, y, z, w); -1 when it is none.
        i32 PartIndex(StringView part)
        {
            const StringView names[] = {u8"x", u8"y", u8"z", u8"w"};
            for (i32 i = 0; i < 4; ++i)
                if (part == names[i])
                    return i;
            return -1;
        }
    }

    MaterialEditorPage::MaterialEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::MaterialAsset::StaticType(), kPath)
    {
        // Its source reflects as arrays no one edits by hand, so the page builds its own rows.
        HideAssetRows();
        m_materialRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_propertyRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_textureRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"material.preview");
            MaterialEditorPage* self = this;
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            if (PreviewViewport* preview = m_pane->Viewport())
            {
                // The scene's clock runs only while it simulates: a material on a shader that
                // animates with Time() (a scrolling surface, moving vertices, wind) moves.
                preview->SetSimulationEnabled(true);
                preview->Camera().position = Float3{0.0f, 0.9f, 2.6f};
                preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});
            }
            AddPreviewControls();
        }
        // The renderer's shader system serves the effects' tables: a group's ranges, and the group
        // of an effect the material's shader declares that the material does not carry yet.
        if (Services().host != nullptr)
            if (auto* render = Services().host->Subsystems().GetSubsystem<engine::render::RenderSubsystem>())
                m_effectSystem = render->Shaders();
        BindShader(); // a material on a shader asset shows the shader's current properties
        BuildPreviewScene();
        LoadPreviewPref(); // the shape or mesh this material was last shown on
        if (m_previewShape != 0 || !m_previewMeshGuid.IsNil())
            ApplyPreviewMesh();
        BuildRows();
        ShowNotices();
        RebuildPreviewMaterial();
        if (m_pane && Asset() != nullptr)
            m_pane->Show();
    }

    MaterialEditorPage::~MaterialEditorPage() = default;

    const materials::MaterialSource* MaterialEditorPage::Source() const
    {
        return Asset() != nullptr ? &Asset()->source : nullptr;
    }

    void MaterialEditorPage::AddPreviewControls()
    {
        MaterialEditorPage* self = this;
        m_shapeChoice = MakeRef<gui::ComboBox>(Allocator());
        m_shapeChoice->AddClass(u8"preview-shape");
        m_shapeChoice->SetTooltip(u8"The shape the preview shows the material on");
        String items;
        for (const StringView shape : kShapes)
        {
            if (!items.IsEmpty())
                items.Append(StringView(u8"|"));
            items.Append(shape);
        }
        m_shapeChoice->SetItemsText(items);
        // Only the user's pick counts: a selection set from code (the page showing the kept shape)
        // must not count as picking a shape, which would drop the kept mesh.
        (void)m_shapeChoice->AddEventListener(gui::EventType::Changed, [self](const gui::Event&)
                                              { self->SetPreviewShape(static_cast<u32>(Max(0, self->m_shapeChoice->GetSelectedIndex()))); });
        m_pane->AddControl(*m_shapeChoice);

        // Or a mesh from the project, with its real UVs (an imported model), in place of the shape.
        m_meshSlot = MakeRef<app::AssetPickerSlot>(Allocator());
        m_meshSlot->AddClass(u8"preview-slot");
        m_meshSlot->SetTooltip(u8"A mesh from the project to show the material on, in place of the shape");
        const app::ResourceRefServices* refs = Services().refs;
        if (refs != nullptr && refs->AcceptedTypes)
            m_meshSlot->SetAcceptedTypes(refs->AcceptedTypes(foundation::geometry::StaticMesh::StaticType()));
        if (refs != nullptr && refs->Pick)
            m_meshSlot->SetOnPick([self, refs]()
                                  {
                                      refs->Pick(self->m_meshSlot->AcceptedTypes(), self->m_previewMeshGuid,
                                                 [self](const Guid& picked) { self->SetPreviewMesh(picked); });
                                  });
        m_meshSlot->SetOnClear([self]() { self->SetPreviewMesh(Guid{}); });
        m_meshSlot->SetOnDropped([self](const Guid& dropped) { self->SetPreviewMesh(dropped); });
        m_pane->AddControl(*m_meshSlot);
    }

    void MaterialEditorPage::SetPreviewShape(u32 shape)
    {
        m_previewShape = shape < static_cast<u32>(sizeof(kShapes) / sizeof(kShapes[0])) ? shape : 0u;
        m_previewMeshGuid = Guid{}; // a shape picked takes over from a mesh
        ApplyPreviewMesh();
        SavePreviewPref();
        m_reshow = true;
    }

    void MaterialEditorPage::SetPreviewMesh(const Guid& mesh)
    {
        m_previewMeshGuid = mesh;
        ApplyPreviewMesh();
        SavePreviewPref();
        m_reshow = true;
    }

    void MaterialEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
        // Texture hot-reload watchdog: rebuild the preview material when any bound texture's live
        // view no longer matches what the material captured.
        for (usize i = 0; i < m_previewTextures.Size(); ++i)
        {
            rhi::TextureView* live = m_previewTextures[i] ? m_previewTextures[i]->View() : nullptr;
            if (live != m_previewTextureViews[i])
            {
                RebuildPreviewMaterial();
                break;
            }
        }
        // The shader asset recooked (its properties may have changed): lay the material out again
        // by it.
        if (m_shader && m_shader->Version() != m_shaderVersion)
        {
            BindShader();
            RebuildPreviewMaterial();
        }
    }

    void MaterialEditorPage::Refresh()
    {
        app::AssetObjectPage::Refresh();
        if (m_rowsDirty)
            BuildRows();
        else if (m_reshow)
            ShowValues();
        m_rowsDirty = false;
        m_reshow = false;
    }

    void MaterialEditorPage::OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void MaterialEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    // === The rows ===

    void MaterialEditorPage::BuildRows()
    {
        MaterialEditorPage* self = this;
        ClearSections();
        m_materialRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_propertyRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_textureRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_effectRows.Clear();
        m_effectRowNames.Clear();
        if (Asset() == nullptr)
            return;
        const materials::MaterialSource& src = Asset()->source;

        // The shader: a shader asset, or a built-in (PBR, Unlit or Phong) while it is on none.
        {
            gui::PropertyInfo& shader = m_materialRows->Add(u8"shader", gui::PropertyKind::Reference);
            shader.DisplayName = String(u8"Shader");
            shader.Description = String(u8"A shader asset the material is drawn with; cleared, a built-in");
            shader.ReferenceType = &foundation::shaders::ShaderResource::StaticType();
            gui::PropertyInfo& builtIn = m_materialRows->Add(u8"builtIn", gui::PropertyKind::Enum);
            builtIn.DisplayName = String(u8"Built-in");
            for (const materials::BuiltinShadingInfo& builtin : materials::kBuiltinShadings)
                builtIn.Choices.PushBack(gui::PropertyChoice{String(builtin.label), static_cast<i64>(builtin.shading)});
            for (const PipelineRow& row : kPipelineRows)
            {
                gui::PropertyInfo& info = m_materialRows->Add(row.path, gui::PropertyKind::Enum);
                info.DisplayName = String(row.label);
                AddChoices(info, row.items);
            }
        }

        // Each value its shader declares, by its type.
        for (usize i = 0; i < src.propertyNames.Size() && i < src.propertyTypes.Size(); ++i)
        {
            const auto type = static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]);
            const MaterialPropertyRow row = MaterialPropertyRowFor(type);
            const StringView name = src.propertyNames[i].AsView();
            if (row == MaterialPropertyRow::None)
                continue; // a sampler has no value to edit (the material system binds it)
            if (row == MaterialPropertyRow::Texture)
            {
                // A texture or a render texture (a monitor's screen, a mirror): what makes a
                // Texture, asked of the builders.
                gui::PropertyInfo& info = m_textureRows->Add(name, gui::PropertyKind::Reference);
                info.ReferenceType = &foundation::texture::Texture::StaticType();
                info.DisplayName = PrettifyPropertyName(name);
                continue;
            }
            // A shader asset's property declares its range.
            const foundation::shaders::ShaderProperty* declared = nullptr;
            for (const foundation::shaders::ShaderProperty& p : m_shaderTable)
                if (p.name.AsView() == name)
                    declared = &p;
            AddValueRow(*m_propertyRows, name, row, declared);
            gui::PropertyInfo* info = m_propertyRows->FindInfo(name);
            if (info == nullptr)
                continue;
            // The standard PBR factors are 0..1 (the normal scale 0..2); any other float a shader
            // gives no range is unbounded.
            const bool zeroToOne = name == u8"Metallic" || name == u8"Roughness" || name == u8"OcclusionStrength" || name == u8"AlphaCutoff";
            if (row == MaterialPropertyRow::Number && !info->HasRange && (zeroToOne || name == u8"NormalScale"))
            {
                info->HasRange = true;
                info->Min = 0.0;
                info->Max = zeroToOne ? 1.0 : 2.0;
                info->Hint = String(u8"slider");
            }
            info->DisplayName = PrettifyPropertyName(name);
        }

        // Each effect group the material carries (Wind), a section of its own: its rows from the
        // effect's table, as a custom shader's properties are, with the table's ranges when the
        // page has the shader system serving it; else the group's own layout, unbounded.
        for (const materials::MaterialEffectGroupSource& group : src.effects)
        {
            UniquePtr<gui::PropertyBag> rows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
            const foundation::shaders::ShaderEffect* table = m_effectSystem != nullptr ? m_effectSystem->FindEffect(group.name.AsView()) : nullptr;
            for (usize i = 0; i < group.propertyNames.Size() && i < group.propertyTypes.Size(); ++i)
            {
                const StringView name = group.propertyNames[i].AsView();
                const MaterialPropertyRow row = MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(group.propertyTypes[i]));
                if (row == MaterialPropertyRow::None || row == MaterialPropertyRow::Texture)
                    continue; // an effect takes uniforms only
                AddValueRow(*rows, name, row, table != nullptr ? table->FindProperty(name) : nullptr);
                if (gui::PropertyInfo* info = rows->FindInfo(name))
                    info->DisplayName = PrettifyPropertyName(name);
            }
            m_effectRows.PushBack(Move(rows));
            m_effectRowNames.PushBack(String(group.name.AsView()));
        }

        Array<gui::PropertyBag*> bags;
        bags.PushBack(m_materialRows.Get());
        bags.PushBack(m_propertyRows.Get());
        bags.PushBack(m_textureRows.Get());
        for (const UniquePtr<gui::PropertyBag>& rows : m_effectRows)
            bags.PushBack(rows.Get());
        for (gui::PropertyBag* rows : bags)
        {
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
            // A gesture's previews show live: each is written to the source and the preview
            // material built again, with no undo step until the commit.
            rows->SetOnPreview([self, rows](StringView path)
                               {
                                   self->m_previewing = true;
                                   const bool reshow = self->m_reshow;
                                   self->Committed(*rows, path, rows->GetValue(path, 0));
                                   self->m_reshow = reshow; // the rows hold the preview already
                                   self->m_previewing = false;
                               });
            rows->SetOnCancel([self](StringView) { self->CancelGesture(); });
        }
        ShowValues();
        AddSection(u8"material", u8"Material", *m_materialRows);
        AddSection(u8"properties", u8"Properties", *m_propertyRows);
        AddSection(u8"textures", u8"Textures", *m_textureRows);
        // After the shader's rows: an effect's values belong to the material, not its shader.
        for (usize g = 0; g < m_effectRows.Size(); ++g)
            AddSection(Format(u8"effect-{}", m_effectRowNames[g]).AsView(), m_effectRowNames[g].AsView(), *m_effectRows[g]);
    }

    gui::PropertyBag* MaterialEditorPage::EffectRows(StringView effect) noexcept
    {
        for (usize g = 0; g < m_effectRowNames.Size(); ++g)
            if (m_effectRowNames[g].AsView() == effect)
                return m_effectRows[g].Get();
        return nullptr;
    }

    void MaterialEditorPage::AddValueRow(gui::PropertyBag& rows, StringView path, MaterialPropertyRow row,
                                         const foundation::shaders::ShaderProperty* declared)
    {
        const bool ranged = declared != nullptr && declared->hasRange;
        switch (row)
        {
        case MaterialPropertyRow::Number:
        {
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::Float);
            if (ranged)
            {
                info.HasRange = true;
                info.Min = declared->rangeMin;
                info.Max = declared->rangeMax;
                info.Hint = String(u8"slider"); // over its range
            }
            info.Step = 0.01;
            break;
        }
        case MaterialPropertyRow::Numbers2:
        case MaterialPropertyRow::Numbers3:
        case MaterialPropertyRow::Numbers4:
        {
            const i32 count = row == MaterialPropertyRow::Numbers2 ? 2 : row == MaterialPropertyRow::Numbers3 ? 3 : 4;
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::Group);
            info.Hint = Format(u8"Float{}", count);
            const StringView parts[] = {u8"x", u8"y", u8"z", u8"w"};
            for (i32 p = 0; p < count; ++p)
                rows.Add(gui::JoinPropertyPath(path, parts[p]).AsView(), gui::PropertyKind::Float).Step = 0.01;
            break;
        }
        case MaterialPropertyRow::Integer:
        {
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::Integer);
            if (ranged)
            {
                info.HasRange = true;
                info.Min = declared->rangeMin;
                info.Max = declared->rangeMax;
            }
            break;
        }
        case MaterialPropertyRow::Color:
            (void)rows.Add(path, gui::PropertyKind::Color);
            break;
        case MaterialPropertyRow::ColorWithIntensity:
        {
            // A colour that may pass white: its colour as entered, its intensity in w (the
            // grid's HDR colour, as a reflected one's parts are named).
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::Group);
            info.Hint = String(u8"ColorHdr");
            rows.Add(gui::JoinPropertyPath(path, u8"color").AsView(), gui::PropertyKind::Color).ShowsAlpha = false;
            gui::PropertyInfo& intensity = rows.Add(gui::JoinPropertyPath(path, u8"intensity").AsView(), gui::PropertyKind::Float);
            intensity.HasRange = true;
            intensity.Min = 0.0;
            intensity.Max = 1e6;
            intensity.Step = 0.05;
            break;
        }
        default:
            break; // a texture's row is a slot (BuildRows); a sampler has none
        }
    }

    void MaterialEditorPage::ShowValues()
    {
        if (Asset() == nullptr)
            return;
        const materials::MaterialSource& src = Asset()->source;
        ShowIfChanged(*m_materialRows, u8"shader", gui::Variant(GuidText(src.shaderId)));
        const materials::BuiltinShadingInfo* builtin = materials::FindBuiltinShadingByShader(src.shaderName.AsView());
        ShowIfChanged(*m_materialRows, u8"builtIn", gui::Variant(static_cast<i64>(builtin != nullptr ? builtin->shading : materials::BuiltinShading::Pbr)));
        if (gui::PropertyInfo* builtIn = m_materialRows->FindInfo(u8"builtIn"); builtIn != nullptr && builtIn->Visible != src.shaderId.IsNil())
        {
            builtIn->Visible = src.shaderId.IsNil(); // a built-in only while it is on no shader asset
            m_materialRows->PropertiesChanged();
        }
        for (const PipelineRow& row : kPipelineRows)
            ShowIfChanged(*m_materialRows, row.path, gui::Variant(static_cast<i64>(row.field(const_cast<materials::MaterialSource&>(src)))));
        for (usize i = 0; i < src.propertyNames.Size() && i < src.propertyTypes.Size(); ++i)
        {
            const StringView name = src.propertyNames[i].AsView();
            const MaterialPropertyRow row = MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]));
            if (row == MaterialPropertyRow::Texture)
            {
                Guid bound;
                for (usize t = 0; t < src.textureSlots.Size() && t < src.textureIds.Size(); ++t)
                    if (src.textureSlots[t].AsView() == name)
                        bound = src.textureIds[t];
                ShowIfChanged(*m_textureRows, name, gui::Variant(GuidText(bound)));
                continue;
            }
            ShowValue(*m_propertyRows, name, name, row);
        }
        // Each effect group's values, addressed as `Effect.Property`.
        for (usize g = 0; g < m_effectRows.Size(); ++g)
        {
            for (const materials::MaterialEffectGroupSource& group : src.effects)
            {
                if (group.name.AsView() != m_effectRowNames[g].AsView())
                    continue;
                for (usize i = 0; i < group.propertyNames.Size() && i < group.propertyTypes.Size(); ++i)
                {
                    const StringView name = group.propertyNames[i].AsView();
                    const String qualified = Format(u8"{}.{}", group.name, name);
                    ShowValue(*m_effectRows[g], name, qualified.AsView(),
                              MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(group.propertyTypes[i])));
                }
            }
        }
        // The preview bar's choices.
        if (m_shapeChoice && m_shapeChoice->GetSelectedIndex() != static_cast<i32>(m_previewShape))
            m_shapeChoice->SetSelectedIndex(static_cast<i32>(m_previewShape));
        if (m_meshSlot)
        {
            app::ResourceRefShown shown;
            const app::ResourceRefServices* refs = Services().refs;
            if (!m_previewMeshGuid.IsNil() && refs != nullptr && refs->Describe && refs->Describe(m_previewMeshGuid, shown))
            {
                m_meshSlot->SetValue(shown.Name.AsView());
                m_meshSlot->SetGlyph(shown.Glyph.AsView(), shown.GlyphClass.AsView());
            }
            else
                m_meshSlot->SetValue(StringView{});
        }
    }

    void MaterialEditorPage::ShowValue(gui::PropertyBag& rows, StringView path, StringView name, MaterialPropertyRow row)
    {
        switch (row)
        {
        case MaterialPropertyRow::Number:
        {
            f32 v = 0.0f;
            ReadUniform(name, &v, sizeof(v));
            ShowIfChanged(rows, path, gui::Variant(static_cast<f64>(v)));
            break;
        }
        case MaterialPropertyRow::Numbers2:
        case MaterialPropertyRow::Numbers3:
        case MaterialPropertyRow::Numbers4:
        {
            Float4 v{0, 0, 0, 0};
            const i32 count = row == MaterialPropertyRow::Numbers2 ? 2 : row == MaterialPropertyRow::Numbers3 ? 3 : 4;
            ReadUniform(name, &v, sizeof(f32) * static_cast<usize>(count));
            const f32 parts[] = {v.x, v.y, v.z, v.w};
            const StringView names[] = {u8"x", u8"y", u8"z", u8"w"};
            for (i32 p = 0; p < count; ++p)
                ShowIfChanged(rows, gui::JoinPropertyPath(path, names[p]).AsView(), gui::Variant(static_cast<f64>(parts[p])));
            break;
        }
        case MaterialPropertyRow::Integer:
        {
            i32 v = 0;
            ReadUniform(name, &v, sizeof(v));
            ShowIfChanged(rows, path, gui::Variant(static_cast<i64>(v)));
            break;
        }
        case MaterialPropertyRow::Color:
        {
            Float4 v{1, 1, 1, 1};
            ReadUniform(name, &v, sizeof(v));
            ShowIfChanged(rows, path, gui::Variant(Color{v.x, v.y, v.z, v.w}));
            break;
        }
        case MaterialPropertyRow::ColorWithIntensity:
        {
            Float4 v{1, 1, 1, 1};
            ReadUniform(name, &v, sizeof(v));
            ShowIfChanged(rows, gui::JoinPropertyPath(path, u8"color").AsView(), gui::Variant(Color{v.x, v.y, v.z, 1.0f}));
            ShowIfChanged(rows, gui::JoinPropertyPath(path, u8"intensity").AsView(), gui::Variant(static_cast<f64>(v.w)));
            break;
        }
        default:
            break;
        }
    }

    void MaterialEditorPage::EditRow(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        rows.SetValue(path, 0, value); // as the grid's commit leaves it
        Committed(rows, path, value);
    }

    void MaterialEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        if (Asset() == nullptr)
            return;
        m_reshow = true; // show what the edit did, or did not do
        if (&rows == m_materialRows.Get())
        {
            if (path == u8"shader")
            {
                SwitchShader(GuidOf(value));
                return;
            }
            if (path == u8"builtIn")
            {
                const i64 chosen = value.AsInt();
                if (chosen >= 0 && static_cast<usize>(chosen) < materials::kBuiltinShadingCount)
                    SwitchToBuiltIn(static_cast<materials::BuiltinShading>(chosen));
                return;
            }
            for (const PipelineRow& row : kPipelineRows)
                if (path == row.path)
                {
                    const u8 index = static_cast<u8>(value.AsInt());
                    auto* field = row.field;
                    ApplyEdit(row.path, [field, index](materials::MaterialSource& s) { field(s) = index; });
                }
            return;
        }
        if (&rows == m_textureRows.Get())
        {
            const String slot(path);
            const Guid picked = GuidOf(value);
            // Pick, drop and clear are one write: nil removes the slot's binding.
            ApplyEdit(path, [slot, picked](materials::MaterialSource& s)
                      {
                          for (usize i = 0; i < s.textureSlots.Size(); ++i)
                          {
                              if (s.textureSlots[i].AsView() != slot.AsView())
                                  continue;
                              if (picked.IsNil())
                              {
                                  s.textureSlots.RemoveAt(i);
                                  s.textureIds.RemoveAt(i);
                              }
                              else
                                  s.textureIds[i] = picked;
                              return;
                          }
                          if (!picked.IsNil())
                          {
                              s.textureSlots.PushBack(slot);
                              s.textureIds.PushBack(picked);
                          }
                      });
            return;
        }
        // A value, or one part of it ("Tint.x", "Emissive.intensity").
        const StringView name = gui::ParentPropertyPath(path).IsEmpty() ? path : gui::ParentPropertyPath(path);
        const StringView part = name.Size() == path.Size() ? StringView{} : path.SubStr(name.Size() + 1, path.Size() - name.Size() - 1);
        const materials::MaterialSource& src = Asset()->source;
        if (&rows == m_propertyRows.Get())
        {
            MaterialPropertyRow row = MaterialPropertyRow::None;
            for (usize i = 0; i < src.propertyNames.Size() && i < src.propertyTypes.Size(); ++i)
                if (src.propertyNames[i].AsView() == name)
                    row = MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]));
            CommitValue(name, part, row, value);
            return;
        }
        // An effect group's parameter, written as `Effect.Property` into the group's values.
        for (usize g = 0; g < m_effectRows.Size(); ++g)
        {
            if (&rows != m_effectRows[g].Get())
                continue;
            for (const materials::MaterialEffectGroupSource& group : src.effects)
            {
                if (group.name.AsView() != m_effectRowNames[g].AsView())
                    continue;
                for (usize i = 0; i < group.propertyNames.Size() && i < group.propertyTypes.Size(); ++i)
                {
                    if (group.propertyNames[i].AsView() != name)
                        continue;
                    const String qualified = Format(u8"{}.{}", group.name, name);
                    CommitValue(qualified.AsView(), part,
                                MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(group.propertyTypes[i])), value);
                    return;
                }
            }
            return;
        }
    }

    void MaterialEditorPage::CommitValue(StringView name, StringView part, MaterialPropertyRow row, const gui::Variant& value)
    {
        MaterialEditorPage* self = this;
        const String key(name);
        switch (row)
        {
        case MaterialPropertyRow::Number:
        {
            const f32 v = static_cast<f32>(value.AsFloat());
            ApplyEdit(name, [self, key, v](materials::MaterialSource& s) { self->WriteUniform(s, key.AsView(), &v, sizeof(v)); });
            break;
        }
        case MaterialPropertyRow::Integer:
        {
            const i32 v = static_cast<i32>(value.AsInt());
            ApplyEdit(name, [self, key, v](materials::MaterialSource& s) { self->WriteUniform(s, key.AsView(), &v, sizeof(v)); });
            break;
        }
        case MaterialPropertyRow::Numbers2:
        case MaterialPropertyRow::Numbers3:
        case MaterialPropertyRow::Numbers4:
        {
            const usize count = row == MaterialPropertyRow::Numbers2 ? 2u : row == MaterialPropertyRow::Numbers3 ? 3u : 4u;
            const i32 index = PartIndex(part);
            if (index < 0 || static_cast<usize>(index) >= count)
                break;
            f32 v[4] = {0, 0, 0, 0};
            ReadUniform(name, v, sizeof(f32) * count);
            v[index] = static_cast<f32>(value.AsFloat());
            const Float4 all{v[0], v[1], v[2], v[3]};
            ApplyEdit(name, [self, key, all, count](materials::MaterialSource& s) { self->WriteUniform(s, key.AsView(), &all, sizeof(f32) * count); });
            break;
        }
        case MaterialPropertyRow::Color:
        {
            const Color c = value.AsColor();
            const Float4 v{c.r, c.g, c.b, c.a};
            ApplyEdit(name, [self, key, v](materials::MaterialSource& s) { self->WriteUniform(s, key.AsView(), &v, sizeof(v)); });
            break;
        }
        case MaterialPropertyRow::ColorWithIntensity:
        {
            Float4 v{1, 1, 1, 1};
            ReadUniform(name, &v, sizeof(v));
            if (part == u8"color")
            {
                const Color c = value.AsColor();
                v = Float4{c.r, c.g, c.b, v.w};
            }
            else if (part == u8"intensity")
                v.w = static_cast<f32>(value.AsFloat());
            ApplyEdit(name, [self, key, v](materials::MaterialSource& s) { self->WriteUniform(s, key.AsView(), &v, sizeof(v)); });
            break;
        }
        default:
            break;
        }
    }

    void MaterialEditorPage::ShowNotices()
    {
        if (Asset() == nullptr)
            return;
        StringView missing;
        const bool complete = !Asset()->source.shaderId.IsNil() || materials::ForwardMaterialSourceIsComplete(Asset()->source, &missing);
        SetNotice(u8"stale", complete ? StringView{}
                                      : Format(u8"This material lacks the forward shader's {}: it was made by an older editor. Make it again; the game refuses to build it.", missing).AsView());
        SetNotice(u8"shader", !Asset()->source.shaderId.IsNil() && m_shaderTable.IsEmpty()
                                  ? StringView(u8"Its shader has not cooked (or has errors): its rows are its own until it does.")
                                  : StringView{});
    }

    // === Edits and undo ===

    void MaterialEditorPage::ApplySourceBlob(const Array<byte>& blob)
    {
        pipeline::MaterialAsset* asset = Asset();
        if (asset == nullptr)
            return;
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        const Guid shaderBefore = asset->source.shaderId;
        const Array<String> layoutBefore = RowLayoutOf(asset->source);
        BinarySerializer ar(stream, SerializeMode::Read);
        asset->source.Serialize(ar);
        // A shader switched (or its switch undone) changes the rows and what the preview binds.
        if (asset->source.shaderId != shaderBefore)
            BindShader();
        m_rowsDirty = m_rowsDirty || !SameLayout(layoutBefore, RowLayoutOf(asset->source));
        m_reshow = true;
        ShowNotices();
        RebuildPreviewMaterial();
    }

    Array<byte> MaterialEditorPage::SnapshotSource() const
    {
        Array<byte> blob;
        if (Asset() == nullptr)
            return blob;
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        Asset()->source.Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
            blob.PushBack(b);
        return blob;
    }

    void MaterialEditorPage::ApplyEdit(StringView mergeKey, Function<void(materials::MaterialSource&)> mutate)
    {
        if (Asset() == nullptr)
            return;
        if (m_previewing)
        {
            // A preview is written to the source and shown; the source from before the gesture is
            // kept.
            if (!m_inGesture)
            {
                m_gestureBefore = SnapshotSource();
                m_inGesture = true;
            }
            mutate(Asset()->source);
            RebuildPreviewMaterial();
            return;
        }
        // The gesture's single undo step starts from the source before its previews.
        Array<byte> before = m_inGesture ? Move(m_gestureBefore) : SnapshotSource();
        m_inGesture = false;
        // The rows follow a change of what the material holds (a built-in switched): the command's
        // Execute below applies `after` over itself, so it cannot see that change.
        const Array<String> layoutBefore = RowLayoutOf(Asset()->source);
        mutate(Asset()->source);
        m_rowsDirty = m_rowsDirty || !SameLayout(layoutBefore, RowLayoutOf(Asset()->source));
        Array<byte> after = SnapshotSource();
        // The mutation already ran; Execute() re-applies `after` (idempotent).
        (void)Commands().Execute(UniquePtr<IEditorCommand>(Allocator().New<EditMaterialCommand>(*this, mergeKey, Move(before), Move(after)), Allocator()));
    }

    void MaterialEditorPage::CancelGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        ApplySourceBlob(m_gestureBefore); // take the previews back, and the preview with them
        m_gestureBefore.Clear();
    }

    // === The preview ===

    void MaterialEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }

        m_shape = scenePtr->CreateEntity(u8"PreviewShape");
        m_previewMesh = foundation::geometry::Primitives::Sphere(Allocator(), 1.0f, 48, 24);
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            engine::render::MeshComponent& mc = meshes->Add(m_shape);
            mc.mesh = m_previewMesh.Get(); // direct override (runtime-built, not an asset)
        }

        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false; // a lone sphere has nothing to shadow
        }
    }

    void MaterialEditorPage::RebuildPreviewMaterial()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (Asset() == nullptr || scenePtr == nullptr || !m_shape.IsAssigned())
        {
            return;
        }
        const materials::MaterialSource& src = Asset()->source;

        // A shader asset draws under its resource's name (binding it registered the source).
        const StringView shaderName =
            m_shader ? m_shader->Name() : src.shaderName.AsView();
        RefPtr<materials::Material> material =
            materials::MaterialFromSource(Allocator(), src, shaderName);

        m_previewTextures.Clear();
        m_previewTextureViews.Clear();
        if (Context().Resources() != nullptr)
        {
            for (usize i = 0; i < src.textureSlots.Size() && i < src.textureIds.Size(); ++i)
            {
                if (src.textureIds[i].IsNil())
                {
                    continue;
                }
                foundation::resource::Proxy<foundation::texture::Texture> tex =
                    Context().Resources()->Bind<foundation::texture::Texture>(src.textureIds[i]);
                // Track the proxy and the raw view captured into the material: a texture
                // hot reload (a cook) destroys that view under us, so OnUpdate watches for
                // the proxy's view changing and rebuilds the preview material (otherwise
                // bind group set 2 keeps a dangling descriptor and validation reports a null
                // imageView).
                m_previewTextures.PushBack(tex);
                m_previewTextureViews.PushBack(tex ? tex->View() : nullptr);
                if (tex && tex->View() != nullptr)
                {
                    material->SetDefaultTexture(src.textureSlots[i].AsView(), tex->View());
                }
            }
        }

        m_previewMaterial = material;
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            if (engine::render::MeshComponent* mc = meshes->Get(m_shape))
            {
                mc->SetMaterial(RefPtr<foundation::materials::Material>(
                    m_previewMaterial.Get())); // direct override
            }
        }
    }

    void MaterialEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        if (const MaterialPreviewSettings* section = store->Find<MaterialPreviewSettings>())
        {
            for (const MaterialPreviewPref& p : section->prefs)
            {
                if (p.asset == InstanceId())
                {
                    m_previewShape = p.shape;
                    m_previewMeshGuid = p.mesh;
                    return;
                }
            }
        }
    }

    void MaterialEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        MaterialPreviewSettings& section = store->Section<MaterialPreviewSettings>();
        bool found = false;
        for (MaterialPreviewPref& p : section.prefs)
        {
            if (p.asset == InstanceId())
            {
                p.shape = m_previewShape;
                p.mesh = m_previewMeshGuid;
                found = true;
                break;
            }
        }
        if (!found)
        {
            section.prefs.PushBack(
                MaterialPreviewPref{InstanceId(), m_previewShape, m_previewMeshGuid});
        }
        store->MarkChanged<MaterialPreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    void MaterialEditorPage::ApplyPreviewMesh()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr || !m_shape.IsAssigned())
        {
            return;
        }
        auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>();
        engine::render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_shape) : nullptr;
        if (mc == nullptr)
        {
            return;
        }

        if (!m_previewMeshGuid.IsNil() && Context().Resources() != nullptr)
        {
            m_previewMesh = nullptr;
            mc->mesh.SetDirect(core::RefPtr<foundation::geometry::StaticMesh>{});
            mc->mesh.SetId(m_previewMeshGuid);
            mc->mesh.Bind(*Context().Resources());
            FramePreview();
            return;
        }

        namespace geometry = foundation::geometry;
        switch (m_previewShape)
        {
        case 1:
            m_previewMesh = geometry::Primitives::Cube(Allocator(), 1.4f);
            break;
        case 2:
            m_previewMesh = geometry::Primitives::Plane(Allocator(), 2.0f, 2.0f);
            break;
        case 3:
            m_previewMesh = geometry::Primitives::Cylinder(Allocator(), 0.7f, 1.6f, 48);
            break;
        case 4:
            m_previewMesh = geometry::Primitives::Torus(Allocator(), 0.8f, 0.35f, 48, 24);
            break;
        case 5:
            m_previewMesh = geometry::Primitives::Cone(Allocator(), 0.8f, 1.6f, 48);
            break;
        default:
            m_previewMesh = geometry::Primitives::Sphere(Allocator(), 1.0f, 48, 24);
            break;
        }
        mc->mesh.SetId(Guid{});
        mc->mesh = m_previewMesh.Get(); // direct override (runtime-built, not an asset)
        FramePreview();
    }

    void MaterialEditorPage::FramePreview()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* meshes = scenePtr != nullptr ? scenePtr->GetSystem<engine::render::MeshComponentManager>() : nullptr;
        engine::render::MeshComponent* mc = meshes != nullptr ? meshes->Get(m_shape) : nullptr;
        const foundation::geometry::StaticMesh* mesh = mc != nullptr ? mc->mesh.Get() : nullptr;
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

    void MaterialEditorPage::ReadUniform(StringView name, void* out, usize bytes) const
    {
        const materials::MaterialSource& s = Asset()->source;
        StringView effect;
        StringView parameter;
        if (materials::SplitEffectPropertyName(name, effect, parameter))
        {
            for (const materials::MaterialEffectGroupSource& group : s.effects)
            {
                if (group.name.AsView() != effect)
                    continue;
                for (usize i = 0; i < group.propertyNames.Size() && i < group.propertyOffsets.Size(); ++i)
                    if (group.propertyNames[i].AsView() == parameter && group.propertyOffsets[i] + bytes <= group.values.Size())
                        MemCopy(out, group.values.Data() + group.propertyOffsets[i], bytes);
                return;
            }
            return;
        }
        for (usize i = 0; i < s.propertyNames.Size(); ++i)
        {
            if (s.propertyNames[i].AsView() != name)
            {
                continue;
            }
            const u32 offset = (i < s.propertyOffsets.Size()) ? s.propertyOffsets[i] : 0u;
            if (offset + bytes <= s.uniformDefaults.Size())
            {
                MemCopy(out, s.uniformDefaults.Data() + offset, bytes);
            }
            return;
        }
    }

    void MaterialEditorPage::WriteUniform(materials::MaterialSource& s, StringView name, const void* value, usize bytes) const
    {
        StringView effect;
        StringView parameter;
        if (materials::SplitEffectPropertyName(name, effect, parameter))
        {
            for (materials::MaterialEffectGroupSource& group : s.effects)
            {
                if (group.name.AsView() != effect)
                    continue;
                for (usize i = 0; i < group.propertyNames.Size() && i < group.propertyOffsets.Size(); ++i)
                    if (group.propertyNames[i].AsView() == parameter && group.propertyOffsets[i] + bytes <= group.values.Size())
                        MemCopy(group.values.Data() + group.propertyOffsets[i], value, bytes);
                return;
            }
            return;
        }
        for (usize i = 0; i < s.propertyNames.Size(); ++i)
        {
            if (s.propertyNames[i].AsView() != name)
            {
                continue;
            }
            const u32 offset = (i < s.propertyOffsets.Size()) ? s.propertyOffsets[i] : 0u;
            if (offset + bytes <= s.uniformDefaults.Size())
            {
                MemCopy(s.uniformDefaults.Data() + offset, value, bytes);
            }
            return;
        }
    }

    RefPtr<foundation::shaders::ShaderSource> MaterialEditorPage::ReadShader(const Guid& shader) const
    {
        if (Context().Project() == nullptr || shader.IsNil())
        {
            return {};
        }
        foundation::content::Instance* product = Context().Project()->CookedDb().GetInstance(shader);
        RefPtr<ISerializable> object =
            product != nullptr ? product->ReadObject() : RefPtr<ISerializable>{};
        return RefPtr<foundation::shaders::ShaderSource>(
            Cast<foundation::shaders::ShaderSource>(object.Get()));
    }

    void MaterialEditorPage::SwitchShader(const Guid& shader)
    {
        if (Asset() == nullptr || shader == Asset()->source.shaderId)
        {
            return;
        }
        if (shader.IsNil())
        {
            SwitchToBuiltIn(materials::BuiltinShading::Pbr);
            return;
        }
        RefPtr<foundation::shaders::ShaderSource> table = ReadShader(shader);
        if (table.Get() == nullptr)
        {
            Context().Notify(NoticeKind::Warning,
                              u8"That shader has not cooked yet: save it (or fix its errors), then pick it again");
            m_reshow = true; // the row shows the shader it still has
            return;
        }
        MaterialEditorPage* self = this;
        ApplyEdit(u8"Shader", Function<void(materials::MaterialSource&)>{
                                  [self, table, shader](materials::MaterialSource& s)
                                  {
                                      pipeline::SetMaterialShader(s, shader, *table);
                                      self->AdoptEffects(s, static_cast<foundation::shaders::ShaderFlags>(table->vertexVariantMask));
                                  }});
    }

    void MaterialEditorPage::SwitchToBuiltIn(materials::BuiltinShading shading)
    {
        if (Asset() == nullptr)
        {
            return;
        }
        const materials::MaterialSource& current = Asset()->source;
        if (current.shaderId.IsNil() && current.shaderName.AsView() == materials::BuiltinShadingOf(shading).shaderName)
        {
            return;
        }
        MaterialEditorPage* self = this;
        ApplyEdit(u8"Shader", Function<void(materials::MaterialSource&)>{
                                  [self, shading](materials::MaterialSource& s)
                                  {
                                      pipeline::SetMaterialBuiltInShader(s, shading);
                                      self->AdoptEffects(s, materials::BuiltinShadingOf(shading).vertexVariants);
                                  }});
    }

    void MaterialEditorPage::BindShader()
    {
        m_shader = {};
        m_shaderVersion = 0;
        m_shaderTable.Clear();
        m_shaderVertexVariants = foundation::shaders::ShaderFlags::None;
        if (Asset() == nullptr)
        {
            return;
        }
        if (Asset()->source.shaderId.IsNil())
        {
            // A built-in: lay its effect groups on the effects' tables. This is a layout, not an
            // edit, as is a shader asset's layout below.
            const Array<String> before = RowLayoutOf(Asset()->source);
            AdoptEffects(Asset()->source, VertexVariantsOf(Asset()->source));
            m_rowsDirty = m_rowsDirty || !SameLayout(before, RowLayoutOf(Asset()->source));
            return;
        }
        const Guid id = Asset()->source.shaderId;
        if (Context().Resources() != nullptr)
        {
            m_shader = Context().Resources()->Bind<foundation::shaders::ShaderResource>(id);
            m_shaderVersion = m_shader ? m_shader->Version() : 0;
        }
        RefPtr<foundation::shaders::ShaderSource> table = ReadShader(id);
        if (table.Get() == nullptr)
        {
            LOG_WARNING(u8"Editor", u8"material '{}': its shader has not cooked - its rows are its own until it does", Title());
            m_rowsDirty = true;
            return;
        }
        m_shaderTable = table->Properties();
        m_shaderVertexVariants = static_cast<foundation::shaders::ShaderFlags>(table->vertexVariantMask);
        // Not an edit: the material follows its shader, as the cook lays it out.
        materials::MaterialSource laid;
        pipeline::LayOutMaterialByShader(Asset()->source, *table, laid);
        pipeline::AssignMaterialSource(Asset()->source, laid);
        AdoptEffects(Asset()->source, m_shaderVertexVariants);
        m_rowsDirty = true;
    }

    void MaterialEditorPage::AdoptEffects(materials::MaterialSource& source, foundation::shaders::ShaderFlags vertexVariants)
    {
        // Without the shader system (no host, as in a test) the groups the source carries are
        // kept as they are.
        if (m_effectSystem == nullptr)
            return;
        materials::AdoptDeclaredEffects(Allocator(), source, m_effectSystem->Effects().All(), vertexVariants);
    }

    foundation::shaders::ShaderFlags MaterialEditorPage::VertexVariantsOf(const materials::MaterialSource& source) const
    {
        if (!source.shaderId.IsNil())
            return m_shaderVertexVariants;
        const materials::BuiltinShadingInfo* builtin = materials::FindBuiltinShadingByShader(source.shaderName.AsView());
        return builtin != nullptr ? builtin->vertexVariants : foundation::shaders::ShaderFlags::None;
    }

    const TypeInfo* MaterialEditorPageFactory::PrimaryType() const
    {
        return &pipeline::MaterialAsset::StaticType();
    }

    UniquePtr<EditorPage> MaterialEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<MaterialEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}

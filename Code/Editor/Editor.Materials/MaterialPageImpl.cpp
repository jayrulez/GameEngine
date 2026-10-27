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
        // Its source reflects as arrays no one edits by hand: the rows are the material's own.
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
        // The user's pick only: a selection set from code (the page showing the kept shape) must not
        // count as a shape picked, which would put the kept mesh away.
        (void)m_shapeChoice->AddEventListener(gui::EventType::Changed, [self](const gui::Event&)
                                              { self->SetPreviewShape(static_cast<u32>(Max(0, self->m_shapeChoice->GetSelectedIndex()))); });
        m_pane->AddControl(*m_shapeChoice);

        // Or a mesh from the project, its real UVs (an imported model), in place of the shape.
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
        // The shader asset recooked (its properties may have changed): re-lay the material by it.
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
        if (Asset() == nullptr)
            return;
        const materials::MaterialSource& src = Asset()->source;

        // The shader: a shader asset, or a built-in (PBR or Unlit) while it is on none.
        {
            gui::PropertyInfo& shader = m_materialRows->Add(u8"shader", gui::PropertyKind::Reference);
            shader.DisplayName = String(u8"Shader");
            shader.Description = String(u8"A shader asset the material is drawn with; cleared, a built-in");
            shader.ReferenceType = &foundation::shaders::ShaderResource::StaticType();
            gui::PropertyInfo& builtIn = m_materialRows->Add(u8"builtIn", gui::PropertyKind::Enum);
            builtIn.DisplayName = String(u8"Built-in");
            const StringView kBuiltIns[] = {u8"PBR", u8"Unlit"};
            AddChoices(builtIn, Span<const StringView>(kBuiltIns, 2));
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
            const String shown = PrettifyPropertyName(name);
            gui::PropertyBag& rows = row == MaterialPropertyRow::Texture ? *m_textureRows : *m_propertyRows;
            gui::PropertyInfo* info = nullptr;
            switch (row)
            {
            case MaterialPropertyRow::None:
                continue; // a sampler has no value to edit (the material system binds it)
            case MaterialPropertyRow::Number:
            {
                info = &rows.Add(name, gui::PropertyKind::Float);
                // A shader asset's property declares its range; the standard PBR factors are 0..1
                // (the normal scale 0..2); any other float is unbounded.
                bool ranged = false;
                for (const foundation::shaders::ShaderProperty& p : m_shaderTable)
                    if (p.name.AsView() == name && p.hasRange)
                    {
                        info->HasRange = true;
                        info->Min = p.rangeMin;
                        info->Max = p.rangeMax;
                        ranged = true;
                    }
                const bool zeroToOne = name == u8"Metallic" || name == u8"Roughness" || name == u8"OcclusionStrength" || name == u8"AlphaCutoff";
                if (!ranged && (zeroToOne || name == u8"NormalScale"))
                {
                    info->HasRange = true;
                    info->Min = 0.0;
                    info->Max = zeroToOne ? 1.0 : 2.0;
                }
                info->Step = 0.01;
                if (info->HasRange)
                    info->Hint = String(u8"slider"); // over its range, as the old page's were
                break;
            }
            case MaterialPropertyRow::Numbers2:
            case MaterialPropertyRow::Numbers3:
            case MaterialPropertyRow::Numbers4:
            {
                const i32 count = row == MaterialPropertyRow::Numbers2 ? 2 : row == MaterialPropertyRow::Numbers3 ? 3 : 4;
                info = &rows.Add(name, gui::PropertyKind::Group);
                info->Hint = Format(u8"Float{}", count);
                const StringView parts[] = {u8"x", u8"y", u8"z", u8"w"};
                for (i32 p = 0; p < count; ++p)
                    rows.Add(gui::JoinPropertyPath(name, parts[p]).AsView(), gui::PropertyKind::Float).Step = 0.01;
                break;
            }
            case MaterialPropertyRow::Integer:
            {
                info = &rows.Add(name, gui::PropertyKind::Integer);
                for (const foundation::shaders::ShaderProperty& p : m_shaderTable)
                    if (p.name.AsView() == name && p.hasRange)
                    {
                        info->HasRange = true;
                        info->Min = p.rangeMin;
                        info->Max = p.rangeMax;
                    }
                break;
            }
            case MaterialPropertyRow::Color:
                info = &rows.Add(name, gui::PropertyKind::Color);
                break;
            case MaterialPropertyRow::ColorWithIntensity:
            {
                // A colour that may pass white: its colour as entered, its intensity in w (the
                // grid's HDR colour, as a reflected one's parts are named).
                info = &rows.Add(name, gui::PropertyKind::Group);
                info->Hint = String(u8"ColorHdr");
                rows.Add(gui::JoinPropertyPath(name, u8"color").AsView(), gui::PropertyKind::Color).ShowsAlpha = false;
                gui::PropertyInfo& intensity = rows.Add(gui::JoinPropertyPath(name, u8"intensity").AsView(), gui::PropertyKind::Float);
                intensity.HasRange = true;
                intensity.Min = 0.0;
                intensity.Max = 1e6;
                intensity.Step = 0.05;
                break;
            }
            case MaterialPropertyRow::Texture:
                // A texture or a render texture (a monitor's screen, a mirror): what makes a
                // Texture, asked of the builders.
                info = &rows.Add(name, gui::PropertyKind::Reference);
                info->ReferenceType = &foundation::texture::Texture::StaticType();
                break;
            }
            if (info != nullptr)
                info->DisplayName = shown;
        }
        gui::PropertyBag* const bags[] = {m_materialRows.Get(), m_propertyRows.Get(), m_textureRows.Get()};
        for (gui::PropertyBag* rows : bags)
        {
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
            // A gesture's previews shown live, as the old page's were: written to the source, the
            // preview material built again, no undo step until the commit.
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
    }

    void MaterialEditorPage::ShowValues()
    {
        if (Asset() == nullptr)
            return;
        const materials::MaterialSource& src = Asset()->source;
        const auto show = [](gui::PropertyBag& rows, StringView path, const gui::Variant& value)
        {
            if (rows.FindInfo(path) == nullptr)
                return;
            const gui::Variant now = rows.GetValue(path, 0);
            if (now.GetType() != value.GetType() || now.Compare(value) != 0)
                rows.SetValue(path, 0, value);
        };
        show(*m_materialRows, u8"shader", gui::Variant(GuidText(src.shaderId)));
        show(*m_materialRows, u8"builtIn", gui::Variant(static_cast<i64>(src.shaderName.AsView() == u8"unlit" ? 1 : 0)));
        if (gui::PropertyInfo* builtIn = m_materialRows->FindInfo(u8"builtIn"); builtIn != nullptr && builtIn->Visible != src.shaderId.IsNil())
        {
            builtIn->Visible = src.shaderId.IsNil(); // a built-in only while it is on no shader asset
            m_materialRows->PropertiesChanged();
        }
        for (const PipelineRow& row : kPipelineRows)
            show(*m_materialRows, row.path, gui::Variant(static_cast<i64>(row.field(const_cast<materials::MaterialSource&>(src)))));
        for (usize i = 0; i < src.propertyNames.Size() && i < src.propertyTypes.Size(); ++i)
        {
            const StringView name = src.propertyNames[i].AsView();
            switch (MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(src.propertyTypes[i])))
            {
            case MaterialPropertyRow::Number:
            {
                f32 v = 0.0f;
                ReadUniform(name, &v, sizeof(v));
                show(*m_propertyRows, name, gui::Variant(static_cast<f64>(v)));
                break;
            }
            case MaterialPropertyRow::Numbers2:
            case MaterialPropertyRow::Numbers3:
            case MaterialPropertyRow::Numbers4:
            {
                Float4 v{0, 0, 0, 0};
                const auto row = MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]));
                const i32 count = row == MaterialPropertyRow::Numbers2 ? 2 : row == MaterialPropertyRow::Numbers3 ? 3 : 4;
                ReadUniform(name, &v, sizeof(f32) * static_cast<usize>(count));
                const f32 parts[] = {v.x, v.y, v.z, v.w};
                const StringView names[] = {u8"x", u8"y", u8"z", u8"w"};
                for (i32 p = 0; p < count; ++p)
                    show(*m_propertyRows, gui::JoinPropertyPath(name, names[p]).AsView(), gui::Variant(static_cast<f64>(parts[p])));
                break;
            }
            case MaterialPropertyRow::Integer:
            {
                i32 v = 0;
                ReadUniform(name, &v, sizeof(v));
                show(*m_propertyRows, name, gui::Variant(static_cast<i64>(v)));
                break;
            }
            case MaterialPropertyRow::Color:
            {
                Float4 v{1, 1, 1, 1};
                ReadUniform(name, &v, sizeof(v));
                show(*m_propertyRows, name, gui::Variant(Color{v.x, v.y, v.z, v.w}));
                break;
            }
            case MaterialPropertyRow::ColorWithIntensity:
            {
                Float4 v{1, 1, 1, 1};
                ReadUniform(name, &v, sizeof(v));
                show(*m_propertyRows, gui::JoinPropertyPath(name, u8"color").AsView(), gui::Variant(Color{v.x, v.y, v.z, 1.0f}));
                show(*m_propertyRows, gui::JoinPropertyPath(name, u8"intensity").AsView(), gui::Variant(static_cast<f64>(v.w)));
                break;
            }
            case MaterialPropertyRow::Texture:
            {
                Guid bound;
                for (usize t = 0; t < src.textureSlots.Size() && t < src.textureIds.Size(); ++t)
                    if (src.textureSlots[t].AsView() == name)
                        bound = src.textureIds[t];
                show(*m_textureRows, name, gui::Variant(GuidText(bound)));
                break;
            }
            default:
                break;
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

    void MaterialEditorPage::EditRow(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        rows.SetValue(path, 0, value); // as the grid's commit leaves it
        Committed(rows, path, value);
    }

    void MaterialEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        if (Asset() == nullptr)
            return;
        MaterialEditorPage* self = this;
        m_reshow = true; // what the edit did, or did not, shown
        if (&rows == m_materialRows.Get())
        {
            if (path == u8"shader")
            {
                SwitchShader(GuidOf(value));
                return;
            }
            if (path == u8"builtIn")
            {
                SwitchToBuiltIn(value.AsInt() == 1);
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
        // A property, or one part of it ("Tint.x", "Emissive.intensity").
        const StringView name = gui::ParentPropertyPath(path).IsEmpty() ? path : gui::ParentPropertyPath(path);
        const StringView part = name.Size() == path.Size() ? StringView{} : path.SubStr(name.Size() + 1, path.Size() - name.Size() - 1);
        const materials::MaterialSource& src = Asset()->source;
        MaterialPropertyRow row = MaterialPropertyRow::None;
        for (usize i = 0; i < src.propertyNames.Size() && i < src.propertyTypes.Size(); ++i)
            if (src.propertyNames[i].AsView() == name)
                row = MaterialPropertyRowFor(static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]));
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
        Array<String> namesBefore;
        for (const String& name : asset->source.propertyNames)
            namesBefore.PushBack(String(name.AsView()));
        BinarySerializer ar(stream, SerializeMode::Read);
        asset->source.Serialize(ar);
        // A shader switched (or its switch undone) changes the rows and what the preview binds.
        if (asset->source.shaderId != shaderBefore)
            BindShader();
        bool sameRows = namesBefore.Size() == asset->source.propertyNames.Size();
        for (usize i = 0; sameRows && i < namesBefore.Size(); ++i)
            sameRows = namesBefore[i].AsView() == asset->source.propertyNames[i].AsView();
        m_rowsDirty = m_rowsDirty || !sameRows;
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
            // A preview: written to the source and shown, the source before the gesture kept.
            if (!m_inGesture)
            {
                m_gestureBefore = SnapshotSource();
                m_inGesture = true;
            }
            mutate(Asset()->source);
            RebuildPreviewMaterial();
            return;
        }
        // The gesture's one step starts from the source before its previews.
        Array<byte> before = m_inGesture ? Move(m_gestureBefore) : SnapshotSource();
        m_inGesture = false;
        // The rows follow a change of what the material holds (a built-in switched): the command's
        // Execute below applies `after` over itself, so it cannot see that change.
        Array<String> namesBefore;
        for (const String& name : Asset()->source.propertyNames)
            namesBefore.PushBack(String(name.AsView()));
        mutate(Asset()->source);
        bool sameRows = namesBefore.Size() == Asset()->source.propertyNames.Size();
        for (usize i = 0; sameRows && i < namesBefore.Size(); ++i)
            sameRows = namesBefore[i].AsView() == Asset()->source.propertyNames[i].AsView();
        m_rowsDirty = m_rowsDirty || !sameRows;
        Array<byte> after = SnapshotSource();
        // The mutation already ran; Execute() re-applies `after` (idempotent).
        (void)Commands().Execute(UniquePtr<IEditorCommand>(Allocator().New<EditMaterialCommand>(*this, mergeKey, Move(before), Move(after)), Allocator()));
    }

    void MaterialEditorPage::CancelGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        ApplySourceBlob(m_gestureBefore); // the previews taken back, the preview with them
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
                // Track the proxy + the RAW view captured into the material: a texture
                // hot-reload (cook) destroys that view under us, so OnUpdate watches for
                // the proxy's view changing and rebuilds the preview material (otherwise
                // set-2 keeps a dangling descriptor - null-imageView validation errors).
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

    void MaterialEditorPage::WriteUniform(materials::MaterialSource& s, StringView name, const void* value, usize bytes)
    {
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
            SwitchToBuiltIn(false);
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
        ApplyEdit(u8"Shader", Function<void(materials::MaterialSource&)>{
                                  [table, shader](materials::MaterialSource& s)
                                  { pipeline::SetMaterialShader(s, shader, *table); }});
    }

    void MaterialEditorPage::SwitchToBuiltIn(bool unlit)
    {
        if (Asset() == nullptr)
        {
            return;
        }
        const materials::MaterialSource& current = Asset()->source;
        if (current.shaderId.IsNil() && (current.shaderName.AsView() == u8"unlit") == unlit)
        {
            return;
        }
        ApplyEdit(u8"Shader", Function<void(materials::MaterialSource&)>{
                                  [unlit](materials::MaterialSource& s)
                                  { pipeline::SetMaterialBuiltInShader(s, unlit); }});
    }

    void MaterialEditorPage::BindShader()
    {
        m_shader = {};
        m_shaderVersion = 0;
        m_shaderTable.Clear();
        if (Asset() == nullptr || Asset()->source.shaderId.IsNil())
        {
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
        // Not an edit: the material follows its shader, as the cook lays it out.
        materials::MaterialSource laid;
        pipeline::LayOutMaterialByShader(Asset()->source, *table, laid);
        pipeline::AssignMaterialSource(Asset()->source, laid);
        m_rowsDirty = true;
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

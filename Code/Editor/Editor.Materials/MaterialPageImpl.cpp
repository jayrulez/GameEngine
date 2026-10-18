// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Materials - :page partition.
//
// MaterialEditorPage (Sedulous MaterialEditorPage shape): edits a MaterialAsset - a preview
// sphere lit by a default sun + procedural sky on the left, the material's parameters on the
// right. The authored form IS the runtime source (MaterialSource), so Save just writes the
// object back and requests a re-cook; every live proxy bound to the cooked product then
// hot-swaps (the same reload path texture/mesh edits ride).
//
// Edits are BLOB-SNAPSHOT commands: each edit captures the whole serialized MaterialSource
// before/after (it is tiny) - robust against the source's parallel-array layout, exact undo,
// and consecutive scrubs of the same field merge into one entry. Every apply rebuilds the
// preview's runtime material in place, so scrubbing reads live on the sphere.
//
// RegisterMaterialEditor is the module's RegisterEditor entry point: registers the
// MaterialAsset page factory and the "PBR Material" / "Unlit Material" creators (presets).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.materials;

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
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
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;   // PreviewViewport (shared viewport + preview scene + camera + render loop)

using namespace foundation::core;
namespace core = foundation::core;
namespace materials = foundation::materials;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace ui = foundation::ui;
namespace vg = foundation::vg;

namespace editor
{
    using app::PrettifyPropertyName; // a row's label from its property's name
    using app::ResourceRefEditor;    // the picker row

    MaterialEditorPage::MaterialEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                           ui::runtime::UIHost& uiHost,
                                           foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // The edited object: the instance's MaterialAsset (kept live; Save writes it back).
        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<pipeline::MaterialAsset>(Cast<pipeline::MaterialAsset>(object.Get()));
        StringView missing;
        if (m_asset.Get() != nullptr &&
            !materials::ForwardMaterialSourceIsComplete(m_asset->source, &missing))
        {
            LOG_ERROR(u8"Editor",
                      u8"material '{}' is missing the forward shader property '{}' (a stale "
                      u8"source) - re-create it; the runtime refuses to build it",
                      m_title, missing);
        }
        if (m_asset.Get() == nullptr)
        {
            LOG_ERROR(u8"Editor", u8"material '{}' failed to read - page opens empty", m_title);
        }
        BindShader(); // a material on a shader asset shows the shader's current properties

        // Shared preview substrate (viewport + preview scene + fly camera + render loop).
        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"material.preview");
        // The scene's clock runs only while it simulates: a material on a shader that animates
        // with Time() (a scrolling surface, moving vertices, wind) moves in its preview.
        m_preview->SetSimulationEnabled(true);
        m_preview->Camera().position = Float3{0.0f, 0.9f, 2.6f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});

        BuildPreviewScene();

        // The context assigns the page's instance id AFTER construction (OpenPage), but the
        // preview-pref restore below keys on it - set it from the instance now (the context's
        // later SetInstanceId writes the same value).
        SetInstanceId(instance.Id());

        // Restore this material's saved preview choice (shape or mesh asset) before the grid
        // builds its rows, so the Shape/Mesh rows show the persisted state.
        LoadPreviewPref();
        if (m_previewShape != 0 || !m_previewMeshGuid.IsNil())
        {
            ApplyPreviewMesh();
        }

        m_grid = MakeRef<foundation::ui::toolkit::PropertyGrid>(Allocator());
        RebuildGrid();

        // Inset the property grid off the pane edge (matches the scene inspector / hierarchy).
        auto gridColumn = MakeRef<foundation::ui::FlexLayout>(Allocator());
        gridColumn->Direction = foundation::ui::Orientation::Vertical;
        gridColumn->Padding = foundation::ui::Thickness{8, 6};
        {
            foundation::ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            gridColumn->AddView(m_grid.Get(), grow);
        }

        auto split = MakeRef<foundation::ui::toolkit::SplitView>(Allocator());
        split->SetSplitRatio(0.62f);
        split->SetPanes(m_preview->View(), gridColumn.Get());
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *split);

        RebuildPreviewMaterial();
    }

    MaterialEditorPage::~MaterialEditorPage() = default;

    void MaterialEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // sync the page's actions to it each frame
        }
        if (m_preview)
        {
            m_preview->Update(dt);
        }

        // Texture hot-reload watchdog: rebuild the preview material when any bound
        // texture's live view no longer matches what the material captured.
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
        if (m_gridDirty)
        {
            m_gridDirty = false;
            RebuildGrid();
        }

        for (const Function<void()>& refresher : m_refreshers)
        {
            refresher();
        }
    }

    void MaterialEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                            foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    Status MaterialEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            // Refresh the cooked product so every scene's proxy hot-swaps to the new look.
            m_context->RequestCook(false);
            LOG_INFO(u8"Editor", u8"saved material '{}'", m_title);
        }
        return saved;
    }

    void MaterialEditorPage::OnClose()
    {
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    void MaterialEditorPage::ApplySourceBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        const Guid shaderBefore = m_asset->source.shaderId;
        Array<String> namesBefore;
        for (const String& name : m_asset->source.propertyNames)
        {
            namesBefore.PushBack(String(name.AsView()));
        }
        BinarySerializer ar(stream, SerializeMode::Read);
        m_asset->source.Serialize(ar);
        // A shader switched (or its switch undone) changes the rows and what the preview binds.
        if (m_asset->source.shaderId != shaderBefore)
        {
            BindShader();
        }
        bool sameRows = namesBefore.Size() == m_asset->source.propertyNames.Size();
        for (usize i = 0; sameRows && i < namesBefore.Size(); ++i)
        {
            sameRows = namesBefore[i].AsView() == m_asset->source.propertyNames[i].AsView();
        }
        m_gridDirty = m_gridDirty || !sameRows;
        RebuildPreviewMaterial();
    }

    Array<byte> MaterialEditorPage::SnapshotSource() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        const_cast<materials::MaterialSource&>(m_asset->source).Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void MaterialEditorPage::ApplyEdit(StringView mergeKey,
                                       Function<void(materials::MaterialSource&)> mutate)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        Array<byte> before = SnapshotSource();
        mutate(m_asset->source);
        Array<byte> after = SnapshotSource();
        // The mutation already ran; Execute() re-applies `after` (idempotent).
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditMaterialCommand>(*this, mergeKey, Move(before), Move(after)),
            Allocator()));
    }

    void MaterialEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }

        m_sphere = scenePtr->CreateEntity(u8"PreviewSphere");
        m_previewMesh = foundation::geometry::Primitives::Sphere(Allocator(), 1.0f, 48, 24);
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            engine::render::MeshComponent& mc = meshes->Add(m_sphere);
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
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (m_asset.Get() == nullptr || scenePtr == nullptr || !m_sphere.IsAssigned())
        {
            return;
        }
        const materials::MaterialSource& src = m_asset->source;

        // A shader asset draws under its resource's name (binding it registered the source).
        const StringView shaderName =
            m_shader ? m_shader->Name() : src.shaderName.AsView();
        RefPtr<materials::Material> material =
            materials::MaterialFromSource(Allocator(), src, shaderName);

        m_previewTextures.Clear();
        m_previewTextureViews.Clear();
        if (m_context->Resources() != nullptr)
        {
            for (usize i = 0; i < src.textureSlots.Size() && i < src.textureIds.Size(); ++i)
            {
                if (src.textureIds[i].IsNil())
                {
                    continue;
                }
                foundation::resource::Proxy<foundation::texture::Texture> tex =
                    m_context->Resources()->Bind<foundation::texture::Texture>(src.textureIds[i]);
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
            if (engine::render::MeshComponent* mc = meshes->Get(m_sphere))
            {
                mc->SetMaterial(RefPtr<foundation::materials::Material>(
                    m_previewMaterial.Get())); // direct override
            }
        }
    }

    void MaterialEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
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
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
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
        m_context->RequestProjectEditorSettingsSave();
    }

    void MaterialEditorPage::ApplyPreviewMesh()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr || !m_sphere.IsAssigned())
        {
            return;
        }
        auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>();
        engine::render::MeshComponent* mc = (meshes != nullptr) ? meshes->Get(m_sphere) : nullptr;
        if (mc == nullptr)
        {
            return;
        }

        if (!m_previewMeshGuid.IsNil() && m_context->Resources() != nullptr)
        {
            m_previewMesh = nullptr;
            mc->mesh.SetDirect(core::RefPtr<foundation::geometry::StaticMesh>{});
            mc->mesh.SetId(m_previewMeshGuid);
            mc->mesh.Bind(*m_context->Resources());
            FramePreview(mc->mesh.Get());
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
        FramePreview(m_previewMesh.Get());
    }

    void MaterialEditorPage::FramePreview(const foundation::geometry::StaticMesh* mesh)
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

    void MaterialEditorPage::RebuildGrid()
    {
        m_grid->Clear();
        m_refreshers.Clear();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        MaterialEditorPage* self = this;
        const materials::MaterialSource& src = m_asset->source;

        // --- Material: shader + pipeline state ---
        // The shader: a shader asset, or nil for a built-in (PBR or Unlit, picked below).
        {
            const StringView shaderTypes[] = {u8"ShaderAsset"};
            auto shader = MakeRef<ResourceRefEditor>(Allocator(), StringView(u8"Shader"),
                                                     AssetNameFor(src.shaderId),
                                                     StringView(u8"Material"),
                                                     Span<const StringView>{shaderTypes, 1});
            shader->SetEmptyText(u8"(built-in)");
            ResourceRefEditor* raw = shader.Get();
            raw->BindAsset(
                *m_context,
                [self]() -> Guid
                { return self->m_asset.Get() != nullptr ? self->m_asset->source.shaderId : Guid{}; },
                [self](const Guid& picked) { self->SwitchShader(picked); });
            AddEditor(raw, [raw]() { raw->Refresh(); });
        }
        if (src.shaderId.IsNil())
        {
            static constexpr StringView kBuiltIns[] = {u8"PBR", u8"Unlit"};
            auto builtIn = MakeRef<ui::toolkit::EnumEditor>(
                Allocator(), StringView(u8"Built-in"),
                src.shaderName.AsView() == u8"unlit" ? 1 : 0, Span<const StringView>{kBuiltIns, 2},
                Function<void(i32)>{[self](i32 index) { self->SwitchToBuiltIn(index == 1); }},
                StringView(u8"Material"));
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(builtIn.Get()));
        }

        // --- Preview: geometry the material is shown on (page-local, not saved) ---
        {
            static constexpr StringView kShapes[] = {u8"Sphere",   u8"Cube",  u8"Plane",
                                                     u8"Cylinder", u8"Torus", u8"Cone"};
            auto shape = MakeRef<ui::toolkit::EnumEditor>(
                Allocator(), StringView(u8"Shape"), static_cast<i32>(m_previewShape),
                Span<const StringView>{kShapes, 6},
                Function<void(i32)>{[self](i32 index)
                                    {
                                        self->m_previewShape =
                                            static_cast<u32>(core::Max(0, index));
                                        self->m_previewMeshGuid =
                                            Guid{}; // shape picks override an asset mesh
                                        self->ApplyPreviewMesh();
                                        self->SavePreviewPref();
                                    }},
                StringView(u8"Preview"));
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(shape.Get()));

            // The preview mesh: nil is the primitive shape above.
            const StringView meshTypes[] = {u8"StaticMeshAsset", u8"SkinnedMeshAsset"};
            auto meshRow = MakeRef<ResourceRefEditor>(Allocator(), StringView(u8"Mesh"),
                                                      StringView(u8"(primitive)"),
                                                      StringView(u8"Preview"),
                                                      Span<const StringView>{meshTypes, 2});
            meshRow->SetEmptyText(u8"(primitive)");
            meshRow->BindAsset(*m_context, [self]() { return self->m_previewMeshGuid; },
                               [self](const Guid& picked)
                               {
                                   self->m_previewMeshGuid = picked;
                                   self->ApplyPreviewMesh();
                                   self->SavePreviewPref();
                               });
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(meshRow.Get()));
        }

        static constexpr StringView kBlendItems[] = {u8"Opaque",     u8"Masked",
                                                     u8"AlphaBlend", u8"Additive",
                                                     u8"Multiply",   u8"PremultipliedAlpha"};
        static constexpr StringView kDepthItems[] = {u8"Disabled", u8"ReadWrite", u8"ReadOnly",
                                                     u8"WriteOnly"};
        static constexpr StringView kCullItems[] = {u8"None", u8"Back", u8"Front"};
        // These render-state fields are now typed enums (each `: u8`); this bespoke dropdown edits
        // the index in place, so it aliases the enum byte through its identical u8 representation.
        // (A future reflection-first material page would use the reflected enum directly.)
        AddPipelineEnumRow(u8"Blend", Span<const StringView>{kBlendItems, 6},
                           [](materials::MaterialSource& s) -> u8& {
                               return reinterpret_cast<u8&>(s.blendMode);
                           });
        AddPipelineEnumRow(u8"Depth", Span<const StringView>{kDepthItems, 4},
                           [](materials::MaterialSource& s) -> u8& {
                               return reinterpret_cast<u8&>(s.depthMode);
                           });
        AddPipelineEnumRow(u8"Cull", Span<const StringView>{kCullItems, 3},
                           [](materials::MaterialSource& s) -> u8& {
                               return reinterpret_cast<u8&>(s.cullMode);
                           });

        // --- Properties: the source's uniform table (Float, Float4, Color, ColorHdr) ---
        for (usize i = 0; i < src.propertyNames.Size(); ++i)
        {
            const auto type = static_cast<materials::MaterialPropertyType>(src.propertyTypes[i]);
            const MaterialPropertyRow row = MaterialPropertyRowFor(type);
            const String name = src.propertyNames[i]; // copy: the grid outlives rebuilds of src arrays
            if (row == MaterialPropertyRow::Number)
            {
                auto value = [self, name]() -> f64
                {
                    f32 v = 0.0f;
                    self->ReadUniform(name.AsView(), &v, sizeof(v));
                    return static_cast<f64>(v);
                };
                auto write = [self, name](f32 f)
                {
                    self->ApplyEdit(name.AsView(),
                                    Function<void(materials::MaterialSource&)>{
                                        [self, name, f](materials::MaterialSource&)
                                        { self->WriteUniform(name.AsView(), &f, sizeof(f)); }});
                };
                // The standard PBR factors are all 0..1 - slider+field like the scene
                // inspector's range rows; unknown floats keep the unbounded field.
                // A shader asset's property declares its own range.
                f32 lo = 0.0f;
                f32 hi = 1.0f;
                bool ranged = false;
                for (const foundation::shaders::ShaderProperty& p : m_shaderTable)
                {
                    if (p.name.AsView() == name.AsView() && p.hasRange)
                    {
                        lo = p.rangeMin;
                        hi = p.rangeMax;
                        ranged = true;
                    }
                }
                const bool zeroToOne =
                    name.AsView() == u8"Metallic" || name.AsView() == u8"Roughness" ||
                    name.AsView() == u8"OcclusionStrength" || name.AsView() == u8"AlphaCutoff";
                const bool zeroToTwo = name.AsView() == u8"NormalScale";
                if (!ranged && zeroToTwo)
                {
                    hi = 2.0f;
                }
                if (ranged || zeroToOne || zeroToTwo)
                {
                    auto editor = MakeRef<ui::toolkit::RangeEditor>(
                        Allocator(), name.AsView(), static_cast<f32>(value()), lo, hi, 0.01f,
                        Function<void(f32)>{[write](f32 v) { write(v); }},
                        StringView(u8"Properties"));
                    editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                    AddEditor(editor.Get(), [value, raw = editor.Get()]()
                              { raw->SetValue(static_cast<f32>(value())); });
                }
                else
                {
                    auto editor = MakeRef<ui::toolkit::FloatEditor>(
                        Allocator(), name.AsView(), value(), 0.0, 1e9, 0.01, 3,
                        Function<void(f64)>{[write](f64 v) { write(static_cast<f32>(v)); }},
                        StringView(u8"Properties"));
                    editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                    AddEditor(editor.Get(),
                              [value, raw = editor.Get()]() { raw->SetValue(value()); });
                }
            }
            else if (row == MaterialPropertyRow::Numbers2)
            {
                auto read = [self, name]() -> Float2
                {
                    Float2 v{0, 0};
                    self->ReadUniform(name.AsView(), &v, sizeof(v));
                    return v;
                };
                auto editor = MakeRef<ui::toolkit::Float2Editor>(
                    Allocator(), name.AsView(), read(), -100000.0f, 100000.0f, 0.01f,
                    Function<void(Float2)>{[self, name](Float2 v)
                                           {
                                               self->ApplyEdit(
                                                   name.AsView(),
                                                   Function<void(materials::MaterialSource&)>{
                                                       [self, name, v](materials::MaterialSource&)
                                                       { self->WriteUniform(name.AsView(), &v, sizeof(v)); }});
                                           }},
                    StringView(u8"Properties"));
                editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                AddEditor(editor.Get(), [read, raw = editor.Get()]() { raw->SetValue(read()); });
            }
            else if (row == MaterialPropertyRow::Numbers3)
            {
                auto read = [self, name]() -> Float3
                {
                    Float3 v{0, 0, 0};
                    self->ReadUniform(name.AsView(), &v, sizeof(v));
                    return v;
                };
                auto editor = MakeRef<ui::toolkit::Float3Editor>(
                    Allocator(), name.AsView(), read(), -100000.0f, 100000.0f, 0.01f,
                    Function<void(Float3)>{[self, name](Float3 v)
                                           {
                                               self->ApplyEdit(
                                                   name.AsView(),
                                                   Function<void(materials::MaterialSource&)>{
                                                       [self, name, v](materials::MaterialSource&)
                                                       { self->WriteUniform(name.AsView(), &v, sizeof(v)); }});
                                           }},
                    StringView(u8"Properties"));
                editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                AddEditor(editor.Get(), [read, raw = editor.Get()]() { raw->SetValue(read()); });
            }
            else if (row == MaterialPropertyRow::Integer)
            {
                auto read = [self, name]() -> i64
                {
                    i32 v = 0;
                    self->ReadUniform(name.AsView(), &v, sizeof(v));
                    return static_cast<i64>(v);
                };
                // A shader asset's int may declare its range.
                i64 lo = static_cast<i64>(-2147483647 - 1);
                i64 hi = static_cast<i64>(2147483647);
                for (const foundation::shaders::ShaderProperty& p : m_shaderTable)
                {
                    if (p.name.AsView() == name.AsView() && p.hasRange)
                    {
                        lo = static_cast<i64>(p.rangeMin);
                        hi = static_cast<i64>(p.rangeMax);
                    }
                }
                auto editor = MakeRef<ui::toolkit::IntEditor>(
                    Allocator(), name.AsView(), read(), lo, hi,
                    Function<void(i64)>{[self, name](i64 value)
                                        {
                                            const i32 v = static_cast<i32>(value);
                                            self->ApplyEdit(
                                                name.AsView(),
                                                Function<void(materials::MaterialSource&)>{
                                                    [self, name, v](materials::MaterialSource&)
                                                    { self->WriteUniform(name.AsView(), &v, sizeof(v)); }});
                                        }},
                    StringView(u8"Properties"));
                editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                AddEditor(editor.Get(), [read, raw = editor.Get()]() { raw->SetValue(read()); });
            }
            else if (row == MaterialPropertyRow::Numbers4 || row == MaterialPropertyRow::Color ||
                     row == MaterialPropertyRow::ColorWithIntensity)
            {
                auto read = [self, name]() -> Float4
                {
                    Float4 v{1, 1, 1, 1};
                    self->ReadUniform(name.AsView(), &v, sizeof(v));
                    return v;
                };
                auto write = [self, name](Float4 v)
                {
                    self->ApplyEdit(name.AsView(),
                                    Function<void(materials::MaterialSource&)>{
                                        [self, name, v](materials::MaterialSource&)
                                        { self->WriteUniform(name.AsView(), &v, sizeof(v)); }});
                };
                if (row == MaterialPropertyRow::Numbers4)
                {
                    // Plain numbers: only a declared colour property gets a colour picker.
                    auto editor = MakeRef<ui::toolkit::Float4Editor>(
                        Allocator(), name.AsView(), read(), -100000.0f, 100000.0f, 0.01f,
                        Function<void(Float4)>{[write](Float4 v) { write(v); }},
                        StringView(u8"Properties"));
                    editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                    AddEditor(editor.Get(), [read, raw = editor.Get()]() { raw->SetValue(read()); });
                    continue;
                }
                // The colour as entered (sRGB, what the picker shows). A ColorHdr keeps its
                // intensity in w, edited on its own row, so the picker's alpha is not it.
                const bool hdr = row == MaterialPropertyRow::ColorWithIntensity;
                auto colorOf = [read, hdr]() -> Color
                {
                    const Float4 v = read();
                    return Color{v.x, v.y, v.z, hdr ? 1.0f : v.w};
                };
                auto editor = MakeRef<ui::toolkit::ColorEditor>(
                    Allocator(), name.AsView(), colorOf(),
                    Function<void(Color)>{[read, write, hdr](Color c)
                                          { write(Float4{c.r, c.g, c.b, hdr ? read().w : c.a}); }},
                    StringView(u8"Properties"));
                editor->SetDisplayName(PrettifyPropertyName(name.AsView()).AsView());
                AddEditor(editor.Get(), [colorOf, raw = editor.Get()]() { raw->SetValue(colorOf()); });
                if (hdr)
                {
                    String intensityName = name;
                    intensityName += u8"Intensity";
                    auto intensity = MakeRef<ui::toolkit::FloatEditor>(
                        Allocator(), intensityName.AsView(), static_cast<f64>(read().w), 0.0, 1e6,
                        0.05, 3,
                        Function<void(f64)>{[read, write](f64 i)
                                            {
                                                Float4 v = read();
                                                v.w = static_cast<f32>(i);
                                                write(v);
                                            }},
                        StringView(u8"Properties"));
                    intensity->SetDisplayName(PrettifyPropertyName(intensityName.AsView()).AsView());
                    AddEditor(intensity.Get(), [read, raw = intensity.Get()]()
                              { raw->SetValue(static_cast<f64>(read().w)); });
                }
            }
            else if (row == MaterialPropertyRow::Texture)
            {
                AddTextureRow(name);
            }
            // A sampler has no value to edit (the material system binds it); no matrix rows.
        }
    }

    void MaterialEditorPage::AddPipelineEnumRow(StringView label, Span<const StringView> items,
                                                u8& (*field)(materials::MaterialSource&))
    {
        MaterialEditorPage* self = this;
        auto read = [self, field]() -> i32
        {
            return (self->m_asset.Get() != nullptr) ? static_cast<i32>(field(self->m_asset->source))
                                                    : 0;
        };
        const String key(label);
        auto editor = MakeRef<ui::toolkit::EnumEditor>(
            Allocator(), label, read(), items,
            Function<void(i32)>{[self, field, key](i32 index)
                                {
                                    self->ApplyEdit(key.AsView(),
                                                    Function<void(materials::MaterialSource&)>{
                                                        [field, index](materials::MaterialSource& s)
                                                        { field(s) = static_cast<u8>(index); }});
                                }},
            StringView(u8"Material"));
        AddEditor(editor.Get(), [read, raw = editor.Get()]() { raw->SetValue(read()); });
    }

    void MaterialEditorPage::AddTextureRow(const String& slot)
    {
        MaterialEditorPage* self = this;
        auto target = [self, slot]() -> Guid
        {
            if (self->m_asset.Get() == nullptr)
            {
                return Guid{};
            }
            const materials::MaterialSource& s = self->m_asset->source;
            for (usize i = 0; i < s.textureSlots.Size() && i < s.textureIds.Size(); ++i)
            {
                if (s.textureSlots[i].AsView() == slot.AsView())
                {
                    return s.textureIds[i];
                }
            }
            return Guid{};
        };
        // A render texture samples like any texture (a monitor's screen, a mirror).
        const StringView textureTypes[] = {u8"TextureAsset", u8"RenderTextureAsset"};
        auto editor = MakeRef<ResourceRefEditor>(Allocator(), slot.AsView(),
                                                 AssetNameFor(target()), StringView(u8"Textures"),
                                                 Span<const StringView>{textureTypes, 2});
        editor->SetDisplayName(PrettifyPropertyName(slot.AsView()).AsView());
        ResourceRefEditor* raw = editor.Get();
        // Pick, drop and clear are one write: nil removes the slot's binding.
        raw->BindAsset(
            *m_context, target,
            [self, slot](const Guid& picked)
            {
                self->ApplyEdit(slot.AsView(),
                                Function<void(materials::MaterialSource&)>{
                                    [slot, picked](materials::MaterialSource& s)
                                    {
                                        for (usize i = 0; i < s.textureSlots.Size(); ++i)
                                        {
                                            if (s.textureSlots[i].AsView() != slot.AsView())
                                            {
                                                continue;
                                            }
                                            if (picked.IsNil())
                                            {
                                                s.textureSlots.RemoveAt(i);
                                                s.textureIds.RemoveAt(i);
                                            }
                                            else
                                            {
                                                s.textureIds[i] = picked;
                                            }
                                            return;
                                        }
                                        if (!picked.IsNil())
                                        {
                                            s.textureSlots.PushBack(String(slot.AsView()));
                                            s.textureIds.PushBack(picked);
                                        }
                                    }});
            });
        AddEditor(raw, [raw]() { raw->Refresh(); });
    }

    StringView MaterialEditorPage::AssetNameFor(const Guid& target)
    {
        return m_context->AssetNameFor(target);
    }

    void MaterialEditorPage::AddEditor(foundation::ui::toolkit::PropertyEditor* editor,
                                       Function<void()> refresher)
    {
        m_grid->AddProperty(RefPtr<foundation::ui::toolkit::PropertyEditor>(editor));
        foundation::ui::toolkit::PropertyEditor* raw = editor;
        m_refreshers.PushBack(Function<void()>{[raw, pull = Move(refresher)]()
                                               {
                                                   if (!raw->IsEditing())
                                                   {
                                                       pull();
                                                   }
                                               }});
    }

    void MaterialEditorPage::ReadUniform(StringView name, void* out, usize bytes) const
    {
        const materials::MaterialSource& s = m_asset->source;
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

    void MaterialEditorPage::WriteUniform(StringView name, const void* value, usize bytes)
    {
        materials::MaterialSource& s = m_asset->source;
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
        if (m_context->Project() == nullptr || shader.IsNil())
        {
            return {};
        }
        foundation::content::Instance* product = m_context->Project()->CookedDb().GetInstance(shader);
        RefPtr<ISerializable> object =
            product != nullptr ? product->ReadObject() : RefPtr<ISerializable>{};
        return RefPtr<foundation::shaders::ShaderSource>(
            Cast<foundation::shaders::ShaderSource>(object.Get()));
    }

    void MaterialEditorPage::SwitchShader(const Guid& shader)
    {
        if (m_asset.Get() == nullptr || shader == m_asset->source.shaderId)
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
            m_context->Notify(NoticeKind::Warning,
                              u8"That shader has not cooked yet - save it (or fix its errors), "
                              u8"then pick it again");
            return;
        }
        ApplyEdit(u8"Shader", Function<void(materials::MaterialSource&)>{
                                  [table, shader](materials::MaterialSource& s)
                                  { pipeline::SetMaterialShader(s, shader, *table); }});
    }

    void MaterialEditorPage::SwitchToBuiltIn(bool unlit)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        const materials::MaterialSource& current = m_asset->source;
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
        if (m_asset.Get() == nullptr || m_asset->source.shaderId.IsNil())
        {
            return;
        }
        const Guid id = m_asset->source.shaderId;
        if (m_context->Resources() != nullptr)
        {
            m_shader = m_context->Resources()->Bind<foundation::shaders::ShaderResource>(id);
            m_shaderVersion = m_shader ? m_shader->Version() : 0;
        }
        RefPtr<foundation::shaders::ShaderSource> table = ReadShader(id);
        if (table.Get() == nullptr)
        {
            LOG_WARNING(u8"Editor",
                        u8"material '{}': its shader has not cooked - its rows are its own until "
                        u8"it does",
                        m_title);
            return;
        }
        m_shaderTable = table->Properties();
        // Not an edit: the material follows its shader, as the cook lays it out.
        materials::MaterialSource laid;
        pipeline::LayOutMaterialByShader(m_asset->source, *table, laid);
        pipeline::AssignMaterialSource(m_asset->source, laid);
        m_gridDirty = true;
    }

    const TypeInfo* MaterialEditorPageFactory::PrimaryType() const
    {
        return &pipeline::MaterialAsset::StaticType();
    }

    UniquePtr<EditorPage>
    MaterialEditorPageFactory::CreatePage(EditorContext& context,
                                          foundation::content::Instance& instance)
    {
        MaterialEditorPage* page =
            editor::EditorRootAllocator().New<MaterialEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}

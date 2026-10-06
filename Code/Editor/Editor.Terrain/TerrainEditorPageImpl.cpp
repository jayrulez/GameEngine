// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `editor.terrain` module (implementation).
//
// The heavy engine.terrain / engine.render / engine.scene / ui.toolkit imports live here, out of the
// interface (GCC module-merger hygiene). The page is a composition + preview surface: it edits the
// TerrainAsset's references + layers + castShadows and previews the cooked product; brushes are not
// hosted here (they are scene-viewport IViewportTools).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.terrain;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.render;
import engine.render;
import engine.terrain;              // TerrainComponentManager / TerrainComponent
import foundation.terrain;          // ChunksPerSide (stats)
import foundation.terrain.resource; // TerrainResource
import foundation.heightfield;      // Heightfield (stats via the resolved product)
import foundation.resource;
import foundation.ui;
import foundation.ui.toolkit;       // SplitView, PropertyGrid, BoolEditor, FloatEditor
import foundation.ui.runtime;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;
namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
namespace ui = foundation::ui;
namespace terrain = foundation::terrain;

namespace editor
{
    namespace
    {
        String AssetName(EditorContext* ctx, const Guid& id)
        {
            if (id.IsNil())
            {
                return String(u8"(none)");
            }
            if (ctx != nullptr && ctx->Project() != nullptr)
            {
                if (auto* inst = ctx->Project()->SourceDb().GetInstance(id))
                {
                    return String(inst->Name());
                }
            }
            return String(u8"(missing)");
        }
    }

    Array<String> TerrainStatLines(const terrain::TerrainResource& product)
    {
        Array<String> lines;
        if (foundation::heightfield::Heightfield* hf = product.heightfield.Get())
        {
            const i32 size = hf->Size();
            const i32 k = terrain::ChunksPerSide(size);
            const Float2 ws = hf->WorldSize();
            lines.PushBack(Format(u8"Grid: {} x {}", size, size));
            lines.PushBack(Format(u8"World: {} x {} m", static_cast<i64>(ws.x),
                                  static_cast<i64>(ws.y)));
            lines.PushBack(Format(u8"Chunks: {} x {} = {}", k, k, k * k));
        }
        else
        {
            lines.PushBack(String(u8"Heightfield: unresolved"));
        }
        lines.PushBack(Format(u8"Palette layers: {}", product.PaletteCount()));
        lines.PushBack(
            Format(u8"Cast shadows: {}", product.castShadows ? StringView(u8"yes")
                                                             : StringView(u8"no")));
        return lines;
    }

    TerrainEditorPage::TerrainEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                         ui::runtime::UIHost& uiHost,
                                         foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        SetInstanceId(instance.Id());
        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<pipeline::TerrainAsset>(Cast<pipeline::TerrainAsset>(object.Get()));

        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"terrain.preview");
        m_preview->Camera().position = Float3{180.0f, 140.0f, 180.0f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});
        BuildPreviewScene();

        m_fields = MakeRef<ui::FlexLayout>(Allocator());
        m_fields->Direction = ui::Orientation::Vertical;
        m_fields->Spacing = 4.0f;
        m_fields->Padding = ui::Thickness{8, 6};

        auto scroll = MakeRef<ui::ScrollView>(Allocator());
        scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
        scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            scroll->AddView(m_fields.Get(), lp);
        }

        auto split = MakeRef<ui::toolkit::SplitView>(Allocator());
        split->SetSplitRatio(0.62f);
        split->SetPanes(m_preview->View(), scroll.Get());
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *split);

        m_undoBaseline = Snapshot();
        BindTerrain(); // binds the cooked product (if cooked) + builds the fields/stats
        RebuildFields();
    }

    void TerrainEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }
        m_entity = scenePtr->CreateEntity(u8"PreviewTerrain");
        if (auto* terrains = scenePtr->GetSystem<engine::terrain::TerrainComponentManager>())
        {
            terrains->Add(m_entity); // terrain bound in BindTerrain once the product resolves
        }
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.5f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.9f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.type = engine::render::LightType::Directional;
            light.castsShadows = true; // show the terrain's self-shadowing in the preview
        }
    }

    void TerrainEditorPage::BindTerrain()
    {
        if (m_context->Resources() != nullptr)
        {
            m_terrainProxy = m_context->Resources()->Bind<terrain::TerrainResource>(InstanceId());
        }
        terrain::TerrainResource* product = m_terrainProxy ? m_terrainProxy.Get() : nullptr;
        PointComponentAtTerrain(product);
        m_lastProduct = product;
        FramePreview(product);
    }

    void TerrainEditorPage::PointComponentAtTerrain(terrain::TerrainResource* product)
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        auto* terrains =
            scenePtr ? scenePtr->GetSystem<engine::terrain::TerrainComponentManager>() : nullptr;
        engine::terrain::TerrainComponent* tc =
            (terrains != nullptr) ? terrains->Get(m_entity) : nullptr;
        if (tc == nullptr)
        {
            return;
        }
        tc->terrain.SetId(Guid{});
        if (product != nullptr)
        {
            tc->terrain = product; // direct override to the cooked product
        }
        else
        {
            tc->terrain.SetDirect(RefPtr<terrain::TerrainResource>{});
        }
    }

    void TerrainEditorPage::FramePreview(terrain::TerrainResource* product)
    {
        if (!m_preview)
        {
            return;
        }
        f32 worldSize = 256.0f;
        f32 midY = 0.0f;
        if (product != nullptr)
        {
            if (foundation::heightfield::Heightfield* hf = product->heightfield.Get())
            {
                const Float2 ws = hf->WorldSize();
                worldSize = Max(ws.x, ws.y);
                midY = 0.5f * (hf->MinY() + hf->MaxY());
            }
        }
        const f32 dist = worldSize * 0.85f;
        m_preview->Camera().position = Float3{dist * 0.7f, dist * 0.6f + midY, dist * 0.7f};
        m_preview->Camera().LookAt(Float3{0.0f, midY, 0.0f});
    }

    // ---- fields ---------------------------------------------------------------------------------

    void TerrainEditorPage::RebuildFieldsDeferred()
    {
        ui::UIContext* ctx = m_content.Get() != nullptr ? m_content->Context : nullptr;
        if (ctx == nullptr)
        {
            RebuildFields();
            return;
        }
        TerrainEditorPage* self = this;
        ctx->MutationQueueRef().QueueAction(core::Function<void()>{[self]() { self->RebuildFields(); }});
    }

    void TerrainEditorPage::RebuildFields()
    {
        if (m_fields.Get() == nullptr || m_asset.Get() == nullptr)
        {
            return;
        }
        while (m_fields->ChildCount() > 0)
        {
            m_fields->RemoveView(m_fields->GetChildAt(0), true);
        }
        m_referenceRows.Clear();
        TerrainEditorPage* self = this;

        const auto addLabel = [&](StringView text, f32 fontSize)
        {
            auto lbl = MakeRef<ui::Label>(Allocator(), text);
            lbl->FontSize.SetValue(Optional<f32>{fontSize});
            m_fields->AddView(lbl.Get());
        };
        const auto addButton = [&](StringView text, core::Function<void()> onClick)
        {
            auto btn = MakeRef<ui::Button>(Allocator(), text);
            btn->FontSize.SetValue(Optional<f32>{12.0f});
            btn->OnClick.Add([cb = Move(onClick)](ui::ButtonBase*) { cb(); });
            m_fields->AddView(btn.Get());
        };

        // References: each a labelled asset slot (pick, drop and clear are one assignment).
        addLabel(u8"References", 13.0f);
        AddReference(u8"Heightfield", u8"HeightfieldAsset", u8"heightfield",
                     [self]() { return self->m_asset->heightfieldId; },
                     [self](const Guid& g) { self->m_asset->heightfieldId = g; });
        AddReference(u8"Weights", u8"SplatmapAsset", u8"weights",
                     [self]() { return self->m_asset->weightsId; },
                     [self](const Guid& g) { self->m_asset->weightsId = g; });
        // No weights yet: offer to author them (the Splat Paint tool needs an existing raster).
        // Resolution is an authoring choice independent of the heightfield (Fable Q3) - presets.
        if (m_asset->weightsId.IsNil())
        {
            addLabel(u8"Create weights:", 11.0f);
            addButton(u8"512", [self]() { self->CreateSplatmap(512); });
            addButton(u8"1024", [self]() { self->CreateSplatmap(1024); });
            addButton(u8"2048", [self]() { self->CreateSplatmap(2048); });
        }

        // BASE layer: what shows wherever paint doesn't cover; never painted (top-K model).
        addLabel(u8"Base layer", 13.0f);
        AddReference(u8"Base albedo", u8"TextureAsset", u8"baseAlbedo",
                     [self]() { return self->m_asset->baseAlbedoId; },
                     [self](const Guid& g) { self->m_asset->baseAlbedoId = g; });
        AddReference(u8"Base normal", u8"TextureAsset", u8"baseNormal",
                     [self]() { return self->m_asset->baseNormalId; },
                     [self](const Guid& g) { self->m_asset->baseNormalId = g; });
        AddReference(u8"Base ORM", u8"TextureAsset", u8"baseOrm",
                     [self]() { return self->m_asset->baseOrmId; },
                     [self](const Guid& g) { self->m_asset->baseOrmId = g; });
        AddReference(u8"Base height", u8"TextureAsset", u8"baseHeight",
                     [self]() { return self->m_asset->baseHeightId; },
                     [self](const Guid& g) { self->m_asset->baseHeightId = g; });

        // The paint layers are a section list: the header's add icon, and each layer a section
        // inside Paint layers with its maps, its tiling and its remove icon (removal remaps the weight raster). A
        // layer's index is its weight channel, so the order is not the user's to change.
        auto layers = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        {
            auto layerList = MakeRef<app::ContainerListEditor>(
                Allocator(), StringView(u8"Paint layers"), StringView(u8"Paint layers"));
            layerList->ElementsAsSections = true;
            for (const Guid& albedo : m_asset->paletteAlbedoIds)
            {
                layerList->slotNames.PushBack(String(m_context->AssetNameFor(albedo)));
            }
            layerList->OnAdd = [self]() { self->AddLayer(); };
            layers->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(layerList.Get()));
        }
        const u32 layerCount = static_cast<u32>(m_asset->paletteAlbedoIds.Size());
        for (u32 i = 0; i < layerCount; ++i)
        {
            const u32 idx = i;
            const String section = LayerSection(i);
            const StringView cat = section.AsView();
            layers->SetCategoryParent(cat, u8"Paint layers");
            layers->SetCategoryHeaderActions(
                cat, app::ContainerListEditor::ElementActions(
                         Allocator(), i, layerCount, core::Function<void(usize, bool)>{},
                         [self](usize index) { self->RemoveLayer(static_cast<u32>(index)); }));
            layers->AddProperty(MakeReference(
                u8"Albedo", u8"TextureAsset", u8"palette", cat,
                [self, idx]()
                {
                    return idx < self->m_asset->paletteAlbedoIds.Size()
                               ? self->m_asset->paletteAlbedoIds[idx]
                               : Guid{};
                },
                [self, idx](const Guid& g)
                {
                    if (idx < self->m_asset->paletteAlbedoIds.Size())
                    {
                        self->m_asset->paletteAlbedoIds[idx] = g;
                    }
                }));
            layers->AddProperty(
                MakeMapReference(idx, u8"Normal", PaletteMap::Normal, u8"paletteNormal", cat));
            layers->AddProperty(MakeMapReference(idx, u8"ORM", PaletteMap::Orm, u8"paletteOrm", cat));
            layers->AddProperty(
                MakeMapReference(idx, u8"Height", PaletteMap::Height, u8"paletteHeight", cat));
            layers->AddProperty(
                MakeMapReference(idx, u8"Mask", PaletteMap::Mask, u8"paletteMask", cat));
            if (i < m_asset->paletteTileScales.Size())
            {
                const String mergeKey = Format(u8"tile{}", i);
                layers->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::FloatEditor>(
                        Allocator(), StringView(u8"Tile"),
                        static_cast<f64>(m_asset->paletteTileScales[i]), 0.1, 8192.0, 1.0, 2,
                        core::Function<void(f64)>{
                            [self, idx, mergeKey](f64 v)
                            {
                                if (idx < self->m_asset->paletteTileScales.Size())
                                {
                                    self->m_asset->paletteTileScales[idx] = static_cast<f32>(v);
                                    self->CommitEdit(mergeKey.AsView());
                                }
                            }},
                        cat)
                        .Get()));
            }
        }
        {
            ui::LayoutStyle layersStyle;
            layersStyle.Width = ui::SizeSpec::Match();
            m_fields->AddView(layers.Get(), layersStyle);
        }

        // Scalars (cast shadows + per-layer tile scale) in a property grid.
        auto grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        {
            auto cs = MakeRef<ui::toolkit::BoolEditor>(
                Allocator(), StringView(u8"Cast Shadows"), m_asset->castShadows,
                core::Function<void(bool)>{[self](bool v)
                                           {
                                               self->m_asset->castShadows = v;
                                               self->CommitEdit(u8"castShadows");
                                           }},
                StringView(u8"Terrain"));
            grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(cs.Get()));
        }
        {
            // Height-blend soft-skirt width: only bites when a layer has a
            // height map; smaller = crisper interlocked seams, larger = a wider skirt.
            auto hb = MakeRef<ui::toolkit::FloatEditor>(
                Allocator(), StringView(u8"Height blend"),
                static_cast<f64>(m_asset->heightBlendContrast), 0.0, 1.0, 0.05, 2,
                core::Function<void(f64)>{[self](f64 v)
                                          {
                                              self->m_asset->heightBlendContrast =
                                                  static_cast<f32>(v);
                                              self->CommitEdit(u8"heightBlend");
                                          }},
                StringView(u8"Terrain"));
            grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(hb.Get()));
        }
        {
            auto fe = MakeRef<ui::toolkit::FloatEditor>(
                Allocator(), StringView(u8"Base tile"),
                static_cast<f64>(m_asset->baseTileScale), 0.1, 8192.0, 1.0, 2,
                core::Function<void(f64)>{[self](f64 v)
                                          {
                                              self->m_asset->baseTileScale = static_cast<f32>(v);
                                              self->CommitEdit(u8"baseTile");
                                          }},
                StringView(u8"Base"));
            grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(fe.Get()));
        }
        m_fields->AddView(grid.Get());

        // Stats (from the cooked product, if resolved)
        addLabel(u8"Stats", 13.0f);
        terrain::TerrainResource* product = m_terrainProxy ? m_terrainProxy.Get() : nullptr;
        if (product != nullptr)
        {
            for (const String& line : TerrainStatLines(*product))
            {
                addLabel(line.AsView(), 12.0f);
            }
        }
        else
        {
            addLabel(u8"(not cooked yet - Save to preview)", 12.0f);
        }
    }

    RefPtr<ui::toolkit::PropertyEditor> TerrainEditorPage::MakeReference(
        StringView label, StringView assetTypeName, StringView mergeKey, StringView category,
        core::Function<Guid()> current, core::Function<void(const Guid&)> apply)
    {
        TerrainEditorPage* self = this;
        const StringView types[] = {assetTypeName};
        auto row = MakeRef<app::ResourceRefEditor>(Allocator(), label, StringView(u8"(none)"),
                                                   category, Span<const StringView>{types, 1});
        // The write rebuilds this pane, deferred; the row outlives its own assignment either way.
        row->BindAsset(*m_context, Move(current),
                       [self, applyFn = Move(apply), key = String(mergeKey)](const Guid& picked)
                       {
                           applyFn(picked);
                           self->CommitEdit(key.AsView());
                           self->PointComponentAtTerrain(
                               self->m_terrainProxy ? self->m_terrainProxy.Get() : nullptr);
                           self->RebuildFieldsDeferred();
                       });
        return RefPtr<ui::toolkit::PropertyEditor>(row.Get());
    }

    void TerrainEditorPage::AddReference(StringView label, StringView assetTypeName,
                                         StringView mergeKey, core::Function<Guid()> current,
                                         core::Function<void(const Guid&)> apply)
    {
        RefPtr<ui::toolkit::PropertyEditor> row =
            MakeReference(label, assetTypeName, mergeKey, StringView{}, Move(current), Move(apply));
        auto line = MakeRef<ui::FlexLayout>(Allocator());
        line->Direction = ui::Orientation::Horizontal;
        line->Spacing = 6.0f;
        auto text = MakeRef<ui::Label>(Allocator(), label);
        text->FontSize.SetValue(Optional<f32>{12.0f});
        ui::LayoutStyle fixedWidth;
        fixedWidth.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(110.0f));
        fixedWidth.AlignSelf = ui::Align::Center;
        line->AddView(text.Get(), fixedWidth);
        ui::LayoutStyle grow;
        grow.FlexGrow = 1.0f;
        line->AddView(row->EditorView(), grow);
        ui::LayoutStyle style;
        style.Width = ui::SizeSpec::Match();
        m_fields->AddView(line.Get(), style);
        m_referenceRows.PushBack(Move(row));
    }

    RefPtr<ui::toolkit::PropertyEditor> TerrainEditorPage::MakeMapReference(
        u32 index, StringView mapLabel, PaletteMap map, StringView mergeKey, StringView category)
    {
        TerrainEditorPage* self = this;
        return MakeReference(mapLabel, u8"TextureAsset", mergeKey, category,
                             [self, index, map]() { return self->PaletteMapId(map, index); },
                             [self, index, map](const Guid& g) { self->SetPaletteMap(map, index, g); });
    }

    String TerrainEditorPage::LayerSection(u32 index)
    {
        return Format(u8"Layer {}", index + 1);
    }

    void TerrainEditorPage::AddLayer()
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        m_asset->paletteAlbedoIds.PushBack(Guid{});
        m_asset->paletteTileScales.PushBack(32.0f);
        // Keep the optional map arrays parallel with the albedo list so rows line up.
        m_asset->paletteNormalIds.PushBack(Guid{});
        m_asset->paletteOrmIds.PushBack(Guid{});
        m_asset->paletteHeightIds.PushBack(Guid{});
        m_asset->paletteMaskIds.PushBack(Guid{});
        CommitEdit(u8"addLayer");
        RebuildFieldsDeferred();
    }

    // Set a per-layer normal / ORM / height / mask map id, growing the (optional) target array to match
    // the albedo list first so an older terrain with no maps still edits cleanly.
    Array<Guid>& TerrainEditorPage::PaletteMapIds(PaletteMap map) const
    {
        return (map == PaletteMap::Normal)   ? m_asset->paletteNormalIds
               : (map == PaletteMap::Orm)    ? m_asset->paletteOrmIds
               : (map == PaletteMap::Height) ? m_asset->paletteHeightIds
                                             : m_asset->paletteMaskIds;
    }

    Guid TerrainEditorPage::PaletteMapId(PaletteMap map, u32 index) const
    {
        if (m_asset.Get() == nullptr)
        {
            return Guid{};
        }
        const Array<Guid>& ids = PaletteMapIds(map);
        return index < ids.Size() ? ids[index] : Guid{};
    }

    void TerrainEditorPage::SetPaletteMap(PaletteMap map, u32 index, const Guid& g)
    {
        if (m_asset.Get() == nullptr || index >= m_asset->paletteAlbedoIds.Size())
        {
            return;
        }
        Array<Guid>& ids = PaletteMapIds(map);
        while (ids.Size() < m_asset->paletteAlbedoIds.Size())
        {
            ids.PushBack(Guid{});
        }
        ids[index] = g;
    }

    void TerrainEditorPage::RemoveLayer(u32 index)
    {
        if (m_asset.Get() == nullptr || index >= m_asset->paletteAlbedoIds.Size())
        {
            return;
        }
        m_asset->paletteAlbedoIds.RemoveAt(index);
        if (index < m_asset->paletteTileScales.Size())
        {
            m_asset->paletteTileScales.RemoveAt(index);
        }
        if (index < m_asset->paletteNormalIds.Size())
        {
            m_asset->paletteNormalIds.RemoveAt(index);
        }
        if (index < m_asset->paletteOrmIds.Size())
        {
            m_asset->paletteOrmIds.RemoveAt(index);
        }
        if (index < m_asset->paletteHeightIds.Size())
        {
            m_asset->paletteHeightIds.RemoveAt(index);
        }
        if (index < m_asset->paletteMaskIds.Size())
        {
            m_asset->paletteMaskIds.RemoveAt(index);
        }
        // Remap the weight raster (ruling R6): slots referencing the removed layer are freed
        // (their weight falls to base); indices above it decrement. Applied to the LIVE runtime
        // raster + persisted to the source sidecars; a full-raster snapshot rides the SAME undo
        // entry as the asset edit (rare op - no region-delta machinery).
        RemapWeightsOnRemove(index);
        CommitEdit(u8"removeLayer");
        m_context->RequestCook(false);
        RebuildFieldsDeferred();
    }

    void TerrainEditorPage::RemapWeightsOnRemove(u32 removedIndex)
    {
        terrain::TerrainResource* product = m_terrainProxy ? m_terrainProxy.Get() : nullptr;
        terrain::SplatWeights* sw = product != nullptr ? product->weights.Get() : nullptr;
        if (sw == nullptr || sw->IsEmpty())
        {
            return; // no live raster (not cooked yet): nothing references the palette indices
        }
        if (!terrain::RemapOnPaletteRemove(*sw, removedIndex))
        {
            return;
        }
        // Persist both sidecars through the standard asset-edit drain (Save flow).
        if (m_context != nullptr && !product->weights.id.IsNil())
        {
            RefPtr<terrain::SplatWeights> weights(sw);
            const Guid id = product->weights.id;
            m_context->RegisterAssetEdit(
                id,
                [weights, id](foundation::content::ContentDatabase& db) -> Status
                {
                    foundation::content::Instance* inst = db.GetInstance(id);
                    if (inst == nullptr || weights.Get() == nullptr)
                    {
                        return Status{ErrorCode::NotFound};
                    }
                    RefPtr<ISerializable> object = inst->ReadObject();
                    auto* asset = Cast<pipeline::SplatmapAsset>(object.Get());
                    if (asset == nullptr)
                    {
                        return Status{ErrorCode::InvalidArgument};
                    }
                    asset->fileName = {};
                    asset->width = weights->Width();
                    asset->height = weights->Height();
                    const Status wrote = inst->WriteObject(*asset);
                    if (!wrote.IsOk())
                    {
                        return wrote;
                    }
                    const Status wroteWeights =
                        inst->WriteData(terrain::kSplatStream,
                                        terrain::SplatWeightsSource::WeightBlob(*weights));
                    if (!wroteWeights.IsOk())
                    {
                        return wroteWeights;
                    }
                    return inst->WriteData(terrain::kSplatIndexStream,
                                           terrain::SplatWeightsSource::IndexBlob(*weights));
                });
        }
    }

    void TerrainEditorPage::CreateSplatmap(i32 size)
    {
        // Composition, not painting: the PAGE authors the splatmap asset (a new source instance,
        // seeded to the base layer) and assigns it; the Splat Paint tool only edits an existing one
        // (Fable ruling Q3 - no stroke ever implies asset creation, so undo stays clean).
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        foundation::content::Group* root = m_context->Project()->SourceDb().RootGroup();
        if (root == nullptr)
        {
            return;
        }
        const i32 dim = size > 0 ? size : 1024;
        const String terrainName = AssetName(m_context, InstanceId());
        const String name = Format(u8"{}_splat", terrainName);
        foundation::content::Instance* inst =
            root->CreateInstance(name.AsView(), pipeline::SplatmapAsset::StaticType());
        if (inst == nullptr)
        {
            m_context->Notify(editor::NoticeKind::Error, u8"Splatmap create FAILED (name in use?).");
            return;
        }
        pipeline::SplatmapAsset sa;
        sa.width = dim;
        sa.height = dim;
        (void)inst->WriteObject(sa);
        // No seeding: an all-zero top-K raster is a valid "pure base" surface by construction
        // (the builder cooks empty sidecars to exactly that).

        m_asset->weightsId = inst->Id();
        CommitEdit(u8"createSplatmap");
        m_context->RequestCook(false); // cook the new SplatmapAsset -> Splatmap product
        PointComponentAtTerrain(m_terrainProxy ? m_terrainProxy.Get() : nullptr);
        RebuildFieldsDeferred();
        m_context->Notify(editor::NoticeKind::Success, u8"Created splatmap.");
    }

    // ---- undo / save ----------------------------------------------------------------------------

    Array<byte> TerrainEditorPage::Snapshot() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, m_asset->GetType() != nullptr ? *m_asset->GetType()
                                                                : pipeline::TerrainAsset::StaticType());
        m_asset->Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void TerrainEditorPage::ApplyBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr || blob.IsEmpty())
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, m_asset->GetType() != nullptr ? *m_asset->GetType()
                                                                : pipeline::TerrainAsset::StaticType());
        m_asset->Serialize(ar);
        m_undoBaseline = blob;
        PointComponentAtTerrain(m_terrainProxy ? m_terrainProxy.Get() : nullptr);
        RebuildFields(); // undo path (outside a live UI event) - rebuild directly
        MarkDirty();
    }

    void TerrainEditorPage::CommitEdit(StringView mergeKey)
    {
        Array<byte> after = Snapshot();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            Allocator().New<EditCommand>(*this, mergeKey, m_undoBaseline, after),
            Allocator()));
        m_undoBaseline = Move(after);
        MarkDirty();
    }

    Status TerrainEditorPage::Save()
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
            m_context->RequestCook(false); // hot-swap the bound TerrainResource
            LOG_INFO(u8"Editor", u8"saved terrain '{}'", m_title);
        }
        return saved;
    }

    // ---- lifecycle ------------------------------------------------------------------------------

    void TerrainEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // sync the page's actions to it each frame
        }
        if (m_preview)
        {
            m_preview->Update(dt);
        }
        // Product resolve / hot-reload watchdog: rebind + reframe + refresh stats on (re)cook.
        terrain::TerrainResource* product = m_terrainProxy ? m_terrainProxy.Get() : nullptr;
        if (product != m_lastProduct)
        {
            PointComponentAtTerrain(product);
            m_lastProduct = product;
            FramePreview(product);
            RebuildFields();
        }
    }

    void TerrainEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                           foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    void TerrainEditorPage::OnClose()
    {
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    UniquePtr<EditorPage>
    TerrainEditorPageFactory::CreatePage(EditorContext& context,
                                         foundation::content::Instance& instance)
    {
        auto* page =
            editor::EditorRootAllocator().New<TerrainEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterTerrainEditor(EditorContext& context, runtime::IApplicationHost& host,
                               ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<TerrainEditorPageFactory>(host, uiHost), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
        {
            RegisterSplatmapThumbnailGenerator(*context.Thumbnails());
        }
    }
}

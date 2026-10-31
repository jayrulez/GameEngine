// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `:page` partition (implementation).
//
// The heavy engine imports (the terrain component, the lights, the scene) live here, out of the
// interface, for GCC module hygiene.

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
import engine.terrain;              // TerrainComponentManager
import foundation.terrain;          // ChunksPerSide, SplatWeights, RemapOnPaletteRemove
import foundation.terrain.resource; // TerrainResource
import foundation.heightfield;      // Heightfield (the facts, the framing)
import foundation.texture.resource; // Texture (the maps' references)
import foundation.resource;
import foundation.gui;
import foundation.gui.toolkit;
import pipeline.core;
import terrain.pipeline;
import heightfield.pipeline; // HeightfieldAsset (the heightfield's reference)
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;

    namespace
    {
        [[nodiscard]] gui::Variant GuidText(const Guid& id)
        {
            if (id.IsNil())
                return gui::Variant(StringView{});
            utf8char text[37];
            id.ToChars(text);
            return gui::Variant(String(StringView(text, 36)));
        }

        [[nodiscard]] Guid GuidOf(const gui::Variant& value)
        {
            Guid id;
            const StringView text = Trim(value.AsString());
            if (!text.IsEmpty())
                (void)Guid::TryParse(text, id);
            return id;
        }

        // A layer map: the row's name and description, and the array that holds it. Each array
        // is parallel to the albedos; an older terrain's may be shorter, with nil past its end.
        struct LayerMap
        {
            StringView path;
            StringView shown;
            StringView about;
            Array<Guid> pipeline::TerrainAsset::*ids;
        };
        constexpr LayerMap kLayerMaps[] = {
            {u8"albedo", u8"Albedo", u8"Its colour", &pipeline::TerrainAsset::paletteAlbedoIds},
            {u8"normal", u8"Normal", u8"Its normal map (none: flat)", &pipeline::TerrainAsset::paletteNormalIds},
            {u8"orm", u8"ORM", u8"Its occlusion, roughness and metalness (none: 1, 1, 0)", &pipeline::TerrainAsset::paletteOrmIds},
            {u8"height", u8"Height", u8"Its height map, which the height blend reads where layers meet (none: flat)", &pipeline::TerrainAsset::paletteHeightIds},
            {u8"mask", u8"Coverage", u8"A mask of where it may show over what it is painted on (none: everywhere)", &pipeline::TerrainAsset::paletteMaskIds},
        };

        // The base layer's maps, and the terrain's own references.
        struct BaseMap
        {
            StringView path;
            StringView shown;
            StringView about;
            Guid pipeline::TerrainAsset::*id;
        };
        constexpr BaseMap kBaseMaps[] = {
            {u8"albedo", u8"Albedo", u8"Its colour (none: white)", &pipeline::TerrainAsset::baseAlbedoId},
            {u8"normal", u8"Normal", u8"Its normal map (none: flat)", &pipeline::TerrainAsset::baseNormalId},
            {u8"orm", u8"ORM", u8"Its occlusion, roughness and metalness (none: 1, 1, 0)", &pipeline::TerrainAsset::baseOrmId},
            {u8"height", u8"Height", u8"Its height map, which the height blend reads (none: flat)", &pipeline::TerrainAsset::baseHeightId},
        };

        constexpr f32 kNewLayerTile = 32.0f;

        // Pads every layer array to the length of the albedos, so a layer's maps line up whatever
        // the asset stored (an older terrain has fewer maps).
        void Square(pipeline::TerrainAsset& asset)
        {
            const usize layers = asset.paletteAlbedoIds.Size();
            for (const LayerMap& map : kLayerMaps)
            {
                Array<Guid>& ids = asset.*map.ids;
                while (ids.Size() < layers)
                    ids.PushBack(Guid{});
            }
            while (asset.paletteTileScales.Size() < layers)
                asset.paletteTileScales.PushBack(kNewLayerTile);
        }

        // The layer a path names ("layers.2.normal": 2), or -1.
        [[nodiscard]] i32 LayerOf(StringView path)
        {
            const StringView element = gui::ParentPropertyPath(path);
            if (gui::ParentPropertyPath(element) != StringView(u8"layers"))
                return -1;
            const Optional<i64> index = ParseInt(element.SubStr(7, element.Size() - 7));
            return index.HasValue() && index.Value() >= 0 ? static_cast<i32>(index.Value()) : -1;
        }

        [[nodiscard]] StringView LastSegment(StringView path)
        {
            for (usize i = path.Size(); i-- > 0;)
                if (path[i] == u8'.')
                    return path.SubStr(i + 1, path.Size() - i - 1);
            return path;
        }

        gui::PropertyInfo& AddReference(gui::PropertyBag& rows, StringView path, StringView shown, StringView about, const TypeInfo& type)
        {
            gui::PropertyInfo& info = rows.Add(path, gui::PropertyKind::Reference, gui::Variant(StringView{}));
            info.DisplayName = String(shown);
            info.Description = String(about);
            info.ReferenceType = &type;
            return info;
        }

        gui::PropertyInfo& AddTile(gui::PropertyInfo& info, StringView about)
        {
            info.DisplayName = String(u8"Tile");
            info.Description = String(about);
            info.HasRange = true;
            info.Min = 0.1;
            info.Max = 8192.0;
            info.Step = 1.0;
            return info;
        }
    }

    Array<TerrainFact> TerrainFacts(const terrain::TerrainResource& product)
    {
        Array<TerrainFact> facts;
        if (foundation::heightfield::Heightfield* hf = product.heightfield.Get())
        {
            const i32 size = hf->Size();
            const i32 chunks = terrain::ChunksPerSide(size);
            const Float2 world = hf->WorldSize();
            facts.PushBack(TerrainFact{String(u8"Grid"), Format(u8"{} x {}", size, size)});
            facts.PushBack(TerrainFact{String(u8"World"), Format(u8"{} x {} m", static_cast<i64>(world.x), static_cast<i64>(world.y))});
            facts.PushBack(TerrainFact{String(u8"Chunks"), Format(u8"{} x {} ({})", chunks, chunks, chunks * chunks)});
        }
        else
            facts.PushBack(TerrainFact{String(u8"Grid"), String(u8"No heightfield")});
        facts.PushBack(TerrainFact{String(u8"Paint layers"), Format(u8"{}", product.PaletteCount())});
        facts.PushBack(TerrainFact{String(u8"Casts shadows"), String(product.castShadows ? StringView(u8"Yes") : StringView(u8"No"))});
        return facts;
    }

    TerrainEditorPage::TerrainEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::TerrainAsset::StaticType(), kPath)
    {
        // The layers live in parallel arrays that no one edits by hand, so the page builds its own
        // rows.
        HideAssetRows(); // an older terrain's shorter arrays are padded by its first edit, not by opening it
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"terrain.preview");
            TerrainEditorPage* self = this;
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            BuildPreviewScene();
            AddSplatButton();
        }
        BuildRows();
        if (Context().Resources() != nullptr)
            m_product = Context().Resources()->Bind<terrain::TerrainResource>(InstanceId());
        ShowRows();
        ShowFacts();
    }

    TerrainEditorPage::~TerrainEditorPage() = default;

    // === The rows ===

    void TerrainEditorPage::BuildRows()
    {
        m_terrainRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_baseRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_layerRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());

        AddReference(*m_terrainRows, u8"heightfield", u8"Heightfield", u8"The heights it is drawn from (shared with its collision and navigation)",
                     pipeline::HeightfieldAsset::StaticType());
        AddReference(*m_terrainRows, u8"weights", u8"Splat weights",
                     u8"Where each paint layer shows, as the Paint Layers tool paints it (none: the base layer everywhere)",
                     pipeline::SplatmapAsset::StaticType());
        gui::PropertyInfo& shadows = m_terrainRows->Add(u8"castShadows", gui::PropertyKind::Bool, gui::Variant(true));
        shadows.DisplayName = String(u8"Casts shadows");
        gui::PropertyInfo& blend = m_terrainRows->Add(u8"heightBlend", gui::PropertyKind::Float, gui::Variant(0.25));
        blend.DisplayName = String(u8"Height blend");
        blend.Description = String(u8"How wide the seam is where layers with height maps meet: smaller is crisper");
        blend.HasRange = true;
        blend.Min = 0.0;
        blend.Max = 1.0;
        blend.Step = 0.01;
        blend.Hint = String(u8"slider");
        gui::PropertyInfo& packed = m_terrainRows->Add(u8"textureSize", gui::PropertyKind::Integer, gui::Variant(static_cast<i64>(1024)));
        packed.DisplayName = String(u8"Layer texture size");
        packed.Description = String(u8"The size every paint layer's maps are packed at, in texels a side");
        packed.HasRange = true;
        packed.Min = 64.0;
        packed.Max = 4096.0;
        packed.Step = 64.0;

        for (const BaseMap& map : kBaseMaps)
            AddReference(*m_baseRows, map.path, map.shown, map.about, foundation::texture::Texture::StaticType());
        AddTile(m_baseRows->Add(u8"tile", gui::PropertyKind::Float, gui::Variant(1.0)), u8"How many metres its textures repeat over");

        // The layers, shown like every page's lists: the count with Add, then each layer as a group
        // of its maps and its tiling. A layer's position is its channel in the splat weights, so
        // layers are added and removed but never moved.
        gui::PropertyInfo& list = m_layerRows->AddList(u8"layers", gui::PropertyKind::Group, gui::Variant{});
        list.DisplayName = String(u8"Paint layers");
        list.Description = String(u8"What the splat weights paint over the base layer, each in its own channel");
        list.ListOps = gui::PropertyListAdd | gui::PropertyListRemove;
        for (const LayerMap& map : kLayerMaps)
            if (gui::PropertyInfo* field = m_layerRows->AddElementField(u8"layers", map.path, gui::PropertyKind::Reference, gui::Variant(StringView{})))
            {
                field->DisplayName = String(map.shown);
                field->Description = String(map.about);
                field->ReferenceType = &foundation::texture::Texture::StaticType();
            }
        if (gui::PropertyInfo* tile = m_layerRows->AddElementField(u8"layers", u8"tile", gui::PropertyKind::Float, gui::Variant(static_cast<f64>(kNewLayerTile))))
            AddTile(*tile, u8"How many metres its textures repeat over");

        TerrainEditorPage* self = this;
        gui::PropertyBag* const bags[] = {m_terrainRows.Get(), m_baseRows.Get(), m_layerRows.Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
        }
        m_layerRows->SetOnListEdited([self](StringView, const gui::ListEdit& edit) { self->EditLayers(edit); });
        // Names each layer by its channel, numbered from 1 like the paint tool's keys, and when
        // collapsed by the albedo it shows.
        m_layerRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                   {
                                       if (gui::ParentPropertyPath(path) != StringView(u8"layers"))
                                           return;
                                       const Optional<i64> index = ParseInt(LastSegment(path));
                                       if (!index.HasValue())
                                           return;
                                       info.DisplayName = Format(u8"Layer {}", index.Value() + 1);
                                       const Guid albedo = GuidOf(self->m_layerRows->GetValue(gui::JoinPropertyPath(path, u8"albedo").AsView(), 0));
                                       foundation::content::Instance* named =
                                           !albedo.IsNil() && self->Context().Project() != nullptr ? self->Context().Project()->SourceDb().GetInstance(albedo) : nullptr;
                                       info.Summary = named != nullptr ? String(named->Name()) : String(u8"No albedo");
                                   });
        AddSection(u8"terrain", u8"Terrain", *m_terrainRows);
        AddSection(u8"base", u8"Base layer", *m_baseRows);
        AddSection(u8"layers", u8"Paint layers", *m_layerRows);
    }

    void TerrainEditorPage::ShowRows()
    {
        const pipeline::TerrainAsset* asset = Asset();
        if (asset == nullptr)
            return;
        m_terrainRows->SetValue(u8"heightfield", 0, GuidText(asset->heightfieldId));
        m_terrainRows->SetValue(u8"weights", 0, GuidText(asset->weightsId));
        m_terrainRows->SetValue(u8"castShadows", 0, gui::Variant(asset->castShadows));
        m_terrainRows->SetValue(u8"heightBlend", 0, gui::Variant(static_cast<f64>(asset->heightBlendContrast)));
        m_terrainRows->SetValue(u8"textureSize", 0, gui::Variant(static_cast<i64>(asset->paletteTextureSize)));
        for (const BaseMap& map : kBaseMaps)
            m_baseRows->SetValue(map.path, 0, GuidText(asset->*map.id));
        m_baseRows->SetValue(u8"tile", 0, gui::Variant(static_cast<f64>(asset->baseTileScale)));

        const usize layers = asset->paletteAlbedoIds.Size();
        m_layerRows->ResizeList(u8"layers", 0, layers);
        for (usize i = 0; i < layers; ++i)
        {
            const String element = Format(u8"layers.{}", i);
            for (const LayerMap& map : kLayerMaps)
            {
                const Array<Guid>& ids = asset->*map.ids;
                m_layerRows->SetValue(gui::JoinPropertyPath(element.AsView(), map.path).AsView(), 0, GuidText(i < ids.Size() ? ids[i] : Guid{}));
            }
            const f32 tile = i < asset->paletteTileScales.Size() ? asset->paletteTileScales[i] : kNewLayerTile;
            m_layerRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"tile").AsView(), 0, gui::Variant(static_cast<f64>(tile)));
        }
        SetNotice(u8"heightfield", asset->heightfieldId.IsNil() ? StringView(u8"This terrain has no heightfield: pick one, or there is nothing to draw.")
                                                                 : StringView{});
        if (m_splat)
            m_splat->SetEnabled(asset->weightsId.IsNil());
    }

    void TerrainEditorPage::ShowFacts()
    {
        terrain::TerrainResource* product = m_product ? m_product.Get() : nullptr;
        if (product == nullptr)
        {
            SetFact(u8"Grid", u8"Not cooked yet: Save to cook it");
            return;
        }
        for (const TerrainFact& fact : TerrainFacts(*product))
            SetFact(fact.name.AsView(), fact.value.AsView());
    }

    void TerrainEditorPage::AddSplatButton()
    {
        // After the bar's separator, like every page's own controls.
        gui::Node* own = Find<gui::Node>(u8"page-own");
        gui::Node* bar = own != nullptr ? own->GetParent() : nullptr;
        if (bar == nullptr || LoadFailed())
            return;
        m_splat = MakeRef<gui::ToolMenuButton>(Allocator());
        m_splat->SetId(u8"terrain-new-weights");
        m_splat->SetText(u8"New Splat Weights");
        m_splat->SetTooltip(u8"Make splat weights for this terrain to paint its layers on, at a size (texels a side)");
        TerrainEditorPage* self = this;
        for (const i32 size : kSplatSizes)
            (void)m_splat->GetMenu().AddItem(Format(u8"{} x {}", size, size).AsView(), [self, size]() { (void)self->CreateSplatWeights(size); });
        bar->AddChildAt(m_splat.Get(), bar->GetChildIndex(own) + 1);
    }

    void TerrainEditorPage::OnObjectChanged()
    {
        ShowRows();
        PointPreviewAt(m_product ? m_product.Get() : nullptr);
    }

    void TerrainEditorPage::EditRow(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        rows.SetValue(path, 0, value); // as the grid's commit would leave it
        Committed(rows, path, value);
    }

    void TerrainEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        if (Asset() == nullptr)
            return;
        if (&rows == m_terrainRows.Get())
        {
            if (path == u8"heightfield" || path == u8"weights")
            {
                const Guid id = GuidOf(value);
                Guid pipeline::TerrainAsset::*field = path == u8"heightfield" ? &pipeline::TerrainAsset::heightfieldId : &pipeline::TerrainAsset::weightsId;
                EditWhole(u8"edit_terrain", [field, id](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).*field = id; });
            }
            else if (path == u8"castShadows")
            {
                const bool casts = value.AsBool();
                EditWhole(u8"edit_terrain", [casts](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).castShadows = casts; });
            }
            else if (path == u8"heightBlend")
            {
                const f32 blend = static_cast<f32>(Clamp(value.AsFloat(), 0.0, 1.0));
                EditWhole(u8"edit_terrain", [blend](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).heightBlendContrast = blend; });
            }
            else if (path == u8"textureSize")
            {
                const i32 size = static_cast<i32>(Clamp<i64>(value.AsInt(), 64, 4096));
                EditWhole(u8"edit_terrain", [size](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).paletteTextureSize = size; });
            }
            return;
        }
        if (&rows == m_baseRows.Get())
        {
            if (path == u8"tile")
            {
                const f32 tile = static_cast<f32>(Max(0.1, value.AsFloat()));
                EditWhole(u8"edit_terrain", [tile](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).baseTileScale = tile; });
                return;
            }
            for (const BaseMap& map : kBaseMaps)
                if (path == map.path)
                {
                    const Guid id = GuidOf(value);
                    Guid pipeline::TerrainAsset::*field = map.id;
                    EditWhole(u8"edit_terrain", [field, id](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).*field = id; });
                }
            return;
        }
        const i32 layer = LayerOf(path);
        if (layer < 0 || static_cast<usize>(layer) >= Asset()->paletteAlbedoIds.Size())
            return;
        const usize index = static_cast<usize>(layer);
        const StringView field = LastSegment(path);
        if (field == u8"tile")
        {
            const f32 tile = static_cast<f32>(Max(0.1, value.AsFloat()));
            EditWhole(u8"edit_terrain", [index, tile](ISerializable& object)
                      {
                          auto& asset = static_cast<pipeline::TerrainAsset&>(object);
                          Square(asset);
                          asset.paletteTileScales[index] = tile;
                      });
            return;
        }
        for (const LayerMap& map : kLayerMaps)
            if (field == map.path)
            {
                const Guid id = GuidOf(value);
                Array<Guid> pipeline::TerrainAsset::*ids = map.ids;
                EditWhole(u8"edit_terrain", [ids, index, id](ISerializable& object)
                          {
                              auto& asset = static_cast<pipeline::TerrainAsset&>(object);
                              Square(asset);
                              (asset.*ids)[index] = id;
                          });
            }
    }

    void TerrainEditorPage::EditLayers(const gui::ListEdit& edit)
    {
        const pipeline::TerrainAsset* asset = Asset();
        if (asset == nullptr)
            return;
        if (edit.Kind == gui::ListEdit::Op::Insert)
        {
            // A new layer goes at the end with its maps unset, taking the next channel of the
            // weights.
            EditWhole(u8"edit_terrain", [](ISerializable& object)
                      {
                          auto& terrainAsset = static_cast<pipeline::TerrainAsset&>(object);
                          Square(terrainAsset);
                          for (const LayerMap& map : kLayerMaps)
                              (terrainAsset.*map.ids).PushBack(Guid{});
                          terrainAsset.paletteTileScales.PushBack(kNewLayerTile);
                      });
        }
        else if (edit.Kind == gui::ListEdit::Op::Remove && edit.Index < asset->paletteAlbedoIds.Size())
        {
            const usize index = edit.Index;
            // Free the layer's weights and move those above it down, in the live weights and in
            // their files.
            RemapWeightsOnRemove(static_cast<u32>(index));
            EditWhole(u8"edit_terrain", [index](ISerializable& object)
                      {
                          auto& terrainAsset = static_cast<pipeline::TerrainAsset&>(object);
                          Square(terrainAsset);
                          for (const LayerMap& map : kLayerMaps)
                              (terrainAsset.*map.ids).RemoveAt(index);
                          terrainAsset.paletteTileScales.RemoveAt(index);
                      });
        }
        ShowRows(); // show the list as the terrain holds it (a move, which the list does not offer, is undone)
    }

    void TerrainEditorPage::RemapWeightsOnRemove(u32 removed)
    {
        terrain::TerrainResource* product = m_product ? m_product.Get() : nullptr;
        terrain::SplatWeights* weights = product != nullptr ? product->weights.Get() : nullptr;
        if (weights == nullptr || weights->IsEmpty() || !terrain::RemapOnPaletteRemove(*weights, removed))
            return; // no live weights (not cooked yet), so nothing refers to the layers by channel
        if (product->weights.id.IsNil())
            return;
        // Written with the next save, as brush strokes are.
        RefPtr<terrain::SplatWeights> kept(weights);
        const Guid id = product->weights.id;
        Context().RegisterAssetEdit(id,
                                    [kept, id](foundation::content::ContentDatabase& db) -> Status
                                    {
                                        foundation::content::Instance* instance = db.GetInstance(id);
                                        if (instance == nullptr || kept.Get() == nullptr)
                                            return Status{ErrorCode::NotFound};
                                        RefPtr<ISerializable> object = instance->ReadObject();
                                        auto* splat = Cast<pipeline::SplatmapAsset>(object.Get());
                                        if (splat == nullptr)
                                            return Status{ErrorCode::InvalidArgument};
                                        splat->fileName = {};
                                        splat->width = kept->Width();
                                        splat->height = kept->Height();
                                        if (const Status wrote = instance->WriteObject(*splat); !wrote.IsOk())
                                            return wrote;
                                        if (const Status wrote = instance->WriteData(terrain::kSplatStream, terrain::SplatWeightsSource::WeightBlob(*kept)); !wrote.IsOk())
                                            return wrote;
                                        return instance->WriteData(terrain::kSplatIndexStream, terrain::SplatWeightsSource::IndexBlob(*kept));
                                    });
    }

    bool TerrainEditorPage::CreateSplatWeights(i32 size)
    {
        // Created here, not by a stroke: the paint tool edits only weights that exist, so a stroke
        // never creates an asset and its undo covers only the stroke.
        const pipeline::TerrainAsset* asset = Asset();
        if (asset == nullptr || !asset->weightsId.IsNil() || Context().Project() == nullptr)
            return false;
        // Beside the terrain, in its group.
        foundation::content::Instance* self = Context().Project()->SourceDb().GetInstance(InstanceId());
        foundation::content::Group* group = self != nullptr ? &self->OwningGroup() : Context().Project()->SourceDb().RootGroup();
        if (group == nullptr)
            return false;
        const String name = Format(u8"{}_splat", Title());
        foundation::content::Instance* made = group->CreateInstance(name.AsView(), pipeline::SplatmapAsset::StaticType());
        if (made == nullptr)
        {
            Context().Notify(NoticeKind::Error, Format(u8"No splat weights made: '{}' is taken.", name).AsView());
            return false;
        }
        pipeline::SplatmapAsset splat;
        splat.width = size > 0 ? size : 1024;
        splat.height = splat.width;
        (void)made->WriteObject(splat); // all zero, so the base layer shows everywhere, as its cook makes it
        const Guid id = made->Id();
        EditWhole(u8"edit_terrain", [id](ISerializable& object) { static_cast<pipeline::TerrainAsset&>(object).weightsId = id; });
        Context().RequestCook(false);
        Context().Notify(NoticeKind::Success, Format(u8"Made splat weights '{}'.", name).AsView());
        return true;
    }

    // === The preview ===

    void TerrainEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr)
            return;
        m_entity = scenePtr->CreateEntity(u8"PreviewTerrain");
        if (auto* terrains = scenePtr->GetSystem<engine::terrain::TerrainComponentManager>())
            terrains->Add(m_entity); // assigned its terrain once the product resolves
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.5f) * Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.9f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.type = engine::render::LightType::Directional;
            light.castsShadows = true; // so the terrain's own shadows show
        }
    }

    void TerrainEditorPage::PointPreviewAt(terrain::TerrainResource* product)
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        auto* terrains = scenePtr != nullptr ? scenePtr->GetSystem<engine::terrain::TerrainComponentManager>() : nullptr;
        engine::terrain::TerrainComponent* component = terrains != nullptr ? terrains->Get(m_entity) : nullptr;
        if (component == nullptr)
            return;
        component->terrain.SetId(Guid{});
        if (product != nullptr)
            component->terrain = product; // the cooked product itself
        else
            component->terrain.SetDirect(RefPtr<terrain::TerrainResource>{});
        if (m_pane)
        {
            if (product != nullptr)
                m_pane->Show();
            else
                m_pane->ShowEmpty(StringView{}, StringView{});
        }
    }

    void TerrainEditorPage::FramePreview()
    {
        if (!m_pane || m_pane->Viewport() == nullptr)
            return;
        terrain::TerrainResource* product = m_product ? m_product.Get() : nullptr;
        foundation::heightfield::Heightfield* hf = product != nullptr ? product->heightfield.Get() : nullptr;
        const Float2 world = hf != nullptr ? hf->WorldSize() : Float2{256.0f, 256.0f};
        const f32 middle = hf != nullptr ? 0.5f * (hf->MinY() + hf->MaxY()) : 0.0f;
        const f32 size = Max(world.x, world.y);
        PreviewViewport* preview = m_pane->Viewport();
        // View from above a corner, far enough that the whole terrain is in view (it is centred on
        // its origin) and its far side is within the camera's reach.
        const Float3 away = Normalized(Float3{1.0f, 0.85f, 1.0f});
        const f32 distance = size * 1.25f;
        preview->Camera().position = Float3{0.0f, middle, 0.0f} + away * distance;
        preview->Camera().LookAt(Float3{0.0f, middle, 0.0f});
        preview->Camera().farZ = Max(preview->Camera().farZ, distance + size);
    }

    void TerrainEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
        // When the cooked terrain arrives, or a cook replaces it, the preview, its framing and the
        // facts update.
        terrain::TerrainResource* product = m_product ? m_product.Get() : nullptr;
        if (product != m_shownProduct)
        {
            const bool first = m_shownProduct == nullptr;
            m_shownProduct = product;
            PointPreviewAt(product);
            if (first)
                FramePreview();
            ShowFacts();
        }
    }

    void TerrainEditorPage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void TerrainEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    UniquePtr<EditorPage> TerrainEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<TerrainEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterTerrainEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<TerrainEditorPageFactory>(services), editor::EditorRootAllocator()));
    }
}

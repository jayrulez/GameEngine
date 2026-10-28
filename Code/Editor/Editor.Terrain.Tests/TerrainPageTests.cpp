// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The terrain page over its markup (Data/Editor/PreviewPage.xml) and a scratch project, without a
// host (no preview): its rows from the asset (the terrain's, the base layer's, the paint layers a
// list of groups that adds and removes but does not reorder), each edit one undo step, a layer
// added and removed with every array kept in line, new splat weights made beside the terrain, and
// a terrain with no heightfield said so.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.texture.resource;
import pipeline.core;
import terrain.pipeline;
import heightfield.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.terrain;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    String Text(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }

    gui::ListEdit ListEditOf(gui::ListEdit::Op kind, usize index)
    {
        gui::ListEdit edit;
        edit.Kind = kind;
        edit.Index = index;
        return edit;
    }

    // A project with a terrain ("Hills": a heightfield, two layers, only the first with a normal
    // map, as an older terrain keeps them) and one with nothing ("Bare").
    struct Bed
    {
        String dir = String(u8"scratch_terrain_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* hills = nullptr;
        foundation::content::Instance* bare = nullptr;
        const Guid heights{0x7e, 0x1};
        const Guid grass{0x7e, 0x2};
        const Guid rock{0x7e, 0x3};
        const Guid grassNormal{0x7e, 0x4};
        const Guid moss{0x7e, 0x5};

        Bed()
        {
            pipeline::RegisterTerrainAsset();
            pipeline::RegisterHeightfieldAsset();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            hills = top->CreateInstance(u8"Hills", pipeline::TerrainAsset::StaticType());
            bare = top->CreateInstance(u8"Bare", pipeline::TerrainAsset::StaticType());
            REQUIRE(hills != nullptr);
            REQUIRE(bare != nullptr);
            pipeline::TerrainAsset terrain;
            REQUIRE(bare->WriteObject(terrain).IsOk());
            terrain.heightfieldId = heights;
            terrain.paletteAlbedoIds.PushBack(grass);
            terrain.paletteAlbedoIds.PushBack(rock);
            terrain.paletteTileScales.PushBack(16.0f);
            terrain.paletteTileScales.PushBack(48.0f);
            terrain.paletteNormalIds.PushBack(grassNormal); // ragged: rock has none, nor any ORM
            REQUIRE(hills->WriteObject(terrain).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<TerrainEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<TerrainEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };

    String Value(gui::PropertyBag& rows, StringView path)
    {
        return String(rows.GetValue(path, 0).AsString());
    }
}

TEST_CASE("terrain page: the terrain's rows, its base layer's, and its paint layers a list of groups that does not reorder")
{
    Bed bed;
    UniquePtr<TerrainEditorPage> page = bed.Open(*bed.hills);
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(Value(page->TerrainRows(), u8"heightfield") == Text(bed.heights));
    CHECK(page->TerrainRows().FindInfo(u8"heightfield")->ReferenceType == &pipeline::HeightfieldAsset::StaticType());
    CHECK(page->TerrainRows().FindInfo(u8"weights")->ReferenceType == &pipeline::SplatmapAsset::StaticType());
    CHECK(page->TerrainRows().GetValue(u8"castShadows", 0).AsBool());
    CHECK(page->BaseRows().FindInfo(u8"albedo")->ReferenceType == &foundation::texture::Texture::StaticType());

    gui::PropertyInfo* list = page->LayerRows().FindInfo(u8"layers");
    REQUIRE(list != nullptr);
    CHECK(list->Kind == gui::PropertyKind::List);
    CHECK((list->ListOps & gui::PropertyListMove) == 0); // a layer's place is its weight channel
    CHECK((list->ListOps & gui::PropertyListAdd) != 0);
    REQUIRE(page->LayerRows().ElementCount(u8"layers", 0) == 2u);
    Array<gui::PropertyInfo> fields;
    page->LayerRows().GetProperties(u8"layers.0", fields);
    REQUIRE(fields.Size() == 6u); // albedo, normal, ORM, height, coverage, tile
    CHECK(fields[0].ShownName() == StringView(u8"Albedo"));
    CHECK(fields[4].ShownName() == StringView(u8"Coverage"));
    CHECK(fields[5].Kind == gui::PropertyKind::Float);
    Array<gui::PropertyInfo> layers;
    page->LayerRows().GetProperties(u8"layers", layers);
    REQUIRE(layers.Size() == 2u);
    CHECK(layers[0].ShownName() == StringView(u8"Layer 1")); // its channel, as the paint tool numbers it
    CHECK(layers[1].Summary == String(u8"No albedo"));       // the albedo's name when the project has it
    CHECK(Value(page->LayerRows(), u8"layers.0.albedo") == Text(bed.grass));
    CHECK(Value(page->LayerRows(), u8"layers.0.normal") == Text(bed.grassNormal));
    CHECK(Value(page->LayerRows(), u8"layers.1.albedo") == Text(bed.rock));
    CHECK(Value(page->LayerRows(), u8"layers.1.normal").IsEmpty()); // past the short array: none
    CHECK(page->LayerRows().GetValue(u8"layers.1.tile", 0).AsFloat() == doctest::Approx(48.0));
    CHECK(page->Notice(u8"heightfield").IsEmpty());
    CHECK(page->Fact(u8"Grid") == String(u8"Not cooked yet: Save to cook it"));

    // Opening it changes nothing; its first edit makes every layer array as long as the albedos.
    const pipeline::TerrainAsset* asset = page->Asset();
    REQUIRE(asset != nullptr);
    CHECK(asset->paletteNormalIds.Size() == 1u);
    CHECK_FALSE(page->IsDirty());
    page->EditRow(page->LayerRows(), u8"layers.0.tile", gui::Variant(20.0));
    CHECK(asset->paletteNormalIds.Size() == 2u);
    CHECK(asset->paletteMaskIds.Size() == 2u);

    UniquePtr<TerrainEditorPage> empty = bed.Open(*bed.bare);
    CHECK_FALSE(empty->Notice(u8"heightfield").IsEmpty());
    CHECK(empty->LayerRows().ElementCount(u8"layers", 0) == 0u);
}

TEST_CASE("terrain page: a map picked, a tiling and a switch, each one undo step")
{
    Bed bed;
    UniquePtr<TerrainEditorPage> page = bed.Open(*bed.hills);
    page->EditRow(page->LayerRows(), u8"layers.1.normal", gui::Variant(Text(bed.moss)));
    page->EditRow(page->LayerRows(), u8"layers.0.tile", gui::Variant(24.0));
    page->EditRow(page->TerrainRows(), u8"castShadows", gui::Variant(false));
    page->EditRow(page->BaseRows(), u8"albedo", gui::Variant(Text(bed.grass)));
    const pipeline::TerrainAsset* asset = page->Asset();
    REQUIRE(asset != nullptr);
    CHECK(asset->paletteNormalIds[1] == bed.moss);
    CHECK(asset->paletteTileScales[0] == doctest::Approx(24.0f));
    CHECK_FALSE(asset->castShadows);
    CHECK(asset->baseAlbedoId == bed.grass);
    CHECK(page->IsDirty());

    page->Commands().Undo();
    CHECK(asset->baseAlbedoId.IsNil());
    page->Commands().Undo();
    CHECK(asset->castShadows);
    CHECK(page->TerrainRows().GetValue(u8"castShadows", 0).AsBool()); // the row follows its undo
    page->Commands().Undo();
    CHECK(asset->paletteTileScales[0] == doctest::Approx(16.0f));
    page->Commands().Undo();
    CHECK(asset->paletteNormalIds.Size() == 1u); // as it was read: the short array back
    CHECK(Value(page->LayerRows(), u8"layers.1.normal").IsEmpty());
}

TEST_CASE("terrain page: a layer added at the end and one removed keep every array in line, each one undo step")
{
    Bed bed;
    UniquePtr<TerrainEditorPage> page = bed.Open(*bed.hills);
    const pipeline::TerrainAsset* asset = page->Asset();
    REQUIRE(asset != nullptr);

    REQUIRE(page->LayerRows().EditList(u8"layers", ListEditOf(gui::ListEdit::Op::Insert, 2)));
    CHECK(asset->paletteAlbedoIds.Size() == 3u);
    CHECK(asset->paletteHeightIds.Size() == 3u);
    CHECK(asset->paletteTileScales.Size() == 3u);
    CHECK(page->LayerRows().ElementCount(u8"layers", 0) == 3u);

    // The first removed: those after it move down, their maps with them.
    REQUIRE(page->LayerRows().EditList(u8"layers", ListEditOf(gui::ListEdit::Op::Remove, 0)));
    REQUIRE(asset->paletteAlbedoIds.Size() == 2u);
    CHECK(asset->paletteAlbedoIds[0] == bed.rock);
    CHECK(asset->paletteNormalIds[0].IsNil()); // grass's normal went with grass
    CHECK(asset->paletteTileScales[0] == doctest::Approx(48.0f));
    CHECK(Value(page->LayerRows(), u8"layers.0.albedo") == Text(bed.rock));

    page->Commands().Undo(); // the removal
    REQUIRE(asset->paletteAlbedoIds.Size() == 3u);
    CHECK(asset->paletteAlbedoIds[0] == bed.grass);
    CHECK(asset->paletteNormalIds[0] == bed.grassNormal);
    page->Commands().Undo(); // the add
    CHECK(asset->paletteAlbedoIds.Size() == 2u);
    CHECK(page->LayerRows().ElementCount(u8"layers", 0) == 2u);
}

TEST_CASE("terrain page: New Splat Weights makes them beside the terrain and gives them to it, once")
{
    Bed bed;
    UniquePtr<TerrainEditorPage> page = bed.Open(*bed.hills);
    REQUIRE(page->SplatButton() != nullptr);
    CHECK(page->SplatButton()->IsEnabled());
    REQUIRE(page->CreateSplatWeights(512));
    const pipeline::TerrainAsset* asset = page->Asset();
    REQUIRE(asset != nullptr);
    REQUIRE_FALSE(asset->weightsId.IsNil());
    foundation::content::Instance* made = bed.project->SourceDb().GetInstance(asset->weightsId);
    REQUIRE(made != nullptr);
    CHECK(made->Name() == StringView(u8"Hills_splat"));
    CHECK(&made->OwningGroup() == &bed.hills->OwningGroup());
    RefPtr<ISerializable> object = made->ReadObject();
    auto* splat = Cast<pipeline::SplatmapAsset>(object.Get());
    REQUIRE(splat != nullptr);
    CHECK(splat->width == 512);
    CHECK(Value(page->TerrainRows(), u8"weights") == Text(asset->weightsId));
    CHECK_FALSE(page->SplatButton()->IsEnabled()); // it has some now
    CHECK_FALSE(page->CreateSplatWeights(1024));

    page->Commands().Undo(); // the terrain lets them go; the asset made stays
    CHECK(asset->weightsId.IsNil());
    CHECK(page->SplatButton()->IsEnabled());
}

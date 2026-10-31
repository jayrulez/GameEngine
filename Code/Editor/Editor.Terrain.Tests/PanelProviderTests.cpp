// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Terrain tool-panel providers: after RegisterTerrainToolPanels, the registry resolves a provider
// for each brush tool id, and each builds its panel from the kit for its concrete tool, showing the
// tool's settings and setting them. Headless; the panel is just a node tree.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.scene;
import foundation.heightfield;
import foundation.terrain.resource;
import engine.terrain;
import editor.core;
import editor.viewporttools;
import editor.app;
import editor.terrain;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace terrain = foundation::terrain;
namespace hf = foundation::heightfield;

TEST_CASE("terrain tool panels: a panel of the kit for each brush; it shows the tool and sets it, the keys' changes too")
{
    editor::RegisterTerrainToolPanels(); // idempotent (the first registration per tool id wins)

    editor::ViewportToolPanelRegistry& reg = editor::ViewportToolPanelRegistry::Get();
    editor::IViewportToolPanelProvider* sculptP = reg.FindByToolId(u8"terrain.sculpt");
    editor::IViewportToolPanelProvider* splatP = reg.FindByToolId(u8"terrain.splat");
    editor::IViewportToolPanelProvider* holeP = reg.FindByToolId(u8"terrain.hole");
    REQUIRE(sculptP != nullptr);
    REQUIRE(splatP != nullptr);
    REQUIRE(holeP != nullptr);
    CHECK(reg.FindByToolId(u8"select") == nullptr); // the default tool has no panel

    // Build the tools the panels drive, over an in-memory terrain with a six-layer palette and
    // weights, so the splat panel's base and unbounded swatch list run with more layers than the
    // four slots per texel (top-K model).
    scene::Scene sceneObj{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(sceneObj);
    auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    RefPtr<terrain::SplatWeights> weights =
        MakeRef<terrain::SplatWeights>(DefaultAllocator(), 16, 16);
    auto res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get();
    res->weights = weights.Get();
    for (i32 i = 0; i < 6; ++i)
    {
        res->palette.PushBack(terrain::TerrainResource::Layer{});
    }
    mgr->Add(sceneObj.CreateEntity(u8"terrain")).terrain = res.Get();
    sceneObj.Start();

    editor::EditorCommandStack commands;
    editor::TerrainSculptTool sculpt(sceneObj, commands, nullptr);
    editor::TerrainSplatTool splat(sceneObj, commands, nullptr);
    splat.SetPaletteIndex(5); // an index past the first four layers
    CHECK(splat.PaletteIndex() == 5u);

    editor::ViewportToolHostContext ctx;
    ctx.scene = &sceneObj;
    ctx.commands = &commands;

    RefPtr<foundation::gui::Node> sculptNode = sculptP->CreatePanel(sculpt, ctx);
    RefPtr<foundation::gui::Node> splatNode = splatP->CreatePanel(splat, ctx);
    editor::TerrainHoleTool hole(sceneObj, commands, nullptr);
    RefPtr<foundation::gui::Node> holeNode = holeP->CreatePanel(hole, ctx);
    auto* sculptPanel = Cast<editor::app::ToolPanelBody>(sculptNode.Get());
    auto* splatPanel = Cast<editor::app::ToolPanelBody>(splatNode.Get());
    auto* holePanel = Cast<editor::app::ToolPanelBody>(holeNode.Get());
    REQUIRE(sculptPanel != nullptr);
    REQUIRE(splatPanel != nullptr);
    REQUIRE(holePanel != nullptr);

    // Sculpt: its four modes as words, with the chosen one checked; a change from a key shows
    // after a Sync.
    REQUIRE(sculptPanel->ChoiceCount(0) == 4u);
    CHECK(sculptPanel->ChoiceAt(0, 0)->GetText() == StringView(u8"Raise"));
    CHECK(sculptPanel->ChoiceAt(0, 0)->IsChecked());
    sculpt.SetMode(editor::TerrainSculptTool::Mode::Flatten); // as the 4 key does
    sculpt.SetRadius(20.0f);                                  // as Shift + wheel does
    sculptPanel->Sync();
    CHECK(sculptPanel->ChoiceAt(0, 3)->IsChecked());
    CHECK_FALSE(sculptPanel->ChoiceAt(0, 0)->IsChecked());
    CHECK(sculptPanel->NumberAt(0)->GetValue() == doctest::Approx(20.0));
    sculptPanel->ChoiceAt(0, 1)->HandleActivate(); // Lower, as a click would
    CHECK(sculpt.GetMode() == editor::TerrainSculptTool::Mode::Lower);

    // Paint Layers: the six layers numbered from 1 (like their keys), each named in its tooltip,
    // with index five checked; the mode is a row of words.
    REQUIRE(splatPanel->ChoiceCount(0) == 6u);
    CHECK(splatPanel->ChoiceAt(0, 0)->GetText() == StringView(u8"1"));
    CHECK(splatPanel->ChoiceAt(0, 5)->IsChecked());
    for (usize i = 0; i < 6; ++i)
        CHECK_FALSE(splatPanel->ChoiceAt(0, i)->GetTooltipText().IsEmpty());
    // The base is shown above them but cannot be chosen (erasing reveals it); its tooltip names
    // it.
    foundation::gui::Image* base = splatPanel->PictureAt(0);
    REQUIRE(base != nullptr);
    REQUIRE(base->GetParent() != nullptr);
    CHECK(String(base->GetParent()->GetTooltipText()).AsView().StartsWith(u8"The base layer"));
    CHECK(splatPanel->PictureAt(1) == nullptr);
    REQUIRE(splatPanel->ChoiceCount(1) == 3u);
    CHECK(splatPanel->ChoiceAt(1, 0)->IsChecked()); // painting
    splatPanel->ChoiceAt(1, 1)->HandleActivate();   // Erase
    CHECK(splat.IsEraser());
    splatPanel->ChoiceAt(1, 2)->HandleActivate();   // Smooth
    CHECK(splat.IsSmooth());
    CHECK_FALSE(splat.IsEraser());
    splatPanel->ChoiceAt(1, 0)->HandleActivate();   // back to painting the same layer
    CHECK_FALSE(splat.IsSmooth());
    CHECK(splat.PaletteIndex() == 5u);
    splatPanel->CheckAt(0)->HandleActivate();       // Airbrush
    CHECK(splat.IsAirbrush());

    // Cut Holes: Cut or Fill.
    REQUIRE(holePanel->ChoiceCount(0) == 2u);
    holePanel->ChoiceAt(0, 1)->HandleActivate();
    CHECK(hole.GetMode() == editor::TerrainHoleTool::Mode::Fill);
}

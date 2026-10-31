// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - the Game tab's resolution choices: the project's first, then the export presets
// that draw at their own size, then the user's preview resolutions, then the panel's own size. A
// saved key finds its entry, and one whose entry is gone falls back to the project's.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import engine.project;
import editor.core;
import editor.play;

using namespace foundation::core;
using namespace editor;

TEST_CASE("game-resolution: the project's first, then the presets, the previews, the panel")
{
    // With no sources at all: the project's entry (at the panel's size) and the panel's.
    {
        const Array<GameResolutionChoice> bare = CollectGameResolutions(nullptr, nullptr, nullptr);
        REQUIRE(bare.Size() == 2u);
        CHECK(bare[0].key == kProjectResolutionKey);
        CHECK(bare[0].label == StringView(u8"Project (panel size)"));
        CHECK(bare[0].width == 0u);
        CHECK(bare[1].key == kPanelResolutionKey);
    }

    engine::project::ProjectSettings settings;
    settings.renderWidth = 640;
    settings.renderHeight = 360;
    ExportPresetSet presets;
    ExportPreset desktop; // draws at the project's size, so it is not offered
    desktop.name = String(u8"Desktop");
    ExportPreset handheld;
    handheld.name = String(u8"Handheld");
    handheld.overridesRender = true;
    handheld.renderWidth = 1280;
    handheld.renderHeight = 800;
    presets.presets.PushBack(desktop);
    presets.presets.PushBack(handheld);
    GamePreviewSettings previews;
    previews.presets.PushBack(GamePreviewResolution{String(u8"QHD"), 2560, 1440});
    previews.presets.PushBack(GamePreviewResolution{String(u8"Broken"), 0, 1440}); // skipped

    const Array<GameResolutionChoice> choices = CollectGameResolutions(&settings, &presets, &previews);
    REQUIRE(choices.Size() == 4u);
    CHECK(choices[0].label == StringView(u8"Project 640x360"));
    CHECK(choices[0].width == 640u);
    CHECK(choices[1].key == StringView(u8"export:Handheld"));
    CHECK(choices[1].label == StringView(u8"Handheld 1280x800"));
    CHECK(choices[2].key == StringView(u8"preset:QHD"));
    CHECK(choices[2].height == 1440u);
    CHECK(choices[3].key == kPanelResolutionKey);
    CHECK(choices[3].width == 0u);

    const Span<const GameResolutionChoice> view(choices.Data(), choices.Size());
    CHECK(FindGameResolution(view, u8"preset:QHD") == 2u);

    // A preset held its own way (a phone-facing Web preset) is offered at the project's size and
    // named with its orientation, which the tab previews it with.
    ExportPreset web;
    web.name = String(u8"Web");
    web.overridesOrientation = true;
    web.orientation = ScreenOrientation::Landscape;
    presets.presets.PushBack(web);
    const Array<GameResolutionChoice> held = CollectGameResolutions(&settings, &presets, &previews);
    REQUIRE(held.Size() == 5u);
    CHECK(held[2].key == StringView(u8"export:Web"));
    CHECK(held[2].label == StringView(u8"Web 640x360, landscape"));
    CHECK(held[2].width == 640u);
    CHECK(held[2].overridesOrientation);
    CHECK(held[2].orientation == ScreenOrientation::Landscape);
    CHECK_FALSE(held[1].overridesOrientation); // the handheld keeps the project's orientation
    // The signature follows an orientation too.
    CHECK(GameResolutionSignature(Span<const GameResolutionChoice>(held.Data(), held.Size()), FitMode::Letterbox) !=
          GameResolutionSignature(view, FitMode::Letterbox));
    CHECK(FindGameResolution(view, u8"preset:Gone") == 0u); // falls back to the project's entry

    // The signature follows the sizes and the fit, so a change rebuilds the dropdown.
    const String before = GameResolutionSignature(view, FitMode::Letterbox);
    CHECK(GameResolutionSignature(view, FitMode::Letterbox) == before);
    CHECK(GameResolutionSignature(view, FitMode::Crop) != before);
}

// The Game tab puts the game on its target as the player puts it in a window, so the phone
// preview (a portrait 720x1560 screen) turns a landscape game as a phone does.
TEST_CASE("game-resolution: the game on a previewed phone is turned as a phone turns it")
{
    CHECK(kPhonePreviewHeight * 9u == kPhonePreviewWidth * 19u + kPhonePreviewWidth / 2u); // 9 by 19.5

    // A landscape game with no resolution of its own is drawn at the phone's pixels, turned.
    const ContentFit turned = GameFitOn(kPhonePreviewWidth, kPhonePreviewHeight, 0, 0, FitMode::Letterbox,
                                        ScreenOrientation::Landscape);
    CHECK(turned.rotation == ContentRotation::ThreeQuarter);
    CHECK(turned.contentSize.x == doctest::Approx(1560.0f));
    CHECK(turned.contentSize.y == doctest::Approx(720.0f));
    CHECK(turned.mode == FitMode::Stretch);

    // With a resolution, it is drawn at that resolution by its fit, turned.
    const ContentFit sized = GameFitOn(kPhonePreviewWidth, kPhonePreviewHeight, 1280, 720, FitMode::Letterbox,
                                       ScreenOrientation::Landscape);
    CHECK(sized.rotation == ContentRotation::ThreeQuarter);
    CHECK(sized.contentSize.x == doctest::Approx(1280.0f));
    CHECK(sized.mode == FitMode::Letterbox);

    // Held any way, or on a target of its own shape: never turned.
    CHECK(GameFitOn(kPhonePreviewWidth, kPhonePreviewHeight, 0, 0, FitMode::Letterbox, ScreenOrientation::Any).rotation ==
          ContentRotation::None);
    const ContentFit panel = GameFitOn(1600, 900, 0, 0, FitMode::Letterbox, ScreenOrientation::Landscape);
    CHECK(panel.rotation == ContentRotation::None);
    CHECK(panel.contentSize.x == doctest::Approx(1600.0f));
}

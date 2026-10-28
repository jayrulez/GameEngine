// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :game_theme partition.
//
// GameTheme: the DEFAULT stylesheet for GAME UI (the UISubsystem's context ships with it;
// games override per-context, per-canvas via cooked UITheme assets, or per-control - all
// existing mechanics). A code theme in the DarkTheme shape: today a teal-accented dark
// palette tuned for over-scene legibility; it diverges further as game controls demand.

module;
#include "Core/Prelude.h"

export module foundation.ui:game_theme;

import foundation.core;
import :style_sheet;
import :theme_palette;
import :dark_theme;
import :light_theme;

using namespace foundation::core;

export namespace foundation::ui
{
    struct GameTheme
    {
        [[nodiscard]] static ThemePalette Palette() noexcept
        {
            ThemePalette p = ThemePalette::Dark();
            // Cooler, higher-contrast accent than the editor's warm orange: game UI sits
            // over arbitrary scene footage, not a neutral dock.
            p.PrimaryAccent = Color{64.0f / 255.0f, 200.0f / 255.0f, 190.0f / 255.0f, 1.0f};
            return p;
        }
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator)
        {
            return DarkTheme::Create(allocator, Palette());
        }
    };

    // The built-in LIGHT variant: the LightTheme shape
    // with the same teal game accent - for games whose UI sits on bright scenes/menus.
    // Swap per context (UISubsystem's Context().SetStyleSheet(GameLightTheme::Create()))
    // or ship a cooked UITheme asset for full control.
    struct GameLightTheme
    {
        [[nodiscard]] static ThemePalette Palette() noexcept
        {
            ThemePalette p = ThemePalette::Light();
            p.PrimaryAccent = Color{22.0f / 255.0f, 142.0f / 255.0f, 134.0f / 255.0f, 1.0f};
            return p;
        }
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator)
        {
            return LightTheme::Create(allocator, Palette());
        }
    };
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - :game_resolution partition.
//
// The resolutions the Game tab offers to draw at. The project's own render resolution comes
// first and is the default; then each export preset that draws at its own size, as that
// platform would; then the user's preview resolutions (GamePreviewSettings, Preferences); then
// the panel's own size. A fixed choice is fitted into the panel by the project's render fit, as
// the player fits it into its window. Kept apart from the page so the list is testable without
// a Game tab.

module;
#include "Core/Prelude.h"

export module editor.play:game_resolution;

import foundation.core;
import engine.project;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    inline constexpr StringView kProjectResolutionKey = u8"project";
    inline constexpr StringView kPanelResolutionKey = u8"panel";

    /// One entry of the Game tab's resolution dropdown.
    struct GameResolutionChoice
    {
        String key;   // "project", "export:<preset>", "preset:<name>" or "panel"
        String label; // what the dropdown shows
        u32 width = 0; // nought for the panel's own size
        u32 height = 0;
        // An export preset held its own way (a phone-facing Web preset says Landscape) is previewed
        // that way in the Game tab; otherwise the project's orientation applies.
        bool overridesOrientation = false;
        ScreenOrientation orientation = ScreenOrientation::Any;
    };

    /// The Game tab's phone preview: a phone held upright, 9 by 19.5.
    inline constexpr u32 kPhonePreviewWidth = 720;
    inline constexpr u32 kPhonePreviewHeight = 1560;

    /// How the game sits on a `width` x `height` target (the panel, or the previewed phone's
    /// screen), as the player puts it in its window: its render resolution (0 x 0 for none) fitted
    /// by `fit`, and turned a quarter when `orientation` does not match the target's shape. With no
    /// resolution of its own, it is drawn turned at the target's pixels.
    [[nodiscard]] inline ContentFit GameFitOn(u32 width, u32 height, u32 renderWidth, u32 renderHeight, FitMode fit,
                                              ScreenOrientation orientation) noexcept
    {
        const f32 w = static_cast<f32>(width);
        const f32 h = static_cast<f32>(height);
        const ContentRotation turn = TurnFor(orientation, w, h);
        const bool swaps = turn == ContentRotation::Quarter || turn == ContentRotation::ThreeQuarter;
        const bool sized = renderWidth > 0 && renderHeight > 0;
        const Float2 content = sized   ? Float2{static_cast<f32>(renderWidth), static_cast<f32>(renderHeight)}
                               : swaps ? Float2{h, w}
                                       : Float2{w, h};
        return ContentFit{Rectangle{0.0f, 0.0f, w, h}, content, sized ? fit : FitMode::Stretch, turn};
    }

    /// The orientation's name for a label, empty for Any.
    [[nodiscard]] inline StringView OrientationLabel(ScreenOrientation orientation) noexcept
    {
        switch (orientation)
        {
        case ScreenOrientation::Landscape:
            return u8"landscape";
        case ScreenOrientation::Portrait:
            return u8"portrait";
        case ScreenOrientation::Any:
        default:
            return u8"";
        }
    }

    /// The choices, in the dropdown's order. Any source may be absent (no project, no preset
    /// file, no user store); the project's entry and the panel's are always there.
    [[nodiscard]] inline Array<GameResolutionChoice> CollectGameResolutions(
        const engine::project::ProjectSettings* settings, const ExportPresetSet* presets,
        const GamePreviewSettings* previews)
    {
        Array<GameResolutionChoice> out;
        {
            GameResolutionChoice project;
            project.key = String(kProjectResolutionKey);
            if (settings != nullptr && settings->HasRenderResolution())
            {
                project.width = settings->renderWidth;
                project.height = settings->renderHeight;
                project.label = Format(u8"Project {}x{}", project.width, project.height);
            }
            else
            {
                project.label = String(u8"Project (panel size)");
            }
            out.PushBack(Move(project));
        }
        if (presets != nullptr)
        {
            const u32 projectWidth = out[0].width;
            const u32 projectHeight = out[0].height;
            for (const ExportPreset& preset : presets->presets)
            {
                // A preset that draws at its own size or is held its own way. One that changes
                // neither looks the same as the project entry.
                const bool ownSize = preset.overridesRender && preset.renderWidth > 0 && preset.renderHeight > 0;
                const StringView held = preset.overridesOrientation ? OrientationLabel(preset.orientation) : StringView();
                if (!ownSize && held.IsEmpty())
                {
                    continue;
                }
                GameResolutionChoice choice;
                choice.key = Format(u8"export:{}", preset.name.AsView());
                choice.width = ownSize ? preset.renderWidth : projectWidth;
                choice.height = ownSize ? preset.renderHeight : projectHeight;
                choice.label = choice.width > 0 ? Format(u8"{} {}x{}", preset.name.AsView(), choice.width, choice.height)
                                                : Format(u8"{} (panel size)", preset.name.AsView());
                if (!held.IsEmpty())
                {
                    choice.label += Format(u8", {}", held).AsView();
                    choice.overridesOrientation = true;
                    choice.orientation = preset.orientation;
                }
                out.PushBack(Move(choice));
            }
        }
        if (previews != nullptr)
        {
            for (const GamePreviewResolution& preview : previews->presets)
            {
                if (preview.width == 0 || preview.height == 0)
                {
                    continue;
                }
                out.PushBack(GameResolutionChoice{
                    Format(u8"preset:{}", preview.name.AsView()),
                    Format(u8"{} {}x{}", preview.name.AsView(), preview.width, preview.height), preview.width,
                    preview.height});
            }
        }
        out.PushBack(GameResolutionChoice{String(kPanelResolutionKey), String(u8"Fit to panel"), 0, 0});
        return out;
    }

    /// The entry a saved key names, or the project's entry (the first) when the key names none.
    [[nodiscard]] inline usize FindGameResolution(Span<const GameResolutionChoice> choices, StringView key)
    {
        for (usize i = 0; i < choices.Size(); ++i)
        {
            if (choices[i].key.AsView() == key)
            {
                return i;
            }
        }
        return 0;
    }

    /// What the choices were built from, and the fit they apply with: the dropdown rebuilds only
    /// when this changes.
    [[nodiscard]] inline String GameResolutionSignature(Span<const GameResolutionChoice> choices, FitMode fit)
    {
        String signature;
        for (const GameResolutionChoice& choice : choices)
        {
            signature += Format(u8"{}={}x{}@{};", choice.key.AsView(), choice.width, choice.height,
                                static_cast<i32>(choice.orientation))
                             .AsView();
        }
        signature += Format(u8"fit={}", static_cast<i32>(fit)).AsView();
        return signature;
    }
}

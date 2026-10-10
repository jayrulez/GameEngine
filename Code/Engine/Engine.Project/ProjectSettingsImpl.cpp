// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Project - ProjectSettings' reflection: the settings the Project Settings dialog edits,
// described once so the dialog and the MCP tools read the same list from the type. Kept out of
// the interface (a REFLECT_MEMBERS body there makes GCC emit a gcm cluster).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.project;

import foundation.core;

using namespace foundation::core;

namespace engine::project
{
    REFLECT_ENUM(WindowMode, "rtti::engine::project")
    {
        builder.Value("Windowed", WindowMode::Windowed)
            .Value("Fullscreen", WindowMode::Fullscreen)
            .Value("Borderless", WindowMode::Borderless);
    }

    REFLECT_MEMBERS(ProjectSettings, "rtti::engine::project")
    {
        builder.DataVersion(9);
        // The enums the display settings take (their enumerators name the dialog's and the MCP
        // tools' choices): FitMode is the core's.
        RegisterCoreTypes();
        RttiRegisterEnum_WindowMode();
        const auto asset = [&builder](StringView label, StringView type, StringView emptyText,
                                      StringView category)
        {
            builder.PropAttribute(kSettingLabelAttribute, String(label))
                .PropAttribute(kSettingCategoryAttribute, String(category))
                .PropAttribute(kSettingAssetTypeAttribute, String(type))
                .PropAttribute(kSettingEmptyTextAttribute, String(emptyText));
        };
        builder.Property<&ProjectSettings::name>("name")
            .PropAttribute(kSettingLabelAttribute, String(u8"Name"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"General"));
        // A project-relative path to the built native game module; empty = scripts only.
        builder.Property<&ProjectSettings::nativeModule>("nativeModule")
            .PropAttribute(kSettingLabelAttribute, String(u8"Native module"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"General"));
        // The scene the player and play-in-editor open.
        builder.Property<&ProjectSettings::defaultSceneId>("defaultSceneId");
        asset(u8"Default scene", u8"SceneDocument", u8"(none)", u8"Startup");
        // The cooked ScriptClass the player (and the Game tab) binds at startup.
        builder.Property<&ProjectSettings::startupScriptId>("startupScriptId");
        asset(u8"Startup script", u8"ScriptClassAsset", u8"(none)", u8"Startup");
        // The cooked map the player (and the Game tab) binds at startup.
        builder.Property<&ProjectSettings::defaultInputMapId>("defaultInputMapId");
        asset(u8"Default input map", u8"InputMapAsset", u8"(none)", u8"Startup");
        // The cooked mixer applied at startup; nil = the built-in neutral four-bus layout.
        builder.Property<&ProjectSettings::defaultBusLayoutId>("defaultBusLayoutId");
        asset(u8"Default bus layout", u8"AudioBusLayoutAsset", u8"(built-in)", u8"Audio");
        // The cooked UITheme the game UI defaults to; nil = the built-in GameTheme.
        builder.Property<&ProjectSettings::defaultUiThemeId>("defaultUiThemeId");
        asset(u8"Default UI theme", u8"UIThemeAsset", u8"(built-in)", u8"UI");
        // The cooked UIDocument shown as the boot splash while the default scene streams.
        builder.Property<&ProjectSettings::loadingDocumentId>("loadingDocumentId");
        asset(u8"Loading screen", u8"UIDocumentAsset", u8"(built-in)", u8"Startup");
        // The cooked font the game UI falls back to when a document names none.
        builder.Property<&ProjectSettings::defaultUiFontId>("defaultUiFontId");
        asset(u8"Default UI font", u8"FontAsset", u8"(built-in)", u8"UI");
        // Scene-pass MSAA samples (the render subsystem's levels: 1 = off, 2, 4).
        builder.Property<&ProjectSettings::renderMsaaSamples>("renderMsaaSamples")
            .PropAttribute(kSettingLabelAttribute, String(u8"MSAA"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"));
        // Cooked fonts the game UI loads beside the default one, each a family a label picks.
        RegisterArrayType<Guid>(); // the list container (the dialog's list, the MCP tools)
        builder.Property<&ProjectSettings::uiFontIds>("uiFontIds");
        asset(u8"Other UI fonts", u8"FontAsset", u8"(none)", u8"UI");
        // The display: the render resolution (0 x 0 is the output's size) and its fit, then the
        // player's window. An export preset may override either per platform.
        const Float4 renderSize{0.0f, 16384.0f, 1.0f, 0.0f};
        const Float4 windowSize{1.0f, 16384.0f, 1.0f, 0.0f};
        builder.Property<&ProjectSettings::renderWidth>("renderWidth")
            .PropAttribute(kSettingLabelAttribute, String(u8"Render width"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", renderSize);
        builder.Property<&ProjectSettings::renderHeight>("renderHeight")
            .PropAttribute(kSettingLabelAttribute, String(u8"Render height"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", renderSize);
        builder.Property<&ProjectSettings::renderFit>("renderFit")
            .PropAttribute(kSettingLabelAttribute, String(u8"Render fit"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::windowWidth>("windowWidth")
            .PropAttribute(kSettingLabelAttribute, String(u8"Window width"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", windowSize);
        builder.Property<&ProjectSettings::windowHeight>("windowHeight")
            .PropAttribute(kSettingLabelAttribute, String(u8"Window height"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"))
            .PropAttribute("range", windowSize);
        builder.Property<&ProjectSettings::windowMode>("windowMode")
            .PropAttribute(kSettingLabelAttribute, String(u8"Window mode"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"));
        builder.Property<&ProjectSettings::windowResizable>("windowResizable")
            .PropAttribute(kSettingLabelAttribute, String(u8"Window resizable"))
            .PropAttribute(kSettingCategoryAttribute, String(u8"Display"));
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :project_settings partition (Sedulous 3de51786)
//
// The open project's settings over MCP: what project_info reports and project_settings_set
// changes. Play reads its default scene, startup script and input map from them, so an agent's
// game binds its input map here. Both read the settings from ProjectSettings' reflection, the
// description the Project Settings dialog builds its rows from, through :reflected_fields. MSAA
// is the one setting with a rule of its own: a level of the render subsystem's table.

module;
#include "Core/Prelude.h"

export module editor.mcp:project_settings;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import engine.project;
import engine.render; // the canonical MSAA level table
import editor.project;
import :session;
import :reflected_fields;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

export namespace editor::mcp::detail
{
    /// The settings that are settings: ProjectSettings' reflected fields.
    inline Array<const PropertyInfo*> ProjectSettingProperties()
    {
        return ReflectedFieldsOf(engine::project::ProjectSettings::StaticType());
    }

    /// The settings as project_info reports them (FieldsJson).
    inline JsonValue ProjectSettingsJson(editor::EditorProject& project)
    {
        const Array<const PropertyInfo*> fields = ProjectSettingProperties();
        return FieldsJson(Instance(&project.Settings(), &engine::project::ProjectSettings::StaticType()),
                          Span<const PropertyInfo* const>(fields.Data(), fields.Size()), &project);
    }

    /// How the settings group, as the Project Settings dialog's tabs show them: each category (in
    /// the order the settings first name it) and the names of its settings, from their reflected
    /// `category` ("General" when none).
    inline JsonValue ProjectSettingCategoriesJson()
    {
        Array<String> categories;
        Array<JsonValue> names;
        for (const PropertyInfo* field : ProjectSettingProperties())
        {
            const StringView category = engine::project::SettingCategory(*field);
            usize index = 0;
            while (index < categories.Size() && categories[index].AsView() != category)
            {
                ++index;
            }
            if (index == categories.Size())
            {
                categories.PushBack(String(category));
                names.PushBack(JsonValue::MakeArray());
            }
            names[index].Add(JsonValue::MakeString(String(PropertyName(*field))));
        }
        JsonValue out = JsonValue::MakeObject();
        for (usize i = 0; i < categories.Size(); ++i)
        {
            out.Set(categories[i], Move(names[i]));
        }
        return out;
    }

    /// project_settings_set's schema: one argument per setting, as reflection describes it.
    inline JsonValue ProjectSettingsSchema()
    {
        const Array<const PropertyInfo*> fields = ProjectSettingProperties();
        foundation::mcp::SchemaBuilder schema;
        AddFieldsToSchema(schema, Span<const PropertyInfo* const>(fields.Data(), fields.Size()));
        return schema.Build();
    }
}

export namespace editor::mcp
{
    inline void RegisterProjectSettingsTool(foundation::mcp::McpServer& server,
                                            ProjectSession& session)
    {
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;
        server.RegisterTool(
            u8"project_settings_set",
            u8"Change the open project's settings, what the editor's Project Settings dialog "
            u8"edits: only what is given changes. Every asset setting must name an asset of its "
            u8"type (the refusal says which), \"\" clears it; a list setting (uiFontIds, the fonts "
            u8"the game UI loads beside the default, each a family a label picks with font-family) "
            u8"takes the whole list, [] for none; a count takes its range (the display's sizes), a "
            u8"choice one of its values by name (renderFit, windowMode), a flag true or false; MSAA "
            u8"takes the render levels. Checked "
            u8"in full before anything changes, then saved to the manifest; the editor re-applies "
            u8"what depends on them (the game UI's font and theme). Returns the settings and their "
            u8"categories as project_info does.",
            detail::ProjectSettingsSchema(), foundation::mcp::ToolAnnotations::Adjusts(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                editor::EditorProject& project = *s->project;
                engine::project::ProjectSettings& settings = project.Settings();
                const Instance instance(&settings, &engine::project::ProjectSettings::StaticType());
                const Array<const PropertyInfo*> properties = detail::ProjectSettingProperties();

                const Span<const PropertyInfo* const> fields(properties.Data(), properties.Size());

                // A misspelled setting never gets here: the schema declares every setting, and the
                // server refuses an argument it does not declare.
                // Everything is checked before anything changes: a refusal leaves the settings as
                // they were. MSAA takes a level of the render subsystem's table besides.
                Result<Array<detail::FieldChange>, String> checked = detail::CheckFields(args, fields, project);
                if (!checked.HasValue())
                {
                    return Err(Move(checked.Error()));
                }
                for (const detail::FieldChange& change : checked.Value())
                {
                    if (change.field->address(instance) != &settings.renderMsaaSamples)
                    {
                        continue;
                    }
                    bool level = false;
                    String levels;
                    for (u32 i = 0; i < engine::render::MsaaLevelCount(); ++i)
                    {
                        level = level || engine::render::kMsaaLevels[i].samples == change.number;
                        levels += levels.IsEmpty() ? u8"" : u8", ";
                        levels += Format(u8"{}", engine::render::kMsaaLevels[i].samples);
                    }
                    if (!level)
                    {
                        return Err(Format(u8"`{}` takes {}", detail::PropertyName(*change.field), levels.AsView()));
                    }
                }
                detail::ApplyFields(instance, Span<const detail::FieldChange>(checked.Value().Data(),
                                                                              checked.Value().Size()));
                // The path mirrors the dialog keeps beside the guids.
                settings.RefreshPathMirrors(
                    [&project](const Guid& id)
                    {
                        content::Instance* asset = project.SourceDb().GetInstance(id);
                        return asset != nullptr ? asset->Path() : String();
                    });
                if (!project.SaveSettings().IsOk())
                {
                    return Err(String(u8"the settings changed but the manifest did not save "
                                      u8"(log_read says why)"));
                }
                if (s->onSettingsChanged)
                {
                    s->onSettingsChanged();
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"settings", detail::ProjectSettingsJson(project));
                out.Set(u8"settingCategories", detail::ProjectSettingCategoriesJson());
                return out;
            });
    }
}

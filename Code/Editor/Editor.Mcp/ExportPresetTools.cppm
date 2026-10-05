// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :export_presets partition (Sedulous 4f483f5e).
//
// export_presets and export_preset_set: the open project's export targets (export_presets.xml),
// what the editor's Export Presets dialog edits, and the templates this machine can export them
// with. project_export runs one. A preset's fields are ExportPreset's reflection, through
// :reflected_fields; the tools' own rules are the preset's name (its identity), removal, and a
// platform and config this machine has templates for. A templateId need not resolve here -
// presets travel with the project and templates do not - so the answer says whether it does.

module;
#include "Core/Prelude.h"

export module editor.mcp:export_presets;

import foundation.core;
import foundation.json;
import foundation.vfs;
import foundation.mcp;
import editor.project;
import :session;
import :reflected_fields;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace vfs = foundation::vfs;

export namespace editor::mcp::detail
{
    /// The templates an export from this host sees: the shared templates root, then the player
    /// beside `hostToolDir` (the host executable's directory), as the inline export builds them.
    inline void RefreshExportTemplates(editor::TemplateRegistry& templates, StringView hostToolDir)
    {
        const String templatesRoot = editor::ResolveTemplatesRoot();
        UniquePtr<vfs::NativeFileSystem> rootFs;
        if (DirectoryExists(templatesRoot.AsView()))
        {
            rootFs = MakeUnique<vfs::NativeFileSystem>(editor::EditorRootAllocator(), templatesRoot.AsView(),
                                                       editor::EditorRootAllocator());
        }
        vfs::NativeFileSystem toolFs(hostToolDir, editor::EditorRootAllocator());
        templates.Refresh(templatesRoot.AsView(), rootFs.Get(), hostToolDir, &toolFs);
    }

    /// ExportPreset's reflected fields.
    inline Array<const PropertyInfo*> ExportPresetFields()
    {
        editor::RegisterExportPresetReflection();
        return ReflectedFieldsOf(TypeOf<editor::ExportPreset>());
    }

    /// The project's presets on disk, else the synthesized host default. True when from the file.
    inline bool LoadProjectPresets(editor::EditorProject& project, editor::ExportPresetSet& out)
    {
        vfs::NativeFileSystem projectFs(project.Directory(), editor::EditorRootAllocator());
        if (editor::LoadExportPresets(projectFs, out).IsOk())
        {
            return true;
        }
        editor::DefaultExportPresets(out);
        return false;
    }

    /// The presets (their fields, and `template`: the id each resolves to here, or null), whether
    /// they were synthesized, and the templates this machine has.
    inline JsonValue ExportPresetsJson(const editor::ExportPresetSet& presets, bool fromFile,
                                       const editor::TemplateRegistry& templates)
    {
        const Array<const PropertyInfo*> fields = ExportPresetFields();
        const Span<const PropertyInfo* const> span(fields.Data(), fields.Size());
        JsonValue list = JsonValue::MakeArray();
        for (const editor::ExportPreset& preset : presets.presets)
        {
            JsonValue json = FieldsJson(
                Instance(const_cast<editor::ExportPreset*>(&preset), &TypeOf<editor::ExportPreset>()), span, nullptr);
            const editor::ExportTemplate* resolved = templates.Resolve(preset);
            json.Set(u8"template", resolved != nullptr ? JsonValue::MakeString(resolved->id) : JsonValue::MakeNull());
            list.Add(Move(json));
        }
        JsonValue templateList = JsonValue::MakeArray();
        for (usize i = 0; i < templates.Count(); ++i)
        {
            const editor::ExportTemplate* tmpl = templates.At(i);
            JsonValue json = JsonValue::MakeObject();
            json.Set(u8"id", JsonValue::MakeString(tmpl->id));
            json.Set(u8"name", JsonValue::MakeString(tmpl->name));
            json.Set(u8"platform", JsonValue::MakeString(tmpl->platform));
            json.Set(u8"config", JsonValue::MakeString(String(tmpl->EffectiveConfig())));
            json.Set(u8"engineVersion", JsonValue::MakeString(tmpl->engineVersion));
            json.Set(u8"notes", JsonValue::MakeString(tmpl->notes));
            json.Set(u8"host", JsonValue::MakeBool(tmpl->isHost));
            templateList.Add(Move(json));
        }
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"presets", Move(list));
        out.Set(u8"synthesized", JsonValue::MakeBool(!fromFile));
        out.Set(u8"templates", Move(templateList));
        return out;
    }

    /// The distinct values a template field takes across `templates` (platforms, configs), comma
    /// separated: what a refusal lists.
    template <typename Get>
    String TemplateValues(const editor::TemplateRegistry& templates, Get get)
    {
        Array<String> values;
        for (usize i = 0; i < templates.Count(); ++i)
        {
            const String value(get(*templates.At(i)));
            bool listed = false;
            for (const String& other : values)
            {
                listed = listed || other == value;
            }
            if (!listed)
            {
                values.PushBack(value);
            }
        }
        String joined;
        for (const String& value : values)
        {
            joined += joined.IsEmpty() ? u8"" : u8", ";
            joined += value.AsView();
        }
        return joined;
    }
}

export namespace editor::mcp
{
    /// `hostToolDir`: the host executable's directory, where its own player (the host template) is.
    inline void RegisterExportPresetTools(foundation::mcp::McpServer& server, ProjectSession& session,
                                          String hostToolDir)
    {
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;
        server.RegisterTool(
            u8"export_presets",
            u8"The open project's export presets and the export templates this machine has. Each "
            u8"preset: its fields as export_preset_set takes them (name, platform, config, templateId - "
            u8"\"\" resolves by platform and config - playerName, outputSubdir, additionalFiles, "
            u8"stageSymbols, pruneToReachable, and the display overrides: overridesRender with "
            u8"renderWidth/renderHeight/renderFit, overridesWindow with windowWidth/windowHeight/"
            u8"windowMode/windowResizable), and `template`: the id it resolves to here, or null when this "
            u8"machine has no template for it. A project without export_presets.xml has one "
            u8"synthesized preset for the host (`synthesized`: true). Each template: id, name, "
            u8"platform, config, engineVersion, notes, host (the player beside this tool, not an "
            u8"installed bundle). export_preset_set changes them; project_export runs one.",
            foundation::mcp::SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, hostToolDir](const JsonValue&) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                editor::ExportPresetSet presets;
                const bool fromFile = detail::LoadProjectPresets(*s->project, presets);
                editor::TemplateRegistry templates;
                detail::RefreshExportTemplates(templates, hostToolDir.AsView());
                return detail::ExportPresetsJson(presets, fromFile, templates);
            });

        const Array<const PropertyInfo*> fields = detail::ExportPresetFields();
        foundation::mcp::SchemaBuilder schema;
        detail::AddFieldsToSchema(schema, Span<const PropertyInfo* const>(fields.Data(), fields.Size()));
        schema.Boolean(u8"remove", u8"delete the preset instead; nothing but `name` may be given");
        server.RegisterTool(
            u8"export_preset_set",
            u8"Create, change or remove one export preset of the open project, by `name` (required): "
            u8"only what is given changes, and a new preset starts as the host's platform, Release. "
            u8"`platform` and `config` must be ones this machine has export templates for (the "
            u8"refusal lists them); a templateId need not exist here - presets travel with the "
            u8"project and templates do not - so the answer's `template` says whether it resolves. "
            u8"Checked in full before anything changes, then saved to export_presets.xml (a project "
            u8"with none starts from its synthesized host preset, which stays). Returns the presets "
            u8"as export_presets does.",
            schema.Build(), foundation::mcp::ToolAnnotations::Overwrites(),
            [s, hostToolDir](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const Array<const PropertyInfo*> fields = detail::ExportPresetFields();
                const Span<const PropertyInfo* const> span(fields.Data(), fields.Size());
                const String name = args.Get(u8"name").AsString();
                if (name.IsEmpty())
                {
                    return Err(String(u8"`name` names the preset to create, change or remove"));
                }
                editor::ExportPresetSet presets;
                (void)detail::LoadProjectPresets(*s->project, presets);
                usize index = presets.presets.Size();
                for (usize i = 0; i < presets.presets.Size(); ++i)
                {
                    index = (index == presets.presets.Size() && presets.presets[i].name == name) ? i : index;
                }
                const bool exists = index < presets.presets.Size();
                editor::TemplateRegistry templates;
                detail::RefreshExportTemplates(templates, hostToolDir.AsView());

                if (args.Has(u8"remove") && args.Get(u8"remove").AsBool())
                {
                    if (args.Keys().Size() > 2)
                    {
                        return Err(String(u8"`remove` takes only `name`"));
                    }
                    if (!exists)
                    {
                        return Err(Format(u8"no preset named '{}'", name.AsView()));
                    }
                    presets.presets.RemoveAt(index);
                }
                else
                {
                    // Everything is checked before anything changes: a refusal leaves the file as it was.
                    Result<Array<detail::FieldChange>, String> checked =
                        detail::CheckFields(args, span, *s->project);
                    if (!checked.HasValue())
                    {
                        return Err(Move(checked.Error()));
                    }
                    editor::ExportPreset preset;
                    if (exists)
                    {
                        preset = presets.presets[index];
                    }
                    else
                    {
                        preset.name = name;
                        preset.platform = String(GetHostPlatformName());
                        preset.outputSubdir = name;
                    }
                    detail::ApplyFields(Instance(&preset, &TypeOf<editor::ExportPreset>()),
                                        Span<const detail::FieldChange>(checked.Value().Data(), checked.Value().Size()));
                    // The platform and config this machine can export: what its templates are.
                    bool platformKnown = false;
                    bool configKnown = preset.config.IsEmpty();
                    for (usize i = 0; i < templates.Count(); ++i)
                    {
                        platformKnown = platformKnown || templates.At(i)->platform == preset.platform;
                        configKnown = configKnown || templates.At(i)->EffectiveConfig() == preset.config.AsView();
                    }
                    if (args.Has(u8"platform") && !platformKnown)
                    {
                        return Err(Format(
                            u8"`platform` takes a platform this machine has export templates for: {}",
                            detail::TemplateValues(templates, [](const editor::ExportTemplate& t)
                                                   { return t.platform.AsView(); })
                                .AsView()));
                    }
                    if (args.Has(u8"config") && !configKnown)
                    {
                        return Err(Format(
                            u8"`config` takes a config this machine has export templates for: {}",
                            detail::TemplateValues(templates, [](const editor::ExportTemplate& t)
                                                   { return t.EffectiveConfig(); })
                                .AsView()));
                    }
                    if (exists)
                    {
                        presets.presets[index] = Move(preset);
                    }
                    else
                    {
                        presets.presets.PushBack(Move(preset));
                    }
                }
                vfs::NativeFileSystem projectFs(s->project->Directory(), editor::EditorRootAllocator());
                if (!editor::SaveExportPresets(*projectFs.AsWritable(), presets).IsOk())
                {
                    return Err(String(u8"the presets changed but export_presets.xml did not save (log_read says why)"));
                }
                return detail::ExportPresetsJson(presets, /*fromFile*/ true, templates);
            });
    }
}

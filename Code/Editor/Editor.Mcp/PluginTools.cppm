// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :plugin_tools partition
//
// The open project's plugins over MCP: plugin_list reports each entry of the project's list as
// the process's plugin host left it (loaded, not enabled here, refused, missing) with its
// settings, plugin_enable adds an entry or changes its path and platforms, plugin_disable removes
// one, plugin_settings_set changes a loaded plugin's settings, and plugin_scaffold makes the game's
// own package or adds parts to it (what the editor's Add Native Code does). A plugin's settings are
// its own reflected type, labelled as the project's settings are, so both read and check them
// through :reflected_fields. An entry whose plugin is not loaded keeps its settings as written:
// nothing here can read them, so nothing here changes them.

module;
#include "Core/Prelude.h"

export module editor.mcp:plugin_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import engine.project;
import editor.project;
import :session;
import :reflected_fields;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace runtime = foundation::runtime;

export namespace editor::mcp::detail
{
    /// The settings object a loaded plugin's parts read, or null.
    [[nodiscard]] inline ISerializable* LoadedPluginSettings(const ProjectSession& session, StringView id)
    {
        return session.plugins != nullptr ? session.plugins->Settings(id) : nullptr;
    }

    /// One entry as plugin_list reports it.
    inline JsonValue PluginEntryJson(const ProjectSession& session, const runtime::EnabledPlugin& entry)
    {
        const engine::project::PluginStatus status =
            engine::project::PluginStatusOf(entry, session.plugins, session.project->Allocator());
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"id", JsonValue::MakeString(entry.id));
        out.Set(u8"path", JsonValue::MakeString(entry.path));
        JsonValue platforms = JsonValue::MakeArray();
        for (const String& platform : entry.platforms)
        {
            platforms.Add(JsonValue::MakeString(platform));
        }
        out.Set(u8"platforms", Move(platforms));
        out.Set(u8"state", JsonValue::MakeString(String(engine::project::PluginStateName(status.state))));
        if (!status.reason.IsEmpty())
        {
            out.Set(u8"reason", JsonValue::MakeString(status.reason));
        }
        JsonValue unsupported = JsonValue::MakeArray();
        for (const String& platform : status.unsupportedPlatforms)
        {
            unsupported.Add(JsonValue::MakeString(platform));
        }
        out.Set(u8"unsupportedPlatforms", Move(unsupported));
        if (const runtime::PluginManifest* manifest =
                session.plugins != nullptr ? session.plugins->Manifest(entry.id.AsView()) : nullptr)
        {
            out.Set(u8"name", JsonValue::MakeString(manifest->name));
            out.Set(u8"version", JsonValue::MakeString(manifest->version));
        }
        ISerializable* settings = LoadedPluginSettings(session, entry.id.AsView());
        if (settings != nullptr)
        {
            const Array<const PropertyInfo*> fields = ReflectedFieldsOf(*settings->GetType());
            out.Set(u8"settings",
                    FieldsJson(Instance(settings, settings->GetType()),
                               Span<const PropertyInfo* const>(fields.Data(), fields.Size()), session.project));
        }
        else
        {
            out.Set(u8"settings", JsonValue::MakeNull());
        }
        out.Set(u8"settingsEditable",
                JsonValue::MakeBool(settings != nullptr && session.plugins->SettingsRead(entry.id.AsView())));
        return out;
    }

    inline JsonValue PluginListJson(const ProjectSession& session)
    {
        JsonValue plugins = JsonValue::MakeArray();
        for (const runtime::EnabledPlugin& entry : session.project->Settings().plugins)
        {
            plugins.Add(PluginEntryJson(session, entry));
        }
        // What the project could enable: its Plugins folder's packages and this host's linked
        // plugins that no entry names, each with why it is not offered when it is not.
        JsonValue available = JsonValue::MakeArray();
        const Array<engine::project::AvailablePlugin> found = engine::project::DiscoverPlugins(
            session.project->Directory(), session.project->Settings(), session.linkedPlugins, session.project->Allocator());
        for (const engine::project::AvailablePlugin& plugin : found)
        {
            JsonValue item = JsonValue::MakeObject();
            item.Set(u8"id", JsonValue::MakeString(plugin.id));
            item.Set(u8"path", JsonValue::MakeString(plugin.path));
            item.Set(u8"name", JsonValue::MakeString(plugin.name));
            item.Set(u8"version", JsonValue::MakeString(plugin.version));
            JsonValue parts = JsonValue::MakeArray();
            for (const String& part : plugin.parts)
            {
                parts.Add(JsonValue::MakeString(part));
            }
            item.Set(u8"parts", Move(parts));
            JsonValue platforms = JsonValue::MakeArray();
            for (const String& platform : plugin.platforms)
            {
                platforms.Add(JsonValue::MakeString(platform));
            }
            item.Set(u8"platforms", Move(platforms));
            item.Set(u8"offered", JsonValue::MakeBool(plugin.problem.IsEmpty()));
            if (!plugin.problem.IsEmpty())
            {
                item.Set(u8"problem", JsonValue::MakeString(plugin.problem));
            }
            available.Add(Move(item));
        }
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"platform", JsonValue::MakeString(String(session.plugins != nullptr ? session.plugins->Platform()
                                                                                      : GetHostPlatformName())));
        out.Set(u8"plugins", Move(plugins));
        out.Set(u8"available", Move(available));
        return out;
    }

    /// Saves the manifest after a change to the list and tells the host; the list, or why not.
    inline Result<JsonValue, String> SavePluginList(ProjectSession& session)
    {
        if (!session.project->SaveSettings().IsOk())
        {
            return Err(String(u8"the plugins changed but the manifest did not save (log_read says why)"));
        }
        if (session.onSettingsChanged)
        {
            session.onSettingsChanged();
        }
        return PluginListJson(session);
    }
}

export namespace editor::mcp
{
    inline void RegisterPluginTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"plugin_list",
            u8"The plugins the open project enables, in its order: each entry's id, path (a "
            u8"project-local package; empty for the engine's), platforms (empty is every platform its "
            u8"manifest supports), and its state in this host on `platform`: loaded, notEnabled (not "
            u8"enabled on this platform), refused or missing, with the reason when it is not loaded. "
            u8"unsupportedPlatforms lists platforms the entry names that its manifest does not support. "
            u8"A loaded plugin with settings reports them (`settings`, as project_info reports the "
            u8"project's), and settingsEditable says whether plugin_settings_set may change them; any "
            u8"other entry's settings are kept as written. `available` lists what the project could "
            u8"enable: each package in its Plugins folder no entry names and each plugin this host links "
            u8"that no entry enables (path empty), with name, version, parts and platforms; `offered` false "
            u8"with a `problem` when it cannot be enabled (its manifest does not read or suit this engine, "
            u8"or its id is taken). Enable one with plugin_enable (its id and path).",
            SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue& /*args*/) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                return detail::PluginListJson(*s);
            });

        server.RegisterTool(
            u8"plugin_enable",
            u8"Enable a plugin in the open project, or change an enabled one's `path` or `platforms`; "
            u8"its settings are kept. `id` is the plugin's manifest id. `path` locates a project-local "
            u8"package (a folder with its Plugin.xml, relative to the project; \"\" for the engine's). "
            u8"`platforms` lists the platforms it is enabled on (Linux64, Win64, Web), [] for every one "
            u8"its manifest supports. Refused when the id is not a plugin id, when the package at `path` "
            u8"is not this plugin's, or when a platform named is one its manifest does not support. A "
            u8"host loads the project's plugins when it opens the project, so the change takes effect "
            u8"there. Returns the list as plugin_list does.",
            SchemaBuilder()
                .Str(u8"id", u8"the plugin's manifest id", true)
                .Str(u8"path", u8"a project-local package, relative to the project (\"\" for the engine's)")
                .Arr(u8"platforms", u8"string", u8"the platforms it is enabled on; [] for all it supports")
                .Build(),
            foundation::mcp::ToolAnnotations::Adjusts(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                editor::EditorProject& project = *s->project;
                const String id(args.Get(u8"id").AsString());
                if (!runtime::IsValidPluginId(id.AsView()))
                {
                    return Err(Format(u8"'{}' is not a plugin id (lower case letters, digits and underscores, "
                                      u8"starting with a letter)",
                                      id));
                }
                runtime::EnabledPlugin* existing = project.Settings().FindPlugin(id.AsView());
                runtime::EnabledPlugin entry;
                entry.id = id;
                if (existing != nullptr)
                {
                    entry.path = existing->path;
                    entry.platforms = existing->platforms;
                }
                if (args.Has(u8"path"))
                {
                    entry.path = args.Get(u8"path").AsString();
                }
                if (args.Has(u8"platforms"))
                {
                    entry.platforms.Clear();
                    const JsonValue platforms = args.Get(u8"platforms");
                    for (i64 i = 0; i < platforms.Count(); ++i)
                    {
                        if (!platforms.At(static_cast<usize>(i)).IsString())
                        {
                            return Err(String(u8"`platforms` takes platform names (Linux64, Win64, Web)"));
                        }
                        const String platform = platforms.At(static_cast<usize>(i)).AsString();
                        bool listed = false;
                        for (const String& known : entry.platforms)
                        {
                            listed = listed || known == platform;
                        }
                        if (!listed)
                        {
                            entry.platforms.PushBack(platform);
                        }
                    }
                }
                // The manifest to check the platforms against: the package's own, else the host's.
                runtime::PluginManifest package;
                const runtime::PluginManifest* manifest =
                    s->plugins != nullptr ? s->plugins->Manifest(id.AsView()) : nullptr;
                if (!entry.path.IsEmpty())
                {
                    const String directory = PathJoin(project.Directory(), entry.path.AsView());
                    if (!runtime::LoadPluginManifest(directory.AsView(), package, project.Allocator()).IsOk())
                    {
                        return Err(Format(u8"'{}' holds no plugin manifest (Plugin.xml) that reads", entry.path));
                    }
                    if (package.id != id)
                    {
                        return Err(Format(u8"the package at '{}' is plugin '{}', not '{}'", entry.path, package.id, id));
                    }
                    manifest = &package;
                }
                if (manifest != nullptr)
                {
                    const Array<String> unsupported = runtime::UnsupportedPlatforms(entry, *manifest, project.Allocator());
                    if (!unsupported.IsEmpty())
                    {
                        String names;
                        for (const runtime::PluginPlatform& platform : manifest->platforms)
                        {
                            names += names.IsEmpty() ? u8"" : u8", ";
                            names += platform.platform.AsView();
                        }
                        return Err(Format(u8"plugin '{}' does not support {} (it supports: {})", id, unsupported[0],
                                          names.IsEmpty() ? StringView(u8"none") : names.AsView()));
                    }
                }
                if (existing != nullptr)
                {
                    existing->path = Move(entry.path);
                    existing->platforms = Move(entry.platforms);
                }
                else
                {
                    project.Settings().plugins.PushBack(Move(entry));
                }
                return detail::SavePluginList(*s);
            });

        server.RegisterTool(
            u8"plugin_disable",
            u8"Disable a plugin in the open project: its entry leaves the list, and its settings with it "
            u8"(plugin_list shows them first). The plugin stays loaded in a host until it next opens the "
            u8"project. Returns the list as plugin_list does.",
            SchemaBuilder().Str(u8"id", u8"the plugin's id, as plugin_list names it", true).Build(),
            foundation::mcp::ToolAnnotations::Adjusts(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String id(args.Get(u8"id").AsString());
                Array<runtime::EnabledPlugin>& plugins = s->project->Settings().plugins;
                for (usize i = 0; i < plugins.Size(); ++i)
                {
                    if (plugins[i].id == id)
                    {
                        plugins.RemoveAt(i);
                        return detail::SavePluginList(*s);
                    }
                }
                return Err(Format(u8"the project enables no plugin '{}' (plugin_list names them)", id));
            });

        server.RegisterTool(
            u8"plugin_scaffold",
            u8"Scaffold the game's native code, as the editor's Add Native Code does: the game's plugin "
            u8"package, Plugins/<Game> (its name from the project's), with the `parts` asked for (Engine, "
            u8"Pipeline, Tooling, Editor; default Engine), each <Game>.<Kind>/ with its CMakeLists and a "
            u8"source whose Load finds the service its host hands that kind. A package already there gains "
            u8"the parts it lacks. Lists them in its Plugin.xml, enables the package in the project and "
            u8"saves. Refused when every part asked for is there, or Plugins/ holds another folder of that "
            u8"name; it never overwrites code. Build it with the editor's Build Project Plugins, then reopen "
            u8"or Reload Project Plugins. Returns the package's id, path and parts.",
            SchemaBuilder()
                .Arr(u8"parts", u8"string", u8"the parts to make: Engine, Pipeline, Tooling, Editor (default Engine)")
                .Build(),
            foundation::mcp::ToolAnnotations::Adjusts(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                Array<runtime::PluginPartKind> parts;
                if (args.Has(u8"parts"))
                {
                    const JsonValue given = args.Get(u8"parts");
                    for (i64 i = 0; i < given.Count(); ++i)
                    {
                        const Optional<runtime::PluginPartKind> kind =
                            runtime::ParsePluginPartKind(given.At(static_cast<usize>(i)).AsString().AsView());
                        if (!kind)
                        {
                            return Err(Format(u8"'{}' is not a part (Engine, Pipeline, Tooling or Editor)",
                                              given.At(static_cast<usize>(i)).AsString()));
                        }
                        parts.PushBack(*kind);
                    }
                }
                if (parts.IsEmpty())
                {
                    parts.PushBack(runtime::PluginPartKind::Engine);
                }
                editor::EditorProject& project = *s->project;
                const Status scaffolded =
                    editor::ScaffoldGamePackage(project, Span<const runtime::PluginPartKind>(parts.Data(), parts.Size()));
                if (scaffolded.Code() == ErrorCode::AlreadyExists)
                {
                    return Err(String(u8"every part asked for is there already, or Plugins/ holds another folder of "
                                      u8"the game package's name; nothing was touched"));
                }
                if (!scaffolded.IsOk())
                {
                    return Err(String(u8"the scaffold failed (log_read says why)"));
                }
                if (s->onSettingsChanged)
                {
                    s->onSettingsChanged();
                }
                const Optional<editor::GamePackage> package = editor::FindGamePackage(project);
                JsonValue out = JsonValue::MakeObject();
                if (package)
                {
                    out.Set(u8"id", JsonValue::MakeString(package->id));
                    out.Set(u8"path", JsonValue::MakeString(package->path));
                    JsonValue kinds = JsonValue::MakeArray();
                    for (runtime::PluginPartKind kind : package->parts)
                    {
                        kinds.Add(JsonValue::MakeString(String(runtime::PluginPartKindName(kind))));
                    }
                    out.Set(u8"parts", Move(kinds));
                }
                return out;
            });

        server.RegisterTool(
            u8"plugin_settings_set",
            u8"Change a loaded plugin's settings: `id` names it, `settings` holds the settings to "
            u8"change by name (only what is given changes), each taking what its kind takes in "
            u8"project_settings_set. Checked in full before anything changes, then written into the "
            u8"project's entry and saved to the manifest; the plugin sees the new values at once. "
            u8"Refused for a plugin that is not loaded here, has no settings, or whose saved settings "
            u8"did not read (those are kept as written). Returns the entry as plugin_list reports it.",
            SchemaBuilder()
                .Str(u8"id", u8"the plugin's id, as plugin_list names it", true)
                .Map(u8"settings", u8"the settings to change, by name", true)
                .Build(),
            foundation::mcp::ToolAnnotations::Adjusts(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                editor::EditorProject& project = *s->project;
                const String id(args.Get(u8"id").AsString());
                runtime::EnabledPlugin* entry = project.Settings().FindPlugin(id.AsView());
                if (entry == nullptr)
                {
                    return Err(Format(u8"the project enables no plugin '{}' (plugin_list names them)", id));
                }
                ISerializable* settings = detail::LoadedPluginSettings(*s, id.AsView());
                if (settings == nullptr)
                {
                    const engine::project::PluginStatus status =
                        engine::project::PluginStatusOf(*entry, s->plugins, project.Allocator());
                    return Err(status.state == engine::project::PluginState::Loaded
                                   ? Format(u8"plugin '{}' has no settings", id)
                                   : Format(u8"{}; its settings are kept as written", status.reason));
                }
                if (!s->plugins->SettingsRead(id.AsView()))
                {
                    return Err(Format(u8"plugin '{}': its saved settings did not read as its settings type, so "
                                      u8"they are kept as written (log_read says why)",
                                      id));
                }
                const Array<const PropertyInfo*> properties = detail::ReflectedFieldsOf(*settings->GetType());
                const Span<const PropertyInfo* const> fields(properties.Data(), properties.Size());
                const JsonValue given = args.Get(u8"settings");
                for (const String& key : given.Keys())
                {
                    bool known = false;
                    for (const PropertyInfo* field : properties)
                    {
                        known = known || detail::PropertyName(*field) == key.AsView();
                    }
                    if (!known)
                    {
                        String names;
                        for (const PropertyInfo* field : properties)
                        {
                            names += names.IsEmpty() ? u8"" : u8", ";
                            names += detail::PropertyName(*field);
                        }
                        return Err(Format(u8"plugin '{}' has no setting '{}' (its settings: {})", id, key,
                                          names.IsEmpty() ? StringView(u8"none") : names.AsView()));
                    }
                }
                Result<Array<detail::FieldChange>, String> checked = detail::CheckFields(given, fields, project);
                if (!checked.HasValue())
                {
                    return Err(Move(checked.Error()));
                }
                const Instance instance(settings, settings->GetType());
                detail::ApplyFields(instance, Span<const detail::FieldChange>(checked.Value().Data(),
                                                                              checked.Value().Size()));
                if (!runtime::WritePluginSettings(*settings, entry->settings, project.Allocator()).IsOk() ||
                    !project.SaveSettings().IsOk())
                {
                    return Err(Format(u8"plugin '{}': the settings changed but the manifest did not save "
                                      u8"(log_read says why)",
                                      id));
                }
                if (s->onSettingsChanged)
                {
                    s->onSettingsChanged();
                }
                return detail::PluginEntryJson(*s, *entry);
            });
    }
}

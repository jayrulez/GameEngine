// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :asset_uses partition
//
// asset_uses: the reverse dependency query, "what uses this asset". An agent should run it before
// any destructive change (delete, rename, move) to see every direct user and the kind of each
// edge before it breaks one.
//
// Edges come from the same sources the engine itself uses, computed live (never a cached graph):
//   - buildable assets: the builder's ScanDependencies (what the cook driver hashes), giving
//     `reads` (content consumed at cook time) and `references` (the product's runtime refs);
//   - scenes and prefabs: LoadScene over the full manager set (Engine.Composition) and a
//     factory-less ResourceManager whose unresolved set is the component Ref list (the same
//     recipe as the export reachability scanner), plus each parked prefab instance's id;
//   - project settings: the manifest's asset settings, found by reflection
//     (ForEachSettingAsset).
// It reports direct users only; run it again on a user to walk the chain outward.

module;
#include "Core/Prelude.h"

export module editor.mcp:asset_uses;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import pipeline.core;
import engine.composition;
import editor.project;
import engine.project; // ForEachSettingAsset (the settings' asset references, by reflection)
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;
namespace vfs = foundation::vfs;

namespace editor::mcp::detail
{
    inline bool ContainsGuid(const Array<Guid>& list, const Guid& id)
    {
        for (const Guid& g : list)
        {
            if (g == id)
            {
                return true;
            }
        }
        return false;
    }

    // One direct user of the queried asset: the using instance and every edge kind it holds.
    struct AssetUse
    {
        content::Instance* user = nullptr;
        String group; // slash-joined source-DB group path ("" at root)
        Array<String> edges;
    };

    // A scene or prefab instance's direct references (component resource Refs and parked
    // prefab-instance ids), as every exporting host scans them. Returns false when the stored
    // stream does not load (project_health counts those; asset_uses skips them).
    inline bool CollectSceneReferences(content::Instance& instance, content::ContentDatabase& db,
                                       Array<Guid>& resources, Array<Guid>& prefabs)
    {
        return engine::ScanSceneReferences(editor::EditorRootAllocator(), instance, db, resources,
                                           prefabs);
    }

    // Walk every source-DB instance (depth-first) and collect those with an edge to `target`.
    inline void CollectUses(content::Group* group, const String& path, const Guid& target,
                            content::ContentDatabase& db, pipeline::BuilderRegistry& builders,
                            vfs::IFileSystem& sourcesMount, Array<AssetUse>& out)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* inst : group->Instances())
        {
            if (inst->Id() == target)
            {
                continue; // self
            }
            AssetUse use;
            const bool isScene = inst->TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = inst->TypeName() == StringView(u8"PrefabDocument");
            if (isScene || isPrefab)
            {
                Array<Guid> resources;
                Array<Guid> prefabs;
                CollectSceneReferences(*inst, db, resources, prefabs);
                if (ContainsGuid(resources, target))
                {
                    use.edges.PushBack(String(u8"scene-resource"));
                }
                if (ContainsGuid(prefabs, target))
                {
                    use.edges.PushBack(String(u8"prefab-instance"));
                }
            }
            else if (pipeline::IAssetBuilder* builder = builders.FindByTypeName(inst->TypeName()))
            {
                RefPtr<ISerializable> object = inst->ReadObject();
                pipeline::Asset* asset = Cast<pipeline::Asset>(object.Get());
                if (asset != nullptr)
                {
                    pipeline::AssetBuildContext ctx{editor::EditorRootAllocator()};
                    ctx.sources = &sourcesMount;
                    ctx.source = inst;
                    ctx.db = &db;
                    pipeline::AssetDependencies deps;
                    builder->ScanDependencies(*asset, ctx, deps);
                    if (ContainsGuid(deps.reads, target))
                    {
                        use.edges.PushBack(String(u8"reads"));
                    }
                    if (ContainsGuid(deps.references, target))
                    {
                        use.edges.PushBack(String(u8"references"));
                    }
                }
            }
            if (!use.edges.IsEmpty())
            {
                use.user = inst;
                use.group = path;
                out.PushBack(Move(use));
            }
        }
        for (content::Group* sub : group->Groups())
        {
            const String childPath =
                path.IsEmpty() ? String(sub->Name()) : Format(u8"{}/{}", path.AsView(), sub->Name());
            CollectUses(sub, childPath, target, db, builders, sourcesMount, out);
        }
    }

    // Every source asset of `project` with a direct edge to `target`: what asset_uses reports, and
    // what asset_delete refuses over.
    inline Array<AssetUse> UsesOf(editor::EditorProject& project, pipeline::BuilderRegistry& builders,
                                  const Guid& target)
    {
        content::ContentDatabase& db = project.SourceDb();
        vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        Array<AssetUse> uses;
        CollectUses(db.RootGroup(), String(), target, db, builders, sourcesMount, uses);
        return uses;
    }

    // The project settings naming `target`, each by the setting's name, as project_info and
    // project_settings_set know it (a list setting is named once).
    inline Array<String> SettingsUsesOf(const engine::project::ProjectSettings& settings, const Guid& target)
    {
        Array<String> names;
        const PropertyInfo* last = nullptr;
        engine::project::ForEachSettingAsset(settings,
                                             [&](const PropertyInfo& property, const Guid& named)
                                             {
                                                 if (named == target && &property != last)
                                                 {
                                                     names.PushBack(String(StringView(
                                                         reinterpret_cast<const utf8char*>(property.name))));
                                                     last = &property;
                                                 }
                                             });
        return names;
    }
}

export namespace editor::mcp
{
    // Registers asset_uses against `server`. `builders` is the host's registry (populated once
    // from Pipeline::Registration) and must outlive the server.
    inline void RegisterAssetUsesTool(foundation::mcp::McpServer& server, ProjectSession& session,
                                      pipeline::BuilderRegistry& builders)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;
        pipeline::BuilderRegistry* bld = &builders;

        server.RegisterTool(
            u8"asset_uses",
            u8"REVERSE dependency query: every DIRECT user of an asset, with the edge kind - "
            u8"'reads' (an asset's cook consumes its content), 'references' (an asset's cooked "
            u8"product refers to it at runtime), 'scene-resource' (a scene/prefab component "
            u8"references it), 'prefab-instance' (a scene/prefab instantiates it), plus any "
            u8"project-settings fields pointing at it (default scene, startup script, ...). "
            u8"Computed live from the source database. Call this BEFORE deleting, renaming, or "
            u8"moving an asset; an empty result means nothing in the source database or project "
            u8"settings points at it. Direct users only - re-run on a user to walk the chain.",
            SchemaBuilder()
                .Str(u8"guid", u8"the asset guid (canonical 8-4-4-4-12 form)", true)
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, bld](const JsonValue& args) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String guidText = args.Get(u8"guid").AsString();
                Guid id;
                if (!Guid::TryParse(guidText.AsView(), id))
                {
                    return Err(Format(u8"invalid guid '{}'", guidText.AsView()));
                }
                content::ContentDatabase& db = s->project->SourceDb();
                content::Instance* target = db.GetInstance(id);
                if (target == nullptr)
                {
                    return Err(Format(u8"no asset with guid '{}' in the source database "
                                      u8"(asset_uses answers over source assets; use asset_list "
                                      u8"to find the right guid)",
                                      guidText.AsView()));
                }

                const Array<detail::AssetUse> uses = detail::UsesOf(*s->project, *bld, id);

                JsonValue usedBy = JsonValue::MakeArray();
                for (const detail::AssetUse& use : uses)
                {
                    JsonValue e = JsonValue::MakeObject();
                    e.Set(u8"guid", detail::GuidToJson(use.user->Id()));
                    e.Set(u8"name", JsonValue::MakeString(String(use.user->Name())));
                    e.Set(u8"type", JsonValue::MakeString(String(use.user->TypeName())));
                    if (!use.group.IsEmpty())
                    {
                        e.Set(u8"group", JsonValue::MakeString(use.group));
                    }
                    JsonValue edges = JsonValue::MakeArray();
                    for (const String& kind : use.edges)
                    {
                        edges.Add(JsonValue::MakeString(kind));
                    }
                    e.Set(u8"edges", Move(edges));
                    usedBy.Add(Move(e));
                }

                // The manifest's own references (a scene can be "in use" by the project itself),
                // each by the setting's name, as project_info and project_settings_set know it.
                JsonValue settingsUses = JsonValue::MakeArray();
                for (const String& name : detail::SettingsUsesOf(s->project->Settings(), id))
                {
                    settingsUses.Add(JsonValue::MakeString(name));
                }

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(target->Id()));
                out.Set(u8"name", JsonValue::MakeString(String(target->Name())));
                out.Set(u8"type", JsonValue::MakeString(String(target->TypeName())));
                out.Set(u8"useCount", JsonValue::MakeNumber(static_cast<f64>(uses.Size())));
                out.Set(u8"usedBy", Move(usedBy));
                out.Set(u8"projectSettingsUses", Move(settingsUses));
                return out;
            });
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :project_health partition
//
// project_health: one call that answers "is this project sound". It sweeps the whole source
// database with the same live machinery the other tools use:
//   - dangling references: every forward edge (the builders' ScanDependencies reads and
//     references; scene and prefab component Refs and prefab instances through the
//     full-manager scan; the ProjectSettings guid fields) whose target no longer exists in the
//     source database;
//   - broken sources: buildable assets whose envelope no longer deserializes (a stale data
//     version or an unknown type) and scenes or prefabs whose stored stream does not load;
//   - cook state: the incremental plan's dirty and up-to-date split, orphaned products, sources
//     with no registered builder, and cook records whose last build failed.
// `sound` is the verdict: true only when nothing is broken (dirty is a normal workflow state and
// never makes a project unsound; cook it with asset_cook).

module;
#include "Core/Prelude.h"

export module editor.mcp:project_health;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import pipeline.core;
import pipeline.cook;
import audio.pipeline; // SoundCueAsset (empty-cue audit)
import editor.project;
import engine.project; // ForEachSettingAsset (the settings' asset references, by reflection)
import :session;
import :asset_uses; // shares the edge machinery (CollectSceneReferences, ContainsGuid)

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;
namespace vfs = foundation::vfs;

namespace editor::mcp::detail
{
    // One broken forward edge: `from` points at `to`, which is not in the source database.
    struct DanglingRef
    {
        content::Instance* from = nullptr;
        Guid to;
        StringView edge; // "reads" | "references" | "scene-resource" | "prefab-instance"
    };

    struct HealthSweep
    {
        Array<DanglingRef> dangling;
        Array<content::Instance*> undeserializable; // buildable, but ReadObject fails
        Array<content::Instance*> unreadableScenes; // scene/prefab stream does not load
        Array<content::Instance*> emptyCues; // SoundCueAsset with no clip in any slot (a warning)
        usize unbuildable = 0; // no registered builder (scenes and prefabs excluded: they are staged)
    };

    inline void AppendDangling(content::Instance* from, const Array<Guid>& targets,
                               StringView edge, content::ContentDatabase& db, HealthSweep& out)
    {
        for (const Guid& id : targets)
        {
            if (!id.IsNil() && db.GetInstance(id) == nullptr)
            {
                out.dangling.PushBack(DanglingRef{from, id, edge});
            }
        }
    }

    // Depth-first sweep of every source instance's forward edges and deserializability.
    inline void SweepHealth(content::Group* group, content::ContentDatabase& db,
                            pipeline::BuilderRegistry& builders, vfs::IFileSystem& sourcesMount,
                            HealthSweep& out)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* inst : group->Instances())
        {
            const bool isScene = inst->TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = inst->TypeName() == StringView(u8"PrefabDocument");
            if (isScene || isPrefab)
            {
                Array<Guid> resources;
                Array<Guid> prefabs;
                if (!CollectSceneReferences(*inst, db, resources, prefabs))
                {
                    out.unreadableScenes.PushBack(inst);
                    continue;
                }
                AppendDangling(inst, resources, u8"scene-resource", db, out);
                AppendDangling(inst, prefabs, u8"prefab-instance", db, out);
                continue;
            }
            pipeline::IAssetBuilder* builder = builders.FindByTypeName(inst->TypeName());
            if (builder == nullptr)
            {
                ++out.unbuildable;
                continue;
            }
            RefPtr<ISerializable> object = inst->ReadObject();
            pipeline::Asset* asset = Cast<pipeline::Asset>(object.Get());
            if (asset == nullptr)
            {
                out.undeserializable.PushBack(inst);
                continue;
            }
            // Empty-cue audit (a warning, not a break): a cue with no clip in any slot cooks to a
            // valid silent product, but is almost always a clip someone forgot to assign.
            if (const auto* cue = Cast<pipeline::SoundCueAsset>(asset))
            {
                bool anyClip = false;
                for (usize i = 0; i < pipeline::kSoundCueSlotCount; ++i)
                {
                    if (!cue->clipIds[i].IsNil())
                    {
                        anyClip = true;
                        break;
                    }
                }
                if (!anyClip)
                {
                    out.emptyCues.PushBack(inst);
                }
            }
            pipeline::AssetBuildContext ctx{editor::EditorRootAllocator()};
            ctx.sources = &sourcesMount;
            ctx.source = inst;
            ctx.db = &db;
            pipeline::AssetDependencies deps;
            builder->ScanDependencies(*asset, ctx, deps);
            AppendDangling(inst, deps.reads, u8"reads", db, out);
            AppendDangling(inst, deps.references, u8"references", db, out);
        }
        for (content::Group* sub : group->Groups())
        {
            SweepHealth(sub, db, builders, sourcesMount, out);
        }
    }

    inline JsonValue InstanceToJson(content::Instance* inst)
    {
        JsonValue e = JsonValue::MakeObject();
        e.Set(u8"guid", GuidToJson(inst->Id()));
        e.Set(u8"name", JsonValue::MakeString(String(inst->Name())));
        e.Set(u8"type", JsonValue::MakeString(String(inst->TypeName())));
        return e;
    }
}

export namespace editor::mcp
{
    // Registers project_health against `server`. `builders` is the host's registry (populated
    // once from Pipeline::Registration) and must outlive the server.
    inline void RegisterProjectHealthTool(foundation::mcp::McpServer& server,
                                          ProjectSession& session,
                                          pipeline::BuilderRegistry& builders)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;
        pipeline::BuilderRegistry* bld = &builders;

        server.RegisterTool(
            u8"project_health",
            u8"Full project soundness sweep: dangling references (any asset/scene/settings edge "
            u8"whose target is missing from the source database), sources that no longer "
            u8"deserialize, scenes/prefabs that no longer load, plus the cook state (dirty vs "
            u8"up-to-date, orphaned products, sources with no builder, last-cook failures). Also "
            u8"warns (without flipping sound) on emptyCues: sound cues with no clip assigned. "
            u8"Returns sound=true only when nothing is broken; a dirty count alone is normal - "
            u8"run asset_cook to clear it. Call after destructive changes (delete/rename) or "
            u8"before an export to catch breakage early; fix dangling refs by re-pointing or "
            u8"restoring the missing asset (asset_uses on the missing guid's users shows impact).",
            SchemaBuilder().Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [s, bld](const JsonValue& /*args*/) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                content::ContentDatabase& db = s->project->SourceDb();
                vfs::NativeFileSystem sourcesMount(s->project->SourcesRoot().AsView(), editor::EditorRootAllocator());
                // The reference and deserializability sweep.
                detail::HealthSweep sweep;
                detail::SweepHealth(db.RootGroup(), db, *bld, sourcesMount, sweep);

                // Settings edges: manifest fields pointing at instances that no longer exist, each by
                // the setting's name, as project_info and project_settings_set know it.
                JsonValue settingsDangling = JsonValue::MakeArray();
                const PropertyInfo* last = nullptr;
                engine::project::ForEachSettingAsset(
                    s->project->Settings(),
                    [&](const PropertyInfo& property, const Guid& named)
                    {
                        if (db.GetInstance(named) == nullptr && &property != last) // a list is named once
                        {
                            settingsDangling.Add(JsonValue::MakeString(
                                String(StringView(reinterpret_cast<const utf8char*>(property.name)))));
                            last = &property;
                        }
                    });

                // Cook state: plan only (no build), and the persisted records' failure flags.
                const String sourcesRoot = s->project->SourcesRoot();
                const String cacheRoot = s->project->CacheRoot();
                vfs::NativeFileSystem planSources(sourcesRoot.AsView(), editor::EditorRootAllocator());
                vfs::NativeFileSystem cacheMount(cacheRoot.AsView(), editor::EditorRootAllocator());
                pipeline::CookDriver driver(editor::EditorRootAllocator(), db,
                                            s->project->CookedDb(), *bld, &planSources,
                                            &cacheMount, nullptr);
                pipeline::CookPlan plan = driver.Plan(/*force=*/false);
                usize failedCooks = 0;
                driver.Db().ForEach(
                    [&failedCooks](const pipeline::CookRecord& record)
                    {
                        if (record.failed)
                        {
                            ++failedCooks;
                        }
                    });

                JsonValue dangling = JsonValue::MakeArray();
                for (const detail::DanglingRef& d : sweep.dangling)
                {
                    JsonValue e = JsonValue::MakeObject();
                    e.Set(u8"from", detail::InstanceToJson(d.from));
                    e.Set(u8"to", detail::GuidToJson(d.to));
                    e.Set(u8"edge", JsonValue::MakeString(String(d.edge)));
                    dangling.Add(Move(e));
                }
                JsonValue undeserializable = JsonValue::MakeArray();
                for (content::Instance* inst : sweep.undeserializable)
                {
                    undeserializable.Add(detail::InstanceToJson(inst));
                }
                JsonValue unreadableScenes = JsonValue::MakeArray();
                for (content::Instance* inst : sweep.unreadableScenes)
                {
                    unreadableScenes.Add(detail::InstanceToJson(inst));
                }
                // Warnings do not flip `sound` (an empty cue is a valid, buildable draft).
                JsonValue emptyCues = JsonValue::MakeArray();
                for (content::Instance* inst : sweep.emptyCues)
                {
                    emptyCues.Add(detail::InstanceToJson(inst));
                }

                const bool sound = sweep.dangling.IsEmpty() && sweep.undeserializable.IsEmpty() &&
                                   sweep.unreadableScenes.IsEmpty() &&
                                   settingsDangling.Count() == 0 && plan.orphans.IsEmpty() &&
                                   failedCooks == 0;

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"sound", JsonValue::MakeBool(sound));
                out.Set(u8"danglingRefs", Move(dangling));
                out.Set(u8"projectSettingsDangling", Move(settingsDangling));
                out.Set(u8"undeserializable", Move(undeserializable));
                out.Set(u8"unreadableScenes", Move(unreadableScenes));
                out.Set(u8"emptyCues", Move(emptyCues)); // warning: cues with no clip assigned
                out.Set(u8"dirty", JsonValue::MakeNumber(static_cast<f64>(plan.dirty.Size())));
                out.Set(u8"upToDate", JsonValue::MakeNumber(static_cast<f64>(plan.upToDate)));
                out.Set(u8"orphans", JsonValue::MakeNumber(static_cast<f64>(plan.orphans.Size())));
                out.Set(u8"unbuildable",
                        JsonValue::MakeNumber(static_cast<f64>(sweep.unbuildable)));
                out.Set(u8"failedCooks", JsonValue::MakeNumber(static_cast<f64>(failedCooks)));
                return out;
            });
    }
}

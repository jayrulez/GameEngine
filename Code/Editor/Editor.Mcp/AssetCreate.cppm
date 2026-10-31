// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :asset_create partition
//
// asset_creators / asset_create: every asset kind the editor's File > New makes, for an agent to
// make. The creators are the pipeline's (pipeline::RegisterAllCreators), and the host's registry
// outlives the server. The work runs through the host's IProjectOperations, so in the editor a
// creation also requests the cook and sets a first scene as the default.

module;
#include "Core/Prelude.h"

export module editor.mcp:asset_create;

import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;
import :session;
import :operations;

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace editor::mcp
{
    inline void RegisterAssetCreateTools(foundation::mcp::McpServer& server,
                                         ProjectSession& session,
                                         const pipeline::AssetCreatorRegistry& creators,
                                         IProjectOperations& operations)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolOutcome;
        ProjectSession* s = &session;
        const pipeline::AssetCreatorRegistry* registry = &creators;
        IProjectOperations* ops = &operations;

        server.RegisterTool(
            u8"asset_creators",
            u8"Every kind of asset asset_create can make (what the editor's File > New offers): "
            u8"each creator's `label`, its menu `category`, the `type` it creates, and the "
            u8"`defaultGroup` its assets land in when no group is given. Imported kinds "
            u8"(textures, models, audio clips, fonts) come from asset_import instead.",
            SchemaBuilder().Build(), foundation::mcp::ToolAnnotations::ReadOnly(),
            [registry](const JsonValue&) -> ToolOutcome
            {
                JsonValue items = JsonValue::MakeArray();
                for (const pipeline::AssetCreator& creator : registry->All())
                {
                    JsonValue item = JsonValue::MakeObject();
                    item.Set(u8"label", JsonValue::MakeString(creator.label));
                    item.Set(u8"category", JsonValue::MakeString(creator.category));
                    item.Set(u8"type", JsonValue::MakeString(String(creator.TypeName())));
                    item.Set(u8"defaultGroup", JsonValue::MakeString(creator.defaultGroup));
                    items.Add(Move(item));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(registry->Count())));
                out.Set(u8"creators", Move(items));
                return out;
            });

        server.RegisterTool(
            u8"asset_create",
            u8"Create a new asset the way the editor's File > New does: a fresh instance seeded "
            u8"with its defaults (an input map with the default sets, a PBR material from the "
            u8"preset, a scene with a sun, a script class from its tier's starter). Name the "
            u8"creator by `creator` label or by `type`. Returns {guid, name, type, path, "
            u8"creator}; read the asset with asset_info, edit it with the scene tools or its "
            u8"page, and call asset_cook before a runtime needs it (the editor host requests the "
            u8"cook itself).",
            SchemaBuilder()
                .Str(u8"creator", u8"the creator's label (asset_creators lists them), any case")
                .Str(u8"type", u8"or the asset type's name, when one creator makes it")
                .Str(u8"name", u8"the asset's name, exact: a name already taken in the group is "
                               u8"refused (default: the creator's own, made unique)")
                .Str(u8"group", u8"source-DB group path, slash-joined, made when missing "
                                u8"(default: the creator's defaultGroup)")
                .Build(),
            foundation::mcp::ToolAnnotations::Creates(),
            [s, registry, ops](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String label = args.Get(u8"creator").AsString();
                const String type = args.Get(u8"type").AsString();
                CreateRequest request;
                if (!label.IsEmpty())
                {
                    request.creator = registry->FindByLabel(label.AsView());
                    if (request.creator == nullptr)
                    {
                        return Err(Format(u8"no creator labelled '{}'; asset_creators lists them",
                                          label.AsView()));
                    }
                }
                else if (!type.IsEmpty())
                {
                    request.creator = registry->FindByType(type.AsView());
                    if (request.creator == nullptr)
                    {
                        return Err(Format(u8"no single creator makes '{}' (none, or several: pass "
                                          u8"`creator` by label); asset_creators lists them",
                                          type.AsView()));
                    }
                }
                else
                {
                    return Err(String(u8"pass `creator` (a label) or `type`"));
                }
                request.groupPath = args.Get(u8"group").AsString();
                request.name = args.Get(u8"name").AsString();
                OperationStep<CreateOutcome> step = ops->Create(call, request);
                if (!step.HasValue())
                {
                    return Err(Move(step.Error()));
                }
                if (!step.Value().HasValue())
                {
                    return ToolOutcome::NotFinished(); // held while a cook reads the databases
                }
                const CreateOutcome& done = step.Value().Value();
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(done.guid));
                out.Set(u8"name", JsonValue::MakeString(done.name));
                out.Set(u8"type", JsonValue::MakeString(done.type));
                out.Set(u8"path", JsonValue::MakeString(done.path));
                out.Set(u8"creator", JsonValue::MakeString(request.creator->label));
                return out;
            });
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :asset_delete partition.
//
// asset_delete: the Assets browser's Delete for an agent, so an agent replacing an asset can
// remove the old one. It is refused while anything still uses the asset, by the same scan
// asset_uses runs, unless `force` says the breakage is intended. The host does the rest through
// IProjectOperations::Delete (the editor closes the asset's page and waits out a cook).

module;
#include "Core/Prelude.h"

export module editor.mcp:asset_delete;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import pipeline.core;
import editor.project;
import :session;
import :operations;
import :asset_uses;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

export namespace editor::mcp
{
    inline void RegisterAssetDeleteTool(foundation::mcp::McpServer& server, ProjectSession& session,
                                        pipeline::BuilderRegistry& builders, IProjectOperations& operations)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolOutcome;
        ProjectSession* s = &session;
        pipeline::BuilderRegistry* bld = &builders;
        IProjectOperations* ops = &operations;
        server.RegisterTool(
            u8"asset_delete",
            u8"Delete a source asset, as the editor's Assets browser does: its stored object and data "
            u8"files go, and the next cook sweeps its cooked product. The original file an import "
            u8"copied under Sources/ is NOT removed - it is an ordinary file. Refused while other "
            u8"assets or the project settings use it (the refusal names them, as asset_uses would); "
            u8"`force` deletes anyway, leaving those references dangling. In the editor its open "
            u8"page closes first, and a running cook is waited out. Returns the deleted asset's "
            u8"identity.",
            SchemaBuilder()
                .Str(u8"guid", u8"the source asset to delete", true)
                .Boolean(u8"force", u8"delete even though something still uses it (default false)")
                .Build(),
            foundation::mcp::ToolAnnotations::Deletes(),
            [s, bld, ops](foundation::mcp::ToolCall& call, const JsonValue& args) -> ToolOutcome
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String guidText = args.Get(u8"guid").AsString();
                Guid id;
                if (!Guid::TryParse(guidText.AsView(), id))
                {
                    return Err(Format(u8"'{}' is not a valid guid", guidText.AsView()));
                }
                content::Instance* instance = s->project->SourceDb().GetInstance(id);
                if (instance == nullptr)
                {
                    // A re-entered call (waiting out a cook) whose asset is already gone: done.
                    if (call.isReentry)
                    {
                        JsonValue gone = JsonValue::MakeObject();
                        gone.Set(u8"guid", detail::GuidToJson(id));
                        gone.Set(u8"deleted", JsonValue::MakeBool(true));
                        return gone;
                    }
                    return Err(Format(u8"no asset with guid {} in the open project", guidText.AsView()));
                }

                // Checked once, on the first entry; a call waiting out a cook does not rescan.
                if (!call.isReentry && !args.Get(u8"force").AsBool())
                {
                    const Array<detail::AssetUse> users = detail::UsesOf(*s->project, *bld, id);
                    const Array<String> settings = detail::SettingsUsesOf(s->project->Settings(), id);
                    if (!users.IsEmpty() || !settings.IsEmpty())
                    {
                        String refusal = Format(u8"refused - '{}' is still used", instance->Name());
                        if (!users.IsEmpty())
                        {
                            refusal += u8" by ";
                            for (usize i = 0; i < users.Size(); ++i)
                            {
                                const String name = users[i].group.IsEmpty()
                                                        ? String(users[i].user->Name())
                                                        : Format(u8"{}/{}", users[i].group.AsView(),
                                                                 users[i].user->Name());
                                refusal += Format(u8"{}'{}'", i > 0 ? StringView(u8", ") : StringView(),
                                                  name.AsView())
                                               .AsView();
                            }
                        }
                        if (!settings.IsEmpty())
                        {
                            refusal += users.IsEmpty() ? u8" by the project settings: " : u8"; and by the project settings: ";
                            for (usize i = 0; i < settings.Size(); ++i)
                            {
                                refusal += i > 0 ? u8", " : u8"";
                                refusal += settings[i].AsView();
                            }
                        }
                        refusal += u8". Change those first, or pass force to delete anyway. Nothing was deleted.";
                        return Err(Move(refusal));
                    }
                }

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(instance->Id()));
                out.Set(u8"name", JsonValue::MakeString(String(instance->Name())));
                out.Set(u8"type", JsonValue::MakeString(String(instance->TypeName())));
                OperationStep<bool> step = ops->Delete(call, id);
                if (!step.HasValue())
                {
                    return Err(Move(step.Error()));
                }
                if (!step.Value().HasValue())
                {
                    return ToolOutcome::NotFinished(); // held while a cook reads the databases
                }
                out.Set(u8"deleted", JsonValue::MakeBool(true));
                return out;
            });
    }
}

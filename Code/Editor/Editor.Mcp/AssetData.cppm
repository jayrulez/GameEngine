// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :asset_data partition
//
// asset_data_read / asset_data_write: any asset's stored object as the text its envelope holds,
// for the data assets no other tool edits (an input map's actions and bindings, a material's
// parameters, a physics material, a sound cue, a bus layout). The write reads the agent's text
// the way a load reads the stored envelope (Instance::ReadObjectFrom), so it accepts exactly what
// the engine can load; then it stores the object and announces the write, as scene_write does.

module;
#include "Core/Prelude.h"

export module editor.mcp:asset_data;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

namespace editor::mcp::detail
{
    /// The asset a call names by `guid` in the open project; the reason when there is none.
    inline Result<content::Instance*, String> ResolveDataAsset(const ProjectSession& session,
                                                               const JsonValue& args)
    {
        if (session.project == nullptr)
        {
            return Err(String(u8"no project is open (call project_open first)"));
        }
        const String text = args.Get(u8"guid").AsString();
        Guid id;
        if (!Guid::TryParse(text.AsView(), id))
        {
            return Err(Format(u8"'{}' is not a valid guid", text.AsView()));
        }
        content::Instance* instance = session.project->SourceDb().GetInstance(id);
        if (instance == nullptr)
        {
            return Err(Format(u8"no asset with guid {} in the open project", text.AsView()));
        }
        return instance;
    }

    inline JsonValue DataAssetIdentity(const content::Instance& instance)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"guid", GuidToJson(instance.Id()));
        out.Set(u8"name", JsonValue::MakeString(String(instance.Name())));
        out.Set(u8"type", JsonValue::MakeString(String(instance.TypeName())));
        return out;
    }
}

export namespace editor::mcp
{
    inline void RegisterAssetDataTools(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"asset_data_read",
            u8"An asset's stored object as its envelope's XML: its guid, its typeNamespace and "
            u8"typeName, the dataVersions and the `payload` holding every field. Edit the payload "
            u8"and hand the whole text to asset_data_write. Enum fields are stored as numbers: "
            u8"type_info on the field's type names the cases. A scene's or prefab's content is not "
            u8"here (scene_read and prefab_read have it), nor is an imported asset's bulk data.",
            SchemaBuilder().Str(u8"guid", u8"the asset's guid (asset_list)", true).Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [s](const JsonValue& args) -> ToolResult
            {
                Result<content::Instance*, String> resolved = detail::ResolveDataAsset(*s, args);
                if (!resolved.HasValue())
                {
                    return Err(Move(resolved.Error()));
                }
                content::Instance& instance = *resolved.Value();
                UniquePtr<IStream> envelope = instance.OpenEnvelope();
                if (!envelope)
                {
                    return Err(Format(u8"'{}' has no stored envelope", instance.Name()));
                }
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(envelope->Size()));
                if (envelope->Read(bytes.Data(), bytes.Size()) != bytes.Size())
                {
                    return Err(Format(u8"could not read '{}''s envelope", instance.Name()));
                }
                JsonValue out = detail::DataAssetIdentity(instance);
                out.Set(u8"xml", JsonValue::MakeString(String(StringView(
                                     reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()))));
                return out;
            });

        server.RegisterTool(
            u8"asset_data_write",
            u8"Replace an asset's stored object with an edited envelope from asset_data_read. The "
            u8"text must name the same guid and type and carry this build's dataVersions, and its "
            u8"payload must read as that type the way a load reads it; anything else is refused "
            u8"and nothing changes. The object is stored normalized (every field, in its order), "
            u8"the editor told of the change (an open page reloads when clean), and a cook picks "
            u8"it up: asset_cook before a runtime needs it.",
            SchemaBuilder()
                .Str(u8"guid", u8"the asset's guid", true)
                .Str(u8"xml", u8"the whole envelope, as asset_data_read gave it, with the payload "
                              u8"edited",
                     true)
                .Build(),
            foundation::mcp::ToolAnnotations::Overwrites(),
            [s](const JsonValue& args) -> ToolResult
            {
                Result<content::Instance*, String> resolved = detail::ResolveDataAsset(*s, args);
                if (!resolved.HasValue())
                {
                    return Err(Move(resolved.Error()));
                }
                content::Instance& instance = *resolved.Value();
                const String xml = args.Get(u8"xml").AsString();
                MemoryStream stream;
                (void)stream.Write(xml.CStr(), xml.Size());
                (void)stream.Seek(0, SeekOrigin::Begin);
                Result<RefPtr<ISerializable>, String> object = instance.ReadObjectFrom(stream);
                if (!object.HasValue())
                {
                    return Err(Format(u8"refused - the text does not load as '{}': {}. Nothing "
                                      u8"was written.",
                                      instance.Name(), object.Error().AsView()));
                }
                if (!instance.WriteObject(*object.Value()).IsOk())
                {
                    return Err(Format(u8"could not write '{}'", instance.Name()));
                }
                if (s->onAssetWritten)
                {
                    s->onAssetWritten(instance.Id());
                }
                JsonValue out = detail::DataAssetIdentity(instance);
                out.Set(u8"written", JsonValue::MakeBool(true));
                return out;
            });
    }
}

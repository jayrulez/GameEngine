// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :resources partition
//
// The open project's scene and prefab XML sources as MCP resources: `project://scene/<guid>` and
// `project://prefab/<guid>`, listed live from the source database (a ResourceProvider rather than
// static registrations, because scenes appear and disappear as the agent or the editor writes
// them) and read as the verbatim stored text. They are read-only context; mutation stays with
// scene_write and its validate-first rule.

module;
#include "Core/Prelude.h"

export module editor.mcp:resources;

import foundation.core;
import foundation.content;
import foundation.mcp;
import editor.project;
import :session;

using namespace foundation::core;
namespace content = foundation::content;

namespace editor::mcp::detail
{
    inline constexpr StringView kSceneUriPrefix = u8"project://scene/";
    inline constexpr StringView kPrefabUriPrefix = u8"project://prefab/";

    inline String DocumentUri(const content::Instance& instance)
    {
        utf8char guid[37];
        instance.Id().ToChars(guid);
        const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
        return Format(u8"{}{}", isScene ? kSceneUriPrefix : kPrefabUriPrefix,
                      StringView(guid, 36));
    }

    // Append every scene/prefab document under `group` as a resource descriptor.
    inline void ListSceneResources(content::Group* group, const String& path,
                                   Array<foundation::mcp::Resource>& out)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* inst : group->Instances())
        {
            const bool isScene = inst->TypeName() == StringView(u8"SceneDocument");
            const bool isPrefab = inst->TypeName() == StringView(u8"PrefabDocument");
            if (!isScene && !isPrefab)
            {
                continue;
            }
            foundation::mcp::Resource r;
            r.uri = DocumentUri(*inst);
            r.name = path.IsEmpty() ? String(inst->Name())
                                    : Format(u8"{}/{}", path.AsView(), inst->Name());
            r.mimeType = String(u8"application/xml");
            r.description = Format(u8"{} source XML (read-only; author changes via {})",
                                   isScene ? StringView(u8"scene") : StringView(u8"prefab"),
                                   isScene ? StringView(u8"scene_write")
                                           : StringView(u8"prefab_write"));
            out.PushBack(Move(r));
        }
        for (content::Group* sub : group->Groups())
        {
            const String childPath =
                path.IsEmpty() ? String(sub->Name()) : Format(u8"{}/{}", path.AsView(), sub->Name());
            ListSceneResources(sub, childPath, out);
        }
    }
}

export namespace editor::mcp
{
    // Registers the open project's scene/prefab XML sources as a dynamic resource set.
    inline void RegisterProjectResources(foundation::mcp::McpServer& server,
                                         ProjectSession& session)
    {
        ProjectSession* s = &session;
        foundation::mcp::ResourceProvider provider;
        provider.list = [s](Array<foundation::mcp::Resource>& out)
        {
            if (!s->project)
            {
                return; // no project open: the set is empty, not an error
            }
            detail::ListSceneResources(s->project->SourceDb().RootGroup(), String(), out);
        };
        provider.read = [s](StringView uri) -> Optional<Result<String, String>>
        {
            StringView guidText;
            if (uri.StartsWith(detail::kSceneUriPrefix))
            {
                guidText = uri.SubStr(detail::kSceneUriPrefix.Size(),
                                      uri.Size() - detail::kSceneUriPrefix.Size());
            }
            else if (uri.StartsWith(detail::kPrefabUriPrefix))
            {
                guidText = uri.SubStr(detail::kPrefabUriPrefix.Size(),
                                      uri.Size() - detail::kPrefabUriPrefix.Size());
            }
            else
            {
                return {}; // not ours: the server tries the next provider
            }
            if (!s->project)
            {
                return Result<String, String>(
                    Err(String(u8"no project is open - call project_open first")));
            }
            Guid id;
            if (!Guid::TryParse(guidText, id))
            {
                return Result<String, String>(Err(Format(u8"invalid guid in uri '{}'", uri)));
            }
            content::Instance* inst = s->project->SourceDb().GetInstance(id);
            if (inst == nullptr)
            {
                return Result<String, String>(
                    Err(Format(u8"no scene/prefab with guid '{}' (see resources/list)", guidText)));
            }
            UniquePtr<IStream> stream = inst->ReadData(u8"scene");
            if (stream.Get() == nullptr)
            {
                return Result<String, String>(
                    Err(Format(u8"'{}' has no stored scene stream", inst->Name())));
            }
            const i64 size = stream->Size();
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(size));
            if (stream->Read(bytes.Data(), bytes.Size()) != static_cast<u64>(size))
            {
                return Result<String, String>(
                    Err(Format(u8"could not read '{}'", inst->Name())));
            }
            return Result<String, String>(String(
                StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size())));
        };
        server.RegisterResourceProvider(Move(provider));
    }
}

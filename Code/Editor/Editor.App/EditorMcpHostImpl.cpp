// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - EditorMcpHost implementation: the tool composition, host_info's editor state,
// and Start (binding the port and writing the token file).
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.app;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.mcp.http;
import foundation.vfs;
import pipeline.core;
import editor.core;
import editor.mcp;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::app
{
    EditorMcpHost::EditorMcpHost(IAllocator& allocator, editor::EditorContext& context,
                                 editor::mcp::ProjectSession& session,
                                 editor::EditorLogBuffer& logBuffer,
                                 pipeline::BuilderRegistry& builders,
                                 pipeline::ImporterRegistry& importers,
                                 const pipeline::AssetCreatorRegistry& creators,
                                 const editor::mcp::EngineToolPaths& paths,
                                 editor::mcp::IProjectOperations& operations, String buildStamp)
        : m_allocator(&allocator), m_http(allocator, m_server), m_session(&session)
    {
        // Distinct from the stdio host's "engine-mcp": an agent talking to both tells them apart.
        m_server.SetServerInfo(u8"engine-editor-mcp", u8"0.1.0");
        editor::mcp::RegisterEngineTools(m_server, *m_session, builders, importers, creators, logBuffer,
                                         paths,
                                         operations);
        context.ApplyMcpToolContributions(m_server); // the domains' live tools
        // An agent's write over a source asset is a change made outside its page, like an
        // apply-to-prefab: the open pages editing it are notified and refresh by their own rule (a
        // clean page reloads, a dirty one warns and keeps its edits).
        editor::EditorContext* ctx = &context;
        m_session->onAssetWritten = [ctx](const Guid& assetId)
        { (void)ctx->NotifyAssetExternallyModified(assetId); };
        // An agent's settings change reapplies what depends on the settings, as the dialog's Save does.
        m_session->onSettingsChanged = [ctx]() { ctx->NotifyProjectSettingsChanged(); };
        foundation::mcp::RegisterHostInfoTool(
            m_server, Move(buildStamp),
            Function<JsonValue()>{[this]()
                                  {
                                      JsonValue host = JsonValue::MakeObject();
                                      host.Set(u8"kind", JsonValue::MakeString(u8"editor"));
                                      host.Set(u8"projectOpen", JsonValue::MakeBool(true));
                                      host.Set(u8"projectName",
                                               JsonValue::MakeString(
                                                   String(m_session->project->Name())));
                                      host.Set(u8"projectDirectory",
                                               JsonValue::MakeString(
                                                   String(m_session->project->Directory())));
                                      return host;
                                  }});
        m_server.SetToolObserver(
            [this](StringView tool, bool isError)
            {
                if (OnToolFinished)
                {
                    OnToolFinished(tool, isError);
                }
            });
    }

    bool EditorMcpHost::Start(const EditorMcpHostConfig& config)
    {
        foundation::mcp::McpHttpConfig http;
        http.port = config.port;
        http.token = config.token;
        if (!m_http.Start(http))
        {
            return false;
        }
        if (!config.tokenFileDirectory.IsEmpty())
        {
            // The same trust domain as the settings file next to it: user-readable, so a local
            // agent reads the secret instead of the user pasting it.
            (void)CreateDirectory(config.tokenFileDirectory.AsView());
            foundation::vfs::NativeFileSystem fs(config.tokenFileDirectory.AsView(), *m_allocator);
            const Status saved = fs.AsWritable()->Save(
                kTokenFileName, Span<const byte>(reinterpret_cast<const byte*>(config.token.Data()),
                                                 config.token.Size()));
            if (!saved.IsOk())
            {
                LOG_WARNING(u8"Editor", u8"MCP: the token file could not be written under '{}' - "
                                     u8"agents must take the token from Preferences",
                         config.tokenFileDirectory.AsView());
            }
        }
        return true;
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.Mcp - headless MCP stdio server.
//
// Speaks newline-delimited JSON-RPC over stdin/stdout so an agent (e.g. Claude Code, one command
// line) gets the engine's introspection surface with the editor CLOSED. STDOUT IS THE WIRE - every
// engine log is routed to stderr instead (a stray stdout print corrupts the protocol stream).
//
// v1 surface: the reflection tools (type_list / type_info) over the Core + JSON reflected types.
// The tool surface grows as modules contribute their own (project/pipeline/scene tools next).

#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.vfs; // ResolveDataRoot (--data-root / the Data/.dataroot walk)
import foundation.json;
import foundation.content; // the after-import seam's primary
import foundation.mcp;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import editor.project; // EditorLogBuffer (the host's log capture)
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;

extern "C" const char* BuildStamp();

namespace
{
    // Routes ALL engine log lines to stderr, keeping stdout clean for the JSON-RPC wire.
    class StderrSink final : public ILogSink
    {
    public:
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            StringBuilder line;
            line.Append(u8'[');
            line.Append(LogLevelName(level));
            line.Append(u8"] ");
            line.Append(category);
            line.Append(u8": ");
            line.Append(message);
            line.Append(u8'\n');
            const String text = line.Take();
            std::fwrite(text.Data(), 1, text.Size(), stderr);
            std::fflush(stderr);
        }
    };

}

int main(int argc, char** argv)
{
    // The log capture FIRST (before anything logs), so log_read sees the whole run; stderr
    // mirror second - stdout is the protocol stream.
    static editor::EditorLogBuffer logBuffer{foundation::core::DefaultAllocator()};
    GlobalLogger().AddSink(&logBuffer);
    static StderrSink stderrSink;
    GlobalLogger().AddSink(&stderrSink);

    // Populate the reflection registry with the surface we can introspect headlessly, plus the
    // full pipeline type set (so type_list sees every asset/product type and asset_cook can build).
    RegisterCoreTypes();
    foundation::json::RegisterJsonTypes();
    pipeline::RegisterPipelineTypes();
    // The COMPLETE engine script surface (every subsystem facade), so script_api reports the whole
    // bound API, not just core - metadata only, no subsystem instantiated (headless). Every enabled
    // backend (angelscript | luau) was already registered by RegisterPipelineTypes above (the
    // composition root registers both, each OPTION_HAS-guarded), so script_api spans them all.
    engine::RegisterAllScriptFacades();
    // Component reflection for every engine domain (data-version gates) - scene_validate parses
    // component payloads through the full manager set (Engine.Composition), which needs the
    // reflected field metadata registered before any scene stream deserializes.
    engine::RegisterAllSceneComponentReflection();

    // The host's builder + importer registries (from the pipeline composition root); populated
    // once, they outlive the server and back asset_cook / asset_import.
    pipeline::BuilderRegistry builders{foundation::core::DefaultAllocator()};
    pipeline::ImporterRegistry importers{foundation::core::DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders);
    pipeline::RegisterAllImporters(importers);
    // File > New's creators, every pipeline domain's (asset_creators / asset_create).
    pipeline::AssetCreatorRegistry creators{foundation::core::DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);

    McpServer server;
    server.SetServerInfo(u8"engine-mcp", u8"0.1.0");

    // The engine data root (the shader cook reads <dataRoot>/Shaders): --data-root, else the
    // Data/.dataroot walk from this tool's executable - the same mechanism every executable uses.
    const String dataRoot = foundation::vfs::ResolveDataRoot(argc, argv);
    if (dataRoot.IsEmpty())
    {
        std::fprintf(stderr, "Tools.Mcp: no data root (put Data/ with its .dataroot marker "
                             "beside the tool, or pass --data-root <dir>)\n");
        return 1;
    }

    // The session every tool works through; this host OWNS the project it opens (project_open
    // stores it in `owner` and points the session at it). Both outlive the server.
    editor::mcp::ProjectOwner owner;
    editor::mcp::ProjectSession session;
    // The engine surface every host serves (one list, in editor.mcp), with the paths a host
    // supplies: the curated docs + known issues by the walk-up from the executable (then the
    // working directory), the export host-template source beside the executable, the data
    // root above.
    editor::mcp::EngineToolPaths paths;
    const String starts[] = {GetExecutableDirectory(), GetCurrentDirectory()};
    editor::mcp::LocateShippingDocs(Span<const String>(starts, 2), paths);
    // This host runs the cook / import / export INLINE: the export stages the player from
    // beside this executable and cooks shaders from the data root.
    editor::mcp::InlineProjectOperations operations(session, builders, GetExecutableDirectory(),
                                                    dataRoot);
    // What the pipeline makes after an import (a model's prefab), as the editor's import has.
    operations.onImported = [](foundation::content::Instance& primary,
                               const pipeline::ImportOptions* options)
    { pipeline::AfterImport(editor::EditorRootAllocator(), primary, options); };
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     paths, operations);
    // This host's additions: an agent opens (or scaffolds) the project it wants to work on.
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, dataRoot.AsView());
    // host_info (ops hygiene): pid + build stamp + versions + the open-project state.
    RegisterHostInfoTool(
        server, String(reinterpret_cast<const char8_t*>(BuildStamp())),
        Function<foundation::json::JsonValue()>{
            [&session]()
            {
                using foundation::json::JsonValue;
                JsonValue host = JsonValue::MakeObject();
                const bool open = session.project != nullptr;
                host.Set(u8"projectOpen", JsonValue::MakeBool(open));
                if (open)
                {
                    host.Set(u8"projectName",
                             JsonValue::MakeString(String(session.project->Name())));
                    host.Set(u8"projectDirectory",
                             JsonValue::MakeString(String(session.project->Directory())));
                }
                return host;
            }});

    StdioTransport transport;
    Serve(server, transport);
    return 0;
}

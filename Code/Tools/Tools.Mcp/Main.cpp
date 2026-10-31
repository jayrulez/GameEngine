// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.Mcp - headless MCP stdio server.
//
// Speaks newline-delimited JSON-RPC over stdin and stdout, so an agent started with one command
// line gets the engine's tools without the editor running. Stdout carries the protocol, so every
// engine log is routed to stderr; a stray print to stdout would corrupt the stream.
//
// It serves the engine tool set every host serves (editor.mcp), plus the tools to open or
// scaffold a project and host_info.

#include "Core/Prelude.h"

#include <cstdio>

import foundation.core;
import foundation.vfs; // ResolveDataRoot (--data-root, or the Data/.dataroot walk)
import foundation.shaders.system; // kShaderFolder (shader_validate's include path)
import foundation.json;
import foundation.content; // Instance, the primary object passed to onImported
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
    // Routes every engine log line to stderr, keeping stdout clean for JSON-RPC.
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
    // The log capture attaches first, before anything logs, so log_read sees the whole run; the
    // stderr mirror comes second, since stdout carries the protocol.
    static editor::EditorLogBuffer logBuffer{foundation::core::DefaultAllocator()};
    GlobalLogger().AddSink(&logBuffer);
    static StderrSink stderrSink;
    GlobalLogger().AddSink(&stderrSink);

    // Populate the reflection registry with the types we can introspect headlessly, plus the full
    // pipeline type set, so type_list sees every asset and product type and asset_cook can build.
    RegisterCoreTypes();
    foundation::json::RegisterJsonTypes();
    pipeline::RegisterPipelineTypes();
    // The complete engine script surface (every subsystem facade), so script_api reports the whole
    // bound API rather than only core. This registers metadata only and instantiates no subsystem.
    // RegisterPipelineTypes above already registered every enabled backend (AngelScript, Luau, each
    // behind its OPTION_HAS guard), so script_api spans them all.
    engine::RegisterAllScriptFacades();
    // Component reflection (with its data-version gates) for every engine domain: scene_validate
    // parses component payloads through the full manager set (Engine.Composition), which needs the
    // reflected field metadata registered before any scene stream deserializes.
    engine::RegisterAllSceneComponentReflection();

    // The engine data root (the shader cook reads <dataRoot>/Shaders): the --data-root argument,
    // else the Data/.dataroot walk up from this tool's executable, as every executable finds it.
    const String dataRoot = foundation::vfs::ResolveDataRoot(argc, argv);
    if (dataRoot.IsEmpty())
    {
        std::fprintf(stderr, "Tools.Mcp: no data root (put Data/ with its .dataroot marker "
                             "beside the tool, or pass --data-root <dir>)\n");
        return 1;
    }

    // The host's builder and importer registries, from the pipeline composition root. They are
    // populated once, outlive the server, and back asset_cook and asset_import.
    pipeline::BuilderRegistry builders{foundation::core::DefaultAllocator()};
    pipeline::ImporterRegistry importers{foundation::core::DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, dataRoot.AsView());
    pipeline::RegisterAllImporters(importers);
    // Every pipeline domain's asset creators, as File > New offers them (asset_creators, asset_create).
    pipeline::AssetCreatorRegistry creators{foundation::core::DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);

    McpServer server;
    server.SetServerInfo(u8"engine-mcp", u8"0.1.0");


    // The session every tool works through. This host owns the project it opens: project_open
    // stores it in `owner` and points the session at it. Both outlive the server.
    editor::mcp::ProjectOwner owner;
    editor::mcp::ProjectSession session;
    // The engine tools every host serves (one list, in editor.mcp), with the paths this host
    // supplies: the curated docs and known issues, found by walking up from the executable and
    // then the working directory; the export's host-template source beside the executable; and
    // the data root above.
    editor::mcp::EngineToolPaths paths;
    const String starts[] = {GetExecutableDirectory(), GetCurrentDirectory()};
    editor::mcp::LocateShippingDocs(Span<const String>(starts, 2), paths);
    // shader_validate compiles against the engine's shader library under the data root.
    paths.shaderLibrary = foundation::vfs::DataPath(dataRoot.AsView(), foundation::shaders::kShaderFolder);
    // This host runs cooks, imports and exports inline: the export stages the player from beside
    // this executable and cooks shaders from the data root.
    editor::mcp::InlineProjectOperations operations(session, builders, GetExecutableDirectory(),
                                                    dataRoot);
    // What the pipeline makes after an import (a model's prefab), as the editor's import does.
    operations.onImported = [](foundation::content::Instance& primary,
                               const pipeline::ImportOptions* options)
    { pipeline::AfterImport(editor::EditorRootAllocator(), primary, options); };
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     paths, operations);
    // This host's additions: an agent opens (or scaffolds) the project it wants to work on.
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, dataRoot.AsView());
    // host_info: the process id, build stamp, versions and open-project state.
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

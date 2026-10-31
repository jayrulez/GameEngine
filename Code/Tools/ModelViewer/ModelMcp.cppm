// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:mcp`: the viewer's commands served to an agent. Each command becomes
// an MCP tool named after it with dots as underscores (`collection.create` is `collection_create`),
// with the command's schema, annotations from its kind, and the command's JSON as its result. A
// failed command is a tool error carrying the command's message. The tool list is the command
// list, so an agent and the window can do exactly the same things. The agent's manual is served
// as `docs://ModelViewerGuide.md`.

module;
#include "Core/Prelude.h"

export module modelviewer:mcp;

import foundation.core;
import foundation.json;
import foundation.mcp;
import :commands;

using namespace foundation::core;

export namespace modelviewer
{
    inline constexpr StringView kGuideFileName = u8"ModelViewerGuide.md";
    inline constexpr StringView kGuideUri = u8"docs://ModelViewerGuide.md";

    /// A command's tool name: its dots as underscores (MCP clients take `[a-z_]` names best).
    [[nodiscard]] inline String ToolNameOf(StringView command)
    {
        String name(command);
        for (usize i = 0; i < name.Size(); ++i)
            if (name[i] == u8'.')
                name[i] = u8'_';
        return name;
    }

    /// The MCP annotations for a command kind.
    [[nodiscard]] inline foundation::mcp::ToolAnnotations AnnotationsOf(CommandKind kind)
    {
        using foundation::mcp::ToolAnnotations;
        switch (kind)
        {
        case CommandKind::Reads: return ToolAnnotations::ReadOnly();
        case CommandKind::Creates: return ToolAnnotations::Creates();
        case CommandKind::Changes: return ToolAnnotations::Overwrites();
        case CommandKind::Deletes: return ToolAnnotations::Deletes();
        case CommandKind::Adjusts: return ToolAnnotations::Adjusts();
        }
        return ToolAnnotations::Overwrites();
    }

    /// Registers every command in `commands` as a tool on `server`. `commands` must outlive the
    /// server's use of it.
    inline void RegisterCommandTools(foundation::mcp::McpServer& server, CommandSet& commands)
    {
        for (const Command& command : commands.All())
        {
            CommandSet* set = &commands;
            const String name = command.name;
            server.RegisterTool(ToolNameOf(command.name.AsView()), command.description, command.schema, AnnotationsOf(command.kind),
                                [set, name](const foundation::json::JsonValue& args) -> foundation::mcp::ToolResult
                                {
                                    CommandResult result = set->Execute(name.AsView(), args);
                                    if (!result.ok)
                                        return Err(result.error);
                                    return foundation::mcp::ToolResult(foundation::core::Move(result.value));
                                });
        }
    }

    /// Finds the agent's manual: staged beside the executable in a distribution, else under
    /// Documentation/Shipping in the checkout, walking up from each start in turn (the
    /// executable's folder, then the working folder), the way the engine's hosts find theirs.
    /// Empty when it is in neither place.
    [[nodiscard]] inline String LocateGuide(Span<const String> starts)
    {
        for (const String& start : starts)
            for (StringView dir = start.AsView(); !dir.IsEmpty(); dir = PathParent(dir))
            {
                const String staged = PathJoin(dir, kGuideFileName);
                if (FileExists(staged.AsView()))
                    return staged;
                const String checkout = PathJoin(PathJoin(PathJoin(dir, u8"Documentation").AsView(), u8"Shipping").AsView(), kGuideFileName);
                if (FileExists(checkout.AsView()))
                    return checkout;
                if (PathParent(dir) == dir)
                    break;
            }
        return {};
    }

    /// Serves the guide at `path` as the `docs://ModelViewerGuide.md` resource; nothing is
    /// registered when `path` is empty.
    inline void RegisterGuideResource(foundation::mcp::McpServer& server, StringView path)
    {
        if (path.IsEmpty())
            return;
        const String file(path);
        server.RegisterResource(String(kGuideUri), String(u8"Model viewer guide"), String(u8"text/markdown"),
                                String(u8"How to work the model viewer's library through its tools: start here."),
                                [file]() -> Result<String, String>
                                {
                                    Result<Array<byte>> bytes = ReadFile(file.AsView());
                                    if (!bytes.HasValue())
                                        return Err(Format(u8"the guide is not readable at {}", file.AsView()));
                                    return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
                                });
    }
}

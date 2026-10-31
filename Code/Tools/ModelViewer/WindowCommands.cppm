// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:window`: the window's commands for the open tabs, the view, the clip,
// the browser and the thumbnails. One table lists their names, arguments and kinds. The window
// registers each with its implementation, and buttons, menu items, drops, shortcuts and agents
// all go through them. A viewer without a window (`--headless`) registers the same names with
// handlers that report there is no window, so an agent sees one command list either way.

module;
#include "Core/Prelude.h"

export module modelviewer:window;

import foundation.core;
import foundation.json;
import :commands;

using namespace foundation::core;

export namespace modelviewer
{
    struct WindowCommandSpec
    {
        StringView name;
        StringView description;
        Span<const CommandArg> args;
        CommandKind kind;
    };

    /// The window's commands, in order.
    // Defined in WindowCommandsImpl.cpp: as an inline `const` table in this interface (dynamically
    // initialized), MSVC emits no definition of it and the viewer fails to link.
    [[nodiscard]] Span<const WindowCommandSpec> WindowCommandSpecs();

    /// Builds a command from a table entry, implemented by `run`.
    [[nodiscard]] inline Command MakeWindowCommand(const WindowCommandSpec& spec, Function<CommandResult(const JsonValue&)> run)
    {
        Command c;
        c.name = String(spec.name);
        c.description = String(spec.description);
        c.schema = ArgsSchema(spec.args);
        c.kind = spec.kind;
        c.run = foundation::core::Move(run);
        return c;
    }

    /// Registers the window's commands for a viewer with no window (`--headless`); each one
    /// returns an error saying there is no window.
    inline void RegisterWindowlessCommands(CommandSet& set)
    {
        for (const WindowCommandSpec& spec : WindowCommandSpecs())
        {
            const String name(spec.name);
            (void)set.Register(MakeWindowCommand(
                spec, [name](const JsonValue&)
                {
                    return CommandResult::Fail(
                        Format(u8"there is no window: {} needs the viewer running with one (Tools.ModelViewer --mcp), not --headless", name.AsView())
                            .AsView());
                }));
        }
    }
}

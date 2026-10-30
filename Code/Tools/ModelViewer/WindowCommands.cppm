// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:window`: the window's commands (Documentation/Specs/model-viewer.md,
// P8): the models open in tabs, the view, the clip, the browser, the thumbnails. One table names
// them, their arguments and kinds; the window registers each with what it does (a button, a menu
// item, a drop, a shortcut, or an agent, all through these), and a viewer without a window
// (`--headless`) registers the same names answering that there is no window, so an agent sees one
// command list either way.

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
    // initialized), MSVC emitted no definition of it and the viewer did not link.
    [[nodiscard]] Span<const WindowCommandSpec> WindowCommandSpecs();

    /// A command from the table, run by `run`.
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

    /// The window's commands with no window to run them (`--headless`): each says so.
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

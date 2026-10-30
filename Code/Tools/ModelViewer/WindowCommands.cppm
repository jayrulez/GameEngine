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

    namespace window_detail
    {
        inline constexpr CommandArg kPath[] = {{u8"path", u8"string", u8"the model file", true}};
        inline constexpr CommandArg kTab[] = {{u8"tab", u8"integer", u8"the tab's place among the model tabs, from 0 (default: the one in front)", false}};
        inline constexpr CommandArg kSelect[] = {{u8"tab", u8"integer", u8"the tab's place among the model tabs, from 0; -1 for Browse, -2 for Library"}};
        inline constexpr CommandArg kShot[] = {{u8"path", u8"string", u8"the PNG to write (default: the screenshot folder)", false}};
        inline constexpr CommandArg kCamera[] = {{u8"frame", u8"boolean", u8"frame the model (the F key)", false},
                                                 {u8"yaw", u8"number", u8"turn about the model, degrees (0: from the front)", false},
                                                 {u8"pitch", u8"number", u8"look down (negative) or up, degrees", false}};
        inline constexpr CommandArg kLight[] = {{u8"exposure", u8"number", u8"stops, -4 to 4", false},
                                                {u8"ambient", u8"number", u8"the flat fill, 0 to 2", false},
                                                {u8"sky", u8"number", u8"the sky's brightness and light, 0 to 3", false},
                                                {u8"turntable", u8"boolean", u8"turn the model slowly", false},
                                                {u8"skeleton", u8"boolean", u8"draw the bones", false}};
        inline constexpr CommandArg kClip[] = {{u8"clip", u8"string", u8"the clip's name (as clip.list says it)"}};
        inline constexpr CommandArg kPlay[] = {{u8"playing", u8"boolean", u8"play (true) or pause (false)", false},
                                               {u8"loop", u8"boolean", u8"start over at the end", false},
                                               {u8"speed", u8"number", u8"playback speed (1: as authored)", false}};
        inline constexpr CommandArg kScrub[] = {{u8"seconds", u8"number", u8"the moment of the clip to show, paused"}};
        inline constexpr CommandArg kFolder[] = {{u8"path", u8"string", u8"the folder to browse"}};

        inline const WindowCommandSpec kSpecs[] = {
            {u8"model.open", u8"Opens a model file in a tab of its own (or brings its tab to the front); returns the tab.", kPath, CommandKind::Adjusts},
            {u8"model.close", u8"Closes a model's tab.", kTab, CommandKind::Adjusts},
            {u8"tabs.list", u8"The model tabs: each one's place, file and whether it is in front; and whether Browse or Library is.", {}, CommandKind::Reads},
            {u8"tabs.select", u8"Brings a tab to the front (-1: Browse, -2: Library).", kSelect, CommandKind::Adjusts},
            {u8"view.screenshot", u8"Saves the 3D view (no panels) as a PNG, written within a frame; returns the file.", kShot, CommandKind::Creates},
            {u8"view.camera", u8"Moves the camera: frames the model, or looks at it from an angle.", kCamera, CommandKind::Adjusts},
            {u8"view.light", u8"Sets the view's light, turntable and skeleton; returns them all.", kLight, CommandKind::Adjusts},
            {u8"clip.list", u8"The clips of the model in front: their names, lengths, and which plays.", {}, CommandKind::Reads},
            {u8"clip.select", u8"Plays a clip of the model in front, from its start.", kClip, CommandKind::Adjusts},
            {u8"clip.play", u8"Plays or pauses the clip, sets its looping and speed; returns the playback.", kPlay, CommandKind::Adjusts},
            {u8"clip.scrub", u8"Shows a moment of the clip, paused.", kScrub, CommandKind::Adjusts},
            {u8"browse.folder", u8"Browses a folder: the Browse tab in front with every model under it.", kFolder, CommandKind::Adjusts},
            {u8"thumbnail.get", u8"A model's thumbnail file: there when drawn before, else drawn soon (ask again).", kPath, CommandKind::Reads},
        };
    }

    /// The window's commands, in order.
    [[nodiscard]] inline Span<const WindowCommandSpec> WindowCommandSpecs() { return Span<const WindowCommandSpec>(window_detail::kSpecs, sizeof(window_detail::kSpecs) / sizeof(window_detail::kSpecs[0])); }

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

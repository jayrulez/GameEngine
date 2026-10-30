// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - the window's command table (WindowCommandSpecs, declared in `modelviewer:window`).

module;
#include "Core/Prelude.h"

module modelviewer;

import foundation.core;

using namespace foundation::core;

namespace modelviewer
{
    namespace
    {
        constexpr CommandArg kPath[] = {{u8"path", u8"string", u8"the model file", true}};
        constexpr CommandArg kTab[] = {{u8"tab", u8"integer", u8"the tab's place among the model tabs, from 0 (default: the one in front)", false}};
        constexpr CommandArg kSelect[] = {{u8"tab", u8"integer", u8"the tab's place among the model tabs, from 0; -1 for Browse, -2 for Library"}};
        constexpr CommandArg kShot[] = {{u8"path", u8"string", u8"the PNG to write (default: the screenshot folder)", false}};
        constexpr CommandArg kCamera[] = {{u8"frame", u8"boolean", u8"frame the model (the F key)", false},
                                          {u8"yaw", u8"number", u8"turn about the model, degrees (0: from the front)", false},
                                          {u8"pitch", u8"number", u8"look down (negative) or up, degrees", false}};
        constexpr CommandArg kLight[] = {{u8"exposure", u8"number", u8"stops, -4 to 4", false},
                                         {u8"ambient", u8"number", u8"the flat fill, 0 to 2", false},
                                         {u8"sky", u8"number", u8"the sky's brightness and light, 0 to 3", false},
                                         {u8"turntable", u8"boolean", u8"turn the model slowly", false},
                                         {u8"skeleton", u8"boolean", u8"draw the bones", false}};
        constexpr CommandArg kClip[] = {{u8"clip", u8"string", u8"the clip's name (as clip.list says it)"}};
        constexpr CommandArg kPlay[] = {{u8"playing", u8"boolean", u8"play (true) or pause (false)", false},
                                        {u8"loop", u8"boolean", u8"start over at the end", false},
                                        {u8"speed", u8"number", u8"playback speed (1: as authored)", false}};
        constexpr CommandArg kScrub[] = {{u8"seconds", u8"number", u8"the moment of the clip to show, paused"}};
        constexpr CommandArg kFolder[] = {{u8"path", u8"string", u8"the folder to browse"}};

        const WindowCommandSpec kSpecs[] = {
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

    Span<const WindowCommandSpec> WindowCommandSpecs()
    {
        return Span<const WindowCommandSpec>(kSpecs, sizeof(kSpecs) / sizeof(kSpecs[0]));
    }
}

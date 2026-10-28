// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :playback_actions partition.
//
// The playback actions over any page that publishes IPlaybackPage: playback.play (a toggle,
// checked while playing: start, pause, resume), playback.stop (stop and rewind) and
// playback.restart. A page binds its transport's buttons to them (BindAction).
module;
#include "Core/Prelude.h"
export module editor.app:playback_actions;

import foundation.core;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    inline void RegisterPlaybackActions(editor::EditorActionRegistry& actions)
    {
        const auto canPlay = [](editor::EditorPage* page)
        {
            editor::IPlaybackPage* playback = editor::ServiceOf<editor::IPlaybackPage>(page);
            return playback != nullptr && playback->CanPlay();
        };
        {
            editor::EditorActionDeclaration d;
            d.id = String(u8"playback.play");
            d.label = String(u8"Play");
            d.description = String(u8"Play, pause or resume the page's preview");
            d.kind = editor::EditorActionKind::Toggle;
            d.enabled = canPlay;
            d.checked = [](editor::EditorPage* page)
            {
                editor::IPlaybackPage* playback = editor::ServiceOf<editor::IPlaybackPage>(page);
                return playback != nullptr && playback->IsPlaying();
            };
            d.execute = [](editor::EditorPage* page)
            {
                if (editor::IPlaybackPage* playback =
                        editor::ServiceOf<editor::IPlaybackPage>(page))
                {
                    if (playback->IsPlaying())
                    {
                        playback->Pause();
                    }
                    else
                    {
                        playback->Play();
                    }
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            editor::EditorActionDeclaration d;
            d.id = String(u8"playback.stop");
            d.label = String(u8"Stop");
            d.description = String(u8"Stop the page's preview and rewind it");
            d.enabled = canPlay;
            d.execute = [](editor::EditorPage* page)
            {
                if (editor::IPlaybackPage* playback =
                        editor::ServiceOf<editor::IPlaybackPage>(page))
                {
                    playback->Stop();
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            editor::EditorActionDeclaration d;
            d.id = String(u8"playback.restart");
            d.label = String(u8"Restart");
            d.description = String(u8"Play the page's preview from the start");
            d.enabled = canPlay;
            d.execute = [](editor::EditorPage* page)
            {
                if (editor::IPlaybackPage* playback =
                        editor::ServiceOf<editor::IPlaybackPage>(page))
                {
                    playback->Restart();
                }
            };
            (void)actions.Register(Move(d));
        }
    }
}

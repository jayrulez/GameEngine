// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.player.main: the desktop player's boot as a library entry point. Engine.Player and the
// project's Engine.GamePlayer are each a thin stub calling it; what tells them apart is the plugins
// each links.

module;
#include "Core/Prelude.h"

export module engine.player.main;

export namespace engine::player
{
    // Runs the player.
    int PlayerMain(int argc, char** argv);
}

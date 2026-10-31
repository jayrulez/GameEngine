// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.player.main: the desktop player's boot as a library entry point. The dev
// Engine.Player executable is a thin stub passing null; the ship stub (ShipMain.cpp)
// imports this module and passes the game's statically-linked plugin.

module;
#include "Core/Prelude.h"

export module engine.player.main;

import foundation.runtime;

export namespace engine::player
{
    // Runs the player. `nativeGame` is the project's statically-provided native game
    // plugin (ship builds; borrowed, owned by the stub), or null (dev builds, where any native
    // module loads dynamically).
    int PlayerMain(int argc, char** argv, foundation::runtime::IRuntimePlugin* nativeGame);
}

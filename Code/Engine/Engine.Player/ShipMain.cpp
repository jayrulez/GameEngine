// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.GamePlayer: the ship entry point. The same launcher the dev player uses, with the
// project's native game plugin statically linked. One checked-in stub covers every game: the
// plugin contract already exports extern "C" CreatePlugin (the symbol PluginHost resolves
// with dlopen in dev builds), so referencing it here pulls the game's registration out of its
// static archive, with no generated main, forced-reference chains or whole-archive linking.

import engine.player.main;
import foundation.runtime;

extern "C" foundation::runtime::IRuntimePlugin* CreatePlugin();

int main(int argc, char** argv)
{
    return engine::player::PlayerMain(argc, argv, CreatePlugin());
}

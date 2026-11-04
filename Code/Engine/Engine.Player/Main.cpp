// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.Player and Engine.GamePlayer: the desktop entry point. The whole boot lives in
// engine.player.main (PlayerMain.cpp, a launcher built as a library); the project's player links
// the project's packages besides, through its generated plugin registry.

import engine.player.main;

int main(int argc, char** argv)
{
    return engine::player::PlayerMain(argc, argv);
}

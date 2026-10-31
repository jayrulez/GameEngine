// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.Player: the dev and generic desktop entry point. The whole boot lives in
// engine.player.main (PlayerMain.cpp, a launcher built as a library); a ship build uses
// ShipMain.cpp instead, which passes the game's statically-linked native plugin.

import engine.player.main;

int main(int argc, char** argv)
{
    return engine::player::PlayerMain(argc, argv, nullptr);
}

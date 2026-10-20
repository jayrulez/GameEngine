// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.docking`: docking: splits, tab groups, drop zones, floating panels in the window and as OS windows
// (through hosting), layouts saved and restored, and its default sheet.
// foundation.ui's counterpart: UI.Toolkit's docking and UI.Application. Round three of the lab's evolution
// (Specs/gui-lab-evolution-3.md): D, after its survey.

module;
#include "Core/Prelude.h"

export module experimental.gui.docking;

export import :layout; // D1: the layout as data and its operations

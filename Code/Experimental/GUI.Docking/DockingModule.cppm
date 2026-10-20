// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.docking`: docking: splits, tab groups, drop zones, floating panels in the window and as OS windows
// (through hosting), layouts saved and restored, and its default sheet.
// foundation.ui's counterpart: UI.Toolkit's docking and UI.Application. Round three of the lab's evolution
// (Specs/gui-lab-evolution-3.md): D, after its survey.

module;
#include "Core/Prelude.h"

export module experimental.gui.docking;

export import :layout;   // D1: the layout as data and its operations
export import :document; // D2: the layout as the serializer reads and writes it
export import :controller; // D3: the layout and its panels
export import :dock_space; // D3: the widget that shows a surface
export import :dock_area;  // D5: the main surface and the floating ones over it, in one window

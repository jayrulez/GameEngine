// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `foundation.gui.docking`: splits, tab groups, drop zones, floating panels in the window
// and as OS windows (through hosting), saving and restoring layouts, and the default sheet.

module;
#include "Core/Prelude.h"

export module foundation.gui.docking;

export import :layout;   // the layout as data and its operations
export import :document; // the layout as the serializer reads and writes it
export import :controller; // the layout and its panels
export import :dock_space; // the widget that shows a surface
export import :dock_area;  // the main surface and the floating ones over it, in one window
export import :dock_host_windows; // the floats in OS windows the host opens

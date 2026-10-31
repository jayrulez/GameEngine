// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `foundation.gui.toolkit`, the editor widgets: tool and status bars, the property grid,
// colour, curves, gradients, the timeline and node graphs. Docking lives in GUI.Docking and the code
// editor in GUI.Code.

module;
#include "Core/Prelude.h"

export module foundation.gui.toolkit;

export import :sheet;    // the default sheet, over the theme's tokens
export import :tool_bar;   // tool bars over the command model
export import :status_bar; // status bars of registered items and messages
export import :property_source; // what a property grid reads and edits through
export import :property_editor;  // a grid's editors and how it chooses them
export import :property_editors; // the editors of bools, numbers, text, enums and flags
export import :property_grid;    // the grid of a source's properties
export import :reflected_source; // core reflection as a property source
export import :time_axis;        // one time axis and its ruler
export import :timeline_source;  // what a dopesheet reads and edits through
export import :curve_source;     // what a curve view reads and edits through
export import :curve_view;       // a source's curves on one canvas
export import :gradient_view;    // a gradient's stops, edited as a value
export import :dopesheet;        // a timeline's channels as rows of keys
export import :curve_editors;    // the grid's curve and gradient editors, and the defaults
export import :graph_source;     // what a graph view reads and edits through
export import :graph_node_picker; // the kinds of node a graph view adds, with a search
export import :graph_view;       // a graph source's nodes on a canvas
export import :breadcrumb_bar;   // where a view is, as segments to navigate by

export namespace foundation::gui
{
    /// Registers the toolkit's element types (ElementTypes::Register) once. A host calls it at
    /// start-up beside RegisterGuiElements, so markup and scripts can make them.
    void RegisterToolkitElements();
}

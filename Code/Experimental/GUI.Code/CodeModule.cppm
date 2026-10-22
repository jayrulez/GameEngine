// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - `experimental.gui.code`: the code editor (C0 to C8 of Specs/gui-lab-evolution-3.md).
// foundation.ui's counterpart: UI.Toolkit's CodeEditView. A satellite of the lab (round three): it
// knows no language; a language's definition and its service are its owners'.

module;
#include "Core/Prelude.h"

export module experimental.gui.code;

export import :code_document; // C0: lines, positions, anchors, edits, undo with selections

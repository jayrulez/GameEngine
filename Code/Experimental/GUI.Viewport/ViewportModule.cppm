// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.viewport`: a node showing content drawn into a target of its own (a 3D scene), registered with its
// window's renderer as an external texture, its input gated to the node.
// foundation.ui's counterpart: UI.Viewport. Round three of the lab's evolution
// (Specs/gui-lab-evolution-3.md): V1, V2.

module;
#include "Core/Prelude.h"

export module experimental.gui.viewport;

import foundation.core;
import experimental.gui;

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.hosting`: the host on foundation.graphics: one GUI, a root per window, each window's VG context,
// renderer, pass targets and input; the frame; pictures drawn offscreen; the picture baker.
// foundation.ui's counterpart: UI.Runtime. Round three of the lab's evolution
// (Specs/gui-lab-evolution-3.md): H1 to H3.

module;
#include "Core/Prelude.h"

export module experimental.gui.hosting;

import foundation.core;
import experimental.gui;

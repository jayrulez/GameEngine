// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.hosting`: the host on foundation.graphics (GuiHost): one GUI, a root per
// window, each window's VG context, renderer, pass targets and input; the frame; the picture baker
// (GpuPictureBaker) serving every window; the VG shaders they draw with (VgShaderLibrary).
// foundation.ui's counterpart: UI.Runtime. Round three of the lab's evolution
// (Specs/gui-lab-evolution-3.md), H1 to H3.

module;
#include "Core/Prelude.h"

export module experimental.gui.hosting;

export import :vg_shaders;
export import :picture_baker;
export import :gui_host;
export import :offscreen;
export import :hosted_content;

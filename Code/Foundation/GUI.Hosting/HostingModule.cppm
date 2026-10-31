// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `foundation.gui.hosting`: the GUI host on foundation.graphics (GuiHost), with one GUI, a
// root per window, and each window's VG context, renderer, pass targets and input; the frame; the
// picture baker (GpuPictureBaker) that serves every window; the VG shaders they draw with
// (VgShaderLibrary); and drawing a root into a pass its caller owns (GuiPassRenderer, for an
// engine's overlays).

module;
#include "Core/Prelude.h"

export module foundation.gui.hosting;

export import :vg_shaders;
export import :atlas_packer;
export import :picture_baker;
export import :gui_host;
export import :offscreen;
export import :hosted_content;
export import :pass_renderer;

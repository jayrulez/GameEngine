// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Extensions::Imgui - `extensions.imgui`, the Dear ImGui debug-UI integration.
///
/// ImguiRenderer renders ImGui draw data through the RHI, and ImguiSubsystem owns the context and the
/// per-frame NewFrame and Render hooks an app drives. Dear ImGui itself is used through its own
/// header (imgui.h, from the vendored Extensions::Imgui library this links PUBLIC): apps include
/// "imgui.h" and call ImGui:: directly, and this module only adds the engine integration. It depends
/// on Runtime, Shell (input), Graphics (FrameContext), RHI and Shaders.

export module extensions.imgui;

export import :renderer;
export import :subsystem;

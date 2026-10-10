// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - `editor.preview`: the shared 3D-preview substrate for bespoke asset
// editor pages (mesh / clip / skeleton / material / particle / animgraph / collision).
//
// PreviewViewport owns the whole substrate each of those pages would otherwise hand-roll: a
// ViewportView, a private preview Scene (simulation off) in its own SceneManager, an
// EditorCamera, the InputRouter, and the EnsureViewportBound + camera-update + RenderScene
// loop. A page CONTAINS one instead of re-implementing it: construct it, populate Scene()
// with the page's own entities (mesh, light, ...), drive Update(dt) from OnUpdate and
// RenderFrame(frame) from OnRenderWindow, mount View() in the layout, and draw overlays
// into SceneDebugDraw() (render->DebugScene(Scene()) - a preview scene has exactly one view,
// so the keyed-view DebugScene contract is unambiguous here).
//
// This interface is deliberately LEAN and PIMPL'd: it names only the types in its public
// signatures (Scene*, a base View*, EditorCamera&, DebugDraw&, FrameContext&) and hides the
// render/scene/rhi/viewport/vg graph behind Impl in the .cpp. That is not just tidiness - GCC
// 15 segfaults in its module merger when a heavy page TU (ScenePage, MaterialPage, ...)
// imports an interface that transitively pulls engine.render + engine.scene, so those imports
// MUST stay out of this interface. Never linked by the runtime.

module;
#include "Core/Prelude.h"

export module editor.preview;

export import :thumbnail_stage; // the GPU thumbnail renderer (shares this substrate's home)

import foundation.core;
import foundation.runtime.client; // IApplicationHost
import foundation.ui.runtime;     // UIHost
import foundation.scene;          // Scene* (returned)
import foundation.render;         // debug::DebugDraw& (returned)
import foundation.graphics;       // FrameContext& (parameter)
import foundation.ui;             // View* (the viewport as a base View, for layout)
export import editor.camera;      // EditorCamera& (returned)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;

    // Shared 3D-preview viewport substrate. Non-copyable, non-movable (PIMPL owns a
    // SceneManager by value + a live ViewportView); hold it by UniquePtr in the page.
    class PreviewViewport
    {
    public:
        // Builds the viewport + a private preview scene named `sceneName` (e.g. "mesh.preview").
        // The scene has simulation disabled; the page adds its own entities via Scene().
        PreviewViewport(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost,
                        StringView sceneName);
        ~PreviewViewport();

        PreviewViewport(const PreviewViewport&) = delete;
        PreviewViewport& operator=(const PreviewViewport&) = delete;

        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] scene::Scene* Scene() const;
        /// Start over with an empty scene (the old one and everything in it destroyed): a page
        /// that previews on different content (a profile on one of the project's scenes) swaps it.
        scene::Scene* ResetScene();
        // The viewport view, as a base View for layout (SplitView panes etc.).
        [[nodiscard]] ui::View* View() const;
        [[nodiscard]] EditorCamera& Camera();

        // Overlay draw target for this preview scene (skeleton wireframe, collision outline, ...).
        // Valid only while IsValid(); the render subsystem must be present.
        [[nodiscard]] foundation::render::debug::DebugDraw& SceneDebugDraw();

        // Background clear color of the preview viewport (default a neutral dark grey). Pages
        // that want a different backdrop (e.g. the particle page's darker field) set it here.
        // sRGB, as every colour is entered.
        void SetClearColor(Color color);

        // Whether the preview scene simulates. Default OFF (static previews: mesh/material/clip/
        // skeleton pose their content directly). The particle page turns it ON so the effect runs.
        void SetSimulationEnabled(bool enabled);

        // Playback speed of the preview scene (the particle page's speed slider drives it).
        // 1.0 = real time; 0 = paused. Applies to the preview scene's own SceneManager only.
        void SetTimeScale(f32 scale);

        // Per-frame input + camera drive. Call from the page's OnUpdate BEFORE any page logic
        // that reads the camera. Safe before the viewport is bound (no-ops until then).
        void Update(f32 dt);

        // Render the preview scene into the viewport. Call from the page's OnRenderWindow.
        void RenderFrame(foundation::graphics::FrameContext& frame);

        // Tear down the viewport + destroy the preview scene. Call from the page's OnClose.
        void Shutdown();

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}

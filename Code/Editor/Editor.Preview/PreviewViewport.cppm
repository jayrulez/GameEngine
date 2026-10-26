// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - `editor.preview`: the shared 3D-preview substrate for bespoke asset
// editor pages (mesh / clip / skeleton / material / particle / animgraph / collision).
//
// PreviewViewport owns the whole substrate each of those pages would otherwise hand-roll: a GUI
// Viewport (GUI.Viewport, hosted by the window's GuiHost, which places it and routes its input), a
// private preview Scene (simulation off) in its own SceneManager, an EditorCamera, and the
// camera-update + RenderScene loop. A page CONTAINS one instead of re-implementing it: construct
// it, put View() in its markup's stage, populate Scene() with the page's own entities (mesh,
// light, ...), drive Update(dt) from OnUpdate and RenderFrame(frame) from OnRenderWindow, and draw
// overlays into SceneDebugDraw() (render->DebugScene(Scene()) - a preview scene has exactly one
// view, so the keyed-view DebugScene contract is unambiguous here). On the GUI
// (Documentation/Specs/editor-gui.md, 5.4, layer 3) as the scene page's viewport is: the camera
// flies on the viewport's mouse while it is hovered or holds a press and on its keyboard only while
// it has the focus, so keys typed in a field elsewhere never fly it; F frames what the page says
// to frame; a hidden viewport (its tab not in front) renders nothing. One backdrop for every
// preview, where each page had its own.
//
// This interface is deliberately LEAN and PIMPL'd: it names only the types in its public
// signatures (Scene*, a GUI Node, EditorCamera&, DebugDraw&, FrameContext&) and hides the
// render/scene/rhi graph behind Impl in the .cpp. That is not just tidiness - GCC 15 segfaults in
// its module merger when a heavy page TU imports an interface that transitively pulls
// engine.render + engine.scene, so those imports MUST stay out of this interface. Never linked by
// the runtime.

module;
#include "Core/Prelude.h"

export module editor.preview;

export import :thumbnail_stage; // the GPU thumbnail renderer (shares this substrate's home)

import foundation.core;
import foundation.runtime.client; // IApplicationHost
import foundation.scene;          // Scene* (returned)
import foundation.render;         // debug::DebugDraw& (returned)
import foundation.graphics;       // FrameContext& (parameter)
import foundation.gui;            // Node (the viewport, for a page's markup)
import foundation.gui.hosting;    // GuiHost
export import editor.camera;      // EditorCamera& (returned)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;

    // Shared 3D-preview viewport substrate. Non-copyable, non-movable (PIMPL owns a
    // SceneManager by value + a live viewport); hold it by UniquePtr in the page.
    class PreviewViewport
    {
    public:
        /// The backdrop every preview shares (sRGB), a page's own only where it must differ.
        static constexpr Color kBackdrop{0.349f, 0.366f, 0.396f, 1.0f};

        // Builds the viewport + a private preview scene named `sceneName` (e.g. "mesh.preview"),
        // hosted by `gui` (null: not hosted, a test: it neither draws nor takes input). The scene
        // has simulation disabled; the page adds its own entities via Scene().
        PreviewViewport(runtime::IApplicationHost& host, foundation::gui::GuiHost* gui, StringView sceneName);
        ~PreviewViewport();

        PreviewViewport(const PreviewViewport&) = delete;
        PreviewViewport& operator=(const PreviewViewport&) = delete;

        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] scene::Scene* Scene() const;
        /// Start over with an empty scene (the old one and everything in it destroyed): a page
        /// that previews on different content (a profile on one of the project's scenes) swaps it.
        scene::Scene* ResetScene();
        /// The viewport, for a page's markup to hold.
        [[nodiscard]] RefPtr<foundation::gui::Node> View() const;
        [[nodiscard]] EditorCamera& Camera();
        /// The camera framed on a sphere at `center` of `radius`, eased there.
        void Frame(Float3 center, f32 radius);

        // Overlay draw target for this preview scene (skeleton wireframe, collision outline, ...).
        // Valid only while IsValid(); the render subsystem must be present.
        [[nodiscard]] foundation::render::debug::DebugDraw& SceneDebugDraw();

        // Background clear color of the preview viewport (kBackdrop by default). sRGB, as every
        // colour is entered.
        void SetClearColor(Color color);

        // Whether the preview scene simulates. Default OFF (static previews: mesh/material/clip/
        // skeleton pose their content directly). The particle page turns it ON so the effect runs.
        void SetSimulationEnabled(bool enabled);

        // Playback speed of the preview scene (the particle page's speed slider drives it).
        // 1.0 = real time; 0 = paused. Applies to the preview scene's own SceneManager only.
        void SetTimeScale(f32 scale);

        // Per-frame input + camera drive. Call from the page's OnUpdate BEFORE any page logic
        // that reads the camera. True when F was pressed over it this frame (the page frames).
        bool Update(f32 dt);

        // Render the preview scene into the viewport. Call from the page's OnRenderWindow.
        void RenderFrame(foundation::graphics::FrameContext& frame);

        // Tear down the preview scene; the camera released. Call from the page's OnClose.
        void Shutdown();

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}

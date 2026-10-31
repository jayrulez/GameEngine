// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview.Viewport - `editor.preview.viewport`: the shared 3D preview for bespoke asset
// editor pages (mesh, clip, skeleton, material, particle, animation graph, collision) and for
// standalone tools on the engine (the model viewer). It needs nothing of the editor's core: its
// owner hands it the allocator it builds from.
//
// PreviewViewport owns what each of those pages would otherwise build by hand: a GUI Viewport
// (GUI.Viewport, hosted by the window's GuiHost, which places it and routes its input), a private
// preview Scene (simulation off) in its own SceneManager, an EditorCamera, and the camera update
// and RenderScene loop. A page contains one instead of re-implementing it: construct it, put View()
// in its markup's stage, populate Scene() with the page's own entities (mesh, light, ...), drive
// Update(dt) from OnUpdate and RenderFrame(frame) from OnRenderWindow, and draw overlays into
// SceneDebugDraw() (render->DebugScene(Scene()); a preview scene has exactly one view, so the
// keyed-view DebugScene contract is unambiguous here). Input works as on the scene page's
// viewport: the camera flies on the viewport's mouse while it is hovered or holds a press, and on
// its keyboard only while it has the focus, so keys typed in a field elsewhere never fly it. F
// frames what the page says to frame, and a hidden viewport (its tab not in front) renders
// nothing. Every preview shares one backdrop.
//
// The interface is lean and PIMPL'd: it names only the types in its public signatures (Scene*, a
// GUI Node, EditorCamera&, DebugDraw&, FrameContext&) and hides the render, scene and RHI graph
// behind Impl in the .cpp. GCC 15 segfaults in its module merger when a heavy page translation unit
// imports an interface that transitively pulls in engine.render and engine.scene, so those imports
// must stay out of this interface. The runtime never links this module.

module;
#include "Core/Prelude.h"

export module editor.preview.viewport;

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

    // The shared 3D preview viewport. It is neither copyable nor movable (the PIMPL owns a
    // SceneManager by value and a live viewport); hold it by UniquePtr in the page.
    class PreviewViewport
    {
    public:
        /// The backdrop every preview shares (sRGB); a page sets its own only where it must differ.
        static constexpr Color kBackdrop{0.349f, 0.366f, 0.396f, 1.0f};

        // Builds the viewport and a private preview scene named `sceneName` (e.g. "mesh.preview"),
        // hosted by `gui` (null in a test, where it neither draws nor takes input). The scene has
        // simulation disabled; the page adds its own entities through Scene().
        PreviewViewport(IAllocator& allocator, runtime::IApplicationHost& host, foundation::gui::GuiHost* gui,
                        StringView sceneName);
        ~PreviewViewport();

        PreviewViewport(const PreviewViewport&) = delete;
        PreviewViewport& operator=(const PreviewViewport&) = delete;

        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] scene::Scene* Scene() const;
        /// Starts over with an empty scene, destroying the old one and everything in it. A page
        /// that previews different content (a profile on one of the project's scenes) swaps scenes
        /// this way.
        scene::Scene* ResetScene();
        /// The viewport, for a page's markup to hold.
        [[nodiscard]] RefPtr<foundation::gui::Node> View() const;
        [[nodiscard]] EditorCamera& Camera();
        /// Frames the camera on a sphere of `radius` at `center`, easing it there.
        void Frame(Float3 center, f32 radius);

        // Overlay draw target for this preview scene (skeleton wireframe, collision outline, ...).
        // Valid only while IsValid(); the render subsystem must be present.
        [[nodiscard]] foundation::render::debug::DebugDraw& SceneDebugDraw();

        // Background clear color of the preview viewport (kBackdrop by default). sRGB, as every
        // colour is entered.
        void SetClearColor(Color color);

        // Whether the preview scene simulates. Off by default (static previews such as mesh,
        // material, clip and skeleton pose their content directly); the particle page turns it on
        // so the effect runs.
        void SetSimulationEnabled(bool enabled);

        // Playback speed of the preview scene (the particle page's speed slider drives it).
        // 1.0 is real time and 0 is paused. Applies to the preview scene's own SceneManager only.
        void SetTimeScale(f32 scale);

        // Per-frame input and camera drive. Call from the page's OnUpdate before any page logic
        // that reads the camera. Returns true when F was pressed over it this frame, for the page
        // to frame its content.
        bool Update(f32 dt);

        // Render the preview scene into the viewport. Call from the page's OnRenderWindow.
        void RenderFrame(foundation::graphics::FrameContext& frame);

        // Tears down the preview scene and releases the camera. Call from the page's OnClose.
        void Shutdown();

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}

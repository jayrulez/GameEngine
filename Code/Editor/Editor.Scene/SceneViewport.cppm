// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_viewport partition.
//
// A scene page's viewport: the GUI's Viewport the scene is drawn into by the real renderer, through
// a camera override, in the app's one renderer bracket before the GUI's frame (the render graph
// imports the colour target in the viewport's state and leaves it ready to sample, so the widget
// keeps it); the editor camera flying on the viewport's input, the mouse while it is hovered or
// holds a press, the keyboard only while it has the focus (a click focuses it), so keys typed in a
// field elsewhere never fly it; GPU picking keyed by the viewport; the capture of what it shows (an
// agent's screenshot, the project's picture); and framing entities. A hidden viewport (a tab not in
// front) renders nothing. Out of the page's class (Documentation/Specs/editor-gui.md, 5.3,
// layer 2), on the GUI where it was on foundation.ui's ViewportView.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.scene:scene_viewport;

import foundation.core;
import foundation.rhi;
import foundation.shell;
import foundation.input; // ScriptedInputSource (a playtest of the view)
import foundation.graphics;
import foundation.runtime.client;
import foundation.scene;
import foundation.render;
import engine.render;
import engine.ui;  // game-UI RenderTexture canvases (live in editing viewports)
import engine.gui; // and the engine GUI's textures
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import editor.core;
import editor.camera;
import editor.viewporttools;
import :gizmo;
import :scene_page_interface;
import :viewport_capture;

using namespace foundation::core;

export namespace editor
{
    /// Whether Shift is held for what the pointer does over a view: the keyboard the view has
    /// while focused, else, while the pointer is over it, the device's own (a wheel goes where the
    /// pointer is, whatever has the keys). Pure, so tested without a window.
    [[nodiscard]] inline bool ShiftHeldOver(foundation::shell::IKeyboard* focused, foundation::shell::IKeyboard* device, bool hovered)
    {
        const auto shift = [](foundation::shell::IKeyboard& keys)
        { return keys.IsKeyDown(foundation::shell::KeyCode::LeftShift) || keys.IsKeyDown(foundation::shell::KeyCode::RightShift); };
        if (focused != nullptr)
            return shift(*focused);
        return hovered && device != nullptr && shift(*device);
    }

    class SceneViewport
    {
    public:
        struct Services
        {
            engine::render::RenderSubsystem* render = nullptr; // null: nothing renders (a test)
            engine::ui::UISubsystem* gameUI = nullptr;
            engine::gui::GuiSubsystem* gameGui = nullptr;
        };

        SceneViewport(IAllocator& allocator, Services services) : m_services(services), m_picker(*this)
        {
            m_view = MakeRef<foundation::gui::Viewport>(allocator);
            m_view->AddClass(u8"scene-view");
            m_view->ClearColor = Color{0.349f, 0.366f, 0.396f, 1.0f};
            // Start framed on the origin (the grid's centre), the orbit's pivot there, the horizon level.
            m_camera.LookAt(Float3{0.0f, 0.0f, 0.0f});
        }
        SceneViewport(const SceneViewport&) = delete;
        SceneViewport& operator=(const SceneViewport&) = delete;

        [[nodiscard]] foundation::gui::Viewport& View() noexcept { return *m_view; }
        [[nodiscard]] RefPtr<foundation::gui::Viewport> ViewNode() const noexcept { return m_view; }
        [[nodiscard]] engine::render::RenderSubsystem* Renderer() const noexcept { return m_services.render; }
        [[nodiscard]] EditorCamera& Camera() noexcept { return m_camera; }
        [[nodiscard]] const EditorCamera& Camera() const noexcept { return m_camera; }
        [[nodiscard]] IViewportPicker& Picker() noexcept { return m_picker; }
        /// What its debug list and its picks are keyed by.
        [[nodiscard]] const void* Key() const noexcept { return m_view.Get(); }

        /// Drawn and given input by `host` from now on (null: not hosted, a test).
        void HostIn(foundation::gui::GuiHost* host)
        {
            if (host != nullptr)
                m_view->HostIn(*host);
        }

        /// Whether it is on screen with targets to draw into.
        [[nodiscard]] bool IsShown() const { return m_view->IsReady() && m_view->IsTreeVisible() && m_view->RenderWidth() > 0 && m_view->RenderHeight() > 0; }
        /// Whether its content has the input this frame: hovered, focused, or holding a press (a
        /// look drag, its pointer locked and hidden, may drift off it).
        [[nodiscard]] bool IsActive() const
        {
            if (m_scripted)
                return true; // a playtest of the view holds it
            const foundation::shell::InputSurface* surface = m_view->HostedSurface();
            return m_view->IsHovered() || m_view->IsFocused() || (surface != nullptr && surface->Captured());
        }
        [[nodiscard]] bool IsHovered() const { return m_scripted ? ScriptedPointerInside() : m_view->IsHovered(); }
        /// The mouse, in the targets' pixels.
        [[nodiscard]] foundation::shell::IMouse* Mouse() const { return m_scripted ? m_scripted->Mouse() : m_view->Mouse(); }
        /// The keyboard while it has the focus or holds a press; null otherwise.
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() const
        {
            if (m_scripted)
                return m_scripted->Keyboard(); // as if focused: the playtest's keys are the view's
            const foundation::shell::InputSurface* surface = m_view->HostedSurface();
            return m_view->IsFocused() || (surface != nullptr && surface->Captured()) ? m_view->Keyboard() : nullptr;
        }

        /// Whether Shift is held for what the pointer does over the view (a brush's Shift + wheel
        /// resize, a Shift click): read with the pointer, so it holds while the keys' focus is
        /// elsewhere (the brush's own panel, picked a moment ago).
        [[nodiscard]] bool ShiftHeld() const
        {
            if (m_scripted)
                return ShiftHeldOver(m_scripted->Keyboard(), nullptr, true);
            const foundation::shell::InputSurface* surface = m_view->HostedSurface();
            foundation::shell::IInputManager* raw = surface != nullptr ? surface->Raw() : nullptr;
            return ShiftHeldOver(Keyboard(), raw != nullptr ? raw->Keyboard() : nullptr, m_view->IsHovered());
        }

        /// The camera's frame: flown while the viewport has the input (`wheel` false leaves the
        /// wheel to a tool), released when it has not, its eased framing carried on either way.
        /// Whether the viewport had the input.
        bool UpdateCamera(f32 deltaSeconds, bool wheel)
        {
            const bool active = IsActive();
            foundation::shell::InputSurface* surface = m_view->HostedSurface();
            if (active)
            {
                m_camera.Update(Keyboard(), Mouse(), deltaSeconds, wheel);
                // An orbit, look or pan drag: the pointer locked meanwhile, granted by the router to
                // this surface alone and ended with the press (not the user's pointer for a playtest's).
                if (surface != nullptr && !m_scripted)
                    surface->SetPointerLock(m_camera.WantsPointerLock());
            }
            else
            {
                // Losing the input releases a captured fly at once (relative mode makes losing it easy).
                m_camera.ReleaseCapture(Mouse());
                if (surface != nullptr)
                    surface->SetPointerLock(false);
            }
            // A framing glides on whether or not the viewport has the input (a hierarchy
            // double-click frames while the mouse is over the hierarchy).
            m_camera.Advance(deltaSeconds);
            return active;
        }
        [[nodiscard]] bool ZoomedThisFrame() const noexcept { return m_camera.zoomedThisUpdate; }

        // === A playtest of the view (viewport_run) ===
        /// `source` stands for the devices over the view from now: its mouse in the targets'
        /// pixels, its keys as if the view had the focus, the pointer over the view while inside
        /// it. Nothing real is merged in: where the user's mouse is must not change a playtest.
        void BeginScripted(UniquePtr<foundation::input::ScriptedInputSource> source)
        {
            m_scripted = Move(source);
            m_scriptedTime = 0.0;
            m_scriptedEnding = false;
            m_scriptedReleased = false;
        }
        /// Ends it: everything held is let go for a frame (a brush lifts, the camera stops), then
        /// the devices are the view's again.
        void EndScripted()
        {
            if (m_scripted)
                m_scriptedEnding = true;
        }
        /// Once a frame, before the view's input is read: the playtest's next frame.
        void AdvanceScripted(f32 deltaSeconds)
        {
            if (!m_scripted)
                return;
            if (!m_scriptedEnding)
            {
                m_scriptedTime += static_cast<f64>(deltaSeconds);
                m_scripted->Advance(m_scriptedTime);
                return;
            }
            if (!m_scriptedReleased)
            {
                m_scripted->ReleaseAll();
                m_scriptedReleased = true;
                return;
            }
            m_scripted.Reset();
        }
        /// Seconds the playtest has run; negative when none does.
        [[nodiscard]] f64 ScriptedTime() const noexcept { return m_scripted ? m_scriptedTime : -1.0; }

        /// Alt orbit, right-button fly, or the Tab-captured fly (which owns WASD too).
        [[nodiscard]] bool CameraOwnsInput() const
        {
            foundation::shell::IMouse* mouse = Mouse();
            foundation::shell::IKeyboard* keyboard = Keyboard();
            return (keyboard != nullptr && (keyboard->IsKeyDown(foundation::shell::KeyCode::LeftAlt) || keyboard->IsKeyDown(foundation::shell::KeyCode::RightAlt))) ||
                   (mouse != nullptr && mouse->IsButtonDown(foundation::shell::MouseButton::Right)) || m_camera.mouseCaptured;
        }

        /// The camera's ray through the mouse, from its basis (no matrix inverse).
        [[nodiscard]] bool MouseRay(GizmoRay& out) const
        {
            foundation::shell::IMouse* mouse = Mouse();
            const u32 w = m_view->RenderWidth();
            const u32 h = m_view->RenderHeight();
            if (mouse == nullptr || w == 0 || h == 0)
                return false;
            const f32 ndcX = 2.0f * (mouse->X() / static_cast<f32>(w)) - 1.0f;
            const f32 ndcY = 1.0f - 2.0f * (mouse->Y() / static_cast<f32>(h));
            m_camera.RayThrough(ndcX, ndcY, static_cast<f32>(w) / static_cast<f32>(h), out.origin, out.direction);
            return true;
        }

        /// The view camera the renderer draws from, at the targets' aspect.
        [[nodiscard]] foundation::render::ViewCamera ViewCamera() const
        {
            const u32 w = m_view->RenderWidth();
            const u32 h = m_view->RenderHeight();
            foundation::render::ViewCamera camera;
            camera.view = Float4x4::LookAtRH(m_camera.DrawEye(), m_camera.DrawEye() + m_camera.Forward(), m_camera.Up());
            camera.projection = m_camera.Projection(h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 16.0f / 9.0f);
            camera.position = m_camera.position;
            camera.farZ = m_camera.DrawFar();
            camera.pullBack = m_camera.DrawPullBack();
            return camera;
        }

        /// The overlays' debug list (this viewport's own, never another view's of the scene); null
        /// with no renderer.
        [[nodiscard]] foundation::render::debug::DebugDraw* Overlay() const
        {
            return m_services.render != nullptr ? &m_services.render->DebugView(Key()) : nullptr;
        }

        /// Adds this frame's view of `scene` (inside the renderer's bracket): nothing while hidden or
        /// with no renderer. `debugView` null on a clean capture's frame.
        void Render(foundation::graphics::FrameContext& frame, foundation::scene::Scene& scene, const foundation::render::ViewPostOverride* post,
                    const foundation::render::ViewDebugView* debugView)
        {
            engine::render::RenderSubsystem* render = m_services.render;
            if (!frame.valid || render == nullptr || !render->IsReady() || !IsShown())
                return;
            const u32 w = m_view->RenderWidth();
            const u32 h = m_view->RenderHeight();
            // RenderTexture canvases stay live in editing viewports too: an in-world screen must not
            // show stale content while its scene is edited (Simulate renders through here as well).
            // The subsystems draw at most once a frame, so several pages share one draw of each.
            if (m_services.gameUI != nullptr)
                m_services.gameUI->RenderCanvasTextures(*frame.encoder, static_cast<i32>(frame.frameIndex));
            if (m_services.gameGui != nullptr)
                m_services.gameGui->RenderTextures(*frame.encoder, frame.frameIndex);

            foundation::render::CameraOverride cameraOverride;
            cameraOverride.camera = ViewCamera();
            // The viewport's colour is a UI colour (sRGB); the render view takes linear.
            const foundation::rhi::ClearColor clear = m_view->LinearClearColor();
            cameraOverride.clearColor = Color{clear.r, clear.g, clear.b, clear.a};
            // The graph imports the colour target and owns its transitions: from the viewport's
            // state (Undefined right after it is made again) to ShaderRead for the GUI to sample.
            foundation::render::TargetState target;
            target.texture = m_view->ColorTexture();
            target.currentState = m_view->ColorState();
            target.finalState = foundation::rhi::ResourceState::ShaderRead;
            render->RenderScene(scene, m_view->ColorView(), m_view->ColorFormat(), w, h, foundation::render::ViewportRect{0, 0, w, h}, &cameraOverride,
                                target, post, Key(), debugView);
            m_view->SetColorState(foundation::rhi::ResourceState::ShaderRead);
            m_renderedThisFrame = true;
            m_captureWidth = w;
            m_captureHeight = h;
        }

        /// A capture asked for, recorded once the renderer composed the frame (RenderScene only
        /// adds the view; EndRendering, before this, writes the image).
        void AfterRender(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame)
        {
            const bool rendered = m_renderedThisFrame;
            m_renderedThisFrame = false;
            if (!rendered || !m_capture.Armed() || host.Graphics() == nullptr)
                return;
            m_capture.Record(host.Graphics()->Raw(), frame.encoder, m_view->ColorTexture(), m_view->ColorFormat(), m_captureWidth, m_captureHeight,
                             foundation::rhi::ResourceState::ShaderRead);
            m_cleanCapture = false;
        }
        /// A capture recorded last frame, written once the GPU finished the copy.
        void CompleteCapture(foundation::runtime::IApplicationHost& host)
        {
            m_capture.Complete(host.Graphics() != nullptr ? host.Graphics()->Raw() : nullptr, host.Ctx().Allocator());
        }
        /// The next rendered frame to `path`; `clean`, without the editor's overlays.
        Status RequestCapture(StringView path, bool clean)
        {
            m_capture.Request(path);
            m_cleanCapture = clean;
            return Status{};
        }
        /// The frame an armed clean capture records: the overlays held back.
        [[nodiscard]] bool CleanCaptureFrame() const noexcept { return m_cleanCapture && m_capture.Armed(); }
        [[nodiscard]] const ViewportCapture& LastCapture() const noexcept { return m_capture.State(); }

        /// Frames `entities`, each with everything under it, as the scene's systems measure them
        /// (a small box at an entity's place when nothing does); false when none resolves.
        bool Frame(foundation::scene::Scene& scene, Span<const Guid> entities, bool ease, IAllocator& allocator)
        {
            AABB merged = AABB::Empty();
            bool any = false;
            for (const Guid& id : entities)
            {
                const foundation::scene::EntityHandle e = scene.FindEntity(id);
                if (!scene.IsValid(e))
                    continue;
                AABB box = AABB::Empty();
                bool measured = false;
                Array<foundation::scene::EntityHandle> pending(allocator);
                pending.PushBack(e);
                while (!pending.IsEmpty())
                {
                    const foundation::scene::EntityHandle current = pending[pending.Size() - 1];
                    pending.PopBack();
                    AABB part;
                    if (foundation::scene::EntityWorldBounds(scene, current, part))
                    {
                        box = measured ? Merge(box, part) : part;
                        measured = true;
                    }
                    for (foundation::scene::EntityHandle child = scene.GetFirstChild(current); scene.IsValid(child); child = scene.GetNextSibling(child))
                        pending.PushBack(child);
                }
                if (!measured)
                {
                    // Nothing in it measures (an empty, a light): its place, the marker's size.
                    const Float4x4 world = scene.GetWorldMatrix(e);
                    box = AABB::FromCenterExtents(Float3{world.m[3][0], world.m[3][1], world.m[3][2]}, Float3{0.35f, 0.35f, 0.35f});
                }
                merged = any ? Merge(merged, box) : box;
                any = true;
            }
            if (!any)
                return false;
            m_camera.FrameSphere(merged.Center(), Length(merged.Extents()), m_camera.fovY, ease);
            return true;
        }

        /// Before the page goes: the camera never closes captured, the picks keyed by it are
        /// cancelled, a picture recorded and not yet written is written (it waits for the GPU: a
        /// save's launcher picture, the editor closing that same frame, was freed while the copy
        /// was still running), then the readback buffer goes while the device lives.
        void Close(foundation::runtime::IApplicationHost& host)
        {
            m_camera.ReleaseCapture(Mouse());
            if (m_services.render != nullptr)
                m_services.render->CancelPicks(Key());
            if (host.Graphics() != nullptr && host.Graphics()->Raw() != nullptr)
            {
                CompleteCapture(host);
                m_capture.Release(host.Graphics()->Raw());
            }
        }

    private:
        // A playtest's pointer is over the view while it is inside its targets.
        [[nodiscard]] bool ScriptedPointerInside() const
        {
            const foundation::shell::IMouse* mouse = m_scripted->Mouse();
            return mouse->X() >= 0.0f && mouse->Y() >= 0.0f && mouse->X() < static_cast<f32>(m_view->RenderWidth()) &&
                   mouse->Y() < static_cast<f32>(m_view->RenderHeight());
        }

        // The GPU pick seam the tools see: the renderer's picks keyed by this viewport (the key its
        // RenderScene carries), hits decoded to entity handles.
        class ViewportPicker final : public IViewportPicker
        {
        public:
            explicit ViewportPicker(SceneViewport& viewport) noexcept : m_viewport(&viewport) {}
            [[nodiscard]] u32 RequestPick(i32 x, i32 y, u32 width, u32 height) override
            {
                engine::render::RenderSubsystem* render = m_viewport->m_services.render;
                if (render == nullptr || !render->IsReady())
                    return 0;
                return render->RequestPick(m_viewport->Key(), x, y, width, height);
            }
            [[nodiscard]] bool TryTakePick(u32 request, Array<foundation::scene::EntityHandle>& hits) override
            {
                engine::render::RenderSubsystem* render = m_viewport->m_services.render;
                if (render == nullptr)
                    return false;
                foundation::render::PickResult result;
                if (!render->TryTakePickResult(request, result))
                    return false;
                hits.Clear();
                for (const foundation::render::PickHit& hit : result.hits)
                    hits.PushBack(foundation::scene::EntityHandle{hit.entityIndex, hit.generation});
                return true;
            }

        private:
            SceneViewport* m_viewport;
        };

        Services m_services;
        RefPtr<foundation::gui::Viewport> m_view;
        EditorCamera m_camera;
        UniquePtr<foundation::input::ScriptedInputSource> m_scripted; // a playtest's devices (BeginScripted)
        f64 m_scriptedTime = 0.0;
        bool m_scriptedEnding = false;
        bool m_scriptedReleased = false;
        ViewportPicker m_picker;
        ViewportCaptureRecorder m_capture;
        bool m_cleanCapture = false;
        bool m_renderedThisFrame = false; // Render added the view this frame, at:
        u32 m_captureWidth = 0;
        u32 m_captureHeight = 0;
    };
}

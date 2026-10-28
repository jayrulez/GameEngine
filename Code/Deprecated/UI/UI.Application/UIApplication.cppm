// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::UI.Application - the `foundation.ui.application` module.
//
// The docking / workbench layer: RuntimeDockableWindowHost implements the toolkit's IDockableWindowHost in
// terms of the runtime host (IApplicationHost::OpenWindow/CloseWindow) + the reusable UIHost + the shell's
// window geometry / global mouse. A DockManager's floating panels become real OS windows (OS-chromed on
// Linux, borderless with app-drawn chrome elsewhere - see UsesOSChrome), each
// its own RootView attached to the shared UIHost. This is the ONLY UI module that pulls in
// foundation.ui.toolkit, so docking cannot bleed into games (which never link it) or into the toolkit-free
// foundation.ui.shell / foundation.ui.runtime layers.
//
// Usage: construct once with the app's IApplicationHost + its UIHost, then
//   dockManager->DockableWindowHost = &host;
// Floating a panel opens an OS window; TryGetDockableWindowBounds/Move/Resize keep it in sync during
// drag/resize (all position/size writes are ATOMIC - per-axis writes race on async X11).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // Cast<DockPanelDragData> for the drag-follow

export module foundation.ui.application;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.runtime.client;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;

namespace core = foundation::core;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
namespace ui = foundation::ui;

// foundation::ui::application nests in foundation::ui, so View / RootView resolve unqualified and
// toolkit::* / runtime-host types resolve via the parent namespace.
export namespace foundation::ui::application
{
    using core::f32;
    using core::i32;
    using core::u32;
    using core::usize;

    /// Platform default for dockable-window chrome: Linux window
    /// systems (X11/Wayland) get OS-chromed floats - Wayland punishes app-positioned borderless
    /// windows and XWayland blocks cross-monitor drags; no true-X11 special case (one platform
    /// default). Everything else keeps borderless floats with app-drawn chrome.
    [[nodiscard]] constexpr bool PrefersOSChromedDockables(shell::WindowSystem system) noexcept
    {
        return system == shell::WindowSystem::X11 || system == shell::WindowSystem::Wayland;
    }

    /// Implements the toolkit docking host on the runtime's multi-window graphics host. Assign to a
    /// DockManager's DockableWindowHost; floated panels become OS windows drawn by the UIHost
    /// (OS-chromed on Linux, borderless elsewhere - see UsesOSChrome).
    class RuntimeDockableWindowHost final : public toolkit::IDockableWindowHost
    {
    public:
        RuntimeDockableWindowHost(foundation::runtime::IApplicationHost& host,
                                  ui::runtime::UIHost& uiHost) noexcept
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        RuntimeDockableWindowHost(const RuntimeDockableWindowHost&) = delete;
        RuntimeDockableWindowHost& operator=(const RuntimeDockableWindowHost&) = delete;

        [[nodiscard]] bool SupportsOSWindows() override { return true; }

        /// Linux (X11/Wayland) floats default to OS-chromed windows; borderless (app-drawn
        /// chrome + app-driven move) is the default elsewhere (Wayland punishes app-positioned
        /// borderless windows, XWayland blocks cross-monitor drags). Override via
        /// SetOSChromeOverride for tests or an explicit setting.
        [[nodiscard]] bool UsesOSChrome() override
        {
            if (m_osChromeOverride.HasValue())
            {
                return m_osChromeOverride.Value();
            }
            if (m_host != nullptr)
            {
                if (shell::IShell* sh = m_host->Shell())
                {
                    if (shell::IWindow* mw = sh->MainWindow())
                    {
                        return PrefersOSChromedDockables(mw->Native().system);
                    }
                }
            }
            return false;
        }

        void SetOSChromeOverride(bool value) { m_osChromeOverride = value; }

        void CreateDockableWindow(View* dockableWindow, f32 width, f32 height, f32 x, f32 y,
                                  core::Function<void(View*)> onCloseRequested = {}) override
        {
            if (dockableWindow == nullptr || m_host == nullptr)
            {
                return;
            }

            i32 mainX = 0, mainY = 0;
            MainOrigin(mainX, mainY);

            // Chromed windows show a real OS title bar - propagate the panel's title.
            core::String title(u8"Panel");
            if (auto* fw = Cast<toolkit::DockableWindow>(dockableWindow))
            {
                if (fw->Panel() != nullptr && fw->Panel()->Title().Size() > 0)
                {
                    title = core::String(fw->Panel()->Title());
                }
            }

            shell::WindowSettings ws;
            ws.title = title.AsView();
            ws.width = static_cast<u32>(width > 1.0f ? width : 1.0f);
            ws.height = static_cast<u32>(height > 1.0f ? height : 1.0f);
            ws.positioned = true;
            ws.x = mainX + static_cast<i32>(x);
            ws.y = mainY + static_cast<i32>(y);
            // Borderless mode: the DockablePanel draws its own title bar. Chromed mode: the OS
            // provides title bar / close / resize borders.
            ws.borderless = !UsesOSChrome();
            ws.resizable = true;

            graphics::RenderWindow* rw = m_host->OpenWindow(ws, graphics::RenderWindowDesc{});
            if (rw == nullptr)
            {
                return;
            }

            // The OS window's RootView owns the dockable-window view (the DockManager keeps only a raw ref).
            core::RefPtr<RootView> root = core::MakeRef<RootView>(core::DefaultAllocator());
            root->AddView(dockableWindow);
            m_uiHost->AttachWindow(rw, root);

            m_entries.PushBack(Entry{dockableWindow, rw, root,
                                     static_cast<core::Function<void(View*)>&&>(onCloseRequested)});
        }

        void DestroyDockableWindow(View* dockableWindow) override
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (m_entries[i].view == dockableWindow)
                {
                    graphics::RenderWindow* rw = m_entries[i].rw;
                    m_uiHost->DetachWindow(
                        rw); // logical detach; payload stays for the window teardown
                    m_host->CloseWindow(
                        rw); // deferred: RenderWindow dtor WaitIdles + frees the payload
                    m_entries.RemoveAt(
                        i); // drops our root ref (the payload still holds one until close)
                    return;
                }
            }
        }

        void MoveDockableWindow(View* dockableWindow, f32 x, f32 y) override
        {
            if (Entry* e = Find(dockableWindow))
            {
                i32 mainX = 0, mainY = 0;
                MainOrigin(mainX, mainY);
                e->rw->Window().SetPosition(mainX + static_cast<i32>(x),
                                            mainY + static_cast<i32>(y)); // atomic
            }
        }

        void ResizeDockableWindow(View* dockableWindow, f32 x, f32 y, f32 width,
                                  f32 height) override
        {
            if (Entry* e = Find(dockableWindow))
            {
                i32 mainX = 0, mainY = 0;
                MainOrigin(mainX, mainY);
                e->rw->Window().SetPosition(mainX + static_cast<i32>(x),
                                            mainY + static_cast<i32>(y)); // atomic
                e->rw->Window().SetSize(static_cast<u32>(width > 1.0f ? width : 1.0f),
                                        static_cast<u32>(height > 1.0f ? height : 1.0f)); // atomic
            }
        }

        [[nodiscard]] bool TryGetDockableWindowBounds(View* dockableWindow, f32& x, f32& y,
                                                      f32& width, f32& height) override
        {
            if (Entry* e = Find(dockableWindow))
            {
                i32 mainX = 0, mainY = 0;
                MainOrigin(mainX, mainY);
                shell::IWindow& win = e->rw->Window();
                x = static_cast<f32>(win.X() - mainX);
                y = static_cast<f32>(win.Y() - mainY);
                width = static_cast<f32>(win.Width());
                height = static_cast<f32>(win.Height());
                return true;
            }
            x = 0;
            y = 0;
            width = 0;
            height = 0;
            return false;
        }

        void GetGlobalMousePosition(f32& globalX, f32& globalY) override
        {
            if (m_host != nullptr)
            {
                if (shell::IShell* sh = m_host->Shell())
                {
                    if (shell::IInputManager* in = sh->Input())
                    {
                        if (shell::IMouse* mouse = in->Mouse())
                        {
                            globalX = mouse->GlobalX();
                            globalY = mouse->GlobalY();
                            return;
                        }
                    }
                }
            }
            globalX = 0.0f;
            globalY = 0.0f;
        }

        /// Per-frame drag-follow for floating OS windows: while a dock-panel drag from one of our OS
        /// windows is active, move that window so the grab point stays under the desktop-global cursor.
        /// Borderless floats have no WM title bar to drag, and the DockManager delegates OS-window movement
        /// to the app (its OnDragOver is a no-op for IsOSWindow), so - like Sedulous's editor - we move it
        /// here. Call once per frame from the app's OnUpdate.
        void Tick()
        {
            // Route native close-button clicks to the toolkit's close flow. Only chromed windows
            // have an OS close button, but matching is harmless either way (the shell posts
            // CloseRequested per window id; non-dockable ids never match our entries).
            DispatchCloseRequests();

            DragDropManager* dd = m_uiHost->Context().DragDrop();
            if (dd == nullptr)
            {
                return;
            }

            // Under OS chrome the float must NOT chase the cursor during a re-dock drag (that
            // app-driven-move pattern is exactly what Wayland punishes); the drag-drop adorner is
            // the in-flight visual instead, and the window stays where the OS put it.
            if (UsesOSChrome())
            {
                m_dragWindow = nullptr;
                return;
            }

            if (!dd->IsDragging())
            {
                m_dragWindow = nullptr; // drag ended (or none active)
                return;
            }

            // Latch the dragged OS window + grab offset once, when the drag begins. Offset = how far the
            // cursor sits from the window's top-left at grab (in global coords); keeping it fixed pins the
            // grab point to the window as it follows.
            if (m_dragWindow == nullptr)
            {
                if (auto* pd = Cast<toolkit::DockPanelDragData>(dd->CurrentDragData()))
                {
                    if (pd->SourceWindow != nullptr)
                    {
                        if (Entry* e = Find(pd->SourceWindow))
                        {
                            f32 gx = 0.0f, gy = 0.0f;
                            GetGlobalMousePosition(gx, gy);
                            m_dragWindow = e->rw;
                            m_dragOffX = gx - static_cast<f32>(e->rw->Window().X());
                            m_dragOffY = gy - static_cast<f32>(e->rw->Window().Y());
                        }
                    }
                }
            }

            if (m_dragWindow != nullptr)
            {
                f32 gx = 0.0f, gy = 0.0f;
                GetGlobalMousePosition(gx, gy);
                m_dragWindow->Window().SetPosition(
                    static_cast<i32>(gx - m_dragOffX),
                    static_cast<i32>(gy - m_dragOffY)); // atomic, global coords
            }
        }

    private:
        struct Entry
        {
            View* view = nullptr;                 // borrowed (RootView owns it)
            graphics::RenderWindow* rw = nullptr; // borrowed (IApplicationHost owns it)
            core::RefPtr<RootView> root;
            core::Function<void(View*)> onClose;
        };

        [[nodiscard]] Entry* Find(View* view)
        {
            for (Entry& e : m_entries)
            {
                if (e.view == view)
                {
                    return &e;
                }
            }
            return nullptr;
        }

        /// Deliver this frame's per-window CloseRequested shell events (OS close button) to the
        /// matching entries' onClose callbacks. The callback typically tears the entry down
        /// (DestroyDockableWindow), so matches are collected first and each callback is moved out
        /// of its entry before invoking.
        void DispatchCloseRequests()
        {
            if (m_host == nullptr)
            {
                return;
            }
            shell::IShell* sh = m_host->Shell();
            if (sh == nullptr || sh->WindowManager() == nullptr)
            {
                return;
            }

            core::Array<View*> closing;
            for (const shell::WindowEvent& we : sh->WindowManager()->Events())
            {
                if (we.type != shell::WindowEventType::CloseRequested)
                {
                    continue;
                }
                for (Entry& e : m_entries)
                {
                    if (e.rw->Window().Id() == we.windowId)
                    {
                        closing.PushBack(e.view);
                        break;
                    }
                }
            }

            for (View* view : closing)
            {
                if (Entry* e = Find(view)) // re-find: an earlier callback may have removed it
                {
                    if (e->onClose)
                    {
                        core::Function<void(View*)> onClose = core::Move(e->onClose);
                        onClose(view);
                    }
                }
            }
        }

        // The main window's screen-space top-left; dockable-window positions are relative to it.
        void MainOrigin(i32& x, i32& y) const
        {
            x = 0;
            y = 0;
            if (m_host != nullptr)
            {
                if (shell::IShell* sh = m_host->Shell())
                {
                    if (shell::IWindow* mw = sh->MainWindow())
                    {
                        x = mw->X();
                        y = mw->Y();
                    }
                }
            }
        }

        foundation::runtime::IApplicationHost* m_host; // borrowed
        ui::runtime::UIHost* m_uiHost;               // borrowed (the app owns it)
        core::Array<Entry> m_entries;

        // Drag-follow state (Tick): the OS window currently being dragged + the grab offset.
        graphics::RenderWindow* m_dragWindow = nullptr;
        f32 m_dragOffX = 0.0f;
        f32 m_dragOffY = 0.0f;

        // Explicit chrome-policy override (tests / user setting); unset = platform default.
        core::Optional<bool> m_osChromeOverride;
    };
}

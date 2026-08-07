// Draconic::UIApplication - the `draconic.ui.application` module.
//
// The docking / workbench layer: RuntimeDockableWindowHost implements the toolkit's IDockableWindowHost in
// terms of the runtime host (IApplicationHost::OpenWindow/CloseWindow) + the reusable UIHost + the shell's
// window geometry / global mouse. A DockManager's floating panels become real borderless OS windows, each
// its own RootView attached to the shared UIHost. This is the ONLY UI module that pulls in
// draconic.ui.toolkit, so docking cannot bleed into games (which never link it) or into the toolkit-free
// draconic.ui.shell / draconic.ui.runtime layers.
//
// Usage: construct once with the app's IApplicationHost + its UIHost, then
//   dockManager->DockableWindowHost = &host;
// Floating a panel opens an OS window; TryGetDockableWindowBounds/Move/Resize keep it in sync during
// drag/resize (all position/size writes are ATOMIC - per-axis writes race on async X11).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h" // Cast<DockPanelDragData> for the drag-follow

export module draconic.ui.application;

import draconic.foundation;
import draconic.shell;
import draconic.graphics;
import draconic.runtime.client;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;

namespace foundation = draconic::foundation;
namespace shell = draconic::shell;
namespace graphics = draconic::graphics;
namespace ui = draconic::ui;

// draconic::ui::application nests in draconic::ui, so View / RootView resolve unqualified and
// toolkit::* / runtime-host types resolve via the parent namespace.
export namespace draconic::ui::application
{
    using foundation::f32;
    using foundation::i32;
    using foundation::u32;
    using foundation::usize;

    /// Implements the toolkit docking host on the runtime's multi-window graphics host. Assign to a
    /// DockManager's DockableWindowHost; floated panels become borderless OS windows drawn by the UIHost.
    class RuntimeDockableWindowHost final : public toolkit::IDockableWindowHost
    {
    public:
        RuntimeDockableWindowHost(draconic::runtime::IApplicationHost& host,
                                  ui::runtime::UIHost& uiHost) noexcept
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        RuntimeDockableWindowHost(const RuntimeDockableWindowHost&) = delete;
        RuntimeDockableWindowHost& operator=(const RuntimeDockableWindowHost&) = delete;

        [[nodiscard]] bool SupportsOSWindows() override { return true; }

        void CreateDockableWindow(View* dockableWindow, f32 width, f32 height, f32 x, f32 y,
                                  foundation::Function<void(View*)> onCloseRequested = {}) override
        {
            if (dockableWindow == nullptr || m_host == nullptr)
            {
                return;
            }

            i32 mainX = 0, mainY = 0;
            MainOrigin(mainX, mainY);

            shell::WindowSettings ws;
            ws.title = u8"Panel";
            ws.width = static_cast<u32>(width > 1.0f ? width : 1.0f);
            ws.height = static_cast<u32>(height > 1.0f ? height : 1.0f);
            ws.positioned = true;
            ws.x = mainX + static_cast<i32>(x);
            ws.y = mainY + static_cast<i32>(y);
            ws.borderless = true; // the DockablePanel draws its own title bar
            ws.resizable = true;

            graphics::RenderWindow* rw = m_host->OpenWindow(ws, graphics::RenderWindowDesc{});
            if (rw == nullptr)
            {
                return;
            }

            // The OS window's RootView owns the dockable-window view (the DockManager keeps only a raw ref).
            foundation::RefPtr<RootView> root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
            root->AddView(dockableWindow);
            m_uiHost->AttachWindow(rw, root);

            m_entries.PushBack(Entry{dockableWindow, rw, root,
                                     static_cast<foundation::Function<void(View*)>&&>(onCloseRequested)});
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
            DragDropManager* dd = m_uiHost->Context().DragDrop();
            if (dd == nullptr)
            {
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
            foundation::RefPtr<RootView> root;
            foundation::Function<void(View*)> onClose;
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

        draconic::runtime::IApplicationHost* m_host; // borrowed
        ui::runtime::UIHost* m_uiHost;               // borrowed (the app owns it)
        foundation::Array<Entry> m_entries;

        // Drag-follow state (Tick): the OS window currently being dragged + the grab offset.
        graphics::RenderWindow* m_dragWindow = nullptr;
        f32 m_dragOffX = 0.0f;
        f32 m_dragOffY = 0.0f;
    };
}

// Draconic::ShellWeb - `draconic.shell.web:window_manager`.
//
// One canvas => one window. The main window is created at construction; additional CreateWindow
// calls fail (a browser page has a single WebGPU canvas here - multi-canvas is a later concern).
// Pump() re-polls the canvas size each frame and emits a Resized event when it changed, so the
// runner reconfigures the swapchain (the browser has no OS resize event queue to drain).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell.web:window_manager;

import draconic.foundation;
import draconic.shell;
import :window;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class WebWindowManager final : public IWindowManager
    {
    public:
        WebWindowManager(foundation::StringView selector, const WindowSettings& main)
        {
            auto window =
                foundation::MakeUnique<WebWindow>(foundation::DefaultAllocator(), m_nextId, selector, main);
            m_mainWindowId = m_nextId++;
            m_window = window.Get();
            m_owned = static_cast<foundation::UniquePtr<WebWindow>&&>(window);
            m_live.PushBack(m_window);
        }

        [[nodiscard]] foundation::Result<IWindow*> CreateWindow(const WindowSettings&) override
        {
            return foundation::Err(foundation::ErrorCode::NotSupported); // single canvas
        }
        void DestroyWindow(IWindow* window) override
        {
            if (window == m_window)
            {
                m_window->Close();
            }
        }
        [[nodiscard]] foundation::Span<IWindow* const> Windows() const noexcept override
        {
            return foundation::Span<IWindow* const>(m_live.Data(), m_live.Size());
        }
        [[nodiscard]] IWindow* MainWindow() const noexcept override
        {
            return m_window != nullptr && m_window->IsOpen() ? m_window : nullptr;
        }
        [[nodiscard]] IWindow* GetWindow(foundation::u32 id) const noexcept override
        {
            return id == m_mainWindowId ? m_window : nullptr;
        }
        [[nodiscard]] foundation::Span<const WindowEvent> Events() const noexcept override
        {
            return foundation::Span<const WindowEvent>(m_events.Data(), m_events.Size());
        }
        void FlushDestroyed() override {}

        // Called from the shell's ProcessEvents: rebuild this frame's event list from a size poll.
        void Pump()
        {
            m_events.Clear();
            if (m_window != nullptr && m_window->QuerySize())
            {
                WindowEvent event;
                event.type = WindowEventType::Resized;
                event.windowId = m_mainWindowId;
                event.width = m_window->Width();
                event.height = m_window->Height();
                m_events.PushBack(event);
            }
        }

    private:
        foundation::UniquePtr<WebWindow> m_owned;
        WebWindow* m_window = nullptr;
        foundation::Array<IWindow*> m_live;
        foundation::Array<WindowEvent> m_events;
        foundation::u32 m_nextId = 1;
        foundation::u32 m_mainWindowId = 0;
    };
}

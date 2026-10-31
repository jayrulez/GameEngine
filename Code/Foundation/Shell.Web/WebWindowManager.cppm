// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell.Web - `foundation.shell.web:window_manager`.
//
// One canvas, one window. The main window is created at construction; additional CreateWindow
// calls fail (a browser page has a single WebGPU canvas here).
// Pump() re-polls the canvas size each frame and emits a Resized event when it changed, so the
// runner reconfigures the swapchain (the browser has no OS resize event queue to drain).

module;
#include "Core/Prelude.h"

export module foundation.shell.web:window_manager;

import foundation.core;
import foundation.shell;
import :window;

namespace core = foundation::core;

export namespace foundation::shell
{
    class WebWindowManager final : public IWindowManager
    {
    public:
        WebWindowManager(core::StringView selector, const WindowSettings& main)
        {
            auto window =
                core::MakeUnique<WebWindow>(core::DefaultAllocator(), m_nextId, selector, main);
            m_mainWindowId = m_nextId++;
            m_window = window.Get();
            m_owned = static_cast<core::UniquePtr<WebWindow>&&>(window);
            m_live.PushBack(m_window);
        }

        [[nodiscard]] core::Result<IWindow*> CreateWindow(const WindowSettings&) override
        {
            return core::Err(core::ErrorCode::NotSupported); // single canvas
        }
        void DestroyWindow(IWindow* window) override
        {
            if (window == m_window)
            {
                m_window->Close();
            }
        }
        [[nodiscard]] core::Span<IWindow* const> Windows() const noexcept override
        {
            return core::Span<IWindow* const>(m_live.Data(), m_live.Size());
        }
        [[nodiscard]] IWindow* MainWindow() const noexcept override
        {
            return m_window != nullptr && m_window->IsOpen() ? m_window : nullptr;
        }
        [[nodiscard]] IWindow* GetWindow(core::u32 id) const noexcept override
        {
            return id == m_mainWindowId ? m_window : nullptr;
        }
        [[nodiscard]] core::Span<const WindowEvent> Events() const noexcept override
        {
            return core::Span<const WindowEvent>(m_events.Data(), m_events.Size());
        }
        // One canvas, which opens no other window: no screens to place one on.
        [[nodiscard]] core::Span<const Screen> Screens() const noexcept override { return {}; }
        // The page's one canvas, which it neither places nor gets a second of.
        [[nodiscard]] WindowCapabilities Capabilities() const noexcept override { return {}; }
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
        core::UniquePtr<WebWindow> m_owned;
        WebWindow* m_window = nullptr;
        core::Array<IWindow*> m_live;
        core::Array<WindowEvent> m_events;
        core::u32 m_nextId = 1;
        core::u32 m_mainWindowId = 0;
    };
}

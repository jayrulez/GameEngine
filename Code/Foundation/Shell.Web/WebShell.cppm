// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell.Web - the `foundation.shell.web` module (primary interface unit).
//
// A self-contained IShell backed by a single HTML <canvas> under Emscripten: the web counterpart
// of the SDL3 desktop backend. The pieces live in partitions: :window (the canvas window),
// :window_manager (the single-window host + resize pump), :input (the input devices), :dialogs (the
// file-dialog service). This unit re-exports them and composes WebShell. The frame cadence is not
// here: the browser drives it through foundation.runtime.web (emscripten_set_main_loop).

module;
#include "Core/Prelude.h"

export module foundation.shell.web;

export import :window;
export import :window_manager;
export import :input;
export import :dialogs;

import foundation.core;
import foundation.shell;

namespace core = foundation::core;

export namespace foundation::shell
{
    class WebShell final : public IShell
    {
    public:
        // The selector defaults to Emscripten's canonical "#canvas". The app can point at a
        // different canvas by constructing with an explicit selector.
        explicit WebShell(core::StringView selector = u8"#canvas", const WindowSettings& settings = {})
            : m_windows(selector, settings)
        {
            // Wire the HTML5 keyboard/mouse callbacks to the canvas now that it exists.
            if (IWindow* main = m_windows.MainWindow())
            {
                m_input.RegisterCallbacks(selector, main->Id());
            }
        }

        [[nodiscard]] IWindowManager* WindowManager() noexcept override { return &m_windows; }
        [[nodiscard]] IWindow* MainWindow() noexcept override { return m_windows.MainWindow(); }
        [[nodiscard]] IInputManager* Input() noexcept override { return &m_input; }
        [[nodiscard]] IDialogService* Dialogs() noexcept override { return &m_dialogs; }
        void ProcessEvents() override
        {
            m_input.Update();  // roll the input frame and drain the async HTML5 event queue
            m_windows.Pump();  // poll the canvas size and emit Resized events
        }
        [[nodiscard]] bool IsRunning() const noexcept override
        {
            IWindow* main = m_windows.MainWindow();
            return m_running && main != nullptr && main->IsOpen();
        }
        void RequestExit() override { m_running = false; }

        // In-memory clipboard; the async navigator.clipboard bridge is not wired.
        void SetClipboardText(core::StringView text) override { m_clipboard = core::String(text); }
        [[nodiscard]] core::String GetClipboardText() const override { return m_clipboard; }
        [[nodiscard]] bool HasClipboardText() const noexcept override
        {
            return m_clipboard.Size() > 0;
        }

    private:
        WebWindowManager m_windows;
        WebInputManager m_input;
        WebDialogService m_dialogs;
        bool m_running = true;
        core::String m_clipboard;
    };
}

// Draconic::ShellWeb - the `draconic.shell.web` module (primary interface unit).
//
// A self-contained IShell backed by a single HTML <canvas> under Emscripten - the web counterpart
// of the SDL3 desktop backend. The pieces live in partitions: :window (the canvas window),
// :window_manager (the single-window host + resize pump), :input (the input devices), :dialogs (the
// file-dialog service). This unit re-exports them and composes WebShell. The frame cadence is NOT
// here: the browser drives it through draconic.runtime.web (emscripten_set_main_loop).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell.web;

export import :window;
export import :window_manager;
export import :input;
export import :dialogs;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class WebShell final : public IShell
    {
    public:
        // The selector defaults to Emscripten's canonical "#canvas". The app can point at a
        // different canvas by constructing with an explicit selector.
        explicit WebShell(foundation::StringView selector = u8"#canvas", const WindowSettings& settings = {})
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
            m_input.Update();  // roll the input frame + drain the async HTML5 event queue
            m_windows.Pump();  // canvas-size poll -> Resized events
        }
        [[nodiscard]] bool IsRunning() const noexcept override
        {
            IWindow* main = m_windows.MainWindow();
            return m_running && main != nullptr && main->IsOpen();
        }
        void RequestExit() override { m_running = false; }

        // In-memory clipboard for now; the async navigator.clipboard bridge is a later pass.
        void SetClipboardText(foundation::StringView text) override { m_clipboard = foundation::String(text); }
        [[nodiscard]] foundation::String GetClipboardText() const override { return m_clipboard; }
        [[nodiscard]] bool HasClipboardText() const noexcept override
        {
            return m_clipboard.Size() > 0;
        }

    private:
        WebWindowManager m_windows;
        WebInputManager m_input;
        WebDialogService m_dialogs;
        bool m_running = true;
        foundation::String m_clipboard;
    };
}

// Draconic::ShellNull - the `draconic.shell.null` module.
//
// A headless IShell implementation: no real window or OS events. Useful for
// tests, tools, and headless servers, and as the reference for what a real
// backend must provide. ProcessEvents is a no-op; IsRunning stays true until
// RequestExit (or the main window is closed), so the runner relies on the
// Application requesting exit to terminate. The window manager is fully
// functional headless (create/destroy/resize) so multi-window logic is testable
// without an OS.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell.null;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class NullWindow final : public IWindow
    {
    public:
        NullWindow(foundation::u32 id, const WindowSettings& settings) noexcept
            : m_id(id), m_width(settings.width), m_height(settings.height),
              m_x(settings.positioned ? settings.x : 0), m_y(settings.positioned ? settings.y : 0)
        {
        }

        [[nodiscard]] foundation::u32 Id() const noexcept override { return m_id; }
        [[nodiscard]] foundation::u32 Width() const noexcept override { return m_width; }
        [[nodiscard]] foundation::u32 Height() const noexcept override { return m_height; }
        [[nodiscard]] foundation::i32 X() const noexcept override { return m_x; }
        [[nodiscard]] foundation::i32 Y() const noexcept override { return m_y; }
        void SetPosition(foundation::i32 x, foundation::i32 y) override
        {
            m_x = x;
            m_y = y;
        } // headless: just record
        void SetSize(foundation::u32 width, foundation::u32 height) override
        {
            m_width = width;
            m_height = height;
        }
        [[nodiscard]] foundation::f32 ContentScale() const noexcept override { return 1.0f; }
        [[nodiscard]] NativeWindow Native() const noexcept override
        {
            return {};
        } // headless: no handles
        [[nodiscard]] bool IsOpen() const noexcept override { return m_open; }
        [[nodiscard]] bool IsMinimized() const noexcept override { return m_minimized; }
        void Close() override { m_open = false; }

        void StartTextInput() override { m_textInputActive = true; }
        void StopTextInput() override { m_textInputActive = false; }
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return m_textInputActive; }

        // --- test/headless controls (no OS to drive these) ---
        void Resize(foundation::u32 w, foundation::u32 h) noexcept
        {
            m_width = w;
            m_height = h;
        }
        void SetMinimized(bool m) noexcept { m_minimized = m; }

    private:
        foundation::u32 m_id;
        foundation::u32 m_width;
        foundation::u32 m_height;
        foundation::i32 m_x = 0;
        foundation::i32 m_y = 0;
        bool m_open = true;
        bool m_minimized = false;
        bool m_textInputActive = false;
    };

    class NullWindowManager final : public IWindowManager
    {
    public:
        explicit NullWindowManager(const WindowSettings& main) { (void)CreateWindow(main); }

        [[nodiscard]] foundation::Result<IWindow*> CreateWindow(const WindowSettings& settings) override
        {
            const foundation::u32 id = m_nextId++;
            auto window = foundation::MakeUnique<NullWindow>(foundation::DefaultAllocator(), id, settings);
            IWindow* borrowed = window.Get();
            m_owned.PushBack(static_cast<foundation::UniquePtr<NullWindow>&&>(window));
            m_live.PushBack(borrowed);
            if (m_mainWindowId == 0)
            {
                m_mainWindowId = id;
            } // the first window created is the main window
            return borrowed;
        }

        void DestroyWindow(IWindow* window) override
        {
            if (!Owns(window))
            {
                return;
            } // no-op for null or windows this manager does not own
            window->Close();
            m_pendingDestroy.PushBack(window->Id());
        }

        [[nodiscard]] foundation::Span<IWindow* const> Windows() const noexcept override
        {
            return foundation::Span<IWindow* const>(m_live.Data(), m_live.Size());
        }
        [[nodiscard]] IWindow* MainWindow() const noexcept override
        {
            // Tracked by id, so destroying/flushing the main window never promotes another
            // window into its place; returns null once the main window is gone.
            return GetWindow(m_mainWindowId);
        }
        [[nodiscard]] IWindow* GetWindow(foundation::u32 id) const noexcept override
        {
            for (IWindow* w : m_live)
            {
                if (w->Id() == id)
                {
                    return w;
                }
            }
            return nullptr;
        }
        [[nodiscard]] foundation::Span<const WindowEvent> Events() const noexcept override
        {
            return foundation::Span<const WindowEvent>(m_events.Data(), m_events.Size());
        }

        void FlushDestroyed() override
        {
            for (foundation::u32 id : m_pendingDestroy)
            {
                for (foundation::usize i = 0; i < m_live.Size(); ++i)
                {
                    if (m_live[i]->Id() == id)
                    {
                        m_live.RemoveAt(i);
                        break;
                    }
                }
                for (foundation::usize i = 0; i < m_owned.Size(); ++i)
                {
                    if (m_owned[i]->Id() == id)
                    {
                        m_owned.RemoveAt(i);
                        break;
                    }
                }
            }
            m_pendingDestroy.Clear();
        }

    private:
        // True only for windows this manager owns (present in m_live). Rejects nullptr too, so
        // DestroyWindow() is a no-op for null/unknown windows. Checked by pointer identity, NOT id:
        // a window from another manager can share an id, and acting on it would corrupt bookkeeping.
        [[nodiscard]] bool Owns(IWindow* window) const noexcept
        {
            for (IWindow* w : m_live)
            {
                if (w == window)
                {
                    return true;
                }
            }
            return false;
        }

        foundation::Array<foundation::UniquePtr<NullWindow>> m_owned;
        foundation::Array<IWindow*> m_live;            // borrowed parallel pointers for the span
        foundation::Array<foundation::u32> m_pendingDestroy; // window ids
        foundation::Array<WindowEvent> m_events;       // always empty (no OS event source)
        foundation::u32 m_nextId = 1;
        foundation::u32 m_mainWindowId = 0; // id of the main window (first created); 0 = none
    };

    // No-op input devices: report nothing held/pressed so headless callers can
    // use Input() uniformly without null checks.
    class NullKeyboard final : public IKeyboard
    {
    public:
        [[nodiscard]] bool IsKeyDown(KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyPressed(KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyReleased(KeyCode) const override { return false; }
        [[nodiscard]] KeyModifiers Modifiers() const override { return KeyModifiers::None; }
    };

    class NullMouse final : public IMouse
    {
    public:
        [[nodiscard]] foundation::f32 X() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 Y() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 GlobalX() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 GlobalY() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 DeltaX() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 DeltaY() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 ScrollX() const override { return 0.0f; }
        [[nodiscard]] foundation::f32 ScrollY() const override { return 0.0f; }
        [[nodiscard]] bool IsButtonDown(MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonPressed(MouseButton) const override { return false; }
        [[nodiscard]] bool IsButtonReleased(MouseButton) const override { return false; }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };

    class NullTouch final : public ITouch
    {
    public:
        [[nodiscard]] foundation::i32 TouchCount() const override { return 0; }
        [[nodiscard]] bool GetTouchPoint(foundation::i32, TouchPoint&) const override { return false; }
        [[nodiscard]] bool HasTouch() const override { return false; }
    };

    class NullInputManager final : public IInputManager
    {
    public:
        [[nodiscard]] IKeyboard* Keyboard() override { return &m_keyboard; }
        [[nodiscard]] IMouse* Mouse() override { return &m_mouse; }
        [[nodiscard]] ITouch* Touch() override { return &m_touch; }
        [[nodiscard]] foundation::i32 GamepadCount() const override { return 0; }
        [[nodiscard]] IGamepad* GetGamepad(foundation::i32) override { return nullptr; }
        [[nodiscard]] foundation::Span<const InputEvent> Events() const override { return {}; }
        [[nodiscard]] foundation::u32 HoverWindow() const override { return 0; }
        [[nodiscard]] foundation::u32 FocusedWindow() const override { return 0; }
        void Update() override {}

    private:
        NullKeyboard m_keyboard;
        NullMouse m_mouse;
        NullTouch m_touch;
    };

    // No-op dialogs: cancel immediately (empty result) so headless callers can drive the dialog
    // path uniformly without a backend check.
    class NullDialogService final : public IDialogService
    {
    public:
        void ShowOpenFile(DialogResultCallback callback, foundation::Span<const FileFilter> = {},
                          foundation::StringView = {}, bool = false, foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowSaveFile(DialogResultCallback callback, foundation::Span<const FileFilter> = {},
                          foundation::StringView = {}, foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowOpenFolder(DialogResultCallback callback, foundation::StringView = {}, bool = false,
                            foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void OpenPath(foundation::StringView) override {} // headless: no OS file manager

    private:
        static void Cancel(DialogResultCallback& callback)
        {
            if (callback)
            {
                callback(foundation::Span<const foundation::String>{});
            }
        }
    };

    class NullShell final : public IShell
    {
    public:
        explicit NullShell(const WindowSettings& settings = {}) noexcept : m_windows(settings) {}

        [[nodiscard]] IWindowManager* WindowManager() noexcept override { return &m_windows; }
        [[nodiscard]] IWindow* MainWindow() noexcept override { return m_windows.MainWindow(); }
        [[nodiscard]] IInputManager* Input() noexcept override { return &m_input; }
        [[nodiscard]] IDialogService* Dialogs() noexcept override { return &m_dialogs; }
        void ProcessEvents() override {} // no OS event source
        [[nodiscard]] bool IsRunning() const noexcept override
        {
            // Running until RequestExit() or the main window is closed/destroyed.
            IWindow* main =
                m_windows.MainWindow(); // MainWindow() is const now - no const_cast needed
            return m_running && main != nullptr && main->IsOpen();
        }
        void RequestExit() override { m_running = false; }

        // In-memory clipboard: no OS backing, but round-trips text so headless tests of the
        // GUI clipboard path (cut/copy/paste) work without a windowing system.
        void SetClipboardText(foundation::StringView text) override { m_clipboard = foundation::String(text); }
        [[nodiscard]] foundation::String GetClipboardText() const override { return m_clipboard; }
        [[nodiscard]] bool HasClipboardText() const noexcept override
        {
            return m_clipboard.Size() > 0;
        }

    private:
        NullWindowManager m_windows;
        NullInputManager m_input;
        NullDialogService m_dialogs;
        bool m_running = true;
        foundation::String m_clipboard;
    };
}

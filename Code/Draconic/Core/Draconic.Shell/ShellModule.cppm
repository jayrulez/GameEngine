// Draconic::Shell - the `draconic.shell` module.
//
// IShell is the raw OS/window service - the "shell" (Sedulous's term for it):
// windowing, the OS event pump, run state, and raw input devices (keyboard,
// mouse, gamepad, touch - see the :input / :input_types partitions). It is a
// PASSIVE service, not a subsystem and not the loop owner: the runner drives it
// (ProcessEvents once per frame) and the Application borrows it to wire
// shell-backed subsystems (e.g. a future InputSubsystem). Native backends
// (Win32/Linux/...) implement it; a null backend serves headless and test runs.
// Interfaces only - depends on Foundation, nothing higher.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell;

export import :input_types;
export import :input;
export import :surface;
export import :dialog;

import draconic.foundation;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    enum class WindowSystem : foundation::u8
    {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
        Web,
    };

    // The native handles RHI needs to create a surface/swapchain itself (RHI does
    // surface creation internally - the shell only hands over the handles).
    // Interpretation depends on `system`:
    //   Win32   - display = HINSTANCE,   window = HWND
    //   X11     - display = Display*,     window = Window (XID, via uintptr)
    //   Wayland - display = wl_display*,  window = wl_surface*
    //   Cocoa   - display = nullptr,      window = NSWindow*
    //   Web     - display = nullptr,      window = const char* (HTML canvas CSS selector)
    struct NativeWindow
    {
        WindowSystem system = WindowSystem::Unknown;
        void* display = nullptr;
        void* window = nullptr;
    };

    struct WindowSettings
    {
        foundation::StringView title = u8"Draconic";
        foundation::u32 width = 1280;
        foundation::u32 height = 720;

        // Initial top-left position in screen coordinates. When `positioned` is false the backend
        // places the window (centered) - the default, matching single-window callers. Dockable /
        // floating windows set an explicit position.
        bool positioned = false;
        foundation::i32 x = 0;
        foundation::i32 y = 0;

        // Window chrome. Defaults match prior behavior (resizable, bordered). Floating dockable
        // overlays are typically created borderless.
        bool resizable = true;
        bool borderless = false;
    };

    class IWindow
    {
    public:
        virtual ~IWindow() = default;

        // Stable per-window id, unique within a shell run. Used to route OS
        // events to the right window and to look windows up. 0 is never a valid id.
        [[nodiscard]] virtual foundation::u32 Id() const noexcept = 0;

        [[nodiscard]] virtual foundation::u32 Width() const noexcept = 0;
        [[nodiscard]] virtual foundation::u32 Height() const noexcept = 0;

        // Top-left position in screen coordinates (the same global frame the OS uses). For a
        // single-window app this is just where the window landed; multi-window / dockable code
        // reads it and writes it via SetPosition to place floating windows relative to the main one.
        [[nodiscard]] virtual foundation::i32 X() const noexcept = 0;
        [[nodiscard]] virtual foundation::i32 Y() const noexcept = 0;
        // Move / resize the window. Position is screen-space top-left; size is in the same units as
        // Width()/Height(). No-ops on headless backends (they just record the values).
        virtual void SetPosition(foundation::i32 x, foundation::i32 y) = 0;
        virtual void SetSize(foundation::u32 width, foundation::u32 height) = 0;
        // DPI / content scale (logical-to-physical factor; 1.0 at 100%). A per-window RootView's
        // DpiScale is seeded from this so UI lays out at the right size on HiDPI displays.
        [[nodiscard]] virtual foundation::f32 ContentScale() const noexcept = 0;

        // Native handles for RHI surface creation (see NativeWindow).
        [[nodiscard]] virtual NativeWindow Native() const noexcept = 0;
        [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
        // True while the window is minimized (renderers skip drawing). Resize is
        // detected by polling Width()/Height().
        [[nodiscard]] virtual bool IsMinimized() const noexcept = 0;
        virtual void Close() = 0;

        // Text-input (IME/composition) control. Platforms only emit TextInput events
        // for a window while text input is active; a GUI enables it when an editable
        // control is focused and disables it otherwise. Default off.
        virtual void StartTextInput() = 0;
        virtual void StopTextInput() = 0;
        [[nodiscard]] virtual bool IsTextInputActive() const noexcept = 0;
    };

    // What happened to a window during the last ProcessEvents() pump. Delivered
    // as a per-frame queue (IWindowManager::Events) rather than a callback -
    // matches the pull-based event model and sidesteps callback lifetime in a
    // -fno-exceptions/-fno-rtti world. The consumer (Application) drains it each
    // frame and reacts (resize that window's swapchain, close it, etc.).
    enum class WindowEventType : foundation::u8
    {
        Resized,
        Moved,
        FocusGained,
        FocusLost,
        CloseRequested,
    };

    struct WindowEvent
    {
        WindowEventType type = WindowEventType::Resized;
        foundation::u32 windowId = 0;
        foundation::u32 width = 0;  // Resized
        foundation::u32 height = 0; // Resized
        foundation::i32 x = 0;      // Moved
        foundation::i32 y = 0;      // Moved
    };

    // Owns the set of OS windows for a shell run. One manager per shell;
    // the main window is just the first one created. Windows can be created and
    // destroyed at runtime (the basis for detachable/dockable UI windows).
    // Destruction is DEFERRED: DestroyWindow() marks a window closed, and
    // FlushDestroyed() (called at frame end, after the GPU is done with it)
    // actually frees it - so a window is never torn down mid-frame.
    class IWindowManager
    {
    public:
        virtual ~IWindowManager() = default;

        [[nodiscard]] virtual foundation::Result<IWindow*>
        CreateWindow(const WindowSettings& settings) = 0;
        // Mark a window for destruction at the next FlushDestroyed(). Safe to call
        // mid-frame. No-op if the window is unknown.
        virtual void DestroyWindow(IWindow* window) = 0;

        // All currently-live windows, in creation order (closed-but-not-yet-flushed
        // ones included until FlushDestroyed runs).
        [[nodiscard]] virtual foundation::Span<IWindow* const> Windows() const noexcept = 0;
        // The main window: the first window created, tracked by identity. Returns null once it has
        // been destroyed; it is never re-assigned to a different window (closing the main window is
        // not masked by other open windows).
        [[nodiscard]] virtual IWindow* MainWindow() const noexcept = 0;
        [[nodiscard]] virtual IWindow* GetWindow(foundation::u32 id) const noexcept = 0;

        // Window events accumulated during the last ProcessEvents() pump. Valid
        // until the next pump. Drained by the runner/Application each frame.
        [[nodiscard]] virtual foundation::Span<const WindowEvent> Events() const noexcept = 0;

        // Free windows marked by DestroyWindow(). Call once per frame, at end,
        // after the GPU has finished the frame that may have used them.
        virtual void FlushDestroyed() = 0;
    };

    // One OS file drop: which window, where (window-space), and the absolute path.
    struct DroppedFile
    {
        foundation::u32 window = 0;
        foundation::f32 x = 0.0f, y = 0.0f;
        foundation::String path;
    };

    class IShell
    {
    public:
        virtual ~IShell() = default;

        // The window manager (always present; owns 0..N windows). The main
        // window is WindowManager()->MainWindow().
        [[nodiscard]] virtual IWindowManager* WindowManager() noexcept = 0;

        // Convenience for single-window callers: == WindowManager()->MainWindow().
        // (Kept so single-window hosts like the RHI sample framework are unchanged.)
        [[nodiscard]] virtual IWindow* MainWindow() noexcept = 0;

        // Aggregate input devices (keyboard/mouse/gamepad/touch). Always present
        // - the null backend returns a no-op manager, so callers need not check.
        [[nodiscard]] virtual IInputManager* Input() noexcept = 0;

        // Native file/folder dialogs (open/save/folder). Always present - the null backend
        // returns a no-op service that cancels immediately - so callers need not check.
        [[nodiscard]] virtual IDialogService* Dialogs() noexcept = 0;

        // Pump pending OS events once per frame (the runner calls this). The
        // backend rolls input state (Input()->Update()) before pumping.
        virtual void ProcessEvents() = 0;

        // OS-level run state: false once the shell should quit (e.g. the main
        // window closed). Distinct from Application::IsRunning() (app-level exit).
        [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

        // Consulted when the MAIN window's close is requested (window button / OS quit).
        // Return false to KEEP RUNNING - the app runs its own confirm flow (e.g. unsaved
        // changes) and exits later via the host. Unset = close proceeds (default behavior).
        foundation::Function<bool()> OnMainWindowCloseRequested;

        // Ask the shell to quit (flips IsRunning()).
        virtual void RequestExit() = 0;

        // OS drag-and-drop of files onto a window. Backends queue drops during
        // ProcessEvents; consumers drain once per frame (order preserved). Backends
        // without drop support leave the default no-op.
        virtual void DrainDroppedFiles(foundation::Array<DroppedFile>& /*out*/) {}

        // System clipboard (text). Process-global on every desktop backend (SDL's clipboard
        // is not tied to a window), so it lives on the shell rather than a window - the
        // counterpart to the per-window IME control above. The GUI reaches it through an
        // abstract gui::IClipboard adapter so the core stays platform-agnostic.
        virtual void SetClipboardText(foundation::StringView text) = 0;
        [[nodiscard]] virtual foundation::String GetClipboardText() const = 0;
        [[nodiscard]] virtual bool HasClipboardText() const noexcept = 0;
    };
}

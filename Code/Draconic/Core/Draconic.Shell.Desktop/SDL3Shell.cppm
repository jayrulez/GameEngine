// Draconic::ShellDesktop - the `draconic.shell.desktop` module.
//
// The desktop shell target (Windows/Linux/macOS), implemented on SDL3:
// SDL3Shell covers Wayland, X11, Win32, and Cocoa in one backend, plus input,
// clipboard, and Vulkan-surface creation for RHI. SDL is linked dynamically
// (system/prebuilt) and bundled for distribution. We own the entry point
// (SDL_MAIN_HANDLED), so SDL does not hijack main; SDL_SetMainReady() is called
// before SDL_Init.
//
// Other SDL-based targets (Emscripten, Android) get their own modules/folders:
// they share this SDL3 IShell shape but differ in run loop (callback vs
// blocking), entry point, and build flags. If the impl ends up duplicated it can
// be extracted into a shared module then.
//
// If SDL video init or window creation fails (e.g. no display), the shell
// degrades: MainWindow() is null and IsRunning() is false, so a runner exits
// immediately rather than crashing.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include <cstdint>
#define SDL_MAIN_HANDLED

#include "Draconic.Shell.Desktop/SdlForward.h" // opaque SDL_Window/Cursor/Gamepad (interface uses pointers only)

export module draconic.shell.desktop;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    // Human-readable WSI name for the diagnostic log line (compared against the RHI's surface-WSI log).
    [[nodiscard]] inline foundation::StringView WindowSystemName(WindowSystem s) noexcept
    {
        switch (s)
        {
        case WindowSystem::Win32:
            return u8"Win32";
        case WindowSystem::X11:
            return u8"X11";
        case WindowSystem::Wayland:
            return u8"Wayland";
        case WindowSystem::Cocoa:
            return u8"Cocoa";
        default:
            return u8"Unknown";
        }
    }

    class SDL3Window final : public IWindow
    {
    public:
        explicit SDL3Window(SDL_Window* window) noexcept;
        ~SDL3Window() override;

        SDL3Window(const SDL3Window&) = delete;
        SDL3Window& operator=(const SDL3Window&) = delete;

        [[nodiscard]] foundation::u32 Id() const noexcept override { return m_id; }
        [[nodiscard]] foundation::u32 Width() const noexcept override { return m_width; }
        [[nodiscard]] foundation::u32 Height() const noexcept override { return m_height; }

        [[nodiscard]] foundation::i32 X() const noexcept override;
        [[nodiscard]] foundation::i32 Y() const noexcept override;
        void SetPosition(foundation::i32 x, foundation::i32 y) override;
        void SetSize(foundation::u32 width, foundation::u32 height) override;
        [[nodiscard]] foundation::f32 ContentScale() const noexcept override;

        // Extract the real native handles from SDL's window properties so RHI can
        // create its own surface (it does not use SDL's Vulkan helpers).
        [[nodiscard]] NativeWindow Native() const noexcept override;

        [[nodiscard]] bool IsOpen() const noexcept override { return m_open; }
        [[nodiscard]] bool IsMinimized() const noexcept override;
        void Close() override { m_open = false; }

        void StartTextInput() override;
        void StopTextInput() override;
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return m_textInputActive; }

        [[nodiscard]] SDL_Window* Handle() const noexcept { return m_window; }
        void OnResized(foundation::u32 w, foundation::u32 h) noexcept;

    private:
        SDL_Window* m_window;
        foundation::u32 m_id = 0;
        foundation::u32 m_width = 0;
        foundation::u32 m_height = 0;
        bool m_open = true;
        bool m_textInputActive = false;
    };

    // Sdl3WindowFlags() (SDL window-creation flags) is a file-local helper in SDL3ShellImpl.cpp.

    // Owns the SDL windows for the run. The main window is the first created.
    // Window destruction is deferred to FlushDestroyed() so a window is never
    // freed mid-frame while the GPU may still reference its swapchain.
    class SDL3WindowManager final : public IWindowManager
    {
    public:
        [[nodiscard]] foundation::Result<IWindow*> CreateWindow(const WindowSettings& settings) override;

        void DestroyWindow(IWindow* window) override;

        [[nodiscard]] foundation::Span<IWindow* const> Windows() const noexcept override;
        [[nodiscard]] IWindow* MainWindow() const noexcept override;
        [[nodiscard]] IWindow* GetWindow(foundation::u32 id) const noexcept override;
        [[nodiscard]] foundation::Span<const WindowEvent> Events() const noexcept override;

        void FlushDestroyed() override;

        // --- event pump wiring (called by SDL3Shell::ProcessEvents) ---
        SDL3Window* Find(foundation::u32 id) noexcept;
        void ClearEvents() noexcept { m_events.Clear(); }
        void PushEvent(const WindowEvent& e) { m_events.PushBack(e); }

        // Destroy every window immediately (SDL3Window dtors call
        // SDL_DestroyWindow). The shell calls this before SDL_Quit().
        void DestroyAllNow();

    private:
        // True only for windows this manager owns (present in m_live). Rejects nullptr too, so
        // DestroyWindow() is a no-op for null/unknown windows. Checked by pointer identity, NOT id:
        // a window from another manager can share an id, and acting on it would corrupt bookkeeping.
        [[nodiscard]] bool Owns(IWindow* window) const noexcept;

        foundation::Array<foundation::UniquePtr<SDL3Window>> m_owned;
        foundation::Array<IWindow*> m_live;
        foundation::Array<foundation::u32> m_pendingDestroy;
        foundation::Array<WindowEvent> m_events;
        foundation::u32 m_mainWindowId = 0; // id of the main window (first created); 0 = none
    };

    // -----------------------------------------------------------------------
    // Input devices - double-buffered state fed by the SDL3 event pump.
    // -----------------------------------------------------------------------
    inline constexpr foundation::u32 kKeyCount = static_cast<foundation::u32>(KeyCode::Count);
    inline constexpr foundation::u32 kMouseButtonCount = static_cast<foundation::u32>(MouseButton::Count);
    inline constexpr foundation::u32 kGamepadButtonCount = static_cast<foundation::u32>(GamepadButton::Count);
    inline constexpr foundation::u32 kCursorCount = static_cast<foundation::u32>(CursorType::Count);

    class SDL3Keyboard final : public IKeyboard
    {
    public:
        [[nodiscard]] bool IsKeyDown(KeyCode key) const override { return m_current[Index(key)]; }
        [[nodiscard]] bool IsKeyPressed(KeyCode key) const override;
        [[nodiscard]] bool IsKeyReleased(KeyCode key) const override;
        [[nodiscard]] KeyModifiers Modifiers() const override { return m_mods; }

        void SetKey(KeyCode key, bool down) { m_current[Index(key)] = down; }
        void SetModifiers(KeyModifiers mods) { m_mods = mods; }
        void BeginFrame();

    private:
        static foundation::u32 Index(KeyCode key) noexcept;
        bool m_current[kKeyCount] = {};
        bool m_previous[kKeyCount] = {};
        KeyModifiers m_mods = KeyModifiers::None;
    };

    class SDL3Mouse final : public IMouse
    {
    public:
        [[nodiscard]] foundation::f32 X() const override { return m_x; }
        [[nodiscard]] foundation::f32 Y() const override { return m_y; }
        [[nodiscard]] foundation::f32 GlobalX() const override;
        [[nodiscard]] foundation::f32 GlobalY() const override;
        [[nodiscard]] foundation::f32 DeltaX() const override { return m_dx; }
        [[nodiscard]] foundation::f32 DeltaY() const override { return m_dy; }
        [[nodiscard]] foundation::f32 ScrollX() const override { return m_sx; }
        [[nodiscard]] foundation::f32 ScrollY() const override { return m_sy; }
        [[nodiscard]] bool IsButtonDown(MouseButton b) const override;
        [[nodiscard]] bool IsButtonPressed(MouseButton b) const override;
        [[nodiscard]] bool IsButtonReleased(MouseButton b) const override;
        [[nodiscard]] bool RelativeMode() const override { return m_relative; }
        void SetRelativeMode(bool enabled) override;
        [[nodiscard]] bool CursorVisible() const override { return m_cursorVisible; }
        void SetCursorVisible(bool visible) override;
        void SetCursor(CursorType cursor) override;
        void SetGlobalCapture(bool enabled) override;

        void SetWindow(SDL_Window* window) { m_window = window; }
        // Frees the lazily-created system cursors. Called before SDL_Quit so no
        // SDL calls happen after the video subsystem is torn down.
        void ReleaseCursors();
        void OnMotion(foundation::f32 x, foundation::f32 y, foundation::f32 relX, foundation::f32 relY);
        void OnButton(foundation::u32 index, bool down);
        void OnWheel(foundation::f32 x, foundation::f32 y);
        void BeginFrame();

    private:
        static foundation::u32 Index(MouseButton b) noexcept;

        SDL_Window* m_window = nullptr;
        foundation::f32 m_x = 0, m_y = 0, m_dx = 0, m_dy = 0, m_sx = 0, m_sy = 0;
        bool m_current[kMouseButtonCount] = {};
        bool m_previous[kMouseButtonCount] = {};
        bool m_relative = false;
        bool m_cursorVisible = true;
        bool m_globalCapture = false;
        CursorType m_cursor = CursorType::Default;
        SDL_Cursor* m_cursors[kCursorCount] = {}; // lazily created, cached
    };

    class SDL3Gamepad final : public IGamepad
    {
    public:
        SDL3Gamepad(SDL_Gamepad* pad, foundation::u32 id, foundation::i32 index,
                    foundation::String name) noexcept
            : m_pad(pad), m_id(id), m_index(index), m_name(static_cast<foundation::String&&>(name))
        {
        }
        // Owns the SDL_Gamepad; closing it here means the owning UniquePtr frees the whole device
        // with no manual bookkeeping. Must run before SDL_Quit, which the shell guarantees by
        // clearing the device list in ReleaseDevices().
        ~SDL3Gamepad() override;
        SDL3Gamepad(const SDL3Gamepad&) = delete;
        SDL3Gamepad& operator=(const SDL3Gamepad&) = delete;

        [[nodiscard]] foundation::i32 Index() const override { return m_index; }
        [[nodiscard]] foundation::StringView Name() const override { return m_name; }
        [[nodiscard]] bool Connected() const override { return m_pad != nullptr; }
        [[nodiscard]] bool IsButtonDown(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonPressed(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonReleased(GamepadButton b) const override;
        [[nodiscard]] foundation::f32 Axis(GamepadAxis a) const override;
        void SetRumble(foundation::f32 lowFreq, foundation::f32 highFreq, foundation::u32 durationMs) override;

        [[nodiscard]] foundation::u32 Id() const noexcept { return m_id; }
        [[nodiscard]] SDL_Gamepad* Handle() const noexcept { return m_pad; }
        void SetIndex(foundation::i32 index) noexcept { m_index = index; }
        void SetButton(GamepadButton b, bool down) { m_current[Index(b)] = down; }
        void Disconnect() noexcept { m_pad = nullptr; }
        void BeginFrame();

    private:
        static foundation::u32 Index(GamepadButton b) noexcept;
        SDL_Gamepad* m_pad;
        foundation::u32 m_id;
        foundation::i32 m_index;
        foundation::String m_name;
        bool m_current[kGamepadButtonCount] = {};
        bool m_previous[kGamepadButtonCount] = {};
    };

    class SDL3Touch final : public ITouch
    {
    public:
        [[nodiscard]] foundation::i32 TouchCount() const override;
        [[nodiscard]] bool GetTouchPoint(foundation::i32 index, TouchPoint& out) const override;
        [[nodiscard]] bool HasTouch() const override { return !m_points.IsEmpty(); }

        void AddOrUpdate(const TouchPoint& tp);
        void Remove(foundation::u64 id);

    private:
        foundation::Array<TouchPoint> m_points;
    };

    class SDL3InputManager final : public IInputManager
    {
    public:
        ~SDL3InputManager() override { ReleaseDevices(); }

        // Frees all SDL-owned input resources (open gamepads, system cursors).
        // The shell calls this before SDL_Quit; idempotent so the destructor
        // can call it again harmlessly.
        void ReleaseDevices();

        [[nodiscard]] IKeyboard* Keyboard() override { return &m_keyboard; }
        [[nodiscard]] IMouse* Mouse() override { return &m_mouse; }
        [[nodiscard]] ITouch* Touch() override { return &m_touch; }
        [[nodiscard]] foundation::i32 GamepadCount() const override;
        [[nodiscard]] IGamepad* GetGamepad(foundation::i32 index) override;
        [[nodiscard]] foundation::Span<const InputEvent> Events() const override;
        [[nodiscard]] foundation::u32 HoverWindow() const override { return m_hoverWindow; }
        [[nodiscard]] foundation::u32 FocusedWindow() const override { return m_focusWindow; }
        void Update() override;

        // --- backend wiring (called by the shell event pump) ---
        SDL3Keyboard& KeyboardDevice() noexcept { return m_keyboard; }
        SDL3Mouse& MouseDevice() noexcept { return m_mouse; }
        SDL3Touch& TouchDevice() noexcept { return m_touch; }
        void SetWindow(SDL_Window* window) { m_mouse.SetWindow(window); }

        // Emit an input event onto this frame's stream (also apply it to the snapshot at the
        // call site - the snapshot is a fold over these events).
        void EmitEvent(const InputEvent& e) { m_events.PushBack(e); }
        void SetHoverWindow(foundation::u32 id) noexcept { m_hoverWindow = id; }
        void SetFocusWindow(foundation::u32 id) noexcept { m_focusWindow = id; }

        void AddGamepad(foundation::u32 id);

        void RemoveGamepad(foundation::u32 id);

        SDL3Gamepad* FindGamepadById(foundation::u32 id);

    private:
        SDL3Keyboard m_keyboard;
        SDL3Mouse m_mouse;
        SDL3Touch m_touch;
        foundation::Array<foundation::UniquePtr<SDL3Gamepad>> m_gamepads;
        foundation::Array<InputEvent> m_events; // this frame's event stream
        foundation::u32 m_hoverWindow = 0;      // window under the pointer
        foundation::u32 m_focusWindow = 0;      // keyboard-focused window
    };

    // Native file/folder dialogs over SDL3 (SDL_Show{Open,Save}FileDialog / SDL_ShowOpenFolderDialog).
    // Async: each Show* returns immediately; SDL fires Trampoline later (during SDL event processing,
    // i.e. the shell's ProcessEvents pump), which hands the callback OWNED path copies and frees the
    // heap context. Modeled on Sedulous's SDL3DialogService, with typed filters + owned result paths.
    class SDL3DialogService final : public IDialogService
    {
    public:
        explicit SDL3DialogService(SDL3WindowManager& windows) noexcept : m_windows(&windows) {}

        void ShowOpenFile(DialogResultCallback callback, foundation::Span<const FileFilter> filters,
                          foundation::StringView defaultPath, bool allowMultiple,
                          foundation::u32 parentWindowId) override;

        void ShowSaveFile(DialogResultCallback callback, foundation::Span<const FileFilter> filters,
                          foundation::StringView defaultPath, foundation::u32 parentWindowId) override;

        void ShowOpenFolder(DialogResultCallback callback, foundation::StringView defaultPath,
                            bool allowMultiple, foundation::u32 parentWindowId) override;

        void OpenPath(foundation::StringView path) override;

    private:
        // The async dialog Context (holds SDL_DialogFileFilter), MakeContext, and the SDL
        // Trampoline callback are file-local helpers in SDL3ShellImpl.cpp.
        [[nodiscard]] SDL_Window* ParentHandle(foundation::u32 id) const noexcept;

        SDL3WindowManager* m_windows;
    };

    class SDL3Shell final : public IShell
    {
    public:
        // Note on the Wayland Vulkan-window quirk: SDL only attaches libdecor
        // client-side decorations to a window backed by a GPU surface, so a plain
        // window comes up bare on GNOME/Mutter. Sdl3WindowFlags() flags every
        // window as Vulkan on Linux (skipped under the "dummy" driver) to fix it.
        explicit SDL3Shell(const WindowSettings& settings = {}) noexcept;
        ~SDL3Shell() override;

        SDL3Shell(const SDL3Shell&) = delete;
        SDL3Shell& operator=(const SDL3Shell&) = delete;

        [[nodiscard]] IWindowManager* WindowManager() noexcept override { return &m_windows; }
        [[nodiscard]] IWindow* MainWindow() noexcept override { return m_windows.MainWindow(); }
        [[nodiscard]] IInputManager* Input() noexcept override { return &m_input; }
        [[nodiscard]] IDialogService* Dialogs() noexcept override { return &m_dialogs; }

        void ProcessEvents() override;

        [[nodiscard]] bool IsRunning() const noexcept override;

        void DrainDroppedFiles(foundation::Array<DroppedFile>& out) override;

        void RequestExit() override { m_running = false; }

        void SetClipboardText(foundation::StringView text) override;

        [[nodiscard]] foundation::String GetClipboardText() const override;

        [[nodiscard]] bool HasClipboardText() const noexcept override;

    private:
        // The SDL-enum mappers (MapKeyCode/MapModifiers/MapGamepadButton/MapGamepadAxis) are
        // file-local helpers in SDL3ShellImpl.cpp.
        // SDL mouse button number (1-based) minus 1 -> MouseButton (Left/Middle/Right/X1/X2).
        static MouseButton MapMouseButton(foundation::u32 idx) noexcept;

        SDL3WindowManager m_windows;
        SDL3InputManager m_input;
        SDL3DialogService m_dialogs{
            m_windows}; // ctor takes m_windows (declared above -> init order OK)
        bool m_initialized = false;
        bool m_running = true;
        foundation::Array<DroppedFile> m_droppedFiles; // queued during ProcessEvents, drained per frame
    };

    // Factory the DRACONIC_APP_MAIN entry point calls to create the shell.
    [[nodiscard]] foundation::UniquePtr<IShell> CreateShell(const WindowSettings& settings = {})
    {
        IShell* shell = foundation::DefaultAllocator().New<SDL3Shell>(settings);
        return foundation::UniquePtr<IShell>(shell, foundation::DefaultAllocator());
    }
}

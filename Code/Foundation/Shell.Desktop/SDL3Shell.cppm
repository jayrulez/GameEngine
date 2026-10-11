// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell.Desktop - the `foundation.shell.desktop` module.
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
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include <cstdint>
#define SDL_MAIN_HANDLED

#include "Shell.Desktop/SdlForward.h" // opaque SDL_Window/Cursor/Gamepad (interface uses pointers only)

export module foundation.shell.desktop;

import foundation.core;
import foundation.shell;

namespace core = foundation::core;

export namespace foundation::shell
{
    // Human-readable WSI name for the diagnostic log line (compared against the RHI's surface-WSI log).
    [[nodiscard]] inline core::StringView WindowSystemName(WindowSystem s) noexcept
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

        [[nodiscard]] core::u32 Id() const noexcept override { return m_id; }
        [[nodiscard]] core::u32 Width() const noexcept override { return m_width; }
        [[nodiscard]] core::u32 Height() const noexcept override { return m_height; }

        [[nodiscard]] core::i32 X() const noexcept override;
        [[nodiscard]] core::i32 Y() const noexcept override;
        void SetPosition(core::i32 x, core::i32 y) override;
        void SetSize(core::u32 width, core::u32 height) override;
        [[nodiscard]] core::f32 ContentScale() const noexcept override;

        // Extract the real native handles from SDL's window properties so RHI can
        // create its own surface (it does not use SDL's Vulkan helpers).
        [[nodiscard]] NativeWindow Native() const noexcept override;

        [[nodiscard]] bool IsOpen() const noexcept override { return m_open; }
        [[nodiscard]] bool IsMinimized() const noexcept override;
        void Close() override { m_open = false; }

        void StartTextInput() override;
        void StopTextInput() override;
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return m_textInputActive; }

        void SetIcon(core::Span<const WindowIconImage> images) override;
        void Show() override;
        bool PresentPixels(core::Span<const core::u8> rgba, core::u32 width, core::u32 height) override;

        [[nodiscard]] SDL_Window* Handle() const noexcept { return m_window; }
        void OnResized(core::u32 w, core::u32 h) noexcept;

    private:
        SDL_Window* m_window;
        core::u32 m_id = 0;
        core::u32 m_width = 0;
        core::u32 m_height = 0;
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
        explicit SDL3WindowManager(core::IAllocator& allocator) noexcept : m_allocator(&allocator)
        {
        }

        [[nodiscard]] core::Result<IWindow*> CreateWindow(const WindowSettings& settings) override;

        void DestroyWindow(IWindow* window) override;

        [[nodiscard]] core::Span<IWindow* const> Windows() const noexcept override;
        [[nodiscard]] IWindow* MainWindow() const noexcept override;
        [[nodiscard]] IWindow* GetWindow(core::u32 id) const noexcept override;
        [[nodiscard]] core::Span<const WindowEvent> Events() const noexcept override;

        void FlushDestroyed() override;

        // --- event pump wiring (called by SDL3Shell::ProcessEvents) ---
        SDL3Window* Find(core::u32 id) noexcept;
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
        core::IAllocator* m_allocator;

        core::Array<core::UniquePtr<SDL3Window>> m_owned;
        core::Array<IWindow*> m_live;
        core::Array<core::u32> m_pendingDestroy;
        core::Array<WindowEvent> m_events;
        core::u32 m_mainWindowId = 0; // id of the main window (first created); 0 = none
    };

    // -----------------------------------------------------------------------
    // Input devices - double-buffered state fed by the SDL3 event pump.
    // -----------------------------------------------------------------------
    inline constexpr core::u32 kKeyCount = static_cast<core::u32>(KeyCode::Count);
    inline constexpr core::u32 kMouseButtonCount = static_cast<core::u32>(MouseButton::Count);
    inline constexpr core::u32 kGamepadButtonCount = static_cast<core::u32>(GamepadButton::Count);
    inline constexpr core::u32 kCursorCount = static_cast<core::u32>(CursorType::Count);

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
        static core::u32 Index(KeyCode key) noexcept;
        bool m_current[kKeyCount] = {};
        bool m_previous[kKeyCount] = {};
        KeyModifiers m_mods = KeyModifiers::None;
    };

    class SDL3Mouse final : public IMouse
    {
    public:
        [[nodiscard]] core::f32 X() const override { return m_x; }
        [[nodiscard]] core::f32 Y() const override { return m_y; }
        [[nodiscard]] core::f32 GlobalX() const override;
        [[nodiscard]] core::f32 GlobalY() const override;
        [[nodiscard]] core::f32 DeltaX() const override { return m_dx; }
        [[nodiscard]] core::f32 DeltaY() const override { return m_dy; }
        [[nodiscard]] core::f32 ScrollX() const override { return m_sx; }
        [[nodiscard]] core::f32 ScrollY() const override { return m_sy; }
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
        void OnMotion(core::f32 x, core::f32 y, core::f32 relX, core::f32 relY);
        void OnButton(core::u32 index, bool down);
        void OnWheel(core::f32 x, core::f32 y);
        void BeginFrame();

    private:
        static core::u32 Index(MouseButton b) noexcept;

        SDL_Window* m_window = nullptr;
        core::f32 m_x = 0, m_y = 0, m_dx = 0, m_dy = 0, m_sx = 0, m_sy = 0;
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
        SDL3Gamepad(SDL_Gamepad* pad, core::u32 id, core::i32 index,
                    core::String name) noexcept
            : m_pad(pad), m_id(id), m_index(index), m_name(static_cast<core::String&&>(name))
        {
        }
        // Owns the SDL_Gamepad; closing it here means the owning UniquePtr frees the whole device
        // with no manual bookkeeping. Must run before SDL_Quit, which the shell guarantees by
        // clearing the device list in ReleaseDevices().
        ~SDL3Gamepad() override;
        SDL3Gamepad(const SDL3Gamepad&) = delete;
        SDL3Gamepad& operator=(const SDL3Gamepad&) = delete;

        [[nodiscard]] core::i32 Index() const override { return m_index; }
        [[nodiscard]] core::StringView Name() const override { return m_name; }
        [[nodiscard]] bool Connected() const override { return m_pad != nullptr; }
        [[nodiscard]] bool IsButtonDown(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonPressed(GamepadButton b) const override;
        [[nodiscard]] bool IsButtonReleased(GamepadButton b) const override;
        [[nodiscard]] core::f32 Axis(GamepadAxis a) const override;
        void SetRumble(core::f32 lowFreq, core::f32 highFreq, core::u32 durationMs) override;

        [[nodiscard]] core::u32 Id() const noexcept { return m_id; }
        [[nodiscard]] SDL_Gamepad* Handle() const noexcept { return m_pad; }
        void SetIndex(core::i32 index) noexcept { m_index = index; }
        void SetButton(GamepadButton b, bool down) { m_current[Index(b)] = down; }
        void Disconnect() noexcept { m_pad = nullptr; }
        void BeginFrame();

    private:
        static core::u32 Index(GamepadButton b) noexcept;
        SDL_Gamepad* m_pad;
        core::u32 m_id;
        core::i32 m_index;
        core::String m_name;
        bool m_current[kGamepadButtonCount] = {};
        bool m_previous[kGamepadButtonCount] = {};
    };

    class SDL3Touch final : public ITouch
    {
    public:
        [[nodiscard]] core::i32 TouchCount() const override;
        [[nodiscard]] bool GetTouchPoint(core::i32 index, TouchPoint& out) const override;
        [[nodiscard]] bool HasTouch() const override { return !m_points.IsEmpty(); }

        void AddOrUpdate(const TouchPoint& tp);
        void Remove(core::u64 id);

    private:
        core::Array<TouchPoint> m_points;
    };

    class SDL3InputManager final : public IInputManager
    {
    public:
        explicit SDL3InputManager(core::IAllocator& allocator) noexcept : m_allocator(&allocator)
        {
        }
        ~SDL3InputManager() override { ReleaseDevices(); }

        // Frees all SDL-owned input resources (open gamepads, system cursors).
        // The shell calls this before SDL_Quit; idempotent so the destructor
        // can call it again harmlessly.
        void ReleaseDevices();

        [[nodiscard]] IKeyboard* Keyboard() override { return &m_keyboard; }
        [[nodiscard]] IMouse* Mouse() override { return &m_mouse; }
        [[nodiscard]] ITouch* Touch() override { return &m_touch; }
        [[nodiscard]] core::i32 GamepadCount() const override;
        [[nodiscard]] IGamepad* GetGamepad(core::i32 index) override;
        [[nodiscard]] core::Span<const InputEvent> Events() const override;
        [[nodiscard]] core::u32 HoverWindow() const override { return m_hoverWindow; }
        [[nodiscard]] core::u32 FocusedWindow() const override { return m_focusWindow; }
        void Update() override;

        // --- backend wiring (called by the shell event pump) ---
        SDL3Keyboard& KeyboardDevice() noexcept { return m_keyboard; }
        SDL3Mouse& MouseDevice() noexcept { return m_mouse; }
        SDL3Touch& TouchDevice() noexcept { return m_touch; }
        void SetWindow(SDL_Window* window) { m_mouse.SetWindow(window); }

        // Emit an input event onto this frame's stream (also apply it to the snapshot at the
        // call site - the snapshot is a fold over these events).
        void EmitEvent(const InputEvent& e) { m_events.PushBack(e); }
        void SetHoverWindow(core::u32 id) noexcept { m_hoverWindow = id; }
        void SetFocusWindow(core::u32 id) noexcept { m_focusWindow = id; }

        void AddGamepad(core::u32 id);

        void RemoveGamepad(core::u32 id);

        SDL3Gamepad* FindGamepadById(core::u32 id);

    private:
        SDL3Keyboard m_keyboard;
        SDL3Mouse m_mouse;
        SDL3Touch m_touch;
        core::IAllocator* m_allocator;
        core::Array<core::UniquePtr<SDL3Gamepad>> m_gamepads;
        core::Array<InputEvent> m_events; // this frame's event stream
        core::u32 m_hoverWindow = 0;      // window under the pointer
        core::u32 m_focusWindow = 0;      // keyboard-focused window
    };

    // Native file/folder dialogs over SDL3 (SDL_Show{Open,Save}FileDialog / SDL_ShowOpenFolderDialog).
    // Async: each Show* returns immediately; SDL fires Trampoline later (during SDL event processing,
    // i.e. the shell's ProcessEvents pump), which hands the callback OWNED path copies and frees the
    // heap context. Modeled on Sedulous's SDL3DialogService, with typed filters + owned result paths.
    class SDL3DialogService final : public IDialogService
    {
    public:
        SDL3DialogService(core::IAllocator& allocator, SDL3WindowManager& windows) noexcept
            : m_allocator(&allocator), m_windows(&windows)
        {
        }

        void ShowOpenFile(DialogResultCallback callback, core::Span<const FileFilter> filters,
                          core::StringView defaultPath, bool allowMultiple,
                          core::u32 parentWindowId) override;

        void ShowSaveFile(DialogResultCallback callback, core::Span<const FileFilter> filters,
                          core::StringView defaultPath, core::u32 parentWindowId) override;

        void ShowOpenFolder(DialogResultCallback callback, core::StringView defaultPath,
                            bool allowMultiple, core::u32 parentWindowId) override;

        void OpenPath(core::StringView path) override;

    private:
        // The async dialog Context (holds SDL_DialogFileFilter), MakeContext, and the SDL
        // Trampoline callback are file-local helpers in SDL3ShellImpl.cpp.
        [[nodiscard]] SDL_Window* ParentHandle(core::u32 id) const noexcept;

        core::IAllocator* m_allocator;
        SDL3WindowManager* m_windows;
    };

    class SDL3Shell final : public IShell
    {
    public:
        // Note on the Wayland Vulkan-window quirk: SDL only attaches libdecor
        // client-side decorations to a window backed by a GPU surface, so a plain
        // window comes up bare on GNOME/Mutter. Sdl3WindowFlags() flags every
        // window as Vulkan on Linux (skipped under the "dummy" driver) to fix it.
        // The allocator (required - the entry point decides) backs windows,
        // gamepads, and dialog contexts.
        SDL3Shell(core::IAllocator& allocator, const WindowSettings& settings = {}) noexcept;
        ~SDL3Shell() override;

        SDL3Shell(const SDL3Shell&) = delete;
        SDL3Shell& operator=(const SDL3Shell&) = delete;

        [[nodiscard]] IWindowManager* WindowManager() noexcept override { return &m_windows; }
        [[nodiscard]] IWindow* MainWindow() noexcept override { return m_windows.MainWindow(); }
        [[nodiscard]] IInputManager* Input() noexcept override { return &m_input; }
        [[nodiscard]] IDialogService* Dialogs() noexcept override { return &m_dialogs; }

        void ProcessEvents() override;

        [[nodiscard]] bool IsRunning() const noexcept override;

        void DrainDroppedFiles(core::Array<DroppedFile>& out) override;

        void RequestExit() override { m_running = false; }

        void SetClipboardText(core::StringView text) override;

        [[nodiscard]] core::String GetClipboardText() const override;

        [[nodiscard]] bool HasClipboardText() const noexcept override;

    private:
        // The SDL-enum mappers (MapKeyCode/MapModifiers/MapGamepadButton/MapGamepadAxis) are
        // file-local helpers in SDL3ShellImpl.cpp.
        // SDL mouse button number (1-based) minus 1 -> MouseButton (Left/Middle/Right/X1/X2).
        static MouseButton MapMouseButton(core::u32 idx) noexcept;

        core::IAllocator* m_allocator = nullptr; // FIRST: managers below init from it
        SDL3WindowManager m_windows;
        SDL3InputManager m_input;
        SDL3DialogService m_dialogs; // ctor takes m_windows (declared above -> init order OK)
        bool m_initialized = false;
        bool m_running = true;
        core::Array<DroppedFile> m_droppedFiles; // queued during ProcessEvents, drained per frame
    };

    // Factory the APP_MAIN entry point calls to create the shell.
    [[nodiscard]] core::UniquePtr<IShell> CreateShell(core::IAllocator& allocator,
                                                      const WindowSettings& settings = {})
    {
        IShell* shell = allocator.New<SDL3Shell>(allocator, settings);
        return core::UniquePtr<IShell>(shell, allocator);
    }
}

// Draconic Shell - draconic.shell.desktop implementation unit.
//
// Out-of-line definitions for the SDL3 backend classes (sec 3.2 / sec 10.6): all the SDL_*
// call sites live here. SDL3Shell.cppm keeps the class declarations (which reference SDL
// handle types) + trivial inline accessors.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include <cstdint>
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

module draconic.shell.desktop;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

namespace draconic::shell
{
    // ---- file-local SDL helpers, PIMPL'd out of SDL3Shell.cppm (sec 3.2 / sec 10.6) ----
    struct Context
    {
        DialogResultCallback callback;
        foundation::Array<foundation::String> filterStrings; // keeps the SDL_DialogFileFilter char* data alive
        foundation::Array<SDL_DialogFileFilter> sdlFilters;
        foundation::String defaultPath;
    };

    static SDL_WindowFlags Sdl3WindowFlags(const WindowSettings& settings) noexcept;
    static SDL_SystemCursor MapSystemCursor(CursorType cursor) noexcept;
    static Context* MakeContext(DialogResultCallback&& callback,
                                foundation::Span<const FileFilter> filters, foundation::StringView defaultPath);
    static void SDLCALL Trampoline(void* userdata, const char* const* filelist, int filter);
    static KeyCode MapKeyCode(SDL_Scancode sc) noexcept;
    static KeyModifiers MapModifiers(SDL_Keymod mod) noexcept;
    static GamepadButton MapGamepadButton(SDL_GamepadButton b) noexcept;
    static GamepadAxis MapGamepadAxis(SDL_GamepadAxis a) noexcept;

    SDL3Window::SDL3Window(SDL_Window* window) noexcept : m_window(window)
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_window, &w, &h);
        m_width = static_cast<foundation::u32>(w);
        m_height = static_cast<foundation::u32>(h);
        m_id = static_cast<foundation::u32>(SDL_GetWindowID(m_window));
    }

    SDL3Window::~SDL3Window()
    {
        if (m_window != nullptr)
        {
            SDL_DestroyWindow(m_window);
        }
    }

    SDL3Gamepad::~SDL3Gamepad()
    {
        if (m_pad != nullptr)
        {
            SDL_CloseGamepad(m_pad);
        }
    }

    static SDL_WindowFlags Sdl3WindowFlags(const WindowSettings& settings) noexcept
    {
        SDL_WindowFlags flags = 0;
        if (settings.resizable)
        {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        if (settings.borderless)
        {
            flags |= SDL_WINDOW_BORDERLESS;
        }
#if defined(__linux__)
        const char* driver = SDL_GetCurrentVideoDriver();
        if (driver != nullptr && SDL_strcmp(driver, "dummy") != 0)
        {
            flags |= SDL_WINDOW_VULKAN;
        }
#endif
        return flags;
    }

    SDL3Shell::SDL3Shell(const WindowSettings& settings) noexcept
    {
        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            m_running = false;
            return;
        }
        m_initialized = true;

        foundation::Result<IWindow*> main = m_windows.CreateWindow(settings);
        if (!main.HasValue())
        {
            m_running = false;
            return;
        }
        if (SDL3Window* w = m_windows.Find(main.Value()->Id()))
        {
            m_input.SetWindow(w->Handle());
        }

        const char* drv = SDL_GetCurrentVideoDriver();
        foundation::ConsoleWrite(u8"[Shell] SDL video driver: ");
        foundation::ConsoleWrite(foundation::StringView(
            reinterpret_cast<const foundation::utf8char*>(drv != nullptr ? drv : "unknown")));
        foundation::ConsoleWrite(u8" | main window WSI: ");
        foundation::ConsoleWrite(WindowSystemName(main.Value()->Native().system));
        foundation::ConsoleWrite(u8"\n");
    }

    SDL3Shell::~SDL3Shell()
    {
        m_input.ReleaseDevices();
        m_windows.DestroyAllNow();
        if (m_initialized)
        {
            SDL_Quit();
        }
    }

    foundation::i32 SDL3Window::X() const noexcept
    {
        int x = 0, y = 0;
        if (m_window != nullptr)
        {
            SDL_GetWindowPosition(m_window, &x, &y);
        }
        return static_cast<foundation::i32>(x);
    }

    foundation::i32 SDL3Window::Y() const noexcept
    {
        int x = 0, y = 0;
        if (m_window != nullptr)
        {
            SDL_GetWindowPosition(m_window, &x, &y);
        }
        return static_cast<foundation::i32>(y);
    }

    void SDL3Window::SetPosition(foundation::i32 x, foundation::i32 y)
    {
        if (m_window != nullptr)
        {
            SDL_SetWindowPosition(m_window, static_cast<int>(x), static_cast<int>(y));
        }
    }

    void SDL3Window::SetSize(foundation::u32 width, foundation::u32 height)
    {
        if (m_window != nullptr)
        {
            SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
            m_width = width;
            m_height = height;
        }
    }

    foundation::f32 SDL3Window::ContentScale() const noexcept
    {
        const float s = (m_window != nullptr) ? SDL_GetWindowDisplayScale(m_window) : 1.0f;
        return s > 0.0f ? static_cast<foundation::f32>(s)
                        : 1.0f; // SDL returns 0 before the window is shown
    }

    NativeWindow SDL3Window::Native() const noexcept
    {
        NativeWindow native;
        if (m_window == nullptr)
        {
            return native;
        }
        const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
#if defined(_WIN32)
        native.system = WindowSystem::Win32;
        native.display =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
        native.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
        native.system = WindowSystem::Cocoa;
        native.window =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
        const char* driver = SDL_GetCurrentVideoDriver();
        if (driver != nullptr && SDL_strcmp(driver, "wayland") == 0)
        {
            native.system = WindowSystem::Wayland;
            native.display =
                SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            native.window =
                SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        }
        else if (driver != nullptr && SDL_strcmp(driver, "x11") == 0)
        {
            native.system = WindowSystem::X11;
            native.display =
                SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            native.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
                SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
        }
#endif
        return native;
    }

    bool SDL3Window::IsMinimized() const noexcept
    {
        return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
    }

    void SDL3Window::StartTextInput()
    {
        if (m_window != nullptr && !m_textInputActive)
        {
            SDL_StartTextInput(m_window);
            m_textInputActive = true;
        }
    }

    void SDL3Window::StopTextInput()
    {
        if (m_window != nullptr && m_textInputActive)
        {
            SDL_StopTextInput(m_window);
            m_textInputActive = false;
        }
    }

    void SDL3Window::OnResized(foundation::u32 w, foundation::u32 h) noexcept
    {
        m_width = w;
        m_height = h;
    }

    foundation::Result<IWindow*> SDL3WindowManager::CreateWindow(const WindowSettings& settings)
    {
        const foundation::String title = foundation::String(settings.title);
        SDL_Window* window = SDL_CreateWindow(
            reinterpret_cast<const char*>(title.CStr()), static_cast<int>(settings.width),
            static_cast<int>(settings.height), Sdl3WindowFlags(settings));
        if (window == nullptr)
        {
            return foundation::Err(foundation::ErrorCode::Unknown);
        }

        // Place the window if an explicit position was requested (dockable/floating windows do;
        // the main window leaves it to the OS/centered default).
        if (settings.positioned)
        {
            SDL_SetWindowPosition(window, static_cast<int>(settings.x),
                                  static_cast<int>(settings.y));
        }

        auto wrapped = foundation::MakeUnique<SDL3Window>(foundation::DefaultAllocator(), window);
        IWindow* borrowed = wrapped.Get();
        m_owned.PushBack(static_cast<foundation::UniquePtr<SDL3Window>&&>(wrapped));
        m_live.PushBack(borrowed);
        if (m_mainWindowId == 0)
        {
            m_mainWindowId = borrowed->Id();
        } // the first window created is the main window
        return borrowed;
    }

    void SDL3WindowManager::DestroyWindow(IWindow* window)
    {
        if (!Owns(window))
        {
            return;
        } // no-op for null or windows this manager does not own
        window->Close();
        m_pendingDestroy.PushBack(window->Id());
    }

    foundation::Span<IWindow* const> SDL3WindowManager::Windows() const noexcept
    {
        return foundation::Span<IWindow* const>(m_live.Data(), m_live.Size());
    }

    IWindow* SDL3WindowManager::MainWindow() const noexcept
    {
        // Tracked by id, so destroying/flushing the main window never promotes another
        // window into its place; returns null once the main window is gone.
        return GetWindow(m_mainWindowId);
    }

    IWindow* SDL3WindowManager::GetWindow(foundation::u32 id) const noexcept
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

    foundation::Span<const WindowEvent> SDL3WindowManager::Events() const noexcept
    {
        return foundation::Span<const WindowEvent>(m_events.Data(), m_events.Size());
    }

    void SDL3WindowManager::FlushDestroyed()
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
                } // dtor destroys SDL window
            }
        }
        m_pendingDestroy.Clear();
    }

    SDL3Window* SDL3WindowManager::Find(foundation::u32 id) noexcept
    {
        for (foundation::UniquePtr<SDL3Window>& w : m_owned)
        {
            if (w->Id() == id)
            {
                return w.Get();
            }
        }
        return nullptr;
    }

    void SDL3WindowManager::DestroyAllNow()
    {
        m_live.Clear();
        m_owned.Clear();
        m_pendingDestroy.Clear();
    }

    bool SDL3WindowManager::Owns(IWindow* window) const noexcept
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

    bool SDL3Keyboard::IsKeyPressed(KeyCode key) const
    {
        const foundation::u32 i = Index(key);
        return m_current[i] && !m_previous[i];
    }

    bool SDL3Keyboard::IsKeyReleased(KeyCode key) const
    {
        const foundation::u32 i = Index(key);
        return !m_current[i] && m_previous[i];
    }

    void SDL3Keyboard::BeginFrame()
    {
        for (foundation::u32 i = 0; i < kKeyCount; ++i)
        {
            m_previous[i] = m_current[i];
        }
    }

    foundation::u32 SDL3Keyboard::Index(KeyCode key) noexcept
    {
        const foundation::u32 i = static_cast<foundation::u32>(key);
        return i < kKeyCount ? i : 0;
    }

    foundation::f32 SDL3Mouse::GlobalX() const
    {
        float gx = 0.0f, gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
        return static_cast<foundation::f32>(gx);
    }

    foundation::f32 SDL3Mouse::GlobalY() const
    {
        float gx = 0.0f, gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
        return static_cast<foundation::f32>(gy);
    }

    bool SDL3Mouse::IsButtonDown(MouseButton b) const { return m_current[Index(b)]; }

    bool SDL3Mouse::IsButtonPressed(MouseButton b) const
    {
        const foundation::u32 i = Index(b);
        return m_current[i] && !m_previous[i];
    }

    bool SDL3Mouse::IsButtonReleased(MouseButton b) const
    {
        const foundation::u32 i = Index(b);
        return !m_current[i] && m_previous[i];
    }

    void SDL3Mouse::SetRelativeMode(bool enabled)
    {
        if (m_window != nullptr)
        {
            SDL_SetWindowRelativeMouseMode(m_window, enabled);
        }
        m_relative = enabled;
    }

    void SDL3Mouse::SetCursorVisible(bool visible)
    {
        if (visible)
        {
            SDL_ShowCursor();
        }
        else
        {
            SDL_HideCursor();
        }
        m_cursorVisible = visible;
    }

    void SDL3Mouse::SetCursor(CursorType cursor)
    {
        const foundation::u32 i = static_cast<foundation::u32>(cursor);
        if (i >= kCursorCount)
        {
            return;
        }
        if (m_cursors[i] == nullptr)
        {
            m_cursors[i] = SDL_CreateSystemCursor(MapSystemCursor(cursor));
        }
        if (m_cursors[i] != nullptr)
        {
            SDL_SetCursor(m_cursors[i]);
            m_cursor = cursor;
        }
    }

    void SDL3Mouse::SetGlobalCapture(bool enabled)
    {
        if (enabled == m_globalCapture)
        {
            return;
        } // SDL_CaptureMouse is refcounted-ish; avoid churn
        SDL_CaptureMouse(enabled);
        m_globalCapture = enabled;
    }

    void SDL3Mouse::ReleaseCursors()
    {
        for (SDL_Cursor*& c : m_cursors)
        {
            if (c != nullptr)
            {
                SDL_DestroyCursor(c);
                c = nullptr;
            }
        }
    }

    void SDL3Mouse::OnMotion(foundation::f32 x, foundation::f32 y, foundation::f32 relX, foundation::f32 relY)
    {
        m_x = x;
        m_y = y;
        m_dx += relX;
        m_dy += relY;
    }

    void SDL3Mouse::OnButton(foundation::u32 index, bool down)
    {
        if (index < kMouseButtonCount)
        {
            m_current[index] = down;
        }
    }

    void SDL3Mouse::OnWheel(foundation::f32 x, foundation::f32 y)
    {
        m_sx += x;
        m_sy += y;
    }

    void SDL3Mouse::BeginFrame()
    {
        for (foundation::u32 i = 0; i < kMouseButtonCount; ++i)
        {
            m_previous[i] = m_current[i];
        }
        m_dx = m_dy = m_sx = m_sy = 0.0f;
    }

    foundation::u32 SDL3Mouse::Index(MouseButton b) noexcept
    {
        const foundation::u32 i = static_cast<foundation::u32>(b);
        return i < kMouseButtonCount ? i : 0;
    }

    static SDL_SystemCursor MapSystemCursor(CursorType cursor) noexcept
    {
        switch (cursor)
        {
        case CursorType::Default:
            return SDL_SYSTEM_CURSOR_DEFAULT;
        case CursorType::Text:
            return SDL_SYSTEM_CURSOR_TEXT;
        case CursorType::Wait:
            return SDL_SYSTEM_CURSOR_WAIT;
        case CursorType::Crosshair:
            return SDL_SYSTEM_CURSOR_CROSSHAIR;
        case CursorType::Progress:
            return SDL_SYSTEM_CURSOR_PROGRESS;
        case CursorType::ResizeNWSE:
            return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        case CursorType::ResizeNESW:
            return SDL_SYSTEM_CURSOR_NESW_RESIZE;
        case CursorType::ResizeEW:
            return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CursorType::ResizeNS:
            return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CursorType::ResizeNW:
            return SDL_SYSTEM_CURSOR_NW_RESIZE;
        case CursorType::ResizeN:
            return SDL_SYSTEM_CURSOR_N_RESIZE;
        case CursorType::ResizeNE:
            return SDL_SYSTEM_CURSOR_NE_RESIZE;
        case CursorType::ResizeE:
            return SDL_SYSTEM_CURSOR_E_RESIZE;
        case CursorType::ResizeSE:
            return SDL_SYSTEM_CURSOR_SE_RESIZE;
        case CursorType::ResizeS:
            return SDL_SYSTEM_CURSOR_S_RESIZE;
        case CursorType::ResizeSW:
            return SDL_SYSTEM_CURSOR_SW_RESIZE;
        case CursorType::ResizeW:
            return SDL_SYSTEM_CURSOR_W_RESIZE;
        case CursorType::Move:
            return SDL_SYSTEM_CURSOR_MOVE;
        case CursorType::NotAllowed:
            return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        case CursorType::Pointer:
            return SDL_SYSTEM_CURSOR_POINTER;
        default:
            return SDL_SYSTEM_CURSOR_DEFAULT;
        }
    }

    bool SDL3Gamepad::IsButtonDown(GamepadButton b) const { return m_current[Index(b)]; }

    bool SDL3Gamepad::IsButtonPressed(GamepadButton b) const
    {
        const foundation::u32 i = Index(b);
        return m_current[i] && !m_previous[i];
    }

    bool SDL3Gamepad::IsButtonReleased(GamepadButton b) const
    {
        const foundation::u32 i = Index(b);
        return !m_current[i] && m_previous[i];
    }

    foundation::f32 SDL3Gamepad::Axis(GamepadAxis a) const
    {
        if (m_pad == nullptr)
        {
            return 0.0f;
        }
        const auto raw =
            SDL_GetGamepadAxis(m_pad, static_cast<SDL_GamepadAxis>(static_cast<foundation::u32>(a)));
        return static_cast<foundation::f32>(raw) / 32767.0f;
    }

    void SDL3Gamepad::SetRumble(foundation::f32 lowFreq, foundation::f32 highFreq, foundation::u32 durationMs)
    {
        if (m_pad != nullptr)
        {
            SDL_RumbleGamepad(m_pad, static_cast<foundation::u16>(lowFreq * 65535.0f),
                              static_cast<foundation::u16>(highFreq * 65535.0f), durationMs);
        }
    }

    void SDL3Gamepad::BeginFrame()
    {
        for (foundation::u32 i = 0; i < kGamepadButtonCount; ++i)
        {
            m_previous[i] = m_current[i];
        }
    }

    foundation::u32 SDL3Gamepad::Index(GamepadButton b) noexcept
    {
        const foundation::u32 i = static_cast<foundation::u32>(b);
        return i < kGamepadButtonCount ? i : 0;
    }

    foundation::i32 SDL3Touch::TouchCount() const { return static_cast<foundation::i32>(m_points.Size()); }

    bool SDL3Touch::GetTouchPoint(foundation::i32 index, TouchPoint& out) const
    {
        if (index < 0 || static_cast<foundation::usize>(index) >= m_points.Size())
        {
            return false;
        }
        out = m_points[static_cast<foundation::usize>(index)];
        return true;
    }

    void SDL3Touch::AddOrUpdate(const TouchPoint& tp)
    {
        for (foundation::usize i = 0; i < m_points.Size(); ++i)
        {
            if (m_points[i].id == tp.id)
            {
                m_points[i] = tp;
                return;
            }
        }
        m_points.PushBack(tp);
    }

    void SDL3Touch::Remove(foundation::u64 id)
    {
        for (foundation::usize i = 0; i < m_points.Size(); ++i)
        {
            if (m_points[i].id == id)
            {
                m_points.RemoveAtSwap(i);
                return;
            }
        }
    }

    void SDL3InputManager::ReleaseDevices()
    {
        m_gamepads.Clear(); // UniquePtr dtors close each SDL handle
        m_mouse.ReleaseCursors();
    }

    foundation::i32 SDL3InputManager::GamepadCount() const
    {
        return static_cast<foundation::i32>(m_gamepads.Size());
    }

    IGamepad* SDL3InputManager::GetGamepad(foundation::i32 index)
    {
        if (index < 0 || static_cast<foundation::usize>(index) >= m_gamepads.Size())
        {
            return nullptr;
        }
        return m_gamepads[static_cast<foundation::usize>(index)].Get();
    }

    foundation::Span<const InputEvent> SDL3InputManager::Events() const
    {
        return foundation::Span<const InputEvent>{m_events.Data(), m_events.Size()};
    }

    void SDL3InputManager::Update()
    {
        m_keyboard.BeginFrame();
        m_mouse.BeginFrame();
        for (auto& g : m_gamepads)
        {
            g->BeginFrame();
        }
        m_events.Clear(); // events are valid only for the frame they were pumped in
    }

    void SDL3InputManager::AddGamepad(SDL_JoystickID id)
    {
        if (FindGamepadById(id) != nullptr)
        {
            return;
        }
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (pad == nullptr)
        {
            return;
        }

        const char* n = SDL_GetGamepadName(pad);
        foundation::String name =
            (n != nullptr)
                ? foundation::String(foundation::StringView(reinterpret_cast<const foundation::utf8char*>(n)))
                : foundation::String{};
        const foundation::i32 index = static_cast<foundation::i32>(m_gamepads.Size());
        m_gamepads.PushBack(foundation::MakeUnique<SDL3Gamepad>(foundation::DefaultAllocator(), pad, id, index,
                                                          static_cast<foundation::String&&>(name)));
    }

    void SDL3InputManager::RemoveGamepad(SDL_JoystickID id)
    {
        for (foundation::usize i = 0; i < m_gamepads.Size(); ++i)
        {
            if (m_gamepads[i]->Id() == id)
            {
                m_gamepads.RemoveAt(i); // UniquePtr dtor closes the SDL handle
                for (foundation::usize j = 0; j < m_gamepads.Size(); ++j)
                {
                    m_gamepads[j]->SetIndex(static_cast<foundation::i32>(j));
                }
                return;
            }
        }
    }

    SDL3Gamepad* SDL3InputManager::FindGamepadById(SDL_JoystickID id)
    {
        for (auto& g : m_gamepads)
        {
            if (g->Id() == id)
            {
                return g.Get();
            }
        }
        return nullptr;
    }

    void SDL3DialogService::ShowOpenFile(DialogResultCallback callback,
                                         foundation::Span<const FileFilter> filters,
                                         foundation::StringView defaultPath, bool allowMultiple,
                                         foundation::u32 parentWindowId)
    {
        Context* ctx =
            MakeContext(static_cast<DialogResultCallback&&>(callback), filters, defaultPath);
        SDL_ShowOpenFileDialog(&Trampoline, ctx, ParentHandle(parentWindowId),
                               ctx->sdlFilters.IsEmpty() ? nullptr : ctx->sdlFilters.Data(),
                               static_cast<int>(ctx->sdlFilters.Size()),
                               ctx->defaultPath.IsEmpty()
                                   ? nullptr
                                   : reinterpret_cast<const char*>(ctx->defaultPath.Data()),
                               allowMultiple);
    }

    void SDL3DialogService::ShowSaveFile(DialogResultCallback callback,
                                         foundation::Span<const FileFilter> filters,
                                         foundation::StringView defaultPath, foundation::u32 parentWindowId)
    {
        Context* ctx =
            MakeContext(static_cast<DialogResultCallback&&>(callback), filters, defaultPath);
        SDL_ShowSaveFileDialog(&Trampoline, ctx, ParentHandle(parentWindowId),
                               ctx->sdlFilters.IsEmpty() ? nullptr : ctx->sdlFilters.Data(),
                               static_cast<int>(ctx->sdlFilters.Size()),
                               ctx->defaultPath.IsEmpty()
                                   ? nullptr
                                   : reinterpret_cast<const char*>(ctx->defaultPath.Data()));
    }

    void SDL3DialogService::ShowOpenFolder(DialogResultCallback callback,
                                           foundation::StringView defaultPath, bool allowMultiple,
                                           foundation::u32 parentWindowId)
    {
        Context* ctx = MakeContext(static_cast<DialogResultCallback&&>(callback), {}, defaultPath);
        SDL_ShowOpenFolderDialog(&Trampoline, ctx, ParentHandle(parentWindowId),
                                 ctx->defaultPath.IsEmpty()
                                     ? nullptr
                                     : reinterpret_cast<const char*>(ctx->defaultPath.Data()),
                                 allowMultiple);
    }

    void SDL3DialogService::OpenPath(foundation::StringView path)
    {
        if (path.IsEmpty())
        {
            return;
        }
        // Reveal via the native, non-blocking Foundation/System backend (Explorer / xdg-open), NOT
        // SDL_OpenURL. SDL_OpenURL wraps ShellExecute on a file:// URL, which on Windows can
        // synchronously block (or pop a protocol chooser) and hang this UI thread - observed
        // reliably here and intermittently in the Beef/Sedulous build. OpenPathInFileManager
        // launches detached and returns immediately.
        if (foundation::OpenPathInFileManager(path))
        {
            DRACONIC_LOG_DEBUG(u8"Shell", u8"OpenPath: revealed '{}'", path);
        }
        else
        {
            DRACONIC_LOG_WARNING(u8"Shell", u8"OpenPath: could not reveal '{}'", path);
        }
    }

    SDL_Window* SDL3DialogService::ParentHandle(foundation::u32 id) const noexcept
    {
        if (id == 0)
        {
            return nullptr;
        }
        SDL3Window* w = m_windows->Find(id);
        return (w != nullptr) ? w->Handle() : nullptr;
    }

    static Context* MakeContext(DialogResultCallback&& callback,
                                foundation::Span<const FileFilter> filters, foundation::StringView defaultPath)
    {
        Context* ctx = foundation::DefaultAllocator().New<Context>();
        ctx->callback = static_cast<DialogResultCallback&&>(callback);
        ctx->defaultPath = foundation::String(defaultPath); // null-terminated copy for the C API
        // Fill filterStrings FIRST (so the array stops growing), THEN alias sdlFilters at them -
        // otherwise a PushBack realloc would dangle the SDL filter pointers.
        for (const FileFilter& f : filters)
        {
            ctx->filterStrings.PushBack(foundation::String(f.name));
            ctx->filterStrings.PushBack(foundation::String(f.pattern));
        }
        for (foundation::usize i = 0; i < filters.Size(); ++i)
        {
            SDL_DialogFileFilter sf{};
            sf.name = reinterpret_cast<const char*>(ctx->filterStrings[i * 2 + 0].Data());
            sf.pattern = reinterpret_cast<const char*>(ctx->filterStrings[i * 2 + 1].Data());
            ctx->sdlFilters.PushBack(sf);
        }
        return ctx;
    }

    static void SDLCALL Trampoline(void* userdata, const char* const* filelist, int /*filter*/)
    {
        Context* ctx = static_cast<Context*>(userdata);
        foundation::Array<foundation::String> paths;
        if (filelist != nullptr) // null => cancelled or error
        {
            for (const char* const* p = filelist; *p != nullptr; ++p)
            {
                paths.PushBack(
                    foundation::String(foundation::StringView(reinterpret_cast<const foundation::utf8char*>(*p))));
            }
        }
        if (ctx->callback)
        {
            ctx->callback(foundation::Span<const foundation::String>(paths.Data(), paths.Size()));
        }
        foundation::DefaultAllocator().Delete(ctx);
    }

    void SDL3Shell::ProcessEvents()
    {
        // Roll input state (current -> previous, clear deltas) before pumping;
        // clear last frame's window events (they're valid only until now).
        m_input.Update();
        m_windows.ClearEvents();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_DROP_FILE:
            {
                if (event.drop.data != nullptr)
                {
                    DroppedFile drop;
                    drop.window = event.drop.windowID;
                    drop.x = event.drop.x;
                    drop.y = event.drop.y;
                    drop.path = foundation::String(
                        foundation::StringView(reinterpret_cast<const foundation::utf8char*>(event.drop.data)));
                    m_droppedFiles.PushBack(foundation::Move(drop));
                }
                break;
            }

            case SDL_EVENT_QUIT:
                // App-level quit: interceptable (unsaved-changes prompts) like the
                // main window's close button below.
                if (OnMainWindowCloseRequested && !OnMainWindowCloseRequested())
                {
                    break;
                }
                if (IWindow* main = m_windows.MainWindow())
                {
                    main->Close();
                    m_windows.PushEvent(WindowEvent{WindowEventType::CloseRequested, main->Id()});
                }
                m_running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            {
                const foundation::u32 id = static_cast<foundation::u32>(event.window.windowID);
                // MAIN window close is interceptable; when vetoed, nothing happens
                // (no event, no teardown - the app exits later via the host).
                IWindow* main = m_windows.MainWindow();
                if (main != nullptr && main->Id() == id && OnMainWindowCloseRequested &&
                    !OnMainWindowCloseRequested())
                {
                    break;
                }
                m_windows.PushEvent(WindowEvent{WindowEventType::CloseRequested, id});
                // Closing the main window stops the shell; the
                // Application handles secondary-window close via the event.
                if (main != nullptr && main->Id() == id)
                {
                    main->Close();
                    m_running = false;
                }
                break;
            }
            case SDL_EVENT_WINDOW_RESIZED:
            {
                const foundation::u32 id = static_cast<foundation::u32>(event.window.windowID);
                if (SDL3Window* w = m_windows.Find(id))
                {
                    const foundation::u32 nw = static_cast<foundation::u32>(event.window.data1);
                    const foundation::u32 nh = static_cast<foundation::u32>(event.window.data2);
                    w->OnResized(nw, nh);
                    m_windows.PushEvent(WindowEvent{WindowEventType::Resized, id, nw, nh});
                }
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            {
                const foundation::u32 id = static_cast<foundation::u32>(event.window.windowID);
                m_input.SetFocusWindow(id); // keyboard/gamepad routing authority
                m_windows.PushEvent(WindowEvent{WindowEventType::FocusGained, id});
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            {
                const foundation::u32 id = static_cast<foundation::u32>(event.window.windowID);
                if (m_input.FocusedWindow() == id)
                {
                    m_input.SetFocusWindow(0);
                }
                m_windows.PushEvent(WindowEvent{WindowEventType::FocusLost, id});
                break;
            }
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                m_input.SetHoverWindow(static_cast<foundation::u32>(event.window.windowID));
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                if (m_input.HoverWindow() == static_cast<foundation::u32>(event.window.windowID))
                {
                    m_input.SetHoverWindow(0);
                }
                break;

            // --- Keyboard --- (emit event, then fold into the snapshot)
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                InputEvent e{};
                e.kind = event.key.down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
                e.window = static_cast<foundation::u32>(event.key.windowID);
                e.key = MapKeyCode(event.key.scancode);
                e.modifiers = MapModifiers(event.key.mod);
                if (e.key == KeyCode::Unknown && event.key.down)
                {
                    const foundation::String scName(reinterpret_cast<const foundation::utf8char*>(
                        SDL_GetScancodeName(event.key.scancode)));
                    DRACONIC_LOG_DEBUG(u8"Shell", u8"unmapped key scancode {} ('{}')",
                                       static_cast<foundation::u32>(event.key.scancode), scName);
                }
                m_input.EmitEvent(e);
                m_input.KeyboardDevice().SetKey(e.key, event.key.down);
                m_input.KeyboardDevice().SetModifiers(e.modifiers);
                break;
            }
            case SDL_EVENT_TEXT_INPUT: // only arrives after SDL_StartTextInput (focus-driven, later)
            {
                InputEvent e{};
                e.kind = InputEventKind::TextInput;
                e.window = static_cast<foundation::u32>(event.text.windowID);
                if (event.text.text != nullptr)
                {
                    foundation::usize n = 0;
                    while (n + 1 < sizeof(e.text) && event.text.text[n] != '\0')
                    {
                        e.text[n] = static_cast<foundation::utf8char>(event.text.text[n]);
                        ++n;
                    }
                    e.text[n] = static_cast<foundation::utf8char>('\0');
                }
                m_input.EmitEvent(e);
                break;
            }

            // --- Mouse ---
            case SDL_EVENT_MOUSE_MOTION:
            {
                m_input.SetHoverWindow(static_cast<foundation::u32>(event.motion.windowID));
                InputEvent e{};
                e.kind = InputEventKind::MouseMove;
                e.window = static_cast<foundation::u32>(event.motion.windowID);
                e.x = event.motion.x;
                e.y = event.motion.y;
                e.dx = event.motion.xrel;
                e.dy = event.motion.yrel;
                m_input.EmitEvent(e);
                m_input.MouseDevice().OnMotion(event.motion.x, event.motion.y, event.motion.xrel,
                                               event.motion.yrel);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                const foundation::u32 btn = static_cast<foundation::u32>(event.button.button) - 1;
                const bool down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                InputEvent e{};
                e.kind = down ? InputEventKind::MouseButtonDown : InputEventKind::MouseButtonUp;
                e.window = static_cast<foundation::u32>(event.button.windowID);
                e.button = MapMouseButton(btn);
                e.x = event.button.x;
                e.y = event.button.y;
                m_input.EmitEvent(e);
                m_input.MouseDevice().OnButton(btn, down);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                InputEvent e{};
                e.kind = InputEventKind::MouseWheel;
                e.window = static_cast<foundation::u32>(event.wheel.windowID);
                e.x = event.wheel.x;
                e.y = event.wheel.y;
                m_input.EmitEvent(e);
                m_input.MouseDevice().OnWheel(event.wheel.x, event.wheel.y);
                break;
            }

            // --- Touch ---
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            {
                InputEvent e{};
                e.kind = (event.type == SDL_EVENT_FINGER_DOWN) ? InputEventKind::TouchDown
                                                               : InputEventKind::TouchMove;
                e.window = static_cast<foundation::u32>(event.tfinger.windowID);
                e.touchId = static_cast<foundation::u64>(event.tfinger.fingerID);
                e.x = event.tfinger.x;
                e.y = event.tfinger.y;
                e.value = event.tfinger.pressure;
                m_input.EmitEvent(e);
                m_input.TouchDevice().AddOrUpdate(TouchPoint{e.touchId, e.x, e.y, e.value});
                break;
            }
            case SDL_EVENT_FINGER_UP:
            {
                InputEvent e{};
                e.kind = InputEventKind::TouchUp;
                e.window = static_cast<foundation::u32>(event.tfinger.windowID);
                e.touchId = static_cast<foundation::u64>(event.tfinger.fingerID);
                e.x = event.tfinger.x;
                e.y = event.tfinger.y;
                m_input.EmitEvent(e);
                m_input.TouchDevice().Remove(e.touchId);
                break;
            }

            // --- Gamepad --- (tagged with the focused window; pads aren't window-bound)
            case SDL_EVENT_GAMEPAD_ADDED:
                m_input.AddGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                m_input.RemoveGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (SDL3Gamepad* pad = m_input.FindGamepadById(event.gbutton.which))
                {
                    const GamepadButton b =
                        MapGamepadButton(static_cast<SDL_GamepadButton>(event.gbutton.button));
                    if (b != GamepadButton::Count)
                    {
                        const bool down = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                        InputEvent e{};
                        e.kind = down ? InputEventKind::GamepadButtonDown
                                      : InputEventKind::GamepadButtonUp;
                        e.window = m_input.FocusedWindow();
                        e.gamepad = pad->Index();
                        e.padButton = b;
                        m_input.EmitEvent(e);
                        pad->SetButton(b, down);
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (SDL3Gamepad* pad = m_input.FindGamepadById(event.gaxis.which))
                {
                    const GamepadAxis a =
                        MapGamepadAxis(static_cast<SDL_GamepadAxis>(event.gaxis.axis));
                    if (a != GamepadAxis::Count)
                    {
                        InputEvent e{};
                        e.kind = InputEventKind::GamepadAxis;
                        e.window = m_input.FocusedWindow();
                        e.gamepad = pad->Index();
                        e.padAxis = a;
                        e.value = static_cast<foundation::f32>(event.gaxis.value) /
                                  32767.0f; // snapshot reads axes live
                        m_input.EmitEvent(e);
                    }
                }
                break;

            default:
                break;
            }
        }
    }

    bool SDL3Shell::IsRunning() const noexcept
    {
        IWindow* main = m_windows.MainWindow(); // MainWindow() is const now - no const_cast needed
        return m_running && main != nullptr && main->IsOpen();
    }

    void SDL3Shell::DrainDroppedFiles(foundation::Array<DroppedFile>& out)
    {
        for (DroppedFile& drop : m_droppedFiles)
        {
            out.PushBack(foundation::Move(drop));
        }
        m_droppedFiles.Clear();
    }

    void SDL3Shell::SetClipboardText(foundation::StringView text)
    {
        const foundation::String owned(text); // guarantee null-termination for the C API
        SDL_SetClipboardText(reinterpret_cast<const char*>(owned.Data()));
    }

    foundation::String SDL3Shell::GetClipboardText() const
    {
        char* text = SDL_GetClipboardText(); // never null (empty string on none); caller frees
        foundation::String result(reinterpret_cast<const foundation::utf8char*>(text));
        SDL_free(text);
        return result;
    }

    bool SDL3Shell::HasClipboardText() const noexcept { return SDL_HasClipboardText(); }

    static KeyCode MapKeyCode(SDL_Scancode sc) noexcept
    {
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::A) + (sc - SDL_SCANCODE_A));
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::Num1) +
                                        (sc - SDL_SCANCODE_1));
        if (sc == SDL_SCANCODE_0)
            return KeyCode::Num0;
        if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::F1) +
                                        (sc - SDL_SCANCODE_F1));
        if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24)
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::F13) +
                                        (sc - SDL_SCANCODE_F13));
        if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) // SDL keypad digits: 1..9 then 0
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::Keypad1) +
                                        (sc - SDL_SCANCODE_KP_1));
        switch (sc)
        {
        case SDL_SCANCODE_RETURN:
            return KeyCode::Return;
        case SDL_SCANCODE_ESCAPE:
            return KeyCode::Escape;
        case SDL_SCANCODE_BACKSPACE:
            return KeyCode::Backspace;
        case SDL_SCANCODE_TAB:
            return KeyCode::Tab;
        case SDL_SCANCODE_SPACE:
            return KeyCode::Space;
        case SDL_SCANCODE_UP:
            return KeyCode::Up;
        case SDL_SCANCODE_DOWN:
            return KeyCode::Down;
        case SDL_SCANCODE_LEFT:
            return KeyCode::Left;
        case SDL_SCANCODE_RIGHT:
            return KeyCode::Right;
        case SDL_SCANCODE_LCTRL:
            return KeyCode::LeftCtrl;
        case SDL_SCANCODE_LSHIFT:
            return KeyCode::LeftShift;
        case SDL_SCANCODE_LALT:
            return KeyCode::LeftAlt;
        case SDL_SCANCODE_LGUI:
            return KeyCode::LeftGui;
        case SDL_SCANCODE_RCTRL:
            return KeyCode::RightCtrl;
        case SDL_SCANCODE_RSHIFT:
            return KeyCode::RightShift;
        case SDL_SCANCODE_RALT:
            return KeyCode::RightAlt;
        case SDL_SCANCODE_RGUI:
            return KeyCode::RightGui;
        case SDL_SCANCODE_DELETE:
            return KeyCode::Delete;
        case SDL_SCANCODE_INSERT:
            return KeyCode::Insert;
        case SDL_SCANCODE_HOME:
            return KeyCode::Home;
        case SDL_SCANCODE_END:
            return KeyCode::End;
        case SDL_SCANCODE_PAGEUP:
            return KeyCode::PageUp;
        case SDL_SCANCODE_PAGEDOWN:
            return KeyCode::PageDown;
        case SDL_SCANCODE_KP_ENTER:
            return KeyCode::KeypadEnter;
        case SDL_SCANCODE_KP_0:
            return KeyCode::Keypad0;
        case SDL_SCANCODE_KP_DIVIDE:
            return KeyCode::KeypadDivide;
        case SDL_SCANCODE_KP_MULTIPLY:
            return KeyCode::KeypadMultiply;
        case SDL_SCANCODE_KP_MINUS:
            return KeyCode::KeypadMinus;
        case SDL_SCANCODE_KP_PLUS:
            return KeyCode::KeypadPlus;
        case SDL_SCANCODE_KP_PERIOD:
            return KeyCode::KeypadDecimal;
        case SDL_SCANCODE_MINUS:
            return KeyCode::Minus;
        case SDL_SCANCODE_EQUALS:
            return KeyCode::Equals;
        case SDL_SCANCODE_LEFTBRACKET:
            return KeyCode::LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET:
            return KeyCode::RightBracket;
        case SDL_SCANCODE_BACKSLASH:
            return KeyCode::Backslash;
        case SDL_SCANCODE_SEMICOLON:
            return KeyCode::Semicolon;
        case SDL_SCANCODE_APOSTROPHE:
            return KeyCode::Apostrophe;
        case SDL_SCANCODE_GRAVE:
            return KeyCode::Grave;
        case SDL_SCANCODE_COMMA:
            return KeyCode::Comma;
        case SDL_SCANCODE_PERIOD:
            return KeyCode::Period;
        case SDL_SCANCODE_SLASH:
            return KeyCode::Slash;
        case SDL_SCANCODE_CAPSLOCK:
            return KeyCode::CapsLock;
        case SDL_SCANCODE_SCROLLLOCK:
            return KeyCode::ScrollLock;
        case SDL_SCANCODE_NUMLOCKCLEAR:
            return KeyCode::NumLock;
        case SDL_SCANCODE_PRINTSCREEN:
            return KeyCode::PrintScreen;
        case SDL_SCANCODE_PAUSE:
            return KeyCode::Pause;
        case SDL_SCANCODE_APPLICATION:
            return KeyCode::Menu;
        default:
            return KeyCode::Unknown;
        }
    }

    static KeyModifiers MapModifiers(SDL_Keymod mod) noexcept
    {
        KeyModifiers m = KeyModifiers::None;
        if (mod & SDL_KMOD_LSHIFT)
        {
            m |= KeyModifiers::LeftShift;
        }
        if (mod & SDL_KMOD_RSHIFT)
        {
            m |= KeyModifiers::RightShift;
        }
        if (mod & SDL_KMOD_LCTRL)
        {
            m |= KeyModifiers::LeftCtrl;
        }
        if (mod & SDL_KMOD_RCTRL)
        {
            m |= KeyModifiers::RightCtrl;
        }
        if (mod & SDL_KMOD_LALT)
        {
            m |= KeyModifiers::LeftAlt;
        }
        if (mod & SDL_KMOD_RALT)
        {
            m |= KeyModifiers::RightAlt;
        }
        if (mod & SDL_KMOD_LGUI)
        {
            m |= KeyModifiers::LeftGui;
        }
        if (mod & SDL_KMOD_RGUI)
        {
            m |= KeyModifiers::RightGui;
        }
        if (mod & SDL_KMOD_NUM)
        {
            m |= KeyModifiers::NumLock;
        }
        if (mod & SDL_KMOD_CAPS)
        {
            m |= KeyModifiers::CapsLock;
        }
        if (mod & SDL_KMOD_SCROLL)
        {
            m |= KeyModifiers::ScrollLock;
        }
        return m;
    }

    static GamepadButton MapGamepadButton(SDL_GamepadButton b) noexcept
    {
        switch (b)
        {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return GamepadButton::South;
        case SDL_GAMEPAD_BUTTON_EAST:
            return GamepadButton::East;
        case SDL_GAMEPAD_BUTTON_WEST:
            return GamepadButton::West;
        case SDL_GAMEPAD_BUTTON_NORTH:
            return GamepadButton::North;
        case SDL_GAMEPAD_BUTTON_BACK:
            return GamepadButton::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE:
            return GamepadButton::Guide;
        case SDL_GAMEPAD_BUTTON_START:
            return GamepadButton::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return GamepadButton::LeftStick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return GamepadButton::RightStick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return GamepadButton::LeftShoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return GamepadButton::RightShoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return GamepadButton::DPadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return GamepadButton::DPadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return GamepadButton::DPadLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return GamepadButton::DPadRight;
        case SDL_GAMEPAD_BUTTON_MISC1:
            return GamepadButton::Misc1;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
            return GamepadButton::LeftPaddle1;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
            return GamepadButton::LeftPaddle2;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
            return GamepadButton::RightPaddle1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
            return GamepadButton::RightPaddle2;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD:
            return GamepadButton::Touchpad;
        default:
            return GamepadButton::Count; // unmapped
        }
    }

    MouseButton SDL3Shell::MapMouseButton(foundation::u32 idx) noexcept
    {
        switch (idx)
        {
        case 0:
            return MouseButton::Left;
        case 1:
            return MouseButton::Middle;
        case 2:
            return MouseButton::Right;
        case 3:
            return MouseButton::X1;
        case 4:
            return MouseButton::X2;
        default:
            return MouseButton::Count;
        }
    }

    static GamepadAxis MapGamepadAxis(SDL_GamepadAxis a) noexcept
    {
        switch (a)
        {
        case SDL_GAMEPAD_AXIS_LEFTX:
            return GamepadAxis::LeftX;
        case SDL_GAMEPAD_AXIS_LEFTY:
            return GamepadAxis::LeftY;
        case SDL_GAMEPAD_AXIS_RIGHTX:
            return GamepadAxis::RightX;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            return GamepadAxis::RightY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return GamepadAxis::LeftTrigger;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return GamepadAxis::RightTrigger;
        default:
            return GamepadAxis::Count; // unmapped
        }
    }

}

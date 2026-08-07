// Draconic::ShellWeb - `draconic.shell.web:input`.
//
// The web shell's input devices, wired to the browser via Emscripten's HTML5 API. Keyboard (window),
// mouse move/button/wheel (canvas) and touch start/move/end (canvas) are EVENT-DRIVEN: the callbacks
// fire ASYNCHRONOUSLY between animation frames, so they ENQUEUE raw events; WebInputManager::Update()
// (called once per frame from the shell's ProcessEvents) drains the queue AFTER snapshotting the
// previous frame's state - which keeps IsKeyPressed/Released ("went down/up THIS frame") correct,
// the same model the SDL3 desktop shell uses (BeginFrame, then apply the frame's events). Gamepads
// are POLL-based (the browser Gamepad API): Update() samples every connected pad each frame.

module;
#include "Draconic.Foundation/Prelude.h"
#include <emscripten/html5.h>

export module draconic.shell.web:input;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    // Map a DOM KeyboardEvent.code (the physical key, layout-independent - "KeyW", "ArrowUp",
    // "Space", "ShiftLeft", ...) to a Draconic KeyCode.
    [[nodiscard]] inline KeyCode KeyCodeFromDom(const char* code) noexcept
    {
        if (code == nullptr || code[0] == '\0')
        {
            return KeyCode::Unknown;
        }
        const foundation::StringView c(reinterpret_cast<const foundation::utf8char*>(code));
        const auto eq = [&c](const char8_t* s) { return c == foundation::StringView(s); };

        // Letters: "KeyA".."KeyZ".
        if (c.Size() == 4 && code[0] == 'K' && code[1] == 'e' && code[2] == 'y')
        {
            const char ch = code[3];
            if (ch >= 'A' && ch <= 'Z')
            {
                return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::A) + (ch - 'A'));
            }
        }
        // Digits: "Digit0".."Digit9".
        if (c.Size() == 6 && code[5] >= '0' && code[5] <= '9' &&
            foundation::StringView(reinterpret_cast<const foundation::utf8char*>("Digit")) == c.SubStr(0, 5))
        {
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::Num0) + (code[5] - '0'));
        }
        // Numpad digits: "Numpad0".."Numpad9".
        if (c.Size() == 7 && foundation::StringView(reinterpret_cast<const foundation::utf8char*>("Numpad")) ==
                                 c.SubStr(0, 6) &&
            code[6] >= '0' && code[6] <= '9')
        {
            return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::Keypad0) + (code[6] - '0'));
        }
        // Function keys "F1".."F24".
        if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9')
        {
            foundation::i32 n = code[1] - '0';
            if (code[2] >= '0' && code[2] <= '9')
            {
                n = n * 10 + (code[2] - '0');
            }
            if (n >= 1 && n <= 24)
            {
                return static_cast<KeyCode>(static_cast<foundation::u32>(KeyCode::F1) + (n - 1));
            }
        }

        // Named keys.
        if (eq(u8"Space")) return KeyCode::Space;
        if (eq(u8"Enter")) return KeyCode::Return;
        if (eq(u8"Escape")) return KeyCode::Escape;
        if (eq(u8"Backspace")) return KeyCode::Backspace;
        if (eq(u8"Tab")) return KeyCode::Tab;
        if (eq(u8"Minus")) return KeyCode::Minus;
        if (eq(u8"Equal")) return KeyCode::Equals;
        if (eq(u8"BracketLeft")) return KeyCode::LeftBracket;
        if (eq(u8"BracketRight")) return KeyCode::RightBracket;
        if (eq(u8"Backslash")) return KeyCode::Backslash;
        if (eq(u8"Semicolon")) return KeyCode::Semicolon;
        if (eq(u8"Quote")) return KeyCode::Apostrophe;
        if (eq(u8"Backquote")) return KeyCode::Grave;
        if (eq(u8"Comma")) return KeyCode::Comma;
        if (eq(u8"Period")) return KeyCode::Period;
        if (eq(u8"Slash")) return KeyCode::Slash;
        if (eq(u8"CapsLock")) return KeyCode::CapsLock;
        if (eq(u8"ArrowRight")) return KeyCode::Right;
        if (eq(u8"ArrowLeft")) return KeyCode::Left;
        if (eq(u8"ArrowDown")) return KeyCode::Down;
        if (eq(u8"ArrowUp")) return KeyCode::Up;
        if (eq(u8"Insert")) return KeyCode::Insert;
        if (eq(u8"Home")) return KeyCode::Home;
        if (eq(u8"PageUp")) return KeyCode::PageUp;
        if (eq(u8"Delete")) return KeyCode::Delete;
        if (eq(u8"End")) return KeyCode::End;
        if (eq(u8"PageDown")) return KeyCode::PageDown;
        if (eq(u8"ControlLeft")) return KeyCode::LeftCtrl;
        if (eq(u8"ShiftLeft")) return KeyCode::LeftShift;
        if (eq(u8"AltLeft")) return KeyCode::LeftAlt;
        if (eq(u8"MetaLeft")) return KeyCode::LeftGui;
        if (eq(u8"ControlRight")) return KeyCode::RightCtrl;
        if (eq(u8"ShiftRight")) return KeyCode::RightShift;
        if (eq(u8"AltRight")) return KeyCode::RightAlt;
        if (eq(u8"MetaRight")) return KeyCode::RightGui;
        if (eq(u8"ContextMenu")) return KeyCode::Menu;
        if (eq(u8"NumpadEnter")) return KeyCode::KeypadEnter;
        if (eq(u8"NumpadAdd")) return KeyCode::KeypadPlus;
        if (eq(u8"NumpadSubtract")) return KeyCode::KeypadMinus;
        if (eq(u8"NumpadMultiply")) return KeyCode::KeypadMultiply;
        if (eq(u8"NumpadDivide")) return KeyCode::KeypadDivide;
        if (eq(u8"NumpadDecimal")) return KeyCode::KeypadDecimal;
        if (eq(u8"PrintScreen")) return KeyCode::PrintScreen;
        if (eq(u8"ScrollLock")) return KeyCode::ScrollLock;
        if (eq(u8"Pause")) return KeyCode::Pause;
        if (eq(u8"NumLock")) return KeyCode::NumLock;
        return KeyCode::Unknown;
    }

    class WebKeyboard final : public IKeyboard
    {
    public:
        [[nodiscard]] bool IsKeyDown(KeyCode k) const override { return m_current[Index(k)]; }
        [[nodiscard]] bool IsKeyPressed(KeyCode k) const override
        {
            return m_current[Index(k)] && !m_previous[Index(k)];
        }
        [[nodiscard]] bool IsKeyReleased(KeyCode k) const override
        {
            return !m_current[Index(k)] && m_previous[Index(k)];
        }
        [[nodiscard]] KeyModifiers Modifiers() const override { return m_mods; }

        void SetKey(KeyCode k, bool down) { m_current[Index(k)] = down; }
        void SetModifiers(KeyModifiers m) { m_mods = m; }
        void BeginFrame()
        {
            for (foundation::u32 i = 0; i < kCount; ++i)
            {
                m_previous[i] = m_current[i];
            }
        }

    private:
        static constexpr foundation::u32 kCount = static_cast<foundation::u32>(KeyCode::Count);
        static foundation::u32 Index(KeyCode k) noexcept
        {
            const foundation::u32 i = static_cast<foundation::u32>(k);
            return i < kCount ? i : 0;
        }
        bool m_current[kCount] = {};
        bool m_previous[kCount] = {};
        KeyModifiers m_mods = KeyModifiers::None;
    };

    class WebMouse final : public IMouse
    {
    public:
        [[nodiscard]] foundation::f32 X() const override { return m_x; }
        [[nodiscard]] foundation::f32 Y() const override { return m_y; }
        [[nodiscard]] foundation::f32 GlobalX() const override { return m_x; }
        [[nodiscard]] foundation::f32 GlobalY() const override { return m_y; }
        [[nodiscard]] foundation::f32 DeltaX() const override { return m_dx; }
        [[nodiscard]] foundation::f32 DeltaY() const override { return m_dy; }
        [[nodiscard]] foundation::f32 ScrollX() const override { return m_sx; }
        [[nodiscard]] foundation::f32 ScrollY() const override { return m_sy; }
        [[nodiscard]] bool IsButtonDown(MouseButton b) const override { return m_current[Index(b)]; }
        [[nodiscard]] bool IsButtonPressed(MouseButton b) const override
        {
            return m_current[Index(b)] && !m_previous[Index(b)];
        }
        [[nodiscard]] bool IsButtonReleased(MouseButton b) const override
        {
            return !m_current[Index(b)] && m_previous[Index(b)];
        }
        [[nodiscard]] bool RelativeMode() const override { return m_relative; }
        void SetRelativeMode(bool enabled) override { m_relative = enabled; } // pointer-lock: later
        [[nodiscard]] bool CursorVisible() const override { return m_cursorVisible; }
        void SetCursorVisible(bool v) override { m_cursorVisible = v; }
        void SetCursor(CursorType) override {}
        void SetGlobalCapture(bool) override {}

        void OnMotion(foundation::f32 x, foundation::f32 y, foundation::f32 dx, foundation::f32 dy)
        {
            m_x = x;
            m_y = y;
            m_dx += dx;
            m_dy += dy;
        }
        void OnButton(foundation::u32 button, bool down)
        {
            if (button < kCount)
            {
                m_current[button] = down;
            }
        }
        void OnWheel(foundation::f32 sx, foundation::f32 sy)
        {
            m_sx += sx;
            m_sy += sy;
        }
        void BeginFrame()
        {
            for (foundation::u32 i = 0; i < kCount; ++i)
            {
                m_previous[i] = m_current[i];
            }
            m_dx = m_dy = m_sx = m_sy = 0.0f;
        }

    private:
        static constexpr foundation::u32 kCount = static_cast<foundation::u32>(MouseButton::Count);
        static foundation::u32 Index(MouseButton b) noexcept
        {
            const foundation::u32 i = static_cast<foundation::u32>(b);
            return i < kCount ? i : 0;
        }
        foundation::f32 m_x = 0, m_y = 0, m_dx = 0, m_dy = 0, m_sx = 0, m_sy = 0;
        bool m_current[kCount] = {};
        bool m_previous[kCount] = {};
        bool m_relative = false;
        bool m_cursorVisible = true;
    };

    class WebTouch final : public ITouch
    {
    public:
        [[nodiscard]] foundation::i32 TouchCount() const override
        {
            return static_cast<foundation::i32>(m_points.Size());
        }
        [[nodiscard]] bool GetTouchPoint(foundation::i32 index, TouchPoint& out) const override
        {
            if (index < 0 || static_cast<foundation::usize>(index) >= m_points.Size())
            {
                return false;
            }
            out = m_points[static_cast<foundation::usize>(index)];
            return true;
        }
        [[nodiscard]] bool HasTouch() const override { return m_points.Size() > 0; }

        // Upsert a touch point (touchstart/touchmove); Remove drops it (touchend/touchcancel).
        void Upsert(foundation::u64 id, foundation::f32 x, foundation::f32 y)
        {
            for (TouchPoint& p : m_points)
            {
                if (p.id == id)
                {
                    p.x = x;
                    p.y = y;
                    return;
                }
            }
            m_points.PushBack(TouchPoint{id, x, y, 1.0f});
        }
        void Remove(foundation::u64 id)
        {
            for (foundation::usize i = 0; i < m_points.Size(); ++i)
            {
                if (m_points[i].id == id)
                {
                    m_points.RemoveAt(i);
                    return;
                }
            }
        }

    private:
        foundation::Array<TouchPoint> m_points;
    };

    // One connected browser gamepad, filled by polling emscripten_get_gamepad_status each frame.
    class WebGamepad final : public IGamepad
    {
    public:
        [[nodiscard]] foundation::i32 Index() const override { return m_index; }
        [[nodiscard]] foundation::StringView Name() const override { return m_name; }
        [[nodiscard]] bool Connected() const override { return m_connected; }

        [[nodiscard]] bool IsButtonDown(GamepadButton b) const override
        {
            const foundation::i32 i = BrowserButton(b);
            return i >= 0 && m_buttons[i];
        }
        [[nodiscard]] bool IsButtonPressed(GamepadButton b) const override
        {
            const foundation::i32 i = BrowserButton(b);
            return i >= 0 && m_buttons[i] && !m_prevButtons[i];
        }
        [[nodiscard]] bool IsButtonReleased(GamepadButton b) const override
        {
            const foundation::i32 i = BrowserButton(b);
            return i >= 0 && !m_buttons[i] && m_prevButtons[i];
        }
        [[nodiscard]] foundation::f32 Axis(GamepadAxis a) const override
        {
            switch (a)
            {
            case GamepadAxis::LeftX: return m_axes[0];
            case GamepadAxis::LeftY: return m_axes[1];
            case GamepadAxis::RightX: return m_axes[2];
            case GamepadAxis::RightY: return m_axes[3];
            case GamepadAxis::LeftTrigger: return m_analog[6];  // standard-mapping trigger buttons
            case GamepadAxis::RightTrigger: return m_analog[7];
            default: return 0.0f;
            }
        }
        void SetRumble(foundation::f32, foundation::f32, foundation::u32) override {} // no html5.h haptics binding yet

        // --- polling side (called by the manager) ---
        void BeginFrame()
        {
            for (foundation::i32 i = 0; i < kMaxButtons; ++i)
            {
                m_prevButtons[i] = m_buttons[i];
            }
        }
        void SetDisconnected() { m_connected = false; }
        void Ingest(const EmscriptenGamepadEvent& e)
        {
            m_connected = e.connected != 0;
            m_index = static_cast<foundation::i32>(e.index);
            if (m_name.Size() == 0)
            {
                m_name = foundation::String(reinterpret_cast<const foundation::utf8char*>(e.id));
            }
            const foundation::i32 nb = static_cast<foundation::i32>(e.numButtons);
            for (foundation::i32 i = 0; i < kMaxButtons; ++i)
            {
                m_buttons[i] = (i < nb) && (e.digitalButton[i] != 0);
                m_analog[i] = (i < nb) ? static_cast<foundation::f32>(e.analogButton[i]) : 0.0f;
            }
            const foundation::i32 na = static_cast<foundation::i32>(e.numAxes);
            for (foundation::i32 i = 0; i < kMaxAxes; ++i)
            {
                m_axes[i] = (i < na) ? static_cast<foundation::f32>(e.axis[i]) : 0.0f;
            }
        }

    private:
        static constexpr foundation::i32 kMaxButtons = 20; // W3C standard-mapping button count
        static constexpr foundation::i32 kMaxAxes = 8;
        // Draconic button -> browser standard-mapping index (-1 = unmapped).
        static foundation::i32 BrowserButton(GamepadButton b) noexcept
        {
            switch (b)
            {
            case GamepadButton::South: return 0;
            case GamepadButton::East: return 1;
            case GamepadButton::West: return 2;
            case GamepadButton::North: return 3;
            case GamepadButton::LeftShoulder: return 4;
            case GamepadButton::RightShoulder: return 5;
            case GamepadButton::Back: return 8;
            case GamepadButton::Start: return 9;
            case GamepadButton::LeftStick: return 10;
            case GamepadButton::RightStick: return 11;
            case GamepadButton::DPadUp: return 12;
            case GamepadButton::DPadDown: return 13;
            case GamepadButton::DPadLeft: return 14;
            case GamepadButton::DPadRight: return 15;
            case GamepadButton::Guide: return 16;
            default: return -1;
            }
        }

        foundation::i32 m_index = 0;
        foundation::String m_name;
        bool m_connected = false;
        bool m_buttons[kMaxButtons] = {};
        bool m_prevButtons[kMaxButtons] = {};
        foundation::f32 m_analog[kMaxButtons] = {};
        foundation::f32 m_axes[kMaxAxes] = {};
    };

    class WebInputManager final : public IInputManager
    {
    public:
        [[nodiscard]] IKeyboard* Keyboard() override { return &m_keyboard; }
        [[nodiscard]] IMouse* Mouse() override { return &m_mouse; }
        [[nodiscard]] ITouch* Touch() override { return &m_touch; }
        [[nodiscard]] foundation::i32 GamepadCount() const override
        {
            return static_cast<foundation::i32>(m_connectedPads.Size());
        }
        [[nodiscard]] IGamepad* GetGamepad(foundation::i32 index) override
        {
            if (index < 0 || static_cast<foundation::usize>(index) >= m_connectedPads.Size())
            {
                return nullptr;
            }
            return m_connectedPads[static_cast<foundation::usize>(index)];
        }
        [[nodiscard]] foundation::Span<const InputEvent> Events() const override
        {
            return foundation::Span<const InputEvent>(m_events.Data(), m_events.Size());
        }
        [[nodiscard]] foundation::u32 HoverWindow() const override { return m_mainWindow; }
        [[nodiscard]] foundation::u32 FocusedWindow() const override { return m_mainWindow; }

        // Register the HTML5 event callbacks (keyboard on the window, mouse on the canvas). Called
        // once by the shell after the canvas exists. The callbacks feed the async queue below.
        void RegisterCallbacks(foundation::StringView canvasSelector, foundation::u32 mainWindowId)
        {
            m_mainWindow = mainWindowId;
            m_selector = foundation::String(canvasSelector);
            const char* canvas = reinterpret_cast<const char*>(m_selector.CStr());
            emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, &OnKey);
            emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, &OnKey);
            emscripten_set_mousemove_callback(canvas, this, EM_TRUE, &OnMouseMove);
            emscripten_set_mousedown_callback(canvas, this, EM_TRUE, &OnMouseButton);
            emscripten_set_mouseup_callback(canvas, this, EM_TRUE, &OnMouseButton);
            emscripten_set_wheel_callback(canvas, this, EM_TRUE, &OnWheel);
            emscripten_set_touchstart_callback(canvas, this, EM_TRUE, &OnTouch);
            emscripten_set_touchmove_callback(canvas, this, EM_TRUE, &OnTouch);
            emscripten_set_touchend_callback(canvas, this, EM_TRUE, &OnTouch);
            emscripten_set_touchcancel_callback(canvas, this, EM_TRUE, &OnTouch);
            // Gamepads are polled in Update(); the browser only reveals a pad after a button press.
        }

        // Per frame (shell ProcessEvents): snapshot the previous frame, then apply the events that
        // arrived since the last Update - keeping pressed/released and per-frame deltas correct.
        void Update() override
        {
            RefreshPointerScale();
            m_keyboard.BeginFrame();
            m_mouse.BeginFrame();
            m_events.Clear();
            for (const RawEvent& r : m_queue)
            {
                Apply(r);
            }
            m_queue.Clear();
            PollGamepads();
        }

    private:
        // CSS-pixel -> backing-pixel scale for pointer coordinates. HTML5 mouse/touch
        // events report CSS pixels, but the window reports the canvas BACKING size (the
        // shell sizes the backing store at min(devicePixelRatio, 2) x the CSS size), so
        // at dpr > 1 unscaled clicks land offset/half-scale for every hit-test consumer.
        // Derived from the two live sizes (not devicePixelRatio) so the shell's cap and
        // any CSS-vs-backing policy are honored automatically.
        void RefreshPointerScale()
        {
            m_pointerScaleX = 1.0f;
            m_pointerScaleY = 1.0f;
            if (m_selector.IsEmpty())
            {
                return;
            }
            const char* canvas = reinterpret_cast<const char*>(m_selector.CStr());
            double cssW = 0.0, cssH = 0.0;
            int backingW = 0, backingH = 0;
            if (emscripten_get_element_css_size(canvas, &cssW, &cssH) !=
                    EMSCRIPTEN_RESULT_SUCCESS ||
                emscripten_get_canvas_element_size(canvas, &backingW, &backingH) !=
                    EMSCRIPTEN_RESULT_SUCCESS)
            {
                return;
            }
            if (cssW > 0.0 && backingW > 0)
            {
                m_pointerScaleX = static_cast<foundation::f32>(backingW / cssW);
            }
            if (cssH > 0.0 && backingH > 0)
            {
                m_pointerScaleY = static_cast<foundation::f32>(backingH / cssH);
            }
        }

        struct RawEvent
        {
            enum class Type : foundation::u8
            {
                Key,
                MouseMove,
                MouseButton,
                Wheel,
                Touch
            } type{};
            KeyCode key{};
            KeyModifiers mods{};
            foundation::u32 button = 0;
            bool down = false;
            foundation::f32 x = 0, y = 0, dx = 0, dy = 0, sx = 0, sy = 0;
            foundation::u64 touchId = 0;
            foundation::u8 touchPhase = 0; // 0 = start, 1 = move, 2 = end/cancel
        };

        void Apply(const RawEvent& r)
        {
            switch (r.type)
            {
            case RawEvent::Type::Key:
            {
                m_keyboard.SetKey(r.key, r.down);
                m_keyboard.SetModifiers(r.mods);
                InputEvent e;
                e.kind = r.down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
                e.window = m_mainWindow;
                e.key = r.key;
                e.modifiers = r.mods;
                m_events.PushBack(e);
                break;
            }
            case RawEvent::Type::MouseMove:
            {
                const foundation::f32 x = r.x * m_pointerScaleX;
                const foundation::f32 y = r.y * m_pointerScaleY;
                const foundation::f32 dx = r.dx * m_pointerScaleX;
                const foundation::f32 dy = r.dy * m_pointerScaleY;
                m_mouse.OnMotion(x, y, dx, dy);
                InputEvent e;
                e.kind = InputEventKind::MouseMove;
                e.window = m_mainWindow;
                e.x = x;
                e.y = y;
                e.dx = dx;
                e.dy = dy;
                m_events.PushBack(e);
                break;
            }
            case RawEvent::Type::MouseButton:
            {
                m_mouse.OnButton(r.button, r.down);
                InputEvent e;
                e.kind = r.down ? InputEventKind::MouseButtonDown : InputEventKind::MouseButtonUp;
                e.window = m_mainWindow;
                e.button = static_cast<MouseButton>(r.button);
                m_events.PushBack(e);
                break;
            }
            case RawEvent::Type::Wheel:
            {
                m_mouse.OnWheel(r.sx, r.sy);
                InputEvent e;
                e.kind = InputEventKind::MouseWheel;
                e.window = m_mainWindow;
                e.x = r.sx;
                e.y = r.sy;
                m_events.PushBack(e);
                break;
            }
            case RawEvent::Type::Touch:
            {
                const foundation::f32 x = r.x * m_pointerScaleX;
                const foundation::f32 y = r.y * m_pointerScaleY;
                if (r.touchPhase == 2)
                {
                    m_touch.Remove(r.touchId);
                }
                else
                {
                    m_touch.Upsert(r.touchId, x, y);
                }
                InputEvent e;
                e.kind = (r.touchPhase == 0)   ? InputEventKind::TouchDown
                         : (r.touchPhase == 1) ? InputEventKind::TouchMove
                                               : InputEventKind::TouchUp;
                e.window = m_mainWindow;
                e.touchId = r.touchId;
                e.x = x;
                e.y = y;
                m_events.PushBack(e);
                break;
            }
            }
        }

        // Poll the browser Gamepad API (poll-based, unlike the event-driven devices). Rebuilds the
        // compact list of connected pads and emits button-transition events into the frame stream.
        void PollGamepads()
        {
            emscripten_sample_gamepad_data();
            const foundation::i32 num = emscripten_get_num_gamepads();
            m_connectedPads.Clear();
            if (num < 0) // EMSCRIPTEN_RESULT_NOT_SUPPORTED: no Gamepad API in this browser
            {
                return;
            }
            const foundation::i32 count = num < kMaxGamepads ? num : kMaxGamepads;
            for (foundation::i32 i = 0; i < count; ++i)
            {
                EmscriptenGamepadEvent ev;
                if (emscripten_get_gamepad_status(i, &ev) != EMSCRIPTEN_RESULT_SUCCESS ||
                    ev.connected == 0)
                {
                    m_slots[i].SetDisconnected();
                    continue;
                }
                m_slots[i].BeginFrame(); // snapshot previous buttons before ingesting this frame
                m_slots[i].Ingest(ev);
                m_connectedPads.PushBack(&m_slots[i]);
                // Events carry the COMPACT index (what GetGamepad() takes) - stamping the
                // browser SLOT would dangle after an earlier pad disconnects.
                EmitGamepadEvents(m_slots[i],
                                  static_cast<foundation::i32>(m_connectedPads.Size()) - 1);
            }
        }

        void EmitGamepadEvents(const WebGamepad& pad, foundation::i32 index)
        {
            for (foundation::u32 b = 0; b < static_cast<foundation::u32>(GamepadButton::Count); ++b)
            {
                const GamepadButton gb = static_cast<GamepadButton>(b);
                if (pad.IsButtonPressed(gb) || pad.IsButtonReleased(gb))
                {
                    InputEvent e;
                    e.kind = pad.IsButtonPressed(gb) ? InputEventKind::GamepadButtonDown
                                                     : InputEventKind::GamepadButtonUp;
                    e.window = m_mainWindow;
                    e.gamepad = index;
                    e.padButton = gb;
                    m_events.PushBack(e);
                }
            }
        }

        [[nodiscard]] static KeyModifiers ModsFrom(const EmscriptenKeyboardEvent* e)
        {
            KeyModifiers m = KeyModifiers::None;
            if (e->shiftKey)
                m = m | KeyModifiers::Shift;
            if (e->ctrlKey)
                m = m | KeyModifiers::Ctrl;
            if (e->altKey)
                m = m | KeyModifiers::Alt;
            if (e->metaKey)
                m = m | KeyModifiers::Gui;
            return m;
        }

        // --- HTML5 callbacks (plain C function pointers; userData is the manager) ---
        static EM_BOOL OnKey(int eventType, const EmscriptenKeyboardEvent* e, void* userData)
        {
            auto* self = static_cast<WebInputManager*>(userData);
            RawEvent r;
            r.type = RawEvent::Type::Key;
            r.key = KeyCodeFromDom(e->code);
            r.down = (eventType == EMSCRIPTEN_EVENT_KEYDOWN);
            r.mods = ModsFrom(e);
            self->m_queue.PushBack(r);
            // Consume plain keys so the page doesn't scroll on space/arrows - but let the
            // BROWSER keep its own chords and function keys (F5 refresh, F12 devtools,
            // Ctrl/Cmd+C/V/R/W...); eating those turns the tab into a trap.
            const bool browserChord = (e->ctrlKey != 0) || (e->metaKey != 0);
            const bool functionKey = e->code[0] == 'F' && e->code[1] >= '0' && e->code[1] <= '9';
            return (browserChord || functionKey) ? EM_FALSE : EM_TRUE;
        }
        static EM_BOOL OnMouseMove(int, const EmscriptenMouseEvent* e, void* userData)
        {
            auto* self = static_cast<WebInputManager*>(userData);
            RawEvent r;
            r.type = RawEvent::Type::MouseMove;
            r.x = static_cast<foundation::f32>(e->targetX);
            r.y = static_cast<foundation::f32>(e->targetY);
            r.dx = static_cast<foundation::f32>(e->movementX);
            r.dy = static_cast<foundation::f32>(e->movementY);
            self->m_queue.PushBack(r);
            return EM_TRUE;
        }
        static EM_BOOL OnMouseButton(int eventType, const EmscriptenMouseEvent* e, void* userData)
        {
            auto* self = static_cast<WebInputManager*>(userData);
            RawEvent r;
            r.type = RawEvent::Type::MouseButton;
            // DOM button: 0 left, 1 middle, 2 right (matches MouseButton Left/Middle/Right order).
            r.button = static_cast<foundation::u32>(e->button);
            r.down = (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN);
            self->m_queue.PushBack(r);
            return EM_TRUE;
        }
        static EM_BOOL OnWheel(int, const EmscriptenWheelEvent* e, void* userData)
        {
            auto* self = static_cast<WebInputManager*>(userData);
            RawEvent r;
            r.type = RawEvent::Type::Wheel;
            // Normalize to "notches", honoring deltaMode: pixels (Chrome, ~100/notch),
            // lines (Firefox, ~3/notch), or pages. Ignoring the mode made Firefox scroll
            // ~30x too slow. Sign matches scroll-up = +.
            const foundation::f32 perNotch = (e->deltaMode == DOM_DELTA_LINE)   ? 3.0f
                                       : (e->deltaMode == DOM_DELTA_PAGE) ? 1.0f
                                                                          : 100.0f;
            r.sx = -static_cast<foundation::f32>(e->deltaX) / perNotch;
            r.sy = -static_cast<foundation::f32>(e->deltaY) / perNotch;
            self->m_queue.PushBack(r);
            return EM_TRUE;
        }
        static EM_BOOL OnTouch(int eventType, const EmscriptenTouchEvent* e, void* userData)
        {
            auto* self = static_cast<WebInputManager*>(userData);
            const foundation::u8 phase = (eventType == EMSCRIPTEN_EVENT_TOUCHSTART)  ? 0
                                   : (eventType == EMSCRIPTEN_EVENT_TOUCHMOVE) ? 1
                                                                               : 2; // end / cancel
            for (int i = 0; i < e->numTouches; ++i)
            {
                const EmscriptenTouchPoint& t = e->touches[i];
                if (t.isChanged == 0)
                {
                    continue; // only the points that actually changed in this event
                }
                RawEvent r;
                r.type = RawEvent::Type::Touch;
                r.touchId = static_cast<foundation::u64>(t.identifier);
                r.x = static_cast<foundation::f32>(t.targetX);
                r.y = static_cast<foundation::f32>(t.targetY);
                r.touchPhase = phase;
                self->m_queue.PushBack(r);
            }
            return EM_TRUE; // consume so the browser doesn't scroll/zoom the page on a canvas touch
        }

        static constexpr foundation::i32 kMaxGamepads = 4;

        WebKeyboard m_keyboard;
        WebMouse m_mouse;
        WebTouch m_touch;
        WebGamepad m_slots[kMaxGamepads];            // one per browser gamepad index (stable state)
        foundation::Array<WebGamepad*> m_connectedPads;    // compact list of currently-connected pads
        foundation::f32 m_pointerScaleX = 1.0f; // CSS -> backing pixels (see RefreshPointerScale)
        foundation::f32 m_pointerScaleY = 1.0f;
        foundation::Array<RawEvent> m_queue;   // filled async by the callbacks, drained in Update()
        foundation::Array<InputEvent> m_events; // this frame's event stream (valid until next Update)
        foundation::u32 m_mainWindow = 0;
        foundation::String m_selector; // kept alive for the mouse callbacks' target
    };
}

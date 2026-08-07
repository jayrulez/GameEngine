// Draconic UI - `draconic.ui.shell`: the platform input bridge for draconic.ui.
//
// Keeps the core draconic.ui platform-agnostic: it exposes UIContext's InputManager (physical-pixel
// Process* API) and this bridge is the only place that knows draconic.shell. It translates a stream of
// shell::InputEvent (already gated/transformed by InputSurface/InputRouter, see [[viewport-input]]) into
// InputManager calls, and drives the window's text input (IME) from focus via UIContext::WantsTextInput()
// - the mechanism Sedulous shell never finished. Reimplemented for Draconic (NOT ported from Sedulous.
// UI.Shell), mirroring the draconic.gui GuiInputBridge, per the port plan: InputSurface consumption lives
// here, never in the core.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.shell;

import draconic.foundation;  // Float2, StringView, DecodeUtf8
import draconic.ui;    // UIContext, InputManager, KeyCode, MouseButton, KeyModifiers
import draconic.shell; // InputEvent, InputSurface, IMouse, IWindow

using namespace draconic::foundation;
namespace shell = draconic::shell;

export namespace draconic::ui
{
    /// Bridges the platform (shell) clipboard into the UI's abstract IClipboard seam, so text controls'
    /// Cut/Copy/Paste work. The app sets it via UIContext::SetClipboard. Borrows the shell (non-owning).
    class ShellClipboard final : public IClipboard
    {
    public:
        explicit ShellClipboard(shell::IShell* shell) noexcept : m_shell(shell) {}

        [[nodiscard]] Status GetText(String& outText) override
        {
            if (m_shell == nullptr)
            {
                return ErrorCode::Unknown;
            }
            outText = m_shell->GetClipboardText();
            return {};
        }
        [[nodiscard]] Status SetText(StringView text) override
        {
            if (m_shell == nullptr)
            {
                return ErrorCode::Unknown;
            }
            m_shell->SetClipboardText(text);
            return {};
        }
        [[nodiscard]] bool HasText() override
        {
            return m_shell != nullptr && m_shell->HasClipboardText();
        }

    private:
        shell::IShell* m_shell;
    };

    /// Translates platform input events into UIContext::InputManager calls.
    class UiInputBridge
    {
    public:
        explicit UiInputBridge(UIContext* context) noexcept : m_context(context) {}

        /// The window whose platform text input (IME) follows UI focus. Once set, the bridge starts text
        /// input while a text-editing view holds focus and stops it otherwise - reconciled after each
        /// Dispatch()/PumpFromSurface(). Pass nullptr to disable.
        void SetTextInputTarget(shell::IWindow* window) noexcept { m_textInputTarget = window; }

        /// Reconcile the target window's text-input state with the focused view's WantsTextInput().
        void SyncTextInput()
        {
            if (m_textInputTarget == nullptr || m_context == nullptr)
            {
                return;
            }
            const bool want = m_context->WantsTextInput();
            if (want && !m_textInputTarget->IsTextInputActive())
            {
                m_textInputTarget->StartTextInput();
            }
            else if (!want && m_textInputTarget->IsTextInputActive())
            {
                m_textInputTarget->StopTextInput();
            }
        }

        /// Translate one platform input event into an InputManager call. Returns true if routed, false if
        /// ignored (gamepad/touch/unknown). Reconciles text-input afterwards (a click/Tab can move focus).
        bool Dispatch(const shell::InputEvent& event)
        {
            if (m_context == nullptr)
            {
                return false;
            }
            InputManager* im = m_context->GetInputManager();
            bool routed = true;
            switch (event.kind)
            {
            case shell::InputEventKind::MouseMove:
                m_lastX = event.x;
                m_lastY = event.y;
                (void)im->ProcessMouseMove(event.x, event.y);
                break;
            case shell::InputEventKind::MouseButtonDown:
                m_lastX = event.x;
                m_lastY = event.y;
                (void)im->ProcessMouseDown(MapButton(event.button), event.x, event.y,
                                           m_context->TotalTime());
                break;
            case shell::InputEventKind::MouseButtonUp:
                m_lastX = event.x;
                m_lastY = event.y;
                (void)im->ProcessMouseUp(MapButton(event.button), event.x, event.y);
                break;
            case shell::InputEventKind::MouseWheel:
                // For wheel, x/y is the scroll delta; position is the last known cursor spot.
                (void)im->ProcessMouseWheel(m_lastX, m_lastY, event.x, event.y,
                                            MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::KeyDown:
                (void)im->ProcessKeyDown(MapKey(event.key), MapModifiers(event.modifiers), false,
                                         m_context->TotalTime());
                break;
            case shell::InputEventKind::KeyUp:
                (void)im->ProcessKeyUp(MapKey(event.key), MapModifiers(event.modifiers),
                                       m_context->TotalTime());
                break;
            case shell::InputEventKind::TextInput:
            {
                const StringView text{event.text};
                usize i = 0;
                while (i < text.Size())
                {
                    (void)im->ProcessTextInput(static_cast<char32_t>(DecodeUtf8(text, i)));
                }
                break;
            }
            default:
                routed = false; // gamepad / touch not routed to the UI
                break;
            }
            SyncTextInput();
            return routed;
        }

        /// Poll a gated InputSurface (its mouse is already content-space) and drive the InputManager:
        /// hover from the cursor, click from press/release edges, plus wheel. Keyboard/text still come
        /// through Dispatch().
        void PumpFromSurface(shell::InputSurface& surface)
        {
            if (m_context == nullptr)
            {
                return;
            }
            shell::IMouse* mouse = surface.Mouse();
            if (mouse == nullptr)
            {
                return;
            }
            InputManager* im = m_context->GetInputManager();

            const f32 x = mouse->X();
            const f32 y = mouse->Y();
            m_lastX = x;
            m_lastY = y;
            (void)im->ProcessMouseMove(x, y);

            const shell::MouseButton buttons[3] = {
                shell::MouseButton::Left, shell::MouseButton::Middle, shell::MouseButton::Right};
            for (const shell::MouseButton button : buttons)
            {
                if (mouse->IsButtonPressed(button))
                {
                    (void)im->ProcessMouseDown(MapButton(button), x, y, m_context->TotalTime());
                }
                if (mouse->IsButtonReleased(button))
                {
                    (void)im->ProcessMouseUp(MapButton(button), x, y);
                }
            }

            const f32 scrollX = mouse->ScrollX();
            const f32 scrollY = mouse->ScrollY();
            if (scrollX != 0.0f || scrollY != 0.0f)
            {
                // Carry the live keyboard modifiers so Shift+wheel scrolls horizontally, etc.
                shell::IKeyboard* keyboard = surface.Keyboard();
                const KeyModifiers mods = (keyboard != nullptr)
                                              ? MapModifiers(keyboard->Modifiers())
                                              : KeyModifiers::None;
                (void)im->ProcessMouseWheel(x, y, scrollX, scrollY, mods);
            }

            SyncTextInput();
        }

        /// Feed the InputManager a mouse update at EXPLICIT content coords (not the surface's own cursor
        /// position). Used for cross-window drag routing: while a floating window is dragged, the desktop
        /// cursor sits over THAT window, but the drag must be delivered to ANOTHER window (the main one) at
        /// global-mouse-relative coords so its drop targets see it. Button state still comes from the raw
        /// mouse, so the held drag and its release (the drop) register. (No wheel during a drag.)
        void PumpMouseAt(f32 x, f32 y, shell::IMouse* mouse)
        {
            if (m_context == nullptr || mouse == nullptr)
            {
                return;
            }
            InputManager* im = m_context->GetInputManager();
            m_lastX = x;
            m_lastY = y;
            (void)im->ProcessMouseMove(x, y);

            const shell::MouseButton buttons[3] = {
                shell::MouseButton::Left, shell::MouseButton::Middle, shell::MouseButton::Right};
            for (const shell::MouseButton button : buttons)
            {
                if (mouse->IsButtonPressed(button))
                {
                    (void)im->ProcessMouseDown(MapButton(button), x, y, m_context->TotalTime());
                }
                if (mouse->IsButtonReleased(button))
                {
                    (void)im->ProcessMouseUp(MapButton(button), x, y);
                }
            }
            SyncTextInput();
        }

        /// Push the hovered view's cursor (InputManager::CurrentCursor, the EffectiveCursor of the view
        /// under the pointer) to the OS mouse, mapping the UI CursorType to the shell's. Borderless windows
        /// (e.g. floating dock panels) have no WM to draw resize grips, so the app drives the OS cursor.
        void SyncCursor(shell::IMouse& mouse)
        {
            if (m_context == nullptr)
            {
                return;
            }
            mouse.SetCursor(MapCursor(m_context->GetInputManager()->CurrentCursor()));
        }

    private:
        // UI CursorType -> shell CursorType (the two enums differ in values and names).
        [[nodiscard]] static shell::CursorType MapCursor(CursorType c) noexcept
        {
            switch (c)
            {
            case CursorType::Hand:
                return shell::CursorType::Pointer;
            case CursorType::IBeam:
                return shell::CursorType::Text;
            case CursorType::Crosshair:
                return shell::CursorType::Crosshair;
            case CursorType::SizeNS:
                return shell::CursorType::ResizeNS;
            case CursorType::SizeWE:
                return shell::CursorType::ResizeEW;
            case CursorType::SizeNWSE:
                return shell::CursorType::ResizeNWSE;
            case CursorType::SizeNESW:
                return shell::CursorType::ResizeNESW;
            case CursorType::Move:
                return shell::CursorType::Move;
            case CursorType::NotAllowed:
                return shell::CursorType::NotAllowed;
            case CursorType::Wait:
                return shell::CursorType::Wait;
            default:
                return shell::CursorType::Default; // Default / Arrow
            }
        }

        [[nodiscard]] static MouseButton MapButton(shell::MouseButton button) noexcept
        {
            switch (button)
            {
            case shell::MouseButton::Left:
                return MouseButton::Left;
            case shell::MouseButton::Middle:
                return MouseButton::Middle;
            case shell::MouseButton::Right:
                return MouseButton::Right;
            case shell::MouseButton::X1:
                return MouseButton::X1;
            case shell::MouseButton::X2:
                return MouseButton::X2;
            default:
                return MouseButton::Left;
            }
        }

        // The shell and UI KeyCode enums share names but differ in value. Letters A-Z are contiguous in
        // both, so map that range arithmetically; map the nav/editing keys the UI interprets explicitly.
        [[nodiscard]] static KeyCode MapKey(shell::KeyCode key) noexcept
        {
            using SK = shell::KeyCode;
            const u32 kv = static_cast<u32>(key);
            if (kv >= static_cast<u32>(SK::A) && kv <= static_cast<u32>(SK::Z))
            {
                return static_cast<KeyCode>(static_cast<u32>(KeyCode::A) +
                                            (kv - static_cast<u32>(SK::A)));
            }
            // Function keys (both sides contiguous per dozen) - editors use F2 (rename) etc.
            if (kv >= static_cast<u32>(SK::F1) && kv <= static_cast<u32>(SK::F12))
            {
                return static_cast<KeyCode>(static_cast<u32>(KeyCode::F1) +
                                            (kv - static_cast<u32>(SK::F1)));
            }
            if (kv >= static_cast<u32>(SK::F13) && kv <= static_cast<u32>(SK::F24))
            {
                return static_cast<KeyCode>(static_cast<u32>(KeyCode::F13) +
                                            (kv - static_cast<u32>(SK::F13)));
            }
            // Digit row (shell counts 0..9, ui counts 1..9 then 0 - SDL layout).
            if (kv >= static_cast<u32>(SK::Num1) && kv <= static_cast<u32>(SK::Num9))
            {
                return static_cast<KeyCode>(static_cast<u32>(KeyCode::Num1) +
                                            (kv - static_cast<u32>(SK::Num1)));
            }
            if (key == SK::Num0)
            {
                return KeyCode::Num0;
            }
            switch (key)
            {
            case SK::Return:
                return KeyCode::Return;
            case SK::KeypadEnter:
                return KeyCode::Return; // both mean "confirm" to the UI
            case SK::Escape:
                return KeyCode::Escape;
            case SK::Backspace:
                return KeyCode::Backspace;
            case SK::Tab:
                return KeyCode::Tab;
            case SK::Space:
                return KeyCode::Space;
            case SK::Delete:
                return KeyCode::Delete;
            case SK::Insert:
                return KeyCode::Insert;
            case SK::Home:
                return KeyCode::Home;
            case SK::End:
                return KeyCode::End;
            case SK::PageUp:
                return KeyCode::PageUp;
            case SK::PageDown:
                return KeyCode::PageDown;
            case SK::Left:
                return KeyCode::Left;
            case SK::Right:
                return KeyCode::Right;
            case SK::Up:
                return KeyCode::Up;
            case SK::Down:
                return KeyCode::Down;
            // Punctuation row - editors bind chords on these (Ctrl+/ = comment toggle).
            case SK::Minus:
                return KeyCode::Minus;
            case SK::Equals:
                return KeyCode::Equals;
            case SK::LeftBracket:
                return KeyCode::LeftBracket;
            case SK::RightBracket:
                return KeyCode::RightBracket;
            case SK::Backslash:
                return KeyCode::Backslash;
            case SK::Semicolon:
                return KeyCode::Semicolon;
            case SK::Apostrophe:
                return KeyCode::Apostrophe;
            case SK::Grave:
                return KeyCode::Grave;
            case SK::Comma:
                return KeyCode::Comma;
            case SK::Period:
                return KeyCode::Period;
            case SK::Slash:
                return KeyCode::Slash;
            default:
                return KeyCode::Unknown;
            }
        }

        [[nodiscard]] static KeyModifiers MapModifiers(shell::KeyModifiers mods) noexcept
        {
            KeyModifiers out = KeyModifiers::None;
            if ((mods & shell::KeyModifiers::Shift) != shell::KeyModifiers::None)
            {
                out = out | KeyModifiers::Shift;
            }
            if ((mods & shell::KeyModifiers::Ctrl) != shell::KeyModifiers::None)
            {
                out = out | KeyModifiers::Ctrl;
            }
            if ((mods & shell::KeyModifiers::Alt) != shell::KeyModifiers::None)
            {
                out = out | KeyModifiers::Alt;
            }
            if ((mods & shell::KeyModifiers::Gui) != shell::KeyModifiers::None)
            {
                out = out | KeyModifiers::Gui;
            }
            return out;
        }

        UIContext* m_context;
        shell::IWindow* m_textInputTarget = nullptr;
        f32 m_lastX = 0.0f;
        f32 m_lastY = 0.0f;
    };
}

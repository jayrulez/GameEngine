// Draconic GUI - `draconic.gui.shell`: the platform input bridge.
//
// Keeps the GUI core platform-agnostic: the core EventDispatcher exposes an abstract
// Inject* API; this bridge is the ONLY place that knows about the platform input layer
// (draconic.shell). It translates a stream of shell::InputEvent (already gated/transformed
// by InputSurface/InputRouter, see [[viewport-input]]) into dispatcher injections, mapping
// platform enums to GUI enums and window-space positions to content space via a ContentFit.
//
// Reimplemented for Draconic (not ported from eepp/Sedulous), per the port plan: the
// InputSurface consumption lives here, never in the core.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui.shell;

import draconic.foundation;  // ContentFit, Float2, String, StringView, u32
import draconic.gui;   // EventDispatcher, MouseButton, KeyMod*, IClipboard
import draconic.shell; // InputEvent, InputEventKind, MouseButton, KeyModifiers, IWindow, IShell

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace shell = draconic::shell;

export namespace draconic::gui
{
    // Translates platform input events into EventDispatcher injections.
    class GuiInputBridge
    {
    public:
        explicit GuiInputBridge(EventDispatcher* dispatcher) noexcept : m_dispatcher(dispatcher) {}

        // The content fit maps window/region-space event positions into GUI content space.
        // Without one, positions pass through unchanged (identity).
        void SetContentFit(const foundation::ContentFit& fit) noexcept
        {
            m_fit = fit;
            m_hasFit = true;
        }
        void ClearContentFit() noexcept { m_hasFit = false; }

        // The window whose platform text input (IME) follows GUI focus. Once set, the bridge
        // enables text input while a text-editing widget holds focus and disables it
        // otherwise - reconciled after each Dispatch()/PumpFromSurface(), so no per-widget or
        // per-app wiring is needed. Pass nullptr to disable the feature.
        void SetTextInputTarget(shell::IWindow* window) noexcept { m_textInputTarget = window; }

        // Reconcile the target window's text-input state with the focused node's wish. Called
        // automatically from Dispatch()/PumpFromSurface(); exposed for apps that drive focus
        // by other means.
        void SyncTextInput()
        {
            if (m_textInputTarget == nullptr || m_dispatcher == nullptr)
                return;
            const bool want = m_dispatcher->WantsTextInput();
            if (want && !m_textInputTarget->IsTextInputActive())
                m_textInputTarget->StartTextInput();
            else if (!want && m_textInputTarget->IsTextInputActive())
                m_textInputTarget->StopTextInput();
        }

        // Translate one platform input event into a dispatcher injection. Returns true if the
        // event was routed to the GUI, false if it was ignored (gamepad/touch/unknown).
        // Reconciles the text-input target afterwards, since a routed event (a click, a Tab)
        // can change focus.
        bool Dispatch(const shell::InputEvent& event)
        {
            if (m_dispatcher == nullptr)
                return false;
            bool routed = true;
            switch (event.kind)
            {
            case shell::InputEventKind::MouseMove:
                m_dispatcher->InjectMouseMove(ToContent(foundation::Float2{event.x, event.y}));
                break;
            case shell::InputEventKind::MouseButtonDown:
                m_dispatcher->InjectMouseDown(ToContent(foundation::Float2{event.x, event.y}),
                                              MapButton(event.button),
                                              MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::MouseButtonUp:
                m_dispatcher->InjectMouseUp(ToContent(foundation::Float2{event.x, event.y}),
                                            MapButton(event.button), MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::MouseWheel:
                // For wheel, x/y is the scroll delta; position is the last known cursor spot.
                m_dispatcher->InjectMouseWheel(m_dispatcher->GetMousePosition(),
                                               foundation::Float2{event.x, event.y});
                break;
            case shell::InputEventKind::KeyDown:
                m_dispatcher->InjectKeyDown(MapKey(event.key), MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::KeyUp:
                m_dispatcher->InjectKeyUp(MapKey(event.key), MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::TextInput:
                m_dispatcher->InjectText(foundation::StringView(event.text));
                break;
            default:
                routed = false; // gamepad / touch not routed to the GUI yet
                break;
            }
            SyncTextInput();
            return routed;
        }

        // Poll a gated InputSurface (its mouse is already content-space) and drive the
        // dispatcher: hover from the cursor, click from press/release edges, plus wheel. The
        // recommended path for a viewport-hosted GUI - the surface handles transform + gating,
        // and a fresh InjectMouseMove each frame keeps hover current. (Keyboard/text still come
        // through the event-based Dispatch() path.)
        void PumpFromSurface(shell::InputSurface& surface)
        {
            if (m_dispatcher == nullptr)
                return;
            shell::IMouse* mouse = surface.Mouse();
            if (mouse == nullptr)
                return;

            const foundation::Float2 position{mouse->X(), mouse->Y()};
            m_dispatcher->InjectMouseMove(position);

            // Current keyboard modifiers, so a modified click (Shift+click to extend a text
            // selection, Ctrl+click, ...) carries its modifiers even on the poll-based path.
            shell::IKeyboard* keyboard = surface.Keyboard();
            const u32 modifiers = keyboard != nullptr ? MapModifiers(keyboard->Modifiers()) : 0u;

            const shell::MouseButton buttons[3] = {
                shell::MouseButton::Left, shell::MouseButton::Middle, shell::MouseButton::Right};
            for (const shell::MouseButton button : buttons)
            {
                if (mouse->IsButtonPressed(button))
                    m_dispatcher->InjectMouseDown(position, MapButton(button), modifiers);
                if (mouse->IsButtonReleased(button))
                    m_dispatcher->InjectMouseUp(position, MapButton(button), modifiers);
            }

            const f32 scrollX = mouse->ScrollX();
            const f32 scrollY = mouse->ScrollY();
            if (scrollX != 0.0f || scrollY != 0.0f)
                m_dispatcher->InjectMouseWheel(position, foundation::Float2{scrollX, scrollY});

            SyncTextInput(); // a click this frame may have focused (or blurred) an editable widget
        }

    private:
        [[nodiscard]] foundation::Float2 ToContent(foundation::Float2 windowPos) const
        {
            if (!m_hasFit)
                return windowPos;
            foundation::Float2 out{0.0f, 0.0f};
            [[maybe_unused]] const bool inside =
                m_fit.ToContent(windowPos, out); // out set regardless
            return out;
        }

        // shell::MouseButton order is Left/Middle/Right - map explicitly, not by value.
        [[nodiscard]] static MouseButton MapButton(shell::MouseButton button) noexcept
        {
            switch (button)
            {
            case shell::MouseButton::Left:
                return MouseButton::Left;
            case shell::MouseButton::Right:
                return MouseButton::Right;
            case shell::MouseButton::Middle:
                return MouseButton::Middle;
            case shell::MouseButton::X1:
                return MouseButton::X1;
            case shell::MouseButton::X2:
                return MouseButton::X2;
            default:
                return MouseButton::Left;
            }
        }

        // Map the platform key code to the GUI's platform-agnostic KeyCode (as a u32).
        // Only the navigation/editing keys the GUI interprets are mapped; everything else
        // (printable characters) arrives via TextInput, so it maps to Unknown here.
        [[nodiscard]] static u32 MapKey(shell::KeyCode key) noexcept
        {
            KeyCode mapped = KeyCode::Unknown;
            switch (key)
            {
            case shell::KeyCode::Return:
                mapped = KeyCode::Return;
                break;
            case shell::KeyCode::Escape:
                mapped = KeyCode::Escape;
                break;
            case shell::KeyCode::Backspace:
                mapped = KeyCode::Backspace;
                break;
            case shell::KeyCode::Tab:
                mapped = KeyCode::Tab;
                break;
            case shell::KeyCode::Space:
                mapped = KeyCode::Space;
                break;
            case shell::KeyCode::Delete:
                mapped = KeyCode::Delete;
                break;
            case shell::KeyCode::Insert:
                mapped = KeyCode::Insert;
                break;
            case shell::KeyCode::Home:
                mapped = KeyCode::Home;
                break;
            case shell::KeyCode::End:
                mapped = KeyCode::End;
                break;
            case shell::KeyCode::PageUp:
                mapped = KeyCode::PageUp;
                break;
            case shell::KeyCode::PageDown:
                mapped = KeyCode::PageDown;
                break;
            case shell::KeyCode::Left:
                mapped = KeyCode::Left;
                break;
            case shell::KeyCode::Right:
                mapped = KeyCode::Right;
                break;
            case shell::KeyCode::Up:
                mapped = KeyCode::Up;
                break;
            case shell::KeyCode::Down:
                mapped = KeyCode::Down;
                break;
            // Letter keys that back editing shortcuts (Ctrl+A/C/V/X).
            case shell::KeyCode::A:
                mapped = KeyCode::A;
                break;
            case shell::KeyCode::C:
                mapped = KeyCode::C;
                break;
            case shell::KeyCode::V:
                mapped = KeyCode::V;
                break;
            case shell::KeyCode::X:
                mapped = KeyCode::X;
                break;
            default:
                mapped = KeyCode::Unknown;
                break;
            }
            return static_cast<u32>(mapped);
        }

        [[nodiscard]] static u32 MapModifiers(shell::KeyModifiers mods) noexcept
        {
            u32 out = 0;
            if ((mods & shell::KeyModifiers::Shift) != shell::KeyModifiers::None)
                out |= KeyModShift;
            if ((mods & shell::KeyModifiers::Ctrl) != shell::KeyModifiers::None)
                out |= KeyModCtrl;
            if ((mods & shell::KeyModifiers::Alt) != shell::KeyModifiers::None)
                out |= KeyModAlt;
            if ((mods & shell::KeyModifiers::Gui) != shell::KeyModifiers::None)
                out |= KeyModSuper;
            return out;
        }

        EventDispatcher* m_dispatcher;
        shell::IWindow* m_textInputTarget =
            nullptr; // window whose IME follows GUI focus (optional)
        foundation::ContentFit m_fit{};
        bool m_hasFit = false;
    };

    // Adapts the platform shell's process-global text clipboard to the GUI core's abstract
    // IClipboard seam. The app constructs one over its shell::IShell and hands it to the
    // dispatcher once (dispatcher.SetClipboard(&clipboard)); TextField cut/copy/paste then
    // reach the OS clipboard with no further wiring. Mirrors how ShellNull's in-memory
    // clipboard backs headless tests.
    class ShellClipboard : public IClipboard
    {
    public:
        explicit ShellClipboard(shell::IShell* shell) noexcept : m_shell(shell) {}

        [[nodiscard]] bool HasText() const override
        {
            return m_shell != nullptr && m_shell->HasClipboardText();
        }
        [[nodiscard]] foundation::String GetText() const override
        {
            return m_shell != nullptr ? m_shell->GetClipboardText() : foundation::String{};
        }
        void SetText(foundation::StringView text) override
        {
            if (m_shell != nullptr)
                m_shell->SetClipboardText(text);
        }

    private:
        shell::IShell* m_shell; // non-owning
    };
}

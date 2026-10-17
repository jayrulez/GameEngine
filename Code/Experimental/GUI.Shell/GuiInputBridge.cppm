// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.shell`: the platform input bridge.
//
// Keeps the GUI core platform-agnostic: the core EventDispatcher exposes an abstract
// Inject* API; this bridge is the ONLY place that knows about the platform input layer
// (foundation.shell). It translates a stream of shell::InputEvent (already gated/transformed
// by InputSurface/InputRouter, see [[viewport-input]]) into dispatcher injections, mapping
// platform enums to GUI enums and window-space positions to content space via a ContentFit.
//
// Reimplemented for us (not ported from eepp/Sedulous), per the port plan: the
// InputSurface consumption lives here, never in the core.

module;
#include "Core/Prelude.h"

export module experimental.gui.shell;

import foundation.core;  // ContentFit, Float2, String, StringView, u32
import experimental.gui;   // EventDispatcher, MouseButton, KeyMod*, IClipboard
import foundation.shell; // InputEvent, InputEventKind, MouseButton, KeyModifiers, IWindow, IShell
import foundation.input; // InputMap (a keymap filled from the game's bindings)

using namespace foundation::core;
namespace core = foundation::core;
namespace shell = foundation::shell;

export namespace experimental::gui
{
    // Translates platform input events into EventDispatcher injections.
    class GuiInputBridge
    {
    public:
        explicit GuiInputBridge(EventDispatcher* dispatcher) noexcept : m_dispatcher(dispatcher) {}

        // The content fit maps window/region-space event positions into GUI content space.
        // Without one, positions pass through unchanged (identity).
        void SetContentFit(const core::ContentFit& fit) noexcept
        {
            m_fit = fit;
            m_hasFit = true;
        }
        void ClearContentFit() noexcept { m_hasFit = false; }

        // The window's size in the coordinates its mouse reports in: touch events arrive
        // normalized over the window, and Dispatch() places them with it (without it, touch
        // events are not forwarded). PumpFromSurface() needs none: its surface places fingers.
        void SetWindowSize(core::Float2 size) noexcept { m_windowSize = size; }

        // The window whose platform text input (IME) follows GUI focus. Once set, the bridge
        // enables text input while a text-editing widget holds focus and disables it
        // otherwise - reconciled after each Dispatch()/PumpFromSurface(), so no per-widget or
        // per-app wiring is needed. Pass nullptr to disable the feature.
        void SetTextInputTarget(shell::IWindow* window) noexcept { m_textInputTarget = window; }

        // The mouse whose cursor follows the GUI's (CSS `cursor` on the node under the pointer),
        // reconciled after each Dispatch()/PumpFromSurface(). Pass nullptr to leave the cursor alone.
        void SetCursorTarget(shell::IMouse* mouse) noexcept
        {
            m_cursorTarget = mouse;
            m_shownCursor = shell::CursorType::Count; // apply on the next sync
        }
        void SyncCursor()
        {
            if (m_cursorTarget == nullptr || m_dispatcher == nullptr)
                return;
            const shell::CursorType cursor = MapCursor(m_dispatcher->GetCursor());
            if (cursor == m_shownCursor)
                return;
            m_shownCursor = cursor;
            m_cursorTarget->SetCursor(cursor);
        }

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
            SyncTextInputArea();
        }
        // Tells the window where the focused field types (its line, the caret across it), in
        // window coordinates, when that moved: the IME shows its candidates beside it.
        void SyncTextInputArea()
        {
            Rect area{};
            f32 caretX = 0.0f;
            if (!m_dispatcher->GetTextInputArea(area, caretX))
                return;
            core::Float2 a{area.x, area.y};
            core::Float2 b{area.x + area.width, area.y + area.height};
            core::Float2 caret{area.x + caretX, area.y};
            if (m_hasFit)
            {
                a = m_fit.FromContent(a);
                b = m_fit.FromContent(b);
                caret = m_fit.FromContent(caret);
            }
            const i32 shown[5] = {static_cast<i32>(core::Min(a.x, b.x)), static_cast<i32>(core::Min(a.y, b.y)),
                                  static_cast<i32>(core::Abs(b.x - a.x) + 0.5f), static_cast<i32>(core::Abs(b.y - a.y) + 0.5f),
                                  static_cast<i32>(caret.x - core::Min(a.x, b.x))};
            bool same = true;
            for (usize i = 0; i < 5; ++i)
                same = same && shown[i] == m_textInputArea[i];
            if (same)
                return;
            for (usize i = 0; i < 5; ++i)
                m_textInputArea[i] = shown[i];
            m_textInputTarget->SetTextInputArea(shown[0], shown[1], shown[2], shown[3], shown[4]);
        }

        // Translate one platform input event into a dispatcher injection. Returns true if the GUI
        // used it (a handler took it, or the pointer is over a node that takes it), so a game host
        // leaves it alone; false if the GUI did not (or the event is not routed: touch, an
        // unmapped button). Reconciles text input and the cursor afterwards, since a routed event
        // (a click, a Tab) can change focus and what is under the pointer.
        bool Dispatch(const shell::InputEvent& event)
        {
            if (m_dispatcher == nullptr)
                return false;
            bool routed = true;
            switch (event.kind)
            {
            case shell::InputEventKind::MouseMove:
            {
                const core::Float2 at = ToContent(core::Float2{event.x, event.y});
                routed = !IsTouchCopy(at) && m_dispatcher->InjectMouseMove(at);
                break;
            }
            case shell::InputEventKind::MouseButtonDown:
            {
                const core::Float2 at = ToContent(core::Float2{event.x, event.y});
                routed = !IsTouchCopy(at) &&
                         m_dispatcher->InjectMouseDown(at, MapButton(event.button), MapModifiers(event.modifiers));
                break;
            }
            case shell::InputEventKind::MouseButtonUp:
            {
                const core::Float2 at = ToContent(core::Float2{event.x, event.y});
                routed = !IsTouchCopy(at) &&
                         m_dispatcher->InjectMouseUp(at, MapButton(event.button), MapModifiers(event.modifiers));
                break;
            }
            case shell::InputEventKind::TouchDown:
            case shell::InputEventKind::TouchMove:
            case shell::InputEventKind::TouchUp:
            {
                if (m_windowSize.x <= 0.0f || m_windowSize.y <= 0.0f)
                {
                    routed = false; // no window size: a finger cannot be placed
                    break;
                }
                const core::Float2 at = ToContent(core::Float2{event.x * m_windowSize.x, event.y * m_windowSize.y});
                routed = ForwardTouch(event.kind, event.touchId, at);
                break;
            }
            case shell::InputEventKind::MouseWheel:
                // For wheel, x/y is the scroll delta; position is the last known cursor spot.
                routed = m_dispatcher->InjectMouseWheel(m_dispatcher->GetMousePosition(),
                                                        core::Float2{event.x, event.y}, MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::KeyDown:
                routed = m_dispatcher->InjectKeyDown(MapKey(event.key), MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::KeyUp:
                routed = m_dispatcher->InjectKeyUp(MapKey(event.key), MapModifiers(event.modifiers));
                break;
            case shell::InputEventKind::TextInput:
                routed = m_dispatcher->InjectText(core::StringView(event.text));
                break;
            case shell::InputEventKind::TextEditing:
                routed = m_dispatcher->InjectComposition(core::StringView(event.text), event.editCursor);
                break;
            case shell::InputEventKind::GamepadButtonDown:
            case shell::InputEventKind::GamepadButtonUp:
                routed = DispatchPadButton(event.padButton, event.kind == shell::InputEventKind::GamepadButtonDown);
                break;
            case shell::InputEventKind::GamepadAxis:
                routed = DispatchStick(event.padAxis, event.value);
                break;
            default:
                routed = false;
                break;
            }
            SyncTextInput();
            SyncCursor();
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

            // Fingers first (the surface places them in content space): the first one down drives
            // the pointer; it went down, moved or lifted since the last pump.
            if (shell::ITouch* touch = surface.Touch(); touch != nullptr)
                PumpTouch(*touch);

            const core::Float2 position{mouse->X(), mouse->Y()};
            if (IsTouchCopy(position))
            {
                SyncTextInput();
                SyncCursor();
                return; // the mouse is SDL's copy of a finger this frame
            }
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
                m_dispatcher->InjectMouseWheel(position, core::Float2{scrollX, scrollY}, modifiers);

            SyncTextInput(); // a click this frame may have focused (or blurred) an editable widget
            SyncCursor();
        }

        /// Whether the pointer is over the GUI now (a node that takes it), for a host that polls.
        [[nodiscard]] bool IsPointerOverGui() const
        {
            return m_dispatcher != nullptr && m_dispatcher->IsPointerOver(m_dispatcher->GetMousePosition());
        }

    private:
        [[nodiscard]] core::Float2 ToContent(core::Float2 windowPos) const
        {
            if (!m_hasFit)
                return windowPos;
            core::Float2 out{0.0f, 0.0f};
            [[maybe_unused]] const bool inside =
                m_fit.ToContent(windowPos, out); // out set regardless
            return out;
        }

        // A finger's event to the dispatcher, remembering where it was for IsTouchCopy.
        bool ForwardTouch(shell::InputEventKind kind, u64 finger, core::Float2 at)
        {
            m_touchSeen = true;
            m_lastTouchAt = at;
            switch (kind)
            {
            case shell::InputEventKind::TouchDown: return m_dispatcher->InjectTouchDown(finger, at);
            case shell::InputEventKind::TouchMove: return m_dispatcher->InjectTouchMove(finger, at);
            default: return m_dispatcher->InjectTouchUp(finger, at);
            }
        }
        // The polled fingers against the one driving the pointer: down when one appears, moves
        // while it stays, up when it is gone.
        void PumpTouch(shell::ITouch& touch)
        {
            shell::TouchPoint point;
            bool found = false;
            shell::TouchPoint first;
            bool any = false;
            for (core::i32 i = 0; i < touch.TouchCount(); ++i)
                if (touch.GetTouchPoint(i, point))
                {
                    if (!any)
                    {
                        first = point;
                        any = true;
                    }
                    if (m_pumpedFinger.HasValue() && point.id == m_pumpedFinger.Value())
                    {
                        found = true;
                        break;
                    }
                }
            if (m_pumpedFinger.HasValue())
            {
                if (found)
                {
                    const core::Float2 at{point.pointerX, point.pointerY};
                    if (at.x != m_lastTouchAt.x || at.y != m_lastTouchAt.y)
                        (void)ForwardTouch(shell::InputEventKind::TouchMove, point.id, at);
                    return;
                }
                (void)ForwardTouch(shell::InputEventKind::TouchUp, m_pumpedFinger.Value(), m_lastTouchAt);
                m_pumpedFinger = {};
            }
            if (any)
            {
                m_pumpedFinger = first.id;
                (void)ForwardTouch(shell::InputEventKind::TouchDown, first.id, core::Float2{first.pointerX, first.pointerY});
            }
        }
        // SDL copies a finger into mouse events by default: a mouse event while a finger is down,
        // or at the place a finger last was, is taken for that copy and ignored. The mouse moving
        // anywhere else is the mouse again.
        [[nodiscard]] bool IsTouchCopy(core::Float2 at)
        {
            if (!m_touchSeen)
                return false;
            if (m_dispatcher->IsTouchActive())
                return true;
            const f32 dx = at.x - m_lastTouchAt.x;
            const f32 dy = at.y - m_lastTouchAt.y;
            if (dx * dx + dy * dy <= 4.0f)
                return true;
            m_touchSeen = false;
            return false;
        }

        // The GUI's cursor as the shell has it; the shell has no grab cursors or help, so those take
        // the nearest it has.
        [[nodiscard]] static shell::CursorType MapCursor(Cursor cursor) noexcept
        {
            switch (cursor)
            {
            case Cursor::Pointer: return shell::CursorType::Pointer;
            case Cursor::Text: return shell::CursorType::Text;
            case Cursor::Move:
            case Cursor::Grab:
            case Cursor::Grabbing: return shell::CursorType::Move;
            case Cursor::NotAllowed: return shell::CursorType::NotAllowed;
            case Cursor::Wait: return shell::CursorType::Wait;
            case Cursor::Progress: return shell::CursorType::Progress;
            case Cursor::Crosshair: return shell::CursorType::Crosshair;
            case Cursor::EwResize: return shell::CursorType::ResizeEW;
            case Cursor::NsResize: return shell::CursorType::ResizeNS;
            case Cursor::NwseResize: return shell::CursorType::ResizeNWSE;
            case Cursor::NeswResize: return shell::CursorType::ResizeNESW;
            default: return shell::CursorType::Default;
            }
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

    public:
        // Map the platform key code to the GUI's platform-agnostic KeyCode (as a u32).
        // Every key the GUI or a binding names; text still arrives through TextInput (a key's code
        // is for shortcuts, not for what it types).
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
            default:
                // Letters, digits and F1 to F12: the editing shortcuts and every binding (G7).
                if (key >= shell::KeyCode::A && key <= shell::KeyCode::Z)
                    mapped = kLetterKeys[static_cast<u32>(key) - static_cast<u32>(shell::KeyCode::A)];
                else if (key >= shell::KeyCode::Num0 && key <= shell::KeyCode::Num9)
                    mapped = static_cast<KeyCode>(static_cast<u32>(KeyCode::Num0) + (static_cast<u32>(key) - static_cast<u32>(shell::KeyCode::Num0)));
                else if (key >= shell::KeyCode::F1 && key <= shell::KeyCode::F12)
                    mapped = static_cast<KeyCode>(static_cast<u32>(KeyCode::F1) + (static_cast<u32>(key) - static_cast<u32>(shell::KeyCode::F1)));
                else
                    mapped = KeyCode::Unknown;
                break;
            }
            return static_cast<u32>(mapped);
        }

    private:

        // A gamepad (G7): the d-pad holds a direction (focus moves, then repeats); the face,
        // shoulder and menu buttons are keystrokes for the keymap (South activates when unbound).
        bool DispatchPadButton(shell::GamepadButton button, bool down)
        {
            NavDirection direction = NavDirection::Up;
            bool isDirection = true;
            switch (button)
            {
            case shell::GamepadButton::DPadUp: direction = NavDirection::Up; break;
            case shell::GamepadButton::DPadDown: direction = NavDirection::Down; break;
            case shell::GamepadButton::DPadLeft: direction = NavDirection::Left; break;
            case shell::GamepadButton::DPadRight: direction = NavDirection::Right; break;
            default: isDirection = false; break;
            }
            if (isDirection)
            {
                if (down)
                    m_dispatcher->PressDirection(direction);
                else
                    m_dispatcher->ReleaseDirection(direction);
                return true;
            }
            if (!down)
                return true;
            PadButton pad = PadButton::South;
            if (!MapPadButton(button, pad))
                return false;
            m_dispatcher->InjectPadButton(pad);
            return true;
        }

        // The left stick as a d-pad: past half way it holds a direction, back under a third it lets
        // go (the gap keeps a stick resting near the threshold from chattering). The right stick
        // scrolls.
        bool DispatchStick(shell::GamepadAxis axis, f32 value)
        {
            // The right stick scrolls the nearest scroller around the focus (the dispatcher's).
            if (axis == shell::GamepadAxis::RightX || axis == shell::GamepadAxis::RightY)
            {
                core::Float2 stick = m_dispatcher->GetScrollStick();
                (axis == shell::GamepadAxis::RightX ? stick.x : stick.y) = value;
                m_dispatcher->SetScrollStick(stick);
                return true;
            }
            if (axis != shell::GamepadAxis::LeftX && axis != shell::GamepadAxis::LeftY)
                return false;
            const bool horizontal = axis == shell::GamepadAxis::LeftX;
            i32& held = horizontal ? m_stickX : m_stickY; // -1, 0, +1
            const i32 now = value > kStickPress ? 1 : value < -kStickPress ? -1 : (core::Abs(value) < kStickRelease ? 0 : held);
            if (now == held)
                return true;
            const auto directionOf = [horizontal](i32 sign)
            { return horizontal ? (sign > 0 ? NavDirection::Right : NavDirection::Left) : (sign > 0 ? NavDirection::Down : NavDirection::Up); };
            if (held != 0)
                m_dispatcher->ReleaseDirection(directionOf(held));
            held = now;
            if (held != 0)
                m_dispatcher->PressDirection(directionOf(held));
            return true;
        }
        static constexpr f32 kStickPress = 0.5f;
        static constexpr f32 kStickRelease = 0.33f;
        i32 m_stickX = 0;
        i32 m_stickY = 0;

    public:
        /// A shell gamepad button as a keystroke's pad button; false for the d-pad and the rest.
        [[nodiscard]] static bool MapPadButton(shell::GamepadButton button, PadButton& out) noexcept
        {
            switch (button)
            {
            case shell::GamepadButton::South: out = PadButton::South; return true;
            case shell::GamepadButton::East: out = PadButton::East; return true;
            case shell::GamepadButton::West: out = PadButton::West; return true;
            case shell::GamepadButton::North: out = PadButton::North; return true;
            case shell::GamepadButton::Start: out = PadButton::Start; return true;
            case shell::GamepadButton::Back: out = PadButton::Back; return true;
            case shell::GamepadButton::LeftShoulder: out = PadButton::LeftShoulder; return true;
            case shell::GamepadButton::RightShoulder: out = PadButton::RightShoulder; return true;
            default: return false;
            }
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

    private:
        EventDispatcher* m_dispatcher;
        shell::IWindow* m_textInputTarget =
            nullptr; // window whose IME follows GUI focus (optional)
        shell::IMouse* m_cursorTarget = nullptr; // mouse whose cursor follows the GUI's (optional)
        shell::CursorType m_shownCursor = shell::CursorType::Count; // last applied (Count: none yet)
        core::ContentFit m_fit{};
        i32 m_textInputArea[5] = {-1, -1, -1, -1, -1}; // what the window was last told (SyncTextInputArea)
        bool m_hasFit = false;
        core::Float2 m_windowSize{0.0f, 0.0f}; // for placing touch events (SetWindowSize)
        bool m_touchSeen = false;              // a finger was used: the mouse may be SDL's copy of it
        core::Float2 m_lastTouchAt{0.0f, 0.0f}; // where the driving finger last was (content space)
        core::Optional<u64> m_pumpedFinger;    // the polled finger driving the pointer
    };

    /// One action of the game's input map and what it does in the GUI.
    struct InputMapCommand
    {
        core::StringView Action;  ///< its name in the input map ("Back", "Confirm")
        core::StringView Command; ///< the command it runs
        core::StringView Context = {}; ///< where (a key-context predicate; empty: everywhere)
        core::StringView Set = {};     ///< the action set to read it from (empty: any)
    };

    /// Binds, in `keymap`, each of `actions` to the keys and gamepad buttons the game's input map
    /// gives it (G7), so the menu's Back is the player's Back, rebinding included: Escape and the
    /// East button by default. How many bindings it made.
    inline u32 BindFromInputMap(Keymap& keymap, const foundation::input::InputMap& map, core::Span<const InputMapCommand> actions)
    {
        namespace input = foundation::input;
        u32 made = 0;
        for (const InputMapCommand& wanted : actions)
            for (const input::ActionSet& set : map.sets)
            {
                if (wanted.Set.Size() != 0 && set.name.AsView() != wanted.Set)
                    continue;
                for (const input::Action& action : set.actions)
                {
                    if (action.name.AsView() != wanted.Action)
                        continue;
                    for (const input::Binding& binding : action.bindings)
                    {
                        Keystroke stroke;
                        if (binding.source == input::BindingSource::Key)
                        {
                            stroke.Code = GuiInputBridge::MapKey(static_cast<shell::KeyCode>(binding.code));
                            stroke.Modifiers = GuiInputBridge::MapModifiers(static_cast<shell::KeyModifiers>(binding.modifiers));
                            if (stroke.Code == static_cast<u32>(KeyCode::Unknown))
                                continue;
                        }
                        else if (binding.source == input::BindingSource::GamepadButton)
                        {
                            PadButton pad = PadButton::South;
                            if (!GuiInputBridge::MapPadButton(static_cast<shell::GamepadButton>(binding.code), pad))
                                continue;
                            stroke = Keystroke{static_cast<u32>(pad), 0, true};
                        }
                        else
                        {
                            continue; // axes, mouse and touch are not keystrokes
                        }
                        if (keymap.Bind(stroke, wanted.Command, wanted.Context))
                            ++made;
                    }
                }
            }
        return made;
    }

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
        [[nodiscard]] core::String GetText() const override
        {
            return m_shell != nullptr ? m_shell->GetClipboardText() : core::String{};
        }
        void SetText(core::StringView text) override
        {
            if (m_shell != nullptr)
                m_shell->SetClipboardText(text);
        }

    private:
        shell::IShell* m_shell; // non-owning
    };
}

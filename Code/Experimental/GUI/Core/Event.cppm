// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :event partition
//
// Event + EventType + EventCallback: the typed events nodes listen for. Derived from eepp's
// Scene::Event / Event::EventType. Input events (pointer, wheel, keys, text) travel a route: from
// the root down to the target (the capture phase), at the target, and back up (the bubble phase);
// a handler marking one Handled ends its route. The rest (geometry, focus, change) go to the one
// node they are about.

module;
#include "Core/Prelude.h"

export module experimental.gui:event;

import foundation.core; // Function, String

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class Node;

    // Payload carried by a drag-and-drop operation: a type tag (what kind of thing is being
    // dragged, so targets can decide whether to accept) plus a string value (the data - an id,
    // a path, text). Richer payloads (a RefPtr<Object>) can be layered on later.
    struct DragPayload
    {
        core::String Type;
        core::String Value;
    };

    enum class EventType : u32
    {
        // Geometry / tree (fired now)
        PositionChanged,
        SizeChanged,
        VisibilityChanged,
        EnabledChanged,
        ParentChanged,
        Close,
        // Input
        MouseDown,
        MouseUp,
        MouseMove,
        MouseEnter,
        MouseLeave,
        MouseClick,
        MouseWheel,
        KeyDown,
        KeyUp,
        TextInput,
        FocusGained,
        FocusLost,
        DragEnter,
        DragOver,
        DragLeave,
        Drop,
        // A widget's value changed (a check box, a slider; a text field's committed value).
        Changed,
        // A text field's text changed with an edit (each one; Changed waits for the commit).
        Input,
        // A text field's Return: its value submitted.
        Submit,
        // A node lost the pointer it had captured (released, taken by another, or the node left).
        PointerCaptureLost,
        // The press in progress was cancelled (a scroller began panning under a finger): no
        // release and no click follow; a widget drops what the press started.
        PointerCancel,
        // A rich text's link was clicked (a LinkEvent, its id).
        Link,
        // An IME's composition so far, at the focused node (a CompositionEvent; empty text: it ended).
        Composition,
    };

    /// What drives the pointer: the mouse, or a finger (the first one down).
    enum class PointerType : u8
    {
        Mouse,
        Touch,
    };

    /// The pointer's look over a node (CSS `cursor`). Auto takes the look of the node around it,
    /// as CSS's inherited cursor does; the host shows the dispatcher's answer (GetCursor).
    enum class Cursor : u8
    {
        Auto,
        Default,
        Pointer,
        Text,
        Move,
        Grab,
        Grabbing,
        NotAllowed,
        Wait,
        Progress,
        Crosshair,
        Help,
        EwResize,
        NsResize,
        NwseResize,
        NeswResize,
    };
    inline constexpr const char8_t* kCursorNames[] = {
        u8"auto", u8"default", u8"pointer", u8"text", u8"move", u8"grab", u8"grabbing", u8"not-allowed",
        u8"wait", u8"progress", u8"crosshair", u8"help", u8"ew-resize", u8"ns-resize", u8"nwse-resize",
        u8"nesw-resize",
    };
    /// The cursor CSS names `name`; Auto for an unknown name.
    [[nodiscard]] inline Cursor CursorNamed(core::StringView name) noexcept
    {
        for (usize i = 0; i < sizeof(kCursorNames) / sizeof(kCursorNames[0]); ++i)
            if (core::StringView(kCursorNames[i]) == name)
                return static_cast<Cursor>(i);
        return Cursor::Auto;
    }
    [[nodiscard]] inline core::StringView CursorName(Cursor cursor) noexcept
    {
        return core::StringView(kCursorNames[static_cast<usize>(cursor)]);
    }

    /// Where on its route an input event is: going down from the root (Capture), at the node it
    /// is for (Target), or coming back up (Bubble).
    enum class EventPhase : u8
    {
        Capture,
        Target,
        Bubble,
    };

    enum class MouseButton : u32
    {
        Left,
        Right,
        Middle,
        X1,
        X2
    };

    // Platform-agnostic key identity for the navigation/editing keys the GUI interprets
    // (printable characters arrive as TextInput, not here). The gui.shell bridge maps
    // platform key codes to these explicitly; a KeyEvent carries the value as a u32, so
    // handlers compare against static_cast<u32>(KeyCode::X). Only the letter keys that back
    // editing shortcuts (Ctrl+A/C/V/X for select-all/copy/paste/cut) are mapped; other
    // printable characters still arrive as TextInput, not here.
    enum class KeyCode : u32
    {
        Unknown = 0,
        Return,
        Escape,
        Backspace,
        Tab,
        Space,
        Delete,
        Insert,
        Home,
        End,
        PageUp,
        PageDown,
        Left,
        Right,
        Up,
        Down,
        A,
        C,
        V,
        X,
        // The rest of the keyboard bindings name (G7), after the first set so its values hold.
        B, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, W, Y, Z,
        Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    };

    /// The letter keys in alphabetical order (their codes are not contiguous: A, C, V and X came first).
    inline constexpr KeyCode kLetterKeys[26] = {
        KeyCode::A, KeyCode::B, KeyCode::C, KeyCode::D, KeyCode::E, KeyCode::F, KeyCode::G, KeyCode::H, KeyCode::I,
        KeyCode::J, KeyCode::K, KeyCode::L, KeyCode::M, KeyCode::N, KeyCode::O, KeyCode::P, KeyCode::Q, KeyCode::R,
        KeyCode::S, KeyCode::T, KeyCode::U, KeyCode::V, KeyCode::W, KeyCode::X, KeyCode::Y, KeyCode::Z,
    };

    // Key modifier bitmask (unscoped for easy OR-ing).
    enum KeyModifier : u32
    {
        KeyModNone = 0,
        KeyModShift = 1u << 0,
        KeyModCtrl = 1u << 1,
        KeyModAlt = 1u << 2,
        KeyModSuper = 1u << 3,
    };

    // Base event. The typed subclasses below carry payload; a listener registered for a
    // given EventType may static_cast to the matching subclass (the dispatcher always
    // constructs the payload type that matches the EventType).
    struct Event
    {
        EventType Type;
        /// The node the event is for (the hovered or pressed node, the focus).
        Node* Target = nullptr;
        /// The node whose handlers are running now, on the event's route.
        Node* CurrentTarget = nullptr;
        EventPhase Phase = EventPhase::Target;
        /// Set by a handler that used the event: its route ends there. A key a widget leaves
        /// unhandled goes on to the keymap and navigation (G7).
        mutable bool Handled = false;

        explicit Event(EventType type, Node* target = nullptr) noexcept
            : Type(type), Target(target), CurrentTarget(target)
        {
        }
        virtual ~Event() = default;
    };

    struct MouseEvent : Event
    {
        core::Float2 Position;
        MouseButton Button;
        u32 Modifiers;
        /// 1 for a press, 2 for the second of a double press, 3 for a triple (near in time and
        /// place, the same button): a press, its release and its click carry the same count.
        u32 ClickCount = 1;
        PointerType Pointer = PointerType::Mouse;
        u64 PointerId = 0; ///< the finger, for touch

        MouseEvent(EventType type, Node* target, core::Float2 position,
                   MouseButton button = MouseButton::Left, u32 modifiers = 0) noexcept
            : Event(type, target), Position(position), Button(button), Modifiers(modifiers)
        {
        }
    };

    struct WheelEvent : Event
    {
        core::Float2 Position;
        core::Float2 Delta;

        WheelEvent(Node* target, core::Float2 position, core::Float2 delta) noexcept
            : Event(EventType::MouseWheel, target), Position(position), Delta(delta)
        {
        }
    };

    struct KeyEvent : Event
    {
        u32 KeyCode;
        u32 Modifiers;

        KeyEvent(EventType type, Node* target, u32 keyCode, u32 modifiers = 0) noexcept
            : Event(type, target), KeyCode(keyCode), Modifiers(modifiers)
        {
        }
    };

    struct TextInputEvent : Event
    {
        core::StringView Text;

        TextInputEvent(Node* target, core::StringView text) noexcept
            : Event(EventType::TextInput, target), Text(text)
        {
        }
    };

    /// An IME's composition so far: its text (empty when the composition ended or was committed,
    /// the committed text then arriving as TextInput) and the caret within it, in characters.
    struct CompositionEvent : Event
    {
        core::StringView Text;
        i32 Cursor = 0;

        CompositionEvent(Node* target, core::StringView text, i32 cursor) noexcept
            : Event(EventType::Composition, target), Text(text), Cursor(cursor)
        {
        }
    };

    /// A rich text's link, clicked: the id its markup gave it ([link=door]).
    struct LinkEvent : Event
    {
        core::StringView Link;

        LinkEvent(Node* target, core::StringView link) noexcept : Event(EventType::Link, target), Link(link) {}
    };

    struct DragEvent : Event
    {
        const DragPayload& Payload;

        DragEvent(EventType type, Node* target, const DragPayload& payload) noexcept
            : Event(type, target), Payload(payload)
        {
        }
    };

    using EventCallback = core::Function<void(const Event&)>;
}

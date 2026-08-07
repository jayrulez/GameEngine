// Draconic GUI - :event partition
//
// Event + EventType + EventCallback: the high-level typed-event foundation nodes register
// listeners on. Derived from eepp's Scene::Event / Event::EventType. The enum starts with
// the geometry/tree events Node fires now; input events are placeholders wired when the
// EventDispatcher lands (Phase 3). MouseEvent/KeyEvent subclasses grow with that phase.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:event;

import draconic.foundation; // Function, String

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class Node;

    // Payload carried by a drag-and-drop operation: a type tag (what kind of thing is being
    // dragged, so targets can decide whether to accept) plus a string value (the data - an id,
    // a path, text). Richer payloads (a RefPtr<Object>) can be layered on later.
    struct DragPayload
    {
        foundation::String Type;
        foundation::String Value;
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
        // Input (placeholders - dispatched from Phase 3)
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
        Node* Target = nullptr;

        explicit Event(EventType type, Node* target = nullptr) noexcept : Type(type), Target(target)
        {
        }
        virtual ~Event() = default;
    };

    struct MouseEvent : Event
    {
        foundation::Float2 Position;
        MouseButton Button;
        u32 Modifiers;

        MouseEvent(EventType type, Node* target, foundation::Float2 position,
                   MouseButton button = MouseButton::Left, u32 modifiers = 0) noexcept
            : Event(type, target), Position(position), Button(button), Modifiers(modifiers)
        {
        }
    };

    struct WheelEvent : Event
    {
        foundation::Float2 Position;
        foundation::Float2 Delta;

        WheelEvent(Node* target, foundation::Float2 position, foundation::Float2 delta) noexcept
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
        foundation::StringView Text;

        TextInputEvent(Node* target, foundation::StringView text) noexcept
            : Event(EventType::TextInput, target), Text(text)
        {
        }
    };

    struct DragEvent : Event
    {
        const DragPayload& Payload;

        DragEvent(EventType type, Node* target, const DragPayload& payload) noexcept
            : Event(type, target), Payload(payload)
        {
        }
    };

    using EventCallback = foundation::Function<void(const Event&)>;
}

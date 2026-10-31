// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :event partition
//
// Event, EventType and EventCallback: the typed events nodes listen for. Derived from eepp's
// Scene::Event / Event::EventType. Input events (pointer, wheel, keys, text) travel a route: down
// from the root to the target (the capture phase), at the target, and back up (the bubble phase). A
// handler that marks an event Handled ends its route. Other events (geometry, focus, change) go
// only to the node they are about.

module;
#include "Core/Prelude.h"

export module foundation.gui:event;

import foundation.core;
import :model_index; // ItemEvent

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class Node;

    /// What a drop does with what is dragged.
    enum class DropEffect : u8
    {
        None,
        Copy,
        Move,
        Link,
    };
    /// The effects a drag's source allows, as flags.
    enum DropEffects : u8
    {
        DropEffectsNone = 0,
        DropEffectsCopy = 1u << 0,
        DropEffectsMove = 1u << 1,
        DropEffectsLink = 1u << 2,
        DropEffectsAll = DropEffectsCopy | DropEffectsMove | DropEffectsLink,
    };
    [[nodiscard]] inline constexpr u8 DropEffectFlag(DropEffect effect) noexcept
    {
        return effect == DropEffect::Copy ? DropEffectsCopy : effect == DropEffect::Move ? DropEffectsMove
                                          : effect == DropEffect::Link ? DropEffectsLink : DropEffectsNone;
    }

    /// What a drag carries: a type that targets choose by (`text`, `files`, `gui/tab`), a value
    /// (a string, a number, a list, or a weak node handle), a label for the preview and for
    /// accessibility, and the effects the drag's source allows.
    struct DragPayload
    {
        core::String Type;
        core::Variant Value;
        core::String Label;
        u8 Allowed = DropEffectsAll;
    };

    /// Returns a `files` payload's paths. An OS drop carries them as a list of strings.
    [[nodiscard]] inline core::Array<core::String> DroppedPaths(const DragPayload& payload)
    {
        core::Array<core::String> paths;
        if (const core::Array<core::Variant>* list = payload.Value.TryGet<core::Array<core::Variant>>())
            for (const core::Variant& item : *list)
                if (const core::String* path = item.TryGet<core::String>())
                    paths.PushBack(*path);
        return paths;
    }

    enum class EventType : u32
    {
        // Geometry and tree (fired immediately)
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
        // Drag and drop (DragEvents): the drag starting and ending, told to its source, and the drag
        // entering, moving over, leaving and dropping on a target, told to the target.
        DragEnter,
        DragOver,
        DragLeave,
        Drop,
        DragStart,
        DragEnd,
        // Asks what a drop here would do (a DragEvent). A listener that takes this event takes the drop.
        DragQuery,
        // A widget's value changed (a check box, a slider, or a text field's committed value).
        Changed,
        // A text field's text changed. It fires for each edit; Changed waits for the commit.
        Input,
        // Return in a text field submitted its value.
        Submit,
        // A node lost the pointer it had captured (released, taken by another node, or the node left
        // the tree).
        PointerCaptureLost,
        // The press in progress was cancelled (a scroller began panning under a finger). No release
        // and no click follow, so a widget should drop whatever the press started.
        PointerCancel,
        // A rich text's link was clicked (a LinkEvent, carrying the link's id).
        Link,
        // An IME's composition so far, at the focused node (a CompositionEvent; empty text means it
        // ended).
        Composition,
        // A popup opened or closed (a ToggleEvent, targeted at the popup; it bubbles to its owner).
        Toggle,
        // A context menu was asked for (a MouseEvent) by a right release, a long press or Shift+F10.
        ContextMenu,
        // A scroll container's offset moved. Only its own listeners hear it; it does not bubble.
        Scroll,
        // A tab asked to close (a TabEvent, at its bar or view). The tab closes unless a listener takes
        // the event.
        TabClose,
        // An item view's row was activated (an ItemEvent) by a double-click, Return or the pad's South
        // button.
        ItemActivate,
        // A two-finger gesture (or a trackpad pinch) over a node whose touch-action does not leave
        // pinch-zoom to the GUI (a GestureEvent). It is started, moved (scale, rotation,
        // translation), ended, or cancelled (the platform took a finger back, or the node went).
        GestureStart,
        GestureChange,
        GestureEnd,
        GestureCancel,
        // A sheet's transition of one property ended, or a keyframe animation ran its last
        // iteration (a MotionEndEvent). It goes to the node that moved and is not routed. A motion
        // replaced or stopped before its end sends no event.
        TransitionEnd,
        AnimationEnd,
        // The input kind changed. It goes to the GUI's root and is not routed; GetInputKind says which
        // kind is current.
        InputKindChanged,
        // An event a library names for itself (a NamedEvent, such as a gamekit screen's
        // `screenenter`). It goes to the node it is about and is not routed; a script listens for it
        // by its name.
        Named,
        // A node's entrance animation (Node::Enter) ended. It goes to that node and is not routed.
        Entered,
    };

    /// The finger actions the GUI handles itself (CSS's touch-action): panning across, panning up
    /// and down (as a scroller does), and pinching, turning and panning with two fingers (a gesture).
    /// Whatever a node leaves out comes to the node instead. A one-finger move along an axis the
    /// node does not leave to the GUI continues as the pointer (the node's own drag), and two
    /// fingers over a node that does not leave pinch-zoom to the GUI make a gesture event.
    inline constexpr u8 kTouchPanX = 1;
    inline constexpr u8 kTouchPanY = 2;
    inline constexpr u8 kTouchPinchZoom = 4;
    inline constexpr u8 kTouchAuto = kTouchPanX | kTouchPanY | kTouchPinchZoom;
    /// Parses a touch-action value: auto, none, manipulation, or any of pan-x, pan-y and pinch-zoom.
    /// Returns nothing for an unknown word, so the declaration is dropped, as CSS drops it.
    [[nodiscard]] inline Optional<u8> TouchActionsNamed(core::StringView k)
    {
        k = core::Trim(k);
        if (k == core::StringView(u8"auto") || k == core::StringView(u8"manipulation"))
            return kTouchAuto;
        if (k == core::StringView(u8"none"))
            return u8{0};
        u8 bits = 0;
        usize at = 0;
        while (at < k.Size())
        {
            while (at < k.Size() && k[at] == u8' ')
                ++at;
            usize end = at;
            while (end < k.Size() && k[end] != u8' ')
                ++end;
            const core::StringView word = k.SubStr(at, end - at);
            if (word == core::StringView(u8"pan-x"))
                bits |= kTouchPanX;
            else if (word == core::StringView(u8"pan-y"))
                bits |= kTouchPanY;
            else if (word == core::StringView(u8"pinch-zoom"))
                bits |= kTouchPinchZoom;
            else if (word.Size() != 0)
                return {};
            at = end;
        }
        return bits == 0 ? Optional<u8>{} : Optional<u8>{bits};
    }
    /// Returns the touch-action text for `bits`.
    [[nodiscard]] inline core::String TouchActionsName(u8 bits)
    {
        if (bits == kTouchAuto)
            return core::String(u8"auto");
        if (bits == 0)
            return core::String(u8"none");
        core::String out;
        const auto add = [&](const char8_t* word)
        {
            if (out.Size() != 0)
                out.Append(u8" ");
            out.Append(word);
        };
        if ((bits & kTouchPanX) != 0)
            add(u8"pan-x");
        if ((bits & kTouchPanY) != 0)
            add(u8"pan-y");
        if ((bits & kTouchPinchZoom) != 0)
            add(u8"pinch-zoom");
        return out;
    }
    /// What drives the pointer: the mouse, or a finger (the first one down).
    enum class PointerType : u8
    {
        Mouse,
        Touch,
    };

    /// The pointer's look over a node (CSS `cursor`). Auto takes the look of the enclosing node, as
    /// CSS's inherited cursor does. The host shows the cursor the dispatcher reports (GetCursor).
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
        Copy,   ///< a drop would copy
        Alias,  ///< a drop would link
        NoDrop, ///< nothing here takes the drag
        None,   ///< no cursor shows (a pad in use hides it; CSS's `none`)
    };
    inline constexpr const char8_t* kCursorNames[] = {
        u8"auto", u8"default", u8"pointer", u8"text", u8"move", u8"grab", u8"grabbing", u8"not-allowed",
        u8"wait", u8"progress", u8"crosshair", u8"help", u8"ew-resize", u8"ns-resize", u8"nwse-resize",
        u8"nesw-resize", u8"copy", u8"alias", u8"no-drop", u8"none",
    };
    /// Returns the cursor CSS calls `name`, or Auto for an unknown name.
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

    /// What the player is driving the GUI with: the pointer (a mouse, a finger), the keyboard or a
    /// pad. A host that tracks it (foundation.input's InputKindTracker, through GUI.Shell's bridge)
    /// supplies it. Without such a host, the dispatcher infers it from the input it receives: a
    /// press, a key, or a pad button or direction.
    enum class InputKind : u8
    {
        Pointer,
        Keyboard,
        Pad,
    };
    /// Returns "pointer", "keyboard" or "pad", the names a sheet's `@media (input: ...)` and
    /// scripts use.
    [[nodiscard]] inline core::StringView InputKindName(InputKind kind) noexcept
    {
        switch (kind)
        {
        case InputKind::Keyboard: return core::StringView(u8"keyboard");
        case InputKind::Pad: return core::StringView(u8"pad");
        case InputKind::Pointer: break;
        }
        return core::StringView(u8"pointer");
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

    // Platform-agnostic key identity for the keys the GUI interprets. The gui.shell bridge maps
    // platform key codes to these explicitly; a KeyEvent carries the value as a u32, so handlers
    // compare against static_cast<u32>(KeyCode::X). Typed characters arrive as TextInput, not
    // here.
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
        // The remaining letters, which bindings name. They come after the first set so that the
        // first set's values stay the same.
        B, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, W, Y, Z,
        Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        // The rest of the keyboard: punctuation, the keypad with codes of its own (so a game can
        // bind Numpad 5 separately from 5), the lock and system keys, and the modifier keys
        // themselves (so a game can bind Left Shift to sprint).
        F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
        Minus, Equals, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Grave, Comma, Period, Slash,
        CapsLock, ScrollLock, NumLock, PrintScreen, Pause, Menu,
        Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
        KeypadDivide, KeypadMultiply, KeypadMinus, KeypadPlus, KeypadEnter, KeypadDecimal,
        LeftCtrl, LeftShift, LeftAlt, LeftSuper, RightCtrl, RightShift, RightAlt, RightSuper,
    };

    /// The letter keys in alphabetical order. Their codes are not contiguous, because A, C, V and X
    /// are defined first.
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

    /// Whether a key types a character (a letter, a digit, punctuation, the keypad's digits and
    /// signs). A widget that takes typed text (type-ahead) claims these keys, so that a binding of
    /// the bare key does not take them away from the text.
    [[nodiscard]] constexpr bool KeyTypesText(KeyCode key) noexcept
    {
        for (const KeyCode letter : kLetterKeys)
            if (letter == key)
                return true;
        return (key >= KeyCode::Num0 && key <= KeyCode::Num9) || (key >= KeyCode::Minus && key <= KeyCode::Slash) ||
               (key >= KeyCode::Keypad0 && key <= KeyCode::KeypadDecimal && key != KeyCode::KeypadEnter);
    }

    /// Returns the modifier a modifier key stands for (Left Shift gives Shift), or none for other
    /// keys. Pressed alone, a modifier key is reported as itself, not as Shift plus itself.
    [[nodiscard]] constexpr u32 ModifierOfKey(KeyCode key) noexcept
    {
        switch (key)
        {
        case KeyCode::LeftShift:
        case KeyCode::RightShift: return KeyModShift;
        case KeyCode::LeftCtrl:
        case KeyCode::RightCtrl: return KeyModCtrl;
        case KeyCode::LeftAlt:
        case KeyCode::RightAlt: return KeyModAlt;
        case KeyCode::LeftSuper:
        case KeyCode::RightSuper: return KeyModSuper;
        default: return KeyModNone;
        }
    }

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
        /// Set by a handler that used the event, which ends its route there. A key that a widget
        /// leaves unhandled goes on to the keymap and to navigation.
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
        /// 1 for a single press, 2 for the second press of a double, 3 for a triple. Presses count
        /// together when they are near in time and place and use the same button. A press, its
        /// release and its click carry the same count.
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
        u32 Modifiers = 0; ///< Shift turns a vertical wheel to the other axis

        WheelEvent(Node* target, core::Float2 position, core::Float2 delta, u32 modifiers = 0) noexcept
            : Event(EventType::MouseWheel, target), Position(position), Delta(delta), Modifiers(modifiers)
        {
        }
    };

    /// A two-finger gesture that pinches, turns and pans at once, as Safari's does. Each value has a
    /// total since the gesture began and a change since the last event. The translation is in the
    /// GUI's units (the root's, not the node's own, which the gesture may turn). The gesture centres
    /// on the point midway between the fingers (`Focus`, in device pixels, as positions arrive).
    struct GestureEvent : Event
    {
        core::Float2 Focus{0.0f, 0.0f};
        f32 Scale = 1.0f;     ///< the fingers' span divided by their span when the gesture began
        f32 Rotation = 0.0f;  ///< the clockwise angle, in radians, the line between the fingers has turned
        core::Float2 Translation{0.0f, 0.0f};
        f32 ScaleDelta = 1.0f; ///< this event's factor; the total is the product of these
        f32 RotationDelta = 0.0f;
        core::Float2 TranslationDelta{0.0f, 0.0f};
        PointerType Pointer = PointerType::Touch; ///< touch, or the mouse for a trackpad

        GestureEvent(EventType type, Node* target) noexcept : Event(type, target) {}
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

    /// Why a popup closed: code closed it, Escape was pressed, a press landed outside it, another
    /// popup it does not contain opened, or its owner left the tree.
    enum class PopupCloseReason : u8
    {
        Code,
        Escape,
        OutsidePress,
        Replaced,
        OwnerRemoved,
    };
    [[nodiscard]] constexpr core::StringView PopupCloseReasonName(PopupCloseReason reason) noexcept
    {
        switch (reason)
        {
        case PopupCloseReason::Escape: return core::StringView(u8"escape");
        case PopupCloseReason::OutsidePress: return core::StringView(u8"outside-press");
        case PopupCloseReason::Replaced: return core::StringView(u8"replaced");
        case PopupCloseReason::OwnerRemoved: return core::StringView(u8"owner-removed");
        default: return core::StringView(u8"code");
        }
    }
    /// Tells that a popup opened, or that it closed and why (the web's `toggle`).
    struct ToggleEvent : Event
    {
        bool Open = false;
        PopupCloseReason Reason = PopupCloseReason::Code; ///< when closed

        ToggleEvent(Node* target, bool open, PopupCloseReason reason) noexcept
            : Event(EventType::Toggle, target), Open(open), Reason(reason)
        {
        }
    };

    /// An event about one tab of a bar or a view, identified by index.
    struct TabEvent : Event
    {
        i32 Index = -1;

        TabEvent(EventType type, Node* target, i32 index) noexcept : Event(type, target), Index(index) {}
    };

    /// An event about one row of an item view: its item (the view's row number) and its model index.
    struct ItemEvent : Event
    {
        i32 Item = -1;
        ModelIndex Index;

        ItemEvent(EventType type, Node* target, i32 item, ModelIndex index) noexcept : Event(type, target), Item(item), Index(index) {}
    };

    /// An IME's composition so far: its text and the caret within it, in characters. The text is
    /// empty when the composition ended or was committed; committed text then arrives as TextInput.
    struct CompositionEvent : Event
    {
        core::StringView Text;
        i32 Cursor = 0;

        CompositionEvent(Node* target, core::StringView text, i32 cursor) noexcept
            : Event(EventType::Composition, target), Text(text), Cursor(cursor)
        {
        }
    };

    /// A click on a rich text's link, carrying the id its markup gave it ([link=door]).
    struct LinkEvent : Event
    {
        core::StringView Link;

        LinkEvent(Node* target, core::StringView link) noexcept : Event(EventType::Link, target), Link(link) {}
    };

    /// A sheet's motion that ended on its node: the property a transition moved, or the
    /// @keyframes an animation ran, and how many seconds it ran, not counting its delay.
    struct MotionEndEvent : Event
    {
        core::StringView Name;
        f32 Elapsed = 0.0f;

        MotionEndEvent(EventType type, Node* target, core::StringView name, f32 elapsed) noexcept
            : Event(type, target), Name(name), Elapsed(elapsed)
        {
        }
    };

    /// An event a library names (EventType::Named): its name, which `on` listens by, and an optional
    /// text detail (a script reads it as `text`).
    struct NamedEvent : Event
    {
        core::StringView Name;
        core::StringView Detail;

        NamedEvent(Node* target, core::StringView name, core::StringView detail = {}) noexcept
            : Event(EventType::Named, target), Name(name), Detail(detail)
        {
        }
    };

    /// A drag's events: the payload, the effect a drop would have (or had, which is none when the
    /// drag was cancelled), where the pointer is (in the root's space) and the modifiers held.
    struct DragEvent : Event
    {
        const DragPayload& Payload;
        DropEffect Effect = DropEffect::None;
        core::Float2 Position{0.0f, 0.0f};
        u32 Modifiers = 0;
        i32 Place = 0; ///< where in the target the drop goes, as the target reported it (an index, a side)

        DragEvent(EventType type, Node* target, const DragPayload& payload) noexcept
            : Event(type, target), Payload(payload)
        {
        }
    };

    /// A request to a source to start a drag, with the press in the source's space. The source
    /// fills in the payload, and may give a preview and the point in the preview where the pointer
    /// holds it. Otherwise the pointer holds the preview where the press was on the source, or sits
    /// beside a default preview.
    struct DragRequest
    {
        core::Float2 Press{0.0f, 0.0f};
        u32 Modifiers = 0;
        PointerType Pointer = PointerType::Mouse;
        bool FromKeyboard = false; ///< started by `drag.pick` at the focus, not by a press
        DragPayload Payload;
        core::RefPtr<Node> Preview;
        core::Float2 GrabOffset{0.0f, 0.0f};
        bool HasGrabOffset = false;
        /// Set by a source that shows itself where the drag is (a window dragged by its title). No
        /// preview is made, and each move of the drag calls this with the pointer in the root's
        /// space, before the drop is queried. The source keeps itself out of hit tests.
        core::Function<void(core::Float2)> Follow;
    };

    /// Asks what a drop here would do. It goes to the node under the pointer and then up through its
    /// ancestors on every move, modifier change and scroll. The node sets the effect (one the source
    /// allows; Preferred is the one the modifiers choose), a reason (what the drop would do, or why
    /// not), and a place it draws. A node that refuses with a reason ends the walk.
    struct DropQuery
    {
        const DragPayload& Payload;
        core::Float2 Local{0.0f, 0.0f}; ///< the pointer in the asked node's space
        u32 Modifiers = 0;
        DropEffect Preferred = DropEffect::Move;
        bool FromKeyboard = false; ///< a keyboard drag: the asked node is the focus, and Local is its centre
        DropEffect Effect = DropEffect::None;
        core::String Reason;
        bool Refused = false;
        i32 Place = 0;

        /// Takes the drop with the preferred effect, or `effect` where the source allows it.
        void Accept(DropEffect effect = DropEffect::None)
        {
            const DropEffect want = effect == DropEffect::None ? Preferred : effect;
            Effect = (Payload.Allowed & DropEffectFlag(want)) != 0 ? want : Preferred;
        }
        /// Refuses the drop with a reason. The walk ends here and the reason is shown.
        void Refuse(core::StringView reason)
        {
            Effect = DropEffect::None;
            Refused = true;
            Reason = core::String(reason);
        }
    };

    /// How a drag ended, told to its source: the effect (none when it was cancelled or dropped
    /// nowhere) and the node that took it.
    struct DragResult
    {
        DropEffect Effect = DropEffect::None;
        Node* Target = nullptr;
        bool Cancelled = false;
        bool Outside = false;     ///< released over no root of the GUI, where the GUI has no window
        bool ScreenKnown = false; ///< Screen is valid (a host placed the roots)
        core::Float2 Screen{};    ///< where on the screen the pointer was when it ended
    };

    using EventCallback = core::Function<void(const Event&)>;

    namespace detail
    {
        struct NamedEventType
        {
            const char8_t* name;
            EventType type;
        };
        // The names scripts use for events (`on("click", ...)`) and the inspector shows.
        inline constexpr NamedEventType kEventTypeNames[] = {
            {u8"click", EventType::MouseClick},      {u8"mousedown", EventType::MouseDown},
            {u8"mouseup", EventType::MouseUp},       {u8"mouseenter", EventType::MouseEnter},
            {u8"mouseleave", EventType::MouseLeave}, {u8"mousemove", EventType::MouseMove},
            {u8"wheel", EventType::MouseWheel},      {u8"keydown", EventType::KeyDown},
            {u8"keyup", EventType::KeyUp},           {u8"textinput", EventType::TextInput},
            {u8"focus", EventType::FocusGained},     {u8"blur", EventType::FocusLost},
            {u8"change", EventType::Changed},        {u8"pointercancel", EventType::PointerCancel},
            {u8"link", EventType::Link},             {u8"input", EventType::Input},
            {u8"submit", EventType::Submit},         {u8"composition", EventType::Composition},
            {u8"toggle", EventType::Toggle},         {u8"close", EventType::Close},
            {u8"contextmenu", EventType::ContextMenu},
            {u8"scroll", EventType::Scroll},
            {u8"tabclose", EventType::TabClose},
            {u8"activate", EventType::ItemActivate},
            {u8"dragstart", EventType::DragStart},  {u8"dragend", EventType::DragEnd},
            {u8"dragenter", EventType::DragEnter},  {u8"dragover", EventType::DragOver},
            {u8"dragleave", EventType::DragLeave},  {u8"drop", EventType::Drop},
            {u8"dragquery", EventType::DragQuery},
            {u8"gesturestart", EventType::GestureStart}, {u8"gesturechange", EventType::GestureChange},
            {u8"gestureend", EventType::GestureEnd},     {u8"gesturecancel", EventType::GestureCancel},
            {u8"transitionend", EventType::TransitionEnd}, {u8"animationend", EventType::AnimationEnd},
            {u8"inputkind", EventType::InputKindChanged},
            {u8"entered", EventType::Entered},
        };
    }
    /// Gives the event type markup and scripts call `name` ("click", "keydown"). Returns false for
    /// an unknown name.
    [[nodiscard]] inline bool EventTypeNamed(core::StringView name, EventType& out) noexcept
    {
        for (const detail::NamedEventType& entry : detail::kEventTypeNames)
            if (core::StringView(entry.name) == name)
            {
                out = entry.type;
                return true;
            }
        return false;
    }
    /// Returns the name markup and scripts use for `type`, or empty when it has none.
    [[nodiscard]] inline core::StringView EventTypeName(EventType type) noexcept
    {
        for (const detail::NamedEventType& entry : detail::kEventTypeNames)
            if (entry.type == type)
                return core::StringView(entry.name);
        return {};
    }
}

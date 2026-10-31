// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :event_dispatcher partition
//
// EventDispatcher: routes abstract input into the node tree. Ported from eepp's
// Scene::EventDispatcher, but the core is platform-agnostic: instead of hooking a window's
// Input, it exposes an Inject* API that a gui.shell bridge feeds with already-abstracted
// events (from InputSurface/InputRouter). It hit-tests through the root's OverFind, tracks the
// hover and press paths, the focus and the pointer capture, and starts each input event's route
// (Node::RouteEvent: capture, target, bubble). It holds non-owning Node* references, which it
// drops when a subtree leaves the tree (NotifySubtreeRemoved).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:event_dispatcher;

import foundation.core; // Float2, StringView
import :node;
import :rect;
import :event;
import :clipboard;
import :navigation; // directional focus
import :popup_placement;
import :keymap;     // Keystroke, PadButton
import :reactive;   // Observable (focus, for commands)

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// How a popup closes, modelled on the web's popover kinds. An `Auto` popup closes on a press
    /// outside it, on `back` (Escape, the pad's East), and when another auto popup that it does
    /// not contain opens. A `Manual` popup (a dialog) closes only from code. A `Hint` (a tooltip)
    /// closes on any press or key; it never closes other popups and never takes focus or the
    /// pointer.
    enum class PopupKind : u8
    {
        Auto,
        Manual,
        Hint,
    };

    struct PopupOptions
    {
        PopupKind Kind = PopupKind::Auto;
        /// While the popup is open, everything below it is inert: it gets no pointer, no keys and
        /// no bindings.
        bool Modal = false;
        /// Focus moves into the popup (to its first focusable node, else to the popup itself if it
        /// is focusable) and returns when it closes. While it is open, it contains navigation.
        bool TakesFocus = false;
        /// What the popup is placed against, again on every layout pass: this node, else
        /// AnchorRect in the root's space (a point for a context menu), else nothing, in which case
        /// it keeps its own position.
        Node* Anchor = nullptr;
        Rect AnchorRect{};
        bool HasAnchorRect = false;
        /// Centres the popup in the root by its size, again on every layout pass (a picker over the
        /// window). The anchor is then ignored.
        bool Centered = false;
        PopupPlacement Placement{};
        /// Sizes the popup to its content, measured again on every layout pass (a tooltip, whose
        /// font a sheet sets after it opens). Otherwise it keeps the size it had when it opened.
        bool FitContent = false;
        /// The pointer passes through it to what is below (a drag's preview).
        bool PassesPointer = false;
        /// On close, the popup goes back to where it was in the tree before it opened, rather than
        /// leaving the tree. A script's own popup or menu needs this, since nothing else keeps it
        /// and leaving the tree would free it.
        bool ReturnHome = false;
        /// Called when the popup closes, with the reason, after the `toggle` event.
        core::Function<void(PopupCloseReason)> OnClosed;
        /// On close, the popup plays the sheet's exit (`:leaving`) before it goes. It leaves the
        /// open popups at once (input and focus move on, and OnClosed is called), but stays drawn on
        /// top, taking no input, until the animations the sheet started have ended (a dialog fading
        /// out). Without this flag it goes at once (a menu).
        bool LeavesOnClose = false;
        /// Called when the popup has gone from the top layer and the tree (or back to its home),
        /// after its exit if it plays one. It is not called if the popup opens again first.
        core::Function<void()> OnGone;
    };

    // The layer a drag shows in the top layer. It is the root's size and passes the pointer
    // through. Each child (the preview, the hint) is placed at the point the dispatcher gives it,
    // at its own measured size.
    class DragLayer : public Node
    {
        RTTI_OBJECT(DragLayer, Node)
    public:
        void Place(Node& child, core::Float2 at)
        {
            for (Placed& p : m_placed)
                if (p.Child == &child)
                {
                    p.At = at;
                    RequestLayout();
                    return;
                }
            m_placed.PushBack(Placed{&child, at});
            RequestLayout();
        }

        // Forgets a placement whose child has gone (a hint replaced). The child pointer is only
        // compared, never dereferenced.
        void OnChildrenChanged() override
        {
            for (usize i = m_placed.Size(); i-- > 0;)
                if (GetChildIndex(m_placed[i].Child) == kInvalidIndex)
                    m_placed.RemoveAt(i);
        }

    protected:
        void OnLayout() override
        {
            for (const Placed& p : m_placed)
                if (GetChildIndex(p.Child) != kInvalidIndex)
                {
                    const core::Float2 size = p.Child->Measure(LayoutConstraints{});
                    p.Child->Arrange(Rect{p.At.x, p.At.y, size.x, size.y});
                }
        }

    private:
        struct Placed
        {
            Node* Child; // one of this layer's children
            core::Float2 At;
        };
        core::Array<Placed> m_placed;
    };

    class EventDispatcher;

    /// Where a GUI's roots are on the screen, supplied by a host that places its windows. With it
    /// a drag can continue over another root of the GUI, and a drag released over no root ends
    /// outside at a screen point. Points are in each root's pointer units (as input arrives).
    class IRootPlaces
    {
    public:
        virtual ~IRootPlaces() = default;
        /// Returns the dispatcher of the root under `screen` and sets `local` to the point in it.
        /// Returns null when no root is there.
        virtual EventDispatcher* DispatcherAt(core::Float2 screen, core::Float2& local) = 0;
        /// Converts `local` in `dispatcher`'s root to a screen point. Returns false when that is not
        /// known.
        virtual bool ToScreen(const EventDispatcher& dispatcher, core::Float2 local, core::Float2& screen) = 0;
    };

    class EventDispatcher
    {
    public:
        explicit EventDispatcher(Node* root) noexcept : m_root(root) {}

        [[nodiscard]] Node* GetRoot() const noexcept { return m_root; }

        // The system-clipboard adapter (non-owning; supplied by the gui.shell bridge or the app).
        // Widgets reach it through GetEventDispatcher()->GetClipboard(); with none set, cut, copy
        // and paste do nothing.
        void SetClipboard(IClipboard* clipboard) noexcept { m_clipboard = clipboard; }
        [[nodiscard]] IClipboard* GetClipboard() const noexcept { return m_clipboard; }

        // === Refusing modal windows (an unattended run) ===
        /// Sets a function that Window::OpenModal asks before a modal window opens here. When it
        /// returns false the window does not open and answers as cancelled, so nothing waits for a
        /// person who is not there (an agent running an action). With no interceptor, every modal
        /// window opens. To nest scopes, take the current interceptor with TakeModalInterceptor
        /// and restore it afterwards.
        void SetModalInterceptor(core::Function<bool(Node& window)> interceptor) { m_modalInterceptor = core::Move(interceptor); }
        [[nodiscard]] core::Function<bool(Node& window)> TakeModalInterceptor() { return core::Move(m_modalInterceptor); }
        /// Whether a modal window may open here now.
        [[nodiscard]] bool AllowsModal(Node& window) { return !m_modalInterceptor || m_modalInterceptor(window); }


        [[nodiscard]] Node* GetOverNode() const noexcept { return m_overNode; }
        /// Whether the keyboard or the pad is driving the GUI (the last input was a key, a pad button
        /// or a direction, not the pointer). A menu opened in this state highlights its first item.
        [[nodiscard]] bool IsUsingKeyboard() const noexcept { return m_inputKind != InputKind::Pointer; }

        // === The input kind ===
        /// What is driving the GUI now: the pointer, the keyboard or a pad. The focus ring follows
        /// it (shown for the keys and the pad, hidden for the pointer), a pad hides the cursor, a
        /// sheet reads it as `@media (input: ...)`, and the root receives `inputkind` when it
        /// changes.
        [[nodiscard]] InputKind GetInputKind() const noexcept { return m_inputKind; }
        /// Returns the input kind and records it as a dependency of the current binding (a button
        /// prompt follows it).
        [[nodiscard]] InputKind ObserveInputKind() const
        {
            m_inputKindChanged.Track();
            return m_inputKind;
        }
        /// Sets the input kind when a host's tracker sees another kind of input. GUI.Shell's bridge
        /// passes on foundation.input's tracker as it changes, for input that reaches no widget (a
        /// mouse moved, a stick pushed). The dispatcher also follows the input it is given (a press,
        /// a key, a pad button or direction), so a host without a tracker, or a test, still sets
        /// the kind.
        void SetInputKind(InputKind kind) { ApplyInputKind(kind); }

        // === Tooltips ===
        // The nearest tooltip text up the parent chain from the hovered node shows as a hint popup
        // after the delay, measured on this dispatcher's clock (the GUI's update ticks it). Just
        // after one tooltip hides, the next shows at once. Any press or key hides the tooltip until
        // the pointer moves to another node. On touch, a long press on a node with no context menu
        // shows its tooltip.
        void SetTooltipDelay(f32 seconds) noexcept { m_tipDelay = core::Max(0.0f, seconds); }
        /// While the keys move the focus, shows the focused control's tooltip below it after the
        /// delay (on by default). The pointer's tooltip takes precedence.
        void SetKeyboardTooltips(bool on) noexcept { m_keyboardTooltips = on; }
        [[nodiscard]] bool HasKeyboardTooltips() const noexcept { return m_keyboardTooltips; }
        [[nodiscard]] f32 GetTooltipDelay() const noexcept { return m_tipDelay; }
        /// The tooltip shown now, or null.
        [[nodiscard]] Node* GetShownTooltip() const { return m_tip && IsPopupOpen(*m_tip) ? m_tip.Get() : nullptr; }

        /// Asks for the focused node's context menu at its bottom-left (Shift+F10, the Menu key) by
        /// sending a `contextmenu` event there. Returns true when something opened one.
        bool InjectContextMenu()
        {
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            UseKeyboard();
            MouseEvent event(EventType::ContextMenu, target, target->ConvertToWorldSpace(core::Float2{0.0f, target->GetSize().y}));
            return target->RouteEvent(event);
        }
        [[nodiscard]] Node* GetFocusNode() const noexcept { return m_focusNode; }
        /// The candidates the last arrow, d-pad or stick move weighed.
        [[nodiscard]] const NavTrace& GetLastNavigation() const noexcept { return m_lastNavigation; }
        /// Returns the focused node and records it as a dependency of the current binding (a
        /// command's availability follows it).
        [[nodiscard]] Node* ObserveFocus() const
        {
            m_focusChanged.Track();
            return m_focusNode;
        }
        [[nodiscard]] core::Float2 GetMousePosition() const noexcept { return m_mousePos; }

        // True if the currently focused node wants platform text input (the gui.shell bridge
        // reconciles the window's IME state against this each frame).
        [[nodiscard]] bool WantsTextInput() const
        {
            return m_focusNode != nullptr && m_focusNode->WantsTextInput();
        }
        /// Whether the focused node's text input is secret (a password).
        [[nodiscard]] bool IsTextInputSecret() const { return m_focusNode != nullptr && m_focusNode->IsTextInputSecret(); }

        // === Injection API (fed by the shell bridge) ===
        // Each returns whether the GUI used the input: a handler took it, or the pointer is over a
        // node that takes it (IsPointerOver). A game host uses this to tell that a click was the
        // UI's.
        /// `modifiers` are the modifiers held during the move (a drag that scrubs finer with Shift).
        bool InjectMouseMove(core::Float2 position, u32 modifiers = 0) { return PointerMove(position, PointerType::Mouse, 0, modifiers); }
        bool InjectMouseDown(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            return PointerDown(position, button, modifiers, PointerType::Mouse, 0);
        }
        bool InjectMouseUp(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            return PointerUp(position, button, modifiers, PointerType::Mouse, 0);
        }

        // === Touch === The first finger down drives the pointer as the mouse does (a press,
        // moves, a release, a click), with its events marked as touch. Every finger down is
        // tracked (where it went down and where it is now; two fingers make a gesture), but no
        // other finger drives the pointer. A finger hovers only while it is down, so nothing stays
        // hovered after a tap.
        //
        // A finger is either a tap or a pan. Once it moves past kPanSlop, the nearest scroller under
        // where it went down that can move in that direction takes it: the press is cancelled (so
        // the row under the finger is not clicked) and the content follows the finger. Released
        // while moving, the scroller flings. A finger that lands on a flinging scroller catches it:
        // the fling stops and that touch clicks nothing.
        bool InjectTouchDown(u64 finger, core::Float2 position)
        {
            if (FindFinger(finger) < 0)
                m_fingers.PushBack(Finger{finger, position, position});
            if (BeginGesture())
                return true; // a second finger over a node that handles gestures
            if (m_touchActive)
                return false; // a second finger is tracked but does not drive the pointer
            m_touchActive = true;
            m_touchFinger = finger;
            m_panning = false;
            m_panScroller = nullptr;
            m_panSampleCount = 0;
            bool caught = false;
            for (Node* n = HitTest(position); n != nullptr; n = n->GetParent())
                if (n->IsFlinging())
                {
                    n->StopFling();
                    caught = true;
                }
            const bool used = PointerDown(position, MouseButton::Left, 0, PointerType::Touch, finger);
            AddPanSample(position);
            if (caught)
            {
                CancelPointer(); // catching a fling is not a tap
                m_longPressArmed = false;
                return true;
            }
            return used;
        }
        bool InjectTouchMove(u64 finger, core::Float2 position)
        {
            if (const i32 at = FindFinger(finger); at >= 0)
                m_fingers[static_cast<usize>(at)].Now = position;
            if (m_gesturing && IsGestureFinger(finger))
            {
                MoveGesture();
                return true;
            }
            if (!m_touchActive || finger != m_touchFinger)
                return false;
            AddPanSample(position);
            if (!m_panning && !m_dragActive && !m_touchDragArmed)
            {
                const core::Float2 moved{position.x - m_pressAt.x, position.y - m_pressAt.y};
                // Pan only along an axis that the touch-action where the finger went down leaves to
                // the GUI. Along another axis the finger stays the pointer (for the node's own drag).
                const Node* under = HitTest(m_pressAt);
                const u8 allowed = under != nullptr ? under->EffectiveTouchActions() : kTouchAuto;
                const u8 axis = core::Abs(moved.x) > core::Abs(moved.y) ? kTouchPanX : kTouchPanY;
                if ((core::Abs(moved.x) > Dips(kPanSlop) || core::Abs(moved.y) > Dips(kPanSlop)) && (allowed & axis) != 0)
                    if (Node* scroller = PanScrollerFor(moved))
                    {
                        m_panning = true;
                        m_panScroller = scroller;
                        m_panLast = m_pressAt; // the content follows the finger from where it went down
                        CancelPointer();
                        m_longPressArmed = false;
                    }
            }
            if (m_panning)
            {
                if (m_panScroller != nullptr)
                    m_panScroller->ScrollBy(InNodeUnits(*m_panScroller, m_panLast, core::Float2{m_panLast.x - position.x, m_panLast.y - position.y}));
                m_panLast = position;
                m_mousePos = position;
                return true;
            }
            return PointerMove(position, PointerType::Touch, finger);
        }
        bool InjectTouchUp(u64 finger, core::Float2 position)
        {
            if (const i32 at = FindFinger(finger); at >= 0)
                m_fingers.RemoveAt(static_cast<usize>(at));
            if (m_gesturing && IsGestureFinger(finger))
            {
                EndGesture(EventType::GestureEnd); // the finger still down does nothing more
                return true;
            }
            if (!m_touchActive || finger != m_touchFinger)
                return false;
            m_touchActive = false;
            if (m_panning)
            {
                AddPanSample(position);
                m_panning = false;
                const core::Float2 v = PanVelocity();
                if (m_panScroller != nullptr && core::Abs(v.x) + core::Abs(v.y) > Dips(kFlingMinSpeed))
                    m_panScroller->Fling(InNodeUnits(*m_panScroller, position, core::Float2{-v.x, -v.y}));
                m_panScroller = nullptr;
                UpdateHoverPath(nullptr, position);
                return true;
            }
            if (m_touchDragArmed && !m_dragActive)
                CancelPointer(); // held to arm a drag, then released without moving: neither a drag nor a tap
            m_touchDragArmed = false;
            const bool used = PointerUp(position, MouseButton::Left, 0, PointerType::Touch, finger);
            UpdateHoverPath(nullptr, position); // the finger lifted: nothing is under it
            return used;
        }
        /// A finger that is down: its id, where it went down and where it is now (device pixels).
        struct Finger
        {
            u64 Id = 0;
            core::Float2 Start{};
            core::Float2 Now{};
        };
        /// The fingers down, in the order they went down.
        [[nodiscard]] core::Span<const Finger> GetFingers() const noexcept { return core::Span<const Finger>(m_fingers.Data(), m_fingers.Size()); }
        /// Handles a finger the platform took back for a system gesture; no release follows. Its
        /// gesture and its press are cancelled, and its pan ends where it is.
        bool InjectTouchCancel(u64 finger)
        {
            if (const i32 at = FindFinger(finger); at >= 0)
                m_fingers.RemoveAt(static_cast<usize>(at));
            if (m_gesturing && IsGestureFinger(finger))
            {
                EndGesture(EventType::GestureCancel);
                return true;
            }
            if (!m_touchActive || finger != m_touchFinger)
                return false;
            if (!m_panning)
                CancelPointer();
            m_touchActive = false;
            m_panning = false;
            m_panScroller = nullptr;
            m_longPressArmed = false;
            m_touchDragArmed = false;
            UpdateHoverPath(nullptr, m_mousePos);
            return true;
        }
        /// A trackpad pinch at the pointer, reported by the platform as a gesture rather than as
        /// touches. `phase` is begin, update or end; `scaleDelta` above 1 means spreading. It sends
        /// the same gesture events that two fingers make, to a node whose touch-action excludes
        /// pinch-zoom, marked as mouse events, with no rotation or translation.
        bool InjectPinch(EventType phase, f32 scaleDelta)
        {
            if (phase == EventType::GestureStart)
            {
                if (m_gesturing)
                    return false;
                Node* target = HitTest(m_mousePos);
                if (target == nullptr || (target->EffectiveTouchActions() & kTouchPinchZoom) != 0)
                    return false;
                m_gestureIds[0] = m_gestureIds[1] = kNoFinger;
                m_gestureFocus0 = m_mousePos;
                m_gestureScale = 1.0f;
                m_gestureRotation = 0.0f;
                m_gestureTranslation = core::Float2{0.0f, 0.0f};
                m_gesturePointer = PointerType::Mouse;
                m_gestureTarget = core::RefPtr<Node>(target);
                m_gesturing = true;
                RouteGesture(EventType::GestureStart, 1.0f, 0.0f, core::Float2{0.0f, 0.0f});
                return true;
            }
            if (!m_gesturing || m_gesturePointer != PointerType::Mouse)
                return false;
            if (phase == EventType::GestureChange)
            {
                if (m_gestureTarget->GetRootNode() != m_root)
                {
                    EndGesture(EventType::GestureCancel);
                    return true;
                }
                m_gestureScale *= scaleDelta;
                RouteGesture(EventType::GestureChange, scaleDelta, 0.0f, core::Float2{0.0f, 0.0f});
                return true;
            }
            EndGesture(phase == EventType::GestureCancel ? EventType::GestureCancel : EventType::GestureEnd);
            return true;
        }
        /// Two fingers are making a gesture.
        [[nodiscard]] bool IsGesturing() const noexcept { return m_gesturing; }
        /// Cancels the gesture in progress (for a finger the platform took back). Its target
        /// receives `gesturecancel`.
        void CancelGesture()
        {
            if (m_gesturing)
                EndGesture(EventType::GestureCancel);
        }
        /// Whether a finger is panning a scroller (its press was cancelled).
        [[nodiscard]] bool IsPanning() const noexcept { return m_panning; }
        [[nodiscard]] bool IsTouchActive() const noexcept { return m_touchActive; }
        [[nodiscard]] i32 FindFinger(u64 id) const noexcept
        {
            for (usize i = 0; i < m_fingers.Size(); ++i)
                if (m_fingers[i].Id == id)
                    return static_cast<i32>(i);
            return -1;
        }

        /// Cancels the press in progress. The pressed node receives PointerCancel (a widget drops
        /// its drag), no release or click follows, and the press path and any capture are cleared.
        /// A scroller that starts panning under a finger calls this, so the row under the finger is
        /// not clicked at the end of the scroll.
        void CancelPointer()
        {
            Node* pressed = m_downNode;
            const core::RefPtr<Node> pin{pressed};
            m_downNode = nullptr;
            ClearPressPath();
            if (pressed != nullptr && pressed->IsTreeEnabled())
            {
                MouseEvent event(EventType::PointerCancel, pressed, m_mousePos);
                event.Pointer = m_touchActive ? PointerType::Touch : PointerType::Mouse;
                event.PointerId = m_touchActive ? m_touchFinger : 0;
                (void)pressed->RouteEvent(event);
            }
            if (m_captureNode != nullptr)
                ReleasePointer(*m_captureNode);
        }

        bool InjectMouseWheel(core::Float2 position, core::Float2 delta, u32 modifiers = 0)
        {
            Node* hit = HitTest(position);
            // The wheel goes up from the node under the pointer until a scroller takes it; a
            // scroller already at its end leaves it to the one around it. A disabled control takes
            // no wheel but does not block it either: the walk starts at the nearest enabled
            // ancestor, so a list of cards scrolls even when some cards are disabled.
            while (hit != nullptr && !hit->IsTreeEnabled())
                hit = hit->GetParent();
            bool used = false;
            if (hit != nullptr)
            {
                WheelEvent event(hit, position, delta, modifiers);
                used = hit->RouteEvent(event);
            }
            return used || IsPointerTaken(hit);
        }

        // === Capturing the next key ===
        /// Sends the next key or pad button to `onKey` before the keymap, navigation or any widget
        /// sees it (for a rebinding screen). A modifier pressed alone is taken when it is released,
        /// so Ctrl followed by S is taken as Ctrl+S. Any text the taken key would type is dropped.
        /// Escape calls `onCancel` instead, unless `takeEscape` is set. A new capture requested while
        /// one is waiting replaces it, and the old one's cancel is called.
        void CaptureNextKey(core::Function<void(const Keystroke&)> onKey, core::Function<void()> onCancel = {}, bool takeEscape = false)
        {
            CancelKeyCapture();
            m_capture.OnKey = core::Move(onKey);
            m_capture.OnCancel = core::Move(onCancel);
            m_capture.TakeEscape = takeEscape;
            m_capture.Waiting = true;
            m_capture.PendingModifier = 0;
        }
        /// Stops a waiting key capture and calls its cancel.
        void CancelKeyCapture()
        {
            if (!m_capture.Waiting)
                return;
            m_capture.Waiting = false;
            core::Function<void()> cancel = core::Move(m_capture.OnCancel);
            m_capture.OnKey = {};
            if (cancel)
                cancel();
        }
        [[nodiscard]] bool IsCapturingKey() const noexcept { return m_capture.Waiting; }

        // === Access keys ===
        /// Whether access keys are underlined now: while Alt is held, or always when set so.
        [[nodiscard]] bool AreMnemonicsShown() const noexcept { return m_mnemonicsAlways || m_altHeld; }
        /// Underlines access keys always, not only while Alt is held (for a platform that does
        /// so).
        void SetMnemonicsAlwaysShown(bool always)
        {
            if (always == m_mnemonicsAlways)
                return;
            const bool was = AreMnemonicsShown();
            m_mnemonicsAlways = always;
            if (was != AreMnemonicsShown())
                RedrawMnemonics();
        }

        bool InjectKeyDown(u32 keyCode, u32 modifiers = 0) { return KeyDownFrom(InputKind::Keyboard, keyCode, modifiers); }
        /// Sets the chord that releases a focus capturing every key (Node::SetCapturesAllKeys).
        /// The default is Shift+F1, the chord a game in an editor's panel offers.
        void SetCaptureRelease(const Keystroke& stroke) noexcept { m_captureRelease = stroke; }
        [[nodiscard]] const Keystroke& GetCaptureRelease() const noexcept { return m_captureRelease; }
        // Handles a key down from the keyboard, or one standing in for a pad button (East is sent as
        // Escape).
        bool KeyDownFrom(InputKind from, u32 keyCode, u32 modifiers)
        {
            NoteInput();
            DropHiddenFocus();
            ApplyInputKind(from);
            m_capture.DropText = false;
            if (keyCode == static_cast<u32>(KeyCode::LeftAlt) || keyCode == static_cast<u32>(KeyCode::RightAlt))
                SetAltHeld(true);
            if (m_capture.Waiting)
            {
                const KeyCode key = static_cast<KeyCode>(keyCode);
                if (key == KeyCode::Escape && modifiers == 0 && !m_capture.TakeEscape)
                {
                    CancelKeyCapture();
                    return true;
                }
                if (ModifierOfKey(key) != 0)
                {
                    m_capture.PendingModifier = keyCode; // taken alone if released before another key
                    return true;
                }
                m_capture.DropText = true; // the text this key types is dropped
                TakeCapturedKey(Keystroke{keyCode, modifiers, false});
                return true;
            }
            // During a keyboard drag, Escape (a pad's East) cancels and Return or Space drops at the
            // focus. Other keys move the focus as usual, and the drop is queried again at the new
            // focus.
            if (m_dragActive && m_dragFromKeyboard)
            {
                if (keyCode == static_cast<u32>(KeyCode::Escape))
                {
                    CancelDrag();
                    return true;
                }
                if ((keyCode == static_cast<u32>(KeyCode::Return) || keyCode == static_cast<u32>(KeyCode::Space)) && modifiers == 0)
                {
                    QueryAtFocus();
                    Drop();
                    return true;
                }
            }
            // A pointer drag takes the keys: Escape (a pad's East) cancels it, and a modifier
            // re-queries the drop.
            if (m_dragActive && !m_dragFromKeyboard)
            {
                if (keyCode == static_cast<u32>(KeyCode::Escape))
                    CancelDrag();
                else if (modifiers != m_dragModifiers)
                {
                    m_dragModifiers = modifiers;
                    QueryDrop(m_mousePos);
                }
                return true;
            }
            // A focus that captures every key (a game in a panel) takes this key, whatever it is, and
            // the GUI acts on none of them. Only the release chord frees it.
            if (m_focusNode != nullptr && m_focusNode->GetCapturesAllKeys())
            {
                const u32 strokeModifiers = modifiers & ~ModifierOfKey(static_cast<KeyCode>(keyCode));
                if (Keystroke{keyCode, strokeModifiers, false} == m_captureRelease)
                {
                    Node* released = m_focusNode;
                    released->SetCapturesAllKeys(false);
                    released->SendEvent(NamedEvent(released, core::StringView(u8"keysreleased")));
                    return true;
                }
                (void)RouteKey(EventType::KeyDown, keyCode, modifiers);
                return true;
            }
            // A key hides a tooltip and keeps it hidden until the pointer moves to another node, as a
            // press does.
            if (m_tipProvider != nullptr)
                m_tipSuppressed = true;
            ClosePopupsOfKind(PopupKind::Hint, PopupCloseReason::Code);
            // The focused widget and its ancestors get the key first (capture, then bubble from the
            // focus).
            if (RouteKey(EventType::KeyDown, keyCode, modifiers))
            {
                if (m_dragActive && m_dragFromKeyboard)
                    QueryAtFocus(); // the widget's own key (a list's arrows) may have moved the drop point
                return true;
            }
            // Tab and Shift+Tab move focus when nothing took them. In a widget that captures Tab
            // they belong to the widget, and Ctrl+Tab and Ctrl+Shift+Tab move focus instead.
            if (keyCode == static_cast<u32>(KeyCode::Tab))
            {
                const bool captured = m_focusNode != nullptr && m_focusNode->GetTabCaptures();
                const u32 traverse = captured ? static_cast<u32>(KeyModCtrl) : 0u;
                if ((modifiers & ~static_cast<u32>(KeyModShift)) == traverse)
                    return (modifiers & KeyModShift) != 0 ? FocusPrevious() : FocusNext();
                if (captured && (modifiers & ~static_cast<u32>(KeyModShift)) == 0)
                    return true; // the key belongs to the widget, even though it did nothing with it
            }
            // Then the key bindings: a keystroke bound to a command, where the contexts on the focus
            // path allow it.
            const u32 strokeModifiers = modifiers & ~ModifierOfKey(static_cast<KeyCode>(keyCode)); // Left Shift alone is "leftshift"
            if (m_keystrokes && m_keystrokes(Keystroke{keyCode, strokeModifiers, false}))
                return true;
            // Alt with an access key that no binding took: the widget marked with that key, in the
            // open menu or the focus scope, takes focus and acts.
            if (modifiers == KeyModAlt && ActivateMnemonic(MnemonicOfKey(static_cast<KeyCode>(keyCode))))
                return true;
            if (modifiers != 0)
                return false;
            // Any key left is for navigation: an arrow moves focus, and Return or Space activates.
            switch (static_cast<KeyCode>(keyCode))
            {
            case KeyCode::Up: return MoveFocus(NavDirection::Up);
            case KeyCode::Down: return MoveFocus(NavDirection::Down);
            case KeyCode::Left: return MoveFocus(NavDirection::Left);
            case KeyCode::Right: return MoveFocus(NavDirection::Right);
            case KeyCode::Return:
            case KeyCode::Space: return Activate();
            default: return false;
            }
        }

        /// Returns the stops Tab steps through now, in order: the shown and enabled tab-focusable
        /// widgets inside the open modal, or inside the dialog that has focus, or else in the whole
        /// root. The debug overlay numbers them.
        [[nodiscard]] core::Array<Node*> TabStops() const
        {
            core::Array<Node*> stops;
            Node* within = FocusScopeRoot();
            if (Node* contain = ContainingScopeOf(m_focusNode); contain != nullptr && IsInSubtree(contain, within))
                within = contain;
            CollectTabStops(within, stops);
            // A focus group is a single stop, at the position of its first member. The stop is the
            // member focused now, else the group's entry (remembered, default, current, first).
            for (usize i = 0; i < stops.Size(); ++i)
            {
                Node* group = OutermostFocusGroup(stops[i], within);
                if (group == nullptr)
                    continue;
                Node* stop = m_focusNode != nullptr && IsInSubtree(m_focusNode, group) && m_focusNode->IsTabFocusable()
                                 ? m_focusNode
                                 : EntryInto(*group, stops[i]);
                stops[i] = stop;
                for (usize j = stops.Size(); j-- > i + 1;)
                    if (OutermostFocusGroup(stops[j], within) == group)
                        stops.RemoveAt(j);
            }
            // tab-index: a stop with a negative index is skipped. Positive indices come first,
            // lowest first; ties and zeros stay in tree order (an insertion sort keeps that order,
            // and there are few stops).
            usize kept = 0;
            for (usize i = 0; i < stops.Size(); ++i)
                if (stops[i]->GetTabIndex() >= 0)
                    stops[kept++] = stops[i];
            stops.Resize(kept);
            const auto before = [](const Node* a, const Node* b)
            {
                const i32 x = a->GetTabIndex(), y = b->GetTabIndex();
                return x > 0 && (y == 0 || x < y);
            };
            for (usize i = 1; i < stops.Size(); ++i)
                for (usize j = i; j > 0 && before(stops[j], stops[j - 1]); --j)
                    Swap(stops[j], stops[j - 1]);
            return stops;
        }

        /// Moves focus to the best widget in `direction`, inside the open modal if there is one and
        /// within focus scopes. With nothing focused, moves it to the GUI's entry. Returns true
        /// when focus moved. The gamepad's d-pad and stick arrive here as the arrow keys do.
        bool MoveFocus(NavDirection direction)
        {
            UseKeyboard();
            Node* root = FocusScopeRoot();
            if (root == nullptr)
                return false;
            (void)HitTest(core::Float2{}); // runs any pending layout pass so navigation sees the current layout
            Node* target = NavigateFrom(*root, m_focusNode, direction, &m_lastNavigation);
            if (target == nullptr || target == m_focusNode)
                return false;
            SetFocusNode(target);
            return true;
        }
        /// Handles a gamepad button (the d-pad goes to PressDirection instead). The bindings get it
        /// first (East is bound to `back` by default). If no binding takes it, South activates and
        /// East is sent as Escape, so a widget can handle its own Escape (cancelling an edit).
        void InjectPadButton(PadButton button)
        {
            if (PadCaptured())
                return;
            NoteInput();
            DropHiddenFocus();
            ApplyInputKind(InputKind::Pad);
            UseKeyboard();
            if (m_capture.Waiting)
            {
                TakeCapturedKey(Keystroke{static_cast<u32>(button), 0, true});
                return;
            }
            if (m_keystrokes && m_keystrokes(Keystroke{static_cast<u32>(button), 0, true}))
                return;
            if (button == PadButton::South)
                (void)Activate();
            else if (button == PadButton::East)
                (void)KeyDownFrom(InputKind::Pad, static_cast<u32>(KeyCode::Escape), 0); // not taken as `back`, so it reaches the widget as Escape
        }
        /// Sets a callback for input the user gives this root (a press, a key, the gamepad). The GUI
        /// uses it to make this the active root.
        void SetOnInput(core::Function<void()> onInput) { m_onInput = core::Move(onInput); }
        /// Sets where unhandled keystrokes go: the SceneRoot's keymap, which returns true when it
        /// took one.
        void SetKeystrokeRouter(core::Function<bool(const Keystroke&)> router) { m_keystrokes = core::Move(router); }

        /// Records that the press in progress has already activated what it pressed (a button that
        /// acts on the press, or a repeating one), so its release makes no click.
        void SuppressClick() noexcept { m_clickSuppressed = true; }
        /// Activates the focused widget (Return, Space, a gamepad's South) as a click would.
        bool Activate()
        {
            if (m_dragActive && m_dragFromKeyboard)
            {
                // The pad's South during a keyboard drag drops at the focus.
                QueryAtFocus();
                Drop();
                return true;
            }
            return m_focusNode != nullptr && m_focusNode->IsTreeEnabled() && m_focusNode->HandleActivate();
        }
        bool InjectKeyUp(u32 keyCode, u32 modifiers = 0)
        {
            if (keyCode == static_cast<u32>(KeyCode::LeftAlt) || keyCode == static_cast<u32>(KeyCode::RightAlt))
                SetAltHeld(false);
            if (m_capture.Waiting)
            {
                // A modifier released with no other key pressed after it is taken as the modifier
                // key itself.
                if (m_capture.PendingModifier == keyCode)
                    TakeCapturedKey(Keystroke{keyCode, modifiers & ~ModifierOfKey(static_cast<KeyCode>(keyCode)), false});
                return true;
            }
            if (m_dragActive && !m_dragFromKeyboard)
            {
                if (modifiers != m_dragModifiers)
                {
                    m_dragModifiers = modifiers;
                    QueryDrop(m_mousePos);
                }
                return true;
            }
            return RouteKey(EventType::KeyUp, keyCode, modifiers);
        }
        bool InjectText(core::StringView text)
        {
            if (m_capture.Waiting || m_capture.DropText)
            {
                m_capture.DropText = false; // drop the captured key's text
                return true;
            }
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            TextInputEvent event(target, text);
            return target->RouteEvent(event);
        }
        /// Sends an IME's composition so far to the focused node, which shows it in place. Empty
        /// text means the composition ended; the committed text follows as InjectText. `cursor` is
        /// in characters.
        bool InjectComposition(core::StringView text, i32 cursor = 0)
        {
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            CompositionEvent event(target, text, cursor);
            return target->RouteEvent(event);
        }
        /// Returns the area where the focused node takes typed text, in input space (as positions
        /// are injected), and the caret's x there, so a host's IME can show its candidates there.
        /// Returns false when the focused node takes no text.
        [[nodiscard]] bool GetTextInputArea(Rect& area, f32& caretX) const
        {
            if (m_focusNode == nullptr || !m_focusNode->WantsTextInput())
                return false;
            Rect local{};
            f32 localCaret = 0.0f;
            if (!m_focusNode->GetTextInputArea(local, localCaret))
                return false;
            const core::Float2 a = m_focusNode->ConvertToWorldSpace(core::Float2{local.x, local.y});
            const core::Float2 b = m_focusNode->ConvertToWorldSpace(core::Float2{local.x + local.width, local.y + local.height});
            const core::Float2 c = m_focusNode->ConvertToWorldSpace(core::Float2{localCaret, local.y});
            area = Rect{core::Min(a.x, b.x), core::Min(a.y, b.y), core::Abs(b.x - a.x), core::Abs(b.y - a.y)};
            caretX = c.x - area.x;
            return true;
        }

        // === Pointer capture ===
        /// Sends the pointer's moves and its release to `node` wherever the pointer goes, until the
        /// capture is released or the press ends (a press captures the pressed node implicitly). A
        /// node that loses the capture (released, taken by another node, or leaving the tree)
        /// receives PointerCaptureLost.
        void CapturePointer(Node& node)
        {
            if (m_captureNode == &node)
                return;
            Node* previous = m_captureNode;
            m_captureNode = &node;
            if (previous != nullptr)
                SendCaptureLost(*previous);
        }
        void ReleasePointer(Node& node)
        {
            if (m_captureNode != &node)
                return;
            m_captureNode = nullptr;
            SendCaptureLost(node);
        }
        [[nodiscard]] Node* GetPointerCapture() const noexcept { return m_captureNode; }
        /// The dispatcher's clock in seconds, advanced by Tick. Click counting and type-ahead read
        /// it.
        [[nodiscard]] f32 GetTime() const noexcept { return m_time; }

        /// Returns the cursor to show now: the nearest `cursor` set on the node under the pointer or
        /// its ancestors (starting from the captured node while one holds the pointer, so a drag
        /// keeps its cursor), else Default. The host (the shell bridge) shows it.
        [[nodiscard]] Cursor GetCursor() const
        {
            if (m_dragActive) // a drag's cursor shows what a drop would do
                return m_dropEffect == DropEffect::Copy ? Cursor::Copy : m_dropEffect == DropEffect::Link ? Cursor::Alias
                       : m_dropEffect == DropEffect::Move ? Cursor::Move : Cursor::NoDrop;
            if (m_inputKind == InputKind::Pad)
                return Cursor::None; // a pad is in use, so the pointer is hidden
            Node* from = m_captureNode != nullptr ? m_captureNode : m_downNode != nullptr ? m_downNode : m_overNode;
            for (Node* n = from; n != nullptr; n = n->GetParent())
                if (const Cursor cursor = n->GetCursorNow(); cursor != Cursor::Auto)
                    return cursor;
            return Cursor::Default;
        }

        /// Whether a pointer at `position` (device pixels) is over the GUI, meaning over a node that
        /// takes the pointer (Node::TakesPointer) and not just over a layout container. A game host
        /// asks this before treating a click as its own.
        [[nodiscard]] bool IsPointerOver(core::Float2 position) const { return IsPointerTaken(HitTest(position)); }
        /// Returns the node a pointer at `position` reaches, found as input finds it: in an open
        /// popup first (a menu, a dialog), then in the root. Returns null below a modal popup,
        /// where everything is inert.
        [[nodiscard]] Node* NodeAt(core::Float2 position) const { return HitTest(position); }

        // === Focus ===
        /// Takes focus away from a focused node that is no longer shown (it or an ancestor is hidden
        /// or out of the layout), as removing it would. The keys then go to its scope and never to
        /// something that cannot be seen, so Return does not press a button left focused on a
        /// screen that has been hidden. Runs each tick and before a key or a pad button.
        void DropHiddenFocus()
        {
            if (m_focusNode != nullptr && !m_focusNode->IsTreeVisible())
                SetFocusNode(nullptr);
        }
        void SetFocusNode(Node* node)
        {
            if (node == m_focusNode || (node != nullptr && node->IsTreeInert())) // a node that is leaving or inert takes no focus
                return;
            Node* previous = m_focusNode;
            m_focusNode = node;
            m_focusChanged.Notify(); // the focus path decides which commands are available
            if (previous)
                previous->HandleFocusLost();
            if (m_focusNode)
            {
                RememberInScopes(*m_focusNode); // returning to a scope later focuses this node again
                m_focusNode->SetFocusVisible(m_inputKind != InputKind::Pointer);
                m_focusNode->HandleFocusGained();
                // The scrollers around the node bring it into view, but only those inside its popup,
                // not those around the popup's owner. A popup is placed in the root's space and does
                // not scroll with them; scrolling them would move the page a little further each
                // time a menu opened in it.
                for (Node* n = m_focusNode; n != nullptr && !n->IsInTopLayer(); n = n->GetParent())
                    if (Node* p = n->GetParent())
                        p->OnDescendantFocused(*m_focusNode);
            }
            if (m_dragActive && m_dragFromKeyboard)
                QueryAtFocus(); // a keyboard drag queries the drop at the new focus
        }

        // === Drag and drop ===
        // A left press (or a finger's press after a long press) that moves past the threshold
        // offers a drag (DragRequest) to the pressed node and then its ancestors. The first source
        // that fills in the request starts the drag; a node that owns its press ends the walk.
        // During the drag, each move, modifier change and scroll asks the node under the pointer
        // and then its ancestors what a drop would do (DropQuery). The first that accepts it is
        // the target; the cursor and the preview show the effect, and any reason shows by the
        // pointer. A release over the target drops. Escape, a right press, the pad's East or the
        // source leaving the tree cancels. The source is told how the drag ended.

        /// How far a press moves before it drags, in DIPs (scaled by the display).
        void SetDragThreshold(f32 distance) noexcept { m_dragThreshold = core::Max(0.0f, distance); }
        [[nodiscard]] f32 GetDragThreshold() const noexcept { return m_dragThreshold; }
        /// Starts a drag from code, like Godot's force_drag; `source` is not asked for a request.
        void StartDrag(Node& source, DragRequest request) { BeginDrag(source, core::Move(request), m_mousePos); }
        [[nodiscard]] bool IsDragging() const noexcept { return m_dragActive; }
        /// Sets where the GUI's roots are on the screen (owned by its GUIContext and set by the
        /// host). With null, only this root is known and a drag past its edge is outside.
        void SetRootPlaces(IRootPlaces* places) noexcept { m_rootPlaces = places; }
        /// Returns the dispatcher of the root that a drag from here is over now (this one, or
        /// another root of the GUI). Returns null while the drag is outside every root or no drag is
        /// in progress.
        [[nodiscard]] EventDispatcher* GetDragOver() const noexcept { return m_dragActive && !m_dragOutside ? m_dragOver : nullptr; }
        /// Called when another root of the GUI goes away. A drag from here that was over it is over
        /// no root until it moves, and forgets the target and the layer it had there.
        void ForgetRoot(const EventDispatcher& gone) noexcept
        {
            if (&gone == this)
                return;
            if (m_dragOver == &gone)
            {
                m_dragOver = this;
                m_dragOutside = true;
                if (m_dropTarget != nullptr && m_dropTarget->GetRootNode() == gone.m_root)
                    m_dropTarget = nullptr; // the target's tree goes with the root, so it is not told
            }
            if (m_dragLayerIn == &gone)
                m_dragLayerIn = nullptr;
        }
        [[nodiscard]] const DragPayload& GetDragPayload() const noexcept { return m_dragPayload; }
        [[nodiscard]] Node* GetDragSource() const noexcept { return m_dragSource; }
        [[nodiscard]] Node* GetDropTarget() const noexcept { return m_dropTarget; }
        /// The effect a drop would have now, and the target's reason (or the reason for a refusal).
        [[nodiscard]] DropEffect GetDropEffect() const noexcept { return m_dropEffect; }
        [[nodiscard]] core::StringView GetDropReason() const noexcept { return m_dropReason.AsView(); }
        /// The preview that follows the pointer, and the hint beside it (null while none shows).
        [[nodiscard]] Node* GetDragPreview() const noexcept { return m_dragPreview.Get(); }
        [[nodiscard]] Node* GetDropHint() const noexcept { return m_dropHint.Get(); }
        void CancelDrag()
        {
            if (m_dragActive)
                FinishDrag(DropEffect::None, nullptr, true);
        }
        /// Sets the depth of the band at a scroller's edge in which a drag scrolls it, and the speed
        /// at the very edge (the speed ramps up with depth into the band).
        void SetDragScrollBand(f32 band, f32 speed = 900.0f) noexcept
        {
            m_dragScrollBand = core::Max(1.0f, band);
            m_dragScrollSpeed = core::Max(0.0f, speed);
        }
        /// Sets how long a drag rests on a node before the node opens (a tree node, an expander, a
        /// tab).
        void SetDragHoverOpenDelay(f32 seconds) noexcept { m_dragHoverOpenDelay = core::Max(0.0f, seconds); }
        /// Handles files the OS dropped at `position`, through the same path as a drag. A `files`
        /// payload, whose value is the paths, with a copy effect, is offered to the node there and
        /// then its ancestors, and dropped on the first that takes it. If none takes it, the GUI's
        /// own handler gets the files. Returns true when something took them. The OS reports no
        /// hover, so nothing shows while a file is dragged over.
        bool InjectFileDrop(core::Float2 position, const core::Array<core::String>& paths)
        {
            if (paths.IsEmpty())
                return false;
            if (m_dragActive)
                CancelDrag();
            NoteInput();
            core::Array<core::Variant> list;
            for (const core::String& path : paths)
                list.PushBack(core::Variant::From<core::String>(path));
            m_dragPayload = DragPayload{};
            m_dragPayload.Type = core::String(u8"files");
            m_dragPayload.Value = core::Variant::From<core::Array<core::Variant>>(core::Move(list));
            m_dragPayload.Label = paths.Size() == 1 ? paths[0] : core::Format(u8"{} files", paths.Size());
            m_dragPayload.Allowed = DropEffectsCopy;
            m_dragActive = true;
            m_dragSource = nullptr;
            m_dragModifiers = 0;
            QueryDrop(position);
            const bool taken = m_dropTarget != nullptr && m_dropEffect != DropEffect::None;
            Drop();
            if (!taken && m_onFileDrop)
                return m_onFileDrop(paths, RootPoint(position));
            return taken;
        }
        /// Sets the GUI's own handler for files no node takes (an import, an open).
        void SetOnFileDrop(core::Function<bool(const core::Array<core::String>&, core::Float2)> handler)
        {
            m_onFileDrop = core::Move(handler);
        }

        /// `drag.pick`, modelled on Godot's accessibility drag: the focused node, or an ancestor,
        /// starts a drag of what it would drag with the pointer. The focus then moves as usual (the
        /// arrows, Tab, the pad), and each node it reaches is queried as the pointer would query
        /// it. `drag.drop` (Return, Space, the pad's South) drops at the focus, and Escape (East)
        /// cancels.
        bool PickDrag()
        {
            if (m_dragActive)
                CancelDrag();
            for (Node* n = m_focusNode; n != nullptr; n = n->GetParent())
            {
                if (!n->IsTreeEnabled())
                    return false;
                DragRequest request;
                request.Press = core::Float2{n->GetSize().x * 0.5f, n->GetSize().y * 0.5f};
                request.FromKeyboard = true;
                if (n->HandleDragRequested(request))
                {
                    request.FromKeyboard = true;
                    UseKeyboard();
                    m_dragModifiers = 0;
                    BeginDrag(*n, core::Move(request), FocusPoint());
                    return true;
                }
            }
            return false;
        }
        /// Whether a drag from the keys or the pad is in progress (the drop is queried at the
        /// focus).
        [[nodiscard]] bool IsKeyboardDrag() const noexcept { return m_dragActive && m_dragFromKeyboard; }
        /// Queries the drop again where the pointer is (when the content moved under a still
        /// pointer).
        void RequeryDrag()
        {
            if (m_dragFromKeyboard)
                QueryAtFocus();
            else if (m_dragActive)
                QueryDrop(m_mousePos);
        }

        // === Popups: the top layer ===
        // A popup stays in its owner's subtree, which gives it its styles and its events' route.
        // While open it is promoted into this root's top layer: drawn after the whole tree in
        // opening order and unclipped, hit-tested first, placed against its anchor every layout
        // pass, and in `:entering` each time it opens. Opening an auto popup closes the open popups
        // that do not contain its owner (nesting comes from the tree), and closing a popup closes
        // those it contains. A closed popup leaves the tree; its owner keeps a reference to it.

        /// Opens `popup` as a child of `owner`, attaching it there if it is not already.
        void OpenPopup(Node& owner, Node& popup, PopupOptions options)
        {
            if (FindPopup(popup) >= 0)
                return;
            // A popup opened again while its exit plays is restored, and its exit and pending
            // removal are forgotten.
            for (usize i = m_leaving.Size(); i-- > 0;)
                if (m_leaving[i].Popup.Get() == &popup)
                {
                    const core::RefPtr<Node> keep(&popup);
                    popup.SetInTopLayer(false);
                    popup.RemoveFromParent(); // ends its exit
                    if (Node* home = m_leaving[i].Home.table != nullptr ? m_leaving[i].Home.table->Resolve(m_leaving[i].Home.slot, m_leaving[i].Home.generation) : nullptr)
                        (void)home->AddChildAt(&popup, core::Min(m_leaving[i].HomeIndex, home->ChildCount()));
                    m_leaving.RemoveAt(i);
                }
            if (options.Kind != PopupKind::Manual)
            {
                // An auto popup closes the popups that do not contain its owner (a hint closes the
                // other hints).
                for (usize i = m_popups.Size(); i-- > 0;)
                {
                    const PopupEntry& e = m_popups[i];
                    if (e.Options.Modal || e.Options.Kind == PopupKind::Manual)
                        break; // popups below a dialog stay open
                    const bool closes = options.Kind == PopupKind::Hint ? e.Options.Kind == PopupKind::Hint
                                                                        : !IsInSubtree(&owner, e.Popup.Get());
                    if (closes && i < m_popups.Size())
                        ClosePopupAt(i, PopupCloseReason::Replaced);
                }
            }
            PopupEntry entry;
            if (options.ReturnHome && popup.GetParent() != nullptr)
            {
                entry.Home = popup.GetParent()->TakeElementRef();
                entry.HomeIndex = popup.GetParent()->GetChildIndex(&popup);
            }
            if (popup.GetParent() != &owner)
            {
                // Hold a reference across the move, since the old parent may hold the only one (a
                // dialog written in a page).
                const core::RefPtr<Node> keep(&popup);
                popup.RemoveFromParent();
                owner.AddChild(&popup);
            }
            entry.Popup = core::RefPtr<Node>(&popup);
            entry.Size = popup.GetSize();
            const bool focusing = (options.TakesFocus || options.Modal) && options.Kind != PopupKind::Hint;
            if (focusing)
            {
                // Save the focus the popup takes and the focused node's ancestors. On close, focus
                // goes back to the first of them that is still there.
                for (Node* n = m_focusNode; n != nullptr; n = n->GetParent())
                    entry.SavedFocus.PushBack(n->TakeElementRef());
                entry.ScopeBefore = popup.GetNavScope();
                if (popup.GetNavScope() == NavScope::None)
                    popup.SetNavScope(NavScope::Contain);
            }
            entry.Options = core::Move(options);
            popup.SetInTopLayer(true);
            popup.SetRouteStop(entry.Options.Modal);
            m_popups.PushBack(core::Move(entry));
            PlacePopups();
            popup.Enter(); // plays the sheet's entrance (`:entering`) each time the popup opens
            if (focusing)
            {
                core::Array<Node*> stops;
                CollectTabStops(&popup, stops);
                SetFocusNode(!stops.IsEmpty() ? stops[0] : (popup.IsTabFocusable() ? &popup : nullptr));
            }
            // Sent after focus has moved in, so a listener may move it further (to a dialog's default
            // button).
            ToggleEvent opened(&popup, true, PopupCloseReason::Code);
            (void)popup.RouteEvent(opened);
        }
        /// Closes `popup` and the popups it contains. Returns false when it is not open.
        bool ClosePopup(Node& popup, PopupCloseReason reason = PopupCloseReason::Code)
        {
            const i32 index = FindPopup(popup);
            if (index < 0)
                return false;
            ClosePopupAt(static_cast<usize>(index), reason);
            return true;
        }
        void CloseAllPopups(PopupCloseReason reason = PopupCloseReason::Code)
        {
            while (!m_popups.IsEmpty())
                ClosePopupAt(0, reason);
        }
        /// Called when input went to another root of the GUI (another window). This root's auto
        /// popups and hints close as a press outside them would close them; popups below a dialog
        /// stay open.
        void CloseLightPopups(PopupCloseReason reason = PopupCloseReason::OutsidePress)
        {
            for (usize i = m_popups.Size(); i-- > 0;)
            {
                if (i >= m_popups.Size())
                    continue;
                if (m_popups[i].Options.Kind == PopupKind::Manual)
                    break;
                ClosePopupAt(i, reason);
            }
        }
        [[nodiscard]] bool IsPopupOpen(const Node& popup) const { return FindPopup(popup) >= 0; }
        /// Closes the topmost auto popup, without looking past a dialog (a manual popup handles
        /// `back` itself). Returns false when there is no such popup. The `back` command tries this
        /// first.
        bool CloseTopAutoPopup(PopupCloseReason reason)
        {
            const i32 top = TopAutoPopup();
            if (top < 0)
                return false;
            ClosePopupAt(static_cast<usize>(top), reason);
            return true;
        }
        [[nodiscard]] bool HasTopAutoPopup() const { return TopAutoPopup() >= 0; }
        [[nodiscard]] usize PopupCount() const noexcept { return m_popups.Size(); }
        /// The open popups, bottom first.
        [[nodiscard]] Node* PopupAt(usize index) const { return index < m_popups.Size() ? m_popups[index].Popup.Get() : nullptr; }
        [[nodiscard]] const PopupOptions* PopupOptionsAt(usize index) const
        {
            return index < m_popups.Size() ? &m_popups[index].Options : nullptr;
        }
        [[nodiscard]] Node* TopPopup() const { return m_popups.IsEmpty() ? nullptr : m_popups.Back().Popup.Get(); }

        /// Places every anchored popup against its anchor within the root (on its side, flipped,
        /// shrunk and clamped by PlacePopup), and each centred popup in the middle of the root. Runs
        /// every layout pass and before drawing, so a popup follows its anchor.
        void PlacePopups() const
        {
            if (m_root == nullptr)
                return;
            const Rect bounds{0.0f, 0.0f, m_root->GetSize().x, m_root->GetSize().y};
            for (const PopupEntry& e : m_popups)
            {
                Rect anchor{};
                PopupPlacement placement = e.Options.Placement;
                if (e.Options.Centered)
                {
                    // Anchor to a line across the root at half the room the popup leaves, centred
                    // along it, which puts the popup in the middle both ways.
                    if (e.Options.FitContent)
                        e.Size = e.Popup->Measure(LayoutConstraints{});
                    anchor = Rect{0.0f, core::Max(0.0f, (bounds.height - e.Size.y) * 0.5f), bounds.width, 0.0f};
                    placement.Side = PopupSide::Below;
                    placement.Align = PopupAlign::Center;
                    placement.Gap = 0.0f;
                    placement.Shift = 0.0f;
                }
                else if (e.Options.Anchor != nullptr)
                {
                    const Node& a = *e.Options.Anchor;
                    const core::Float2 p0 = a.ConvertToRootSpace(core::Float2{0.0f, 0.0f});
                    const core::Float2 p1 = a.ConvertToRootSpace(a.GetSize());
                    anchor = Rect{core::Min(p0.x, p1.x), core::Min(p0.y, p1.y), core::Abs(p1.x - p0.x), core::Abs(p1.y - p0.y)};
                }
                else if (e.Options.HasAnchorRect)
                    anchor = e.Options.AnchorRect;
                else
                    continue;
                Node& popup = *e.Popup;
                if (e.Options.FitContent && !e.Options.Centered)
                    e.Size = popup.Measure(LayoutConstraints{});
                // Use the anchor's writing direction for sides and alignment, else the popup's own
                // (inherited from its owner).
                const bool rightToLeft = e.Options.Anchor != nullptr && !e.Options.Centered ? e.Options.Anchor->IsRightToLeft()
                                                                                            : popup.IsRightToLeft();
                if (placement.AlignContent)
                {
                    // Measure from the content's edge, using the padding and border as styled now
                    // rather than when the popup opened.
                    const Thickness p = popup.GetPadding();
                    const Thickness b = popup.GetBorderThickness();
                    const bool along = placement.Side == PopupSide::Below || placement.Side == PopupSide::Above;
                    placement.Shift -= along ? (rightToLeft ? p.Right + b.Right : p.Left + b.Left) : p.Top + b.Top;
                }
                PlacedPopup placed = PlacePopup(anchor, e.Size, bounds, placement, rightToLeft);
                // Snap to the device grid as arrange does, so the two agree: the near edge rounds
                // down and the far edge up, so snapping never makes the popup smaller than it
                // measured (a menu a fraction too short for its rows would scroll).
                const f32 scale = core::Max(popup.GetPixelScale(), 0.01f);
                const auto near = [scale](f32 v) { return core::Floor(v * scale + 0.001f) / scale; };
                const auto far = [scale](f32 v) { return core::Ceil(v * scale - 0.001f) / scale; };
                placed.Size = core::Float2{far(placed.Position.x + placed.Size.x) - near(placed.Position.x),
                                           far(placed.Position.y + placed.Size.y) - near(placed.Position.y)};
                placed.Position = core::Float2{near(placed.Position.x), near(placed.Position.y)};
                if (e.Options.FitContent)
                {
                    // Sized to its content: arrange it but leave its style size auto, so the next
                    // measure follows its content (a tooltip whose font arrives after it opened).
                    popup.Arrange(Rect{placed.Position.x, placed.Position.y, placed.Size.x, placed.Size.y});
                    continue;
                }
                Node::StyleWriteScope writing(popup); // a write by the top layer, not by code, so it pins nothing
                if (placed.Position.x != popup.GetPosition().x || placed.Position.y != popup.GetPosition().y)
                    popup.SetPosition(placed.Position);
                if (placed.Size.x != popup.GetSize().x || placed.Size.y != popup.GetSize().y)
                    popup.SetSize(placed.Size);
            }
        }
        /// Sets an open popup's size from code (a list grew) and places it again from that size.
        void SetPopupSize(Node& popup, core::Float2 size)
        {
            const i32 index = FindPopup(popup);
            if (index < 0)
            {
                popup.SetSize(size);
                return;
            }
            m_popups[static_cast<usize>(index)].Size = size;
            popup.SetSize(size);
            PlacePopups();
        }
        /// Moves the rectangle (in root space) that an open popup is placed against (a code
        /// editor's caret, after scrolling or typing). Returns false when the popup is not open.
        bool SetPopupAnchorRect(const Node& popup, Rect anchor)
        {
            const i32 index = FindPopup(popup);
            if (index < 0)
                return false;
            PopupOptions& options = m_popups[static_cast<usize>(index)].Options;
            options.Anchor = nullptr;
            options.AnchorRect = anchor;
            options.HasAnchorRect = true;
            PlacePopups();
            return true;
        }

        /// Draws the top layer; the root calls this after drawing its tree. Popups are drawn in
        /// opening order, and a modal one over its backdrop (its `::backdrop` part, over a dimming
        /// of the whole root).
        void DrawPopups(DrawContext& ctx)
        {
            PlacePopups();
            // Draw the popups playing their exit first, under the open ones (a dialog fading out
            // under a menu opened since).
            for (const LeavingPopup& leaving : m_leaving)
            {
                const core::RefPtr<Node> popup = leaving.Popup;
                if (popup->GetParent() == nullptr)
                    continue;
                if (leaving.Modal && m_root != nullptr)
                    popup->DrawBackdrop(ctx, Rect{0.0f, 0.0f, m_root->GetSize().x, m_root->GetSize().y});
                popup->Draw(ctx);
            }
            for (usize i = 0; i < m_popups.Size(); ++i)
            {
                const core::RefPtr<Node> popup = m_popups[i].Popup; // held in case drawing changes the tree, though it should not
                if (m_popups[i].Options.Modal && m_root != nullptr)
                {
                    const Rect all{0.0f, 0.0f, m_root->GetSize().x, m_root->GetSize().y};
                    popup->DrawBackdrop(ctx, all);
                }
                popup->Draw(ctx);
            }
        }
        /// Whether `node` takes input now: every node while no modal popup is open, else only nodes
        /// in the topmost modal popup or in popups above it.
        [[nodiscard]] bool IsInputLive(const Node* node) const { return IsLive(node); }
        /// The node that focus, Tab and the arrows stay inside: the topmost popup that takes focus,
        /// else the root.
        [[nodiscard]] Node* GetFocusScopeRoot() const { return FocusScopeRoot(); }

        /// Returns the node at a world point in the topmost popup under it (hints pass the pointer
        /// through). Returns null when the point is in no popup.
        [[nodiscard]] Node* FindInPopups(core::Float2 worldPoint) const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if (m_popups[i].Options.Kind != PopupKind::Hint && !m_popups[i].Options.PassesPointer)
                    if (Node* hit = m_popups[i].Popup->OverFind(worldPoint))
                        return hit;
            return nullptr;
        }

        // === Tab navigation ===
        // Moves focus to the next or previous tab-focusable node in tree pre-order (visible and
        // enabled), wrapping around. Returns true if focus moved. With nothing focused, Next
        // targets the first stop and Previous the last.
        bool FocusNext()
        {
            UseKeyboard();
            return MoveTabFocus(+1);
        }
        bool FocusPrevious()
        {
            UseKeyboard();
            return MoveTabFocus(-1);
        }

        // === A held direction ===
        // A gamepad's d-pad or stick held in a direction steps once at once, then repeatedly after
        // a delay, faster the longer it is held. Each step reaches the focused widget as an arrow
        // key does, so a slider changes its value and a list its selection; only a step the widget
        // does not take moves focus. A held keyboard arrow repeats through the system's own key
        // repeat instead. The GUI's update (GUIContext::Update) ticks this.
        void PressDirection(NavDirection direction)
        {
            if (PadCaptured())
                return;
            NoteInput();
            ApplyInputKind(InputKind::Pad);
            m_holding = true;
            m_heldDirection = direction;
            m_heldFor = 0.0f;
            m_nextRepeatAt = kRepeatDelay;
            StepDirection(direction);
        }
        void ReleaseDirection(NavDirection direction)
        {
            if (m_holding && m_heldDirection == direction)
                m_holding = false;
        }
        [[nodiscard]] bool IsHoldingDirection() const noexcept { return m_holding; }
        /// Sets the pad's right stick, each axis -1 to 1 (down and right positive). While it is
        /// pushed past its dead zone, Tick scrolls the nearest scroller around the focus (or under
        /// the pointer when nothing is focused), faster the further it is pushed.
        void SetScrollStick(core::Float2 value) noexcept { m_scrollStick = PadCaptured() ? core::Float2{0.0f, 0.0f} : value; }
        /// Whether a focus that captures every key (a game in a panel) also has the gamepad. It
        /// reads the pad itself, and meanwhile the GUI does not move focus, activate or scroll by
        /// the pad.
        [[nodiscard]] bool PadCaptured() const noexcept { return m_focusNode != nullptr && m_focusNode->GetCapturesAllKeys(); }
        [[nodiscard]] core::Float2 GetScrollStick() const noexcept { return m_scrollStick; }
        void Tick(f32 seconds)
        {
            m_time += seconds; // read by click counting, tooltips and long presses
            SettleLeavingPopups();
            DropHiddenFocus();
            if (m_dragActive)
                TickDrag(seconds);
            UpdateTooltip(seconds);
            ScrollByStick(seconds);
            if (m_longPressArmed && m_touchActive)
            {
                m_pressHeld += seconds;
                if (m_pressHeld >= kLongPress)
                    LongPress();
            }
            if (m_holding && PadCaptured())
                m_holding = false; // the game took the focus while held, so the GUI takes no further steps
            if (!m_holding)
                return;
            m_heldFor += seconds;
            for (i32 guard = 0; guard < 8 && m_heldFor >= m_nextRepeatAt; ++guard)
            {
                StepDirection(m_heldDirection);
                // Ramp from the first interval down to the fastest over the time held past the
                // delay.
                const f32 ramp = core::Min(1.0f, (m_heldFor - kRepeatDelay) / kRepeatRampTime);
                m_nextRepeatAt += kRepeatSlowest + (kRepeatFastest - kRepeatSlowest) * ramp;
            }
        }
        static constexpr f32 kRepeatDelay = 0.4f;    // before the first repeat
        static constexpr f32 kRepeatSlowest = 0.16f; // interval of the first repeats
        static constexpr f32 kRepeatFastest = 0.05f; // interval once held for kRepeatRampTime
        static constexpr f32 kRepeatRampTime = 1.2f; // from slowest to fastest

        // One step of a held direction, handled as an arrow key: the focused widget gets it first
        // (a slider steps its value, a list its selection), and navigation gets it if the widget
        // does not take it.
        void StepDirection(NavDirection direction)
        {
            UseKeyboard();
            const KeyCode key = direction == NavDirection::Up     ? KeyCode::Up
                                : direction == NavDirection::Down ? KeyCode::Down
                                : direction == NavDirection::Left ? KeyCode::Left
                                                                  : KeyCode::Right;
            if (!RouteKey(EventType::KeyDown, static_cast<u32>(key), 0))
                (void)MoveFocus(direction);
        }


        // A subtree is leaving the tree (Node::RemoveChild tells the dispatcher before it
        // detaches): every ref into it is dropped, since the tree no longer keeps those nodes
        // alive. A sort that rebuilds a table's header frees the hovered header cell, and the next
        // mouse move would otherwise read a freed node. A popup inside it is simply gone; a popup
        // whose opener leaves is dismissed while the opener still lives.
        void NotifySubtreeRemoved(Node& removed)
        {
            // Drop the popups leaving with the subtree (their owner went), topmost first, and give
            // focus back to the node they took it from when that node stays.
            for (usize i = m_popups.Size(); i-- > 0;)
                if (i < m_popups.Size() && IsInSubtree(m_popups[i].Popup.Get(), &removed))
                    DropPopupAt(i, PopupCloseReason::OwnerRemoved, &removed);
            if (IsInSubtree(m_tipProvider, &removed))
                m_tipProvider = nullptr;
            PrunePath(m_hoverPath, removed, /*hover*/ true);
            m_overNode = m_hoverPath.IsEmpty() ? nullptr : m_hoverPath[m_hoverPath.Size() - 1];
            PrunePath(m_pressPath, removed, /*hover*/ false);
            if (IsInSubtree(m_downNode, &removed))
                m_downNode = nullptr;
            if (IsInSubtree(m_panScroller, &removed))
                m_panScroller = nullptr; // the finger no longer pans anything
            if (IsInSubtree(m_captureNode, &removed))
            {
                Node* lost = m_captureNode;
                m_captureNode = nullptr;
                SendCaptureLost(*lost); // sent while the node still lives
            }
            if (IsInSubtree(m_focusNode, &removed))
                SetFocusNode(nullptr);
            if (IsInSubtree(m_dropTarget, &removed))
                m_dropTarget = nullptr;
            if (IsInSubtree(m_dragSource, &removed))
            {
                m_dragSource = nullptr; // the source is gone, so it is not told
                CancelDrag();
            }
        }

    private:
        [[nodiscard]] Node* HitTest(core::Float2 position) const
        {
            // Input must see the current layout, so a layout pass still waiting for the frame runs
            // first (nothing happens when none is waiting).
            for (i32 round = 0; m_root != nullptr && round < 8 && (m_root->IsLayoutDirty() || m_root->HasLayoutDirtyDescendant());
                 ++round)
                m_root->ArrangeAgain();
            PlacePopups();
            Node* hit = FindInPopups(position);
            if (hit == nullptr && m_root != nullptr)
                hit = m_root->OverFind(position);
            // Below the topmost modal popup everything is inert, so a hit there is swallowed.
            if (!IsLive(hit))
                return nullptr;
            return hit;
        }

        bool PointerMove(core::Float2 position, PointerType pointer, u64 id, u32 modifiers = 0)
        {
            m_mousePos = position;
            if (m_longPressArmed && core::Abs(position.x - m_pressAt.x) + core::Abs(position.y - m_pressAt.y) > Dips(kTapSlop))
                m_longPressArmed = false; // a moving finger is a drag or a scroll, not a long press
            // During a drag, a pointer move only queries where a drop would land.
            if (m_dragActive)
            {
                m_dragModifiers = modifiers;
                QueryDrop(position);
                return true;
            }
            // A press that moved past the threshold starts a drag, if the pressed node or an ancestor
            // offers one.
            if (m_downNode != nullptr && !m_dragTried && (pointer == PointerType::Mouse || m_touchDragArmed) &&
                core::Abs(position.x - m_pressAt.x) + core::Abs(position.y - m_pressAt.y) > Dips(m_dragThreshold))
                if (TryDrag(position, modifiers, pointer))
                    return true;
            Node* hit = HitTest(position);
            UpdateHoverPath(hit, position);
            // The captured node (explicitly, or the pressed one while a button is held) keeps the
            // moves, so a drag goes on when the pointer leaves it; otherwise the hovered node.
            Node* target = PointerTarget(hit);
            bool used = false;
            if (target != nullptr && target->IsTreeEnabled())
            {
                MouseEvent event(EventType::MouseMove, target, position, MouseButton::Left, modifiers);
                event.Pointer = pointer;
                event.PointerId = id;
                used = target->RouteEvent(event);
            }
            return used || IsPointerTaken(hit);
        }

        bool PointerDown(core::Float2 position, MouseButton button, u32 modifiers, PointerType pointer, u64 id)
        {
            NoteInput();
            // The pointer is in use, so the focus ring hides until the next key or d-pad move.
            ApplyInputKind(InputKind::Pointer);
            m_keysMovedFocus = false; // the focused control's tooltip waits for the keys again
            m_mousePos = position;
            if (m_dragActive)
            {
                // A second press during a drag (a right press) cancels the drag and is consumed.
                CancelDrag();
                m_swallowing = true;
                return true;
            }
            m_dragTried = button != MouseButton::Left; // only a left press (a finger's) drags
            m_touchDragArmed = false;
            Node* hit = HitTest(position);
            UpdateHoverPath(hit, position);
            // Any press hides a tooltip. A press also closes the auto popups above the one it lands
            // in. A press that lands in no popup and closes some is consumed with the rest of its
            // gesture, so closing a menu never acts on what lies under it, and pressing the opener
            // again closes the menu without reopening it.
            ClosePopupsOfKind(PopupKind::Hint, PopupCloseReason::OutsidePress);
            if (DismissOutside(hit))
            {
                m_swallowing = true;
                m_downNode = nullptr;
                return true;
            }
            CountClick(position, button, pointer);
            m_longPressArmed = pointer == PointerType::Touch && hit != nullptr;
            m_pressHeld = 0.0f;
            m_pressAt = position;
            m_downNode = hit;
            m_clickSuppressed = false;
            SetPressPath(hit);
            // Click-to-focus: focus the nearest widget that takes focus, at or above the hit node.
            // If there is none (a click on a backdrop or a label), clear focus as browsers do, so
            // navigation starts again from the GUI's entry rather than from the backdrop. If a node
            // that keeps the focus is met first (a completion list's row), focus stays where it is.
            Node* focusable = hit;
            bool keep = false;
            while (focusable != nullptr && !(focusable->IsTabFocusable() && focusable->IsTreeEnabled()))
            {
                if (focusable->GetKeepsFocus())
                {
                    keep = true;
                    break;
                }
                focusable = focusable->GetParent();
            }
            if (!keep)
                SetFocusNode(focusable);
            // A disabled widget still takes the press, so nothing behind it gets it, but it is not
            // told about it.
            bool used = false;
            if (hit != nullptr && hit->IsTreeEnabled())
            {
                MouseEvent event(EventType::MouseDown, hit, position, button, modifiers);
                event.ClickCount = m_clickCount;
                event.Pointer = pointer;
                event.PointerId = id;
                used = hit->RouteEvent(event);
            }
            return used || IsPointerTaken(hit);
        }

        bool PointerUp(core::Float2 position, MouseButton button, u32 modifiers, PointerType pointer, u64 id)
        {
            m_mousePos = position;
            m_longPressArmed = false;
            if (m_swallowing)
            {
                m_swallowing = false; // the release of a press that closed a popup is consumed too
                return true;
            }
            // A release during a drag queries the target once more where it is, drops on it if it
            // accepts, and ends the drag. The press was cancelled when the drag began.
            if (m_dragActive)
            {
                m_dragModifiers = modifiers;
                QueryDrop(position);
                Drop();
                return true;
            }
            Node* hit = HitTest(position);
            // Both nodes are pinned for the rest of this call: a handler may mutate the tree
            // (e.g. a header click re-sorts a model and the view rebuilds its header cells),
            // dropping the last owning reference to the very node still on the call stack.
            const core::RefPtr<Node> hitPin{hit};
            Node* target = PointerTarget(hit); // the captured or pressed node gets the release
            const core::RefPtr<Node> targetPin{target};
            const core::RefPtr<Node> downPin{m_downNode};
            bool used = false;
            if (target != nullptr && target->IsTreeEnabled())
            {
                MouseEvent event(EventType::MouseUp, target, position, button, modifiers);
                event.ClickCount = m_clickCount;
                event.Pointer = pointer;
                event.PointerId = id;
                used = target->RouteEvent(event);
            }
            // A right release asks for a context menu at its position, from the node there up through
            // its ancestors.
            if (button == MouseButton::Right && hit != nullptr && hit->IsTreeEnabled() && IsInSubtree(hit, m_root))
            {
                MouseEvent menu(EventType::ContextMenu, hit, position, button, modifiers);
                menu.Pointer = pointer;
                used = hit->RouteEvent(menu) || used;
            }
            // A click (activation) happens for the left button only, on the nearest common ancestor
            // of the pressed node and the node released over, as in the DOM: a press dragged onto a
            // sibling clicks their container, not either sibling. Right and middle presses still
            // deliver down and up (for a context menu) but never activate a control. A cancelled
            // press has no pressed node, so it makes no click.
            if (button == MouseButton::Left && m_downNode != nullptr && hit != nullptr && !m_clickSuppressed)
                if (Node* clicked = CommonAncestor(m_downNode, hit); clicked != nullptr && clicked->IsTreeEnabled())
                {
                    MouseEvent event(EventType::MouseClick, clicked, position, button, modifiers);
                    event.ClickCount = m_clickCount;
                    event.Pointer = pointer;
                    event.PointerId = id;
                    used = clicked->RouteEvent(event) || used;
                }
            m_downNode = nullptr;
            ClearPressPath();
            if (m_captureNode != nullptr)
                ReleasePointer(*m_captureNode); // a capture ends with the press, as in the DOM
            return used || IsPointerTaken(hit);
        }

        // The node a pointer move or release goes to: the captured node, else the pressed one
        // while a button is held, else the one under the pointer.
        [[nodiscard]] Node* PointerTarget(Node* hit) const
        {
            return m_captureNode != nullptr ? m_captureNode : m_downNode != nullptr ? m_downNode : hit;
        }
        // Whether `hit`, or a node above it short of the root, takes the pointer.
        [[nodiscard]] bool IsPointerTaken(Node* hit) const
        {
            for (Node* n = hit; n != nullptr && n != m_root; n = n->GetParent())
                if (n->TakesPointer())
                    return true;
            return false;
        }
        // Keys go to the focused widget, or to the root when nothing is focused (so its capture and
        // bubble listeners still hear them). A disabled target gets nothing.
        [[nodiscard]] Node* KeyTarget() const
        {
            Node* target = m_focusNode != nullptr ? m_focusNode : m_root;
            if (!IsLive(target))
                target = FocusScopeRoot(); // below a modal, the keys go to the modal
            return target != nullptr && target->IsTreeEnabled() ? target : nullptr;
        }
        bool RouteKey(EventType type, u32 keyCode, u32 modifiers)
        {
            (void)HitTest(core::Float2{}); // keys see the current layout, as the pointer does
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            KeyEvent event(type, target, keyCode, modifiers);
            return target->RouteEvent(event);
        }
        // The nearest common ancestor of both (a node counts as its own ancestor); null when they
        // are in different trees.
        [[nodiscard]] static Node* CommonAncestor(Node* a, Node* b)
        {
            for (Node* n = a; n != nullptr; n = n->GetParent())
                if (IsInSubtree(b, n))
                    return n;
            return nullptr;
        }
        // The hover path follows the node under the pointer. The nodes that leave it are told
        // (deepest first), then the nodes that join it (outermost first), each individually. Every
        // node on the path reads as hovered, so `:hover` holds for a card while a button inside it
        // is under the pointer, as in CSS.
        void UpdateHoverPath(Node* hit, core::Float2 position)
        {
            Array<Node*> path;
            for (Node* n = hit; n != nullptr; n = n->GetParent())
                path.PushBack(n);
            for (usize i = 0, j = path.Size(); i + 1 < j; ++i, --j)
                core::Swap(path[i], path[j - 1]); // root first
            usize common = 0;
            while (common < m_hoverPath.Size() && common < path.Size() && m_hoverPath[common] == path[common])
                ++common;
            if (common == m_hoverPath.Size() && common == path.Size())
                return;
            // Hold references while the handlers run, since an enter or a leave may change the tree.
            Array<core::RefPtr<Node>> leaving;
            for (usize i = m_hoverPath.Size(); i-- > common;)
                leaving.PushBack(core::RefPtr<Node>(m_hoverPath[i]));
            Array<core::RefPtr<Node>> joining;
            for (usize i = common; i < path.Size(); ++i)
                joining.PushBack(core::RefPtr<Node>(path[i]));
            m_hoverPath = core::Move(path);
            m_overNode = hit;
            for (const core::RefPtr<Node>& node : leaving)
            {
                node->SetHovered(false);
                MouseEvent event(EventType::MouseLeave, node.Get(), position);
                node->DeliverRouted(event);
            }
            for (const core::RefPtr<Node>& node : joining)
            {
                node->SetHovered(true);
                MouseEvent event(EventType::MouseEnter, node.Get(), position);
                node->DeliverRouted(event);
            }
        }
        // The press path: the pressed node and its ancestors read as pressed (`:active`) until the
        // release.
        void SetPressPath(Node* hit)
        {
            ClearPressPath();
            if (hit == nullptr || !hit->IsTreeEnabled())
                return;
            for (Node* n = hit; n != nullptr; n = n->GetParent())
            {
                n->SetPressed(true);
                m_pressPath.PushBack(n);
            }
        }
        void ClearPressPath()
        {
            Array<Node*> path = core::Move(m_pressPath);
            m_pressPath = Array<Node*>{};
            for (Node* n : path)
                n->SetPressed(false);
        }
        // Drops the part of a hover or press path inside a subtree that is leaving the tree.
        static void PrunePath(Array<Node*>& path, Node& removed, bool hover)
        {
            for (usize i = 0; i < path.Size(); ++i)
                if (IsInSubtree(path[i], &removed))
                {
                    for (usize j = i; j < path.Size(); ++j)
                        hover ? path[j]->SetHovered(false) : path[j]->SetPressed(false);
                    while (path.Size() > i)
                        path.PopBack();
                    return;
                }
        }
        void SendCaptureLost(Node& node)
        {
            const core::RefPtr<Node> pin{&node};
            Event event(EventType::PointerCaptureLost, &node);
            node.DeliverRouted(event);
        }
        // === Gestures === Two fingers down over a node whose touch-action excludes pinch-zoom (the
        // node itself, or the common ancestor of both fingers' nodes) make one gesture from the
        // moment the second finger lands. The first finger's press is cancelled, and the node
        // receives gesturestart, then gesturechange as the fingers move (scale, rotation and
        // translation together, as in Safari's gesture events), and gestureend when a finger lifts.
        // Over a node that leaves pinch-zoom to the GUI, a second finger does nothing.
        bool BeginGesture()
        {
            if (m_gesturing || m_fingers.Size() != 2)
                return false;
            Node* target = CommonAncestor(HitTest(m_fingers[0].Now), HitTest(m_fingers[1].Now));
            if (target == nullptr || (target->EffectiveTouchActions() & kTouchPinchZoom) != 0)
                return false;
            if (m_touchActive)
            {
                CancelPointer(); // cancel the first finger's press: this is a gesture, not a tap
                m_longPressArmed = false;
                m_touchDragArmed = false;
                m_panning = false;
                m_panScroller = nullptr;
                m_touchActive = false;
                UpdateHoverPath(nullptr, m_fingers[0].Now);
            }
            m_gestureIds[0] = m_fingers[0].Id;
            m_gestureIds[1] = m_fingers[1].Id;
            const core::Float2 a = m_fingers[0].Now, b = m_fingers[1].Now;
            m_gestureSpan0 = core::Max(core::Length(core::Float2{b.x - a.x, b.y - a.y}), 1.0f);
            m_gestureAngle = core::Atan2(b.y - a.y, b.x - a.x);
            m_gestureFocus0 = core::Float2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            m_gestureScale = 1.0f;
            m_gestureRotation = 0.0f;
            m_gestureTranslation = core::Float2{0.0f, 0.0f};
            m_gesturePointer = PointerType::Touch;
            m_gestureTarget = core::RefPtr<Node>(target);
            m_gesturing = true;
            RouteGesture(EventType::GestureStart, 1.0f, 0.0f, core::Float2{0.0f, 0.0f});
            return true;
        }
        [[nodiscard]] bool IsGestureFinger(u64 id) const noexcept
        {
            return m_gesturePointer == PointerType::Touch && (id == m_gestureIds[0] || id == m_gestureIds[1]);
        }
        void MoveGesture()
        {
            const i32 ia = FindFinger(m_gestureIds[0]), ib = FindFinger(m_gestureIds[1]);
            if (ia < 0 || ib < 0)
                return;
            if (m_gestureTarget->GetRootNode() != m_root)
            {
                EndGesture(EventType::GestureCancel); // the gesture's node left the tree
                return;
            }
            const core::Float2 a = m_fingers[static_cast<usize>(ia)].Now, b = m_fingers[static_cast<usize>(ib)].Now;
            const f32 scale = core::Max(core::Length(core::Float2{b.x - a.x, b.y - a.y}), 1.0f) / m_gestureSpan0;
            const f32 angle = core::Atan2(b.y - a.y, b.x - a.x);
            f32 turned = angle - m_gestureAngle; // take the shorter way round: past a half turn it wraps
            if (turned > core::kPi)
                turned -= 2.0f * core::kPi;
            else if (turned < -core::kPi)
                turned += 2.0f * core::kPi;
            m_gestureAngle = angle;
            const core::Float2 focus{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            // In the GUI's units, as positions are, not in the node's own units, which its gesture
            // may rotate.
            const core::Float2 translation = InNodeUnits(*m_root, m_gestureFocus0,
                                                         core::Float2{focus.x - m_gestureFocus0.x, focus.y - m_gestureFocus0.y});
            const f32 scaleDelta = scale / m_gestureScale;
            const core::Float2 translationDelta{translation.x - m_gestureTranslation.x, translation.y - m_gestureTranslation.y};
            m_gestureScale = scale;
            m_gestureRotation += turned;
            m_gestureTranslation = translation;
            RouteGesture(EventType::GestureChange, scaleDelta, turned, translationDelta);
        }
        void EndGesture(EventType type)
        {
            m_gesturing = false;
            const core::RefPtr<Node> target = core::Move(m_gestureTarget);
            if (target && target->GetRootNode() == m_root)
            {
                GestureEvent event(type, target.Get());
                event.Pointer = m_gesturePointer;
                event.Focus = GestureFocus();
                event.Scale = m_gestureScale;
                event.Rotation = m_gestureRotation;
                event.Translation = m_gestureTranslation;
                (void)target->RouteEvent(event);
            }
        }
        [[nodiscard]] core::Float2 GestureFocus() const
        {
            const i32 ia = FindFinger(m_gestureIds[0]), ib = FindFinger(m_gestureIds[1]);
            if (ia < 0 || ib < 0)
                return m_gestureFocus0;
            const core::Float2 a = m_fingers[static_cast<usize>(ia)].Now, b = m_fingers[static_cast<usize>(ib)].Now;
            return core::Float2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        }
        void RouteGesture(EventType type, f32 scaleDelta, f32 rotationDelta, core::Float2 translationDelta)
        {
            GestureEvent event(type, m_gestureTarget.Get());
            event.Pointer = m_gesturePointer;
            event.Focus = GestureFocus();
            event.Scale = m_gestureScale;
            event.Rotation = m_gestureRotation;
            event.Translation = m_gestureTranslation;
            event.ScaleDelta = scaleDelta;
            event.RotationDelta = rotationDelta;
            event.TranslationDelta = translationDelta;
            (void)m_gestureTarget->RouteEvent(event);
        }

        /// Converts `dips` to the device pixels that positions arrive in, by the root's display
        /// scale.
        [[nodiscard]] f32 Dips(f32 dips) const noexcept { return dips * (m_root != nullptr ? m_root->GetPixelScale() : 1.0f); }
        // A second press of the same button close in time and place is a double click, and a third
        // is a triple click (positions are in device pixels).
        void CountClick(core::Float2 position, MouseButton button, PointerType pointer)
        {
            const f32 dx = position.x - m_lastPressAt.x;
            const f32 dy = position.y - m_lastPressAt.y;
            const f32 slop = Dips(pointer == PointerType::Touch ? kTapSlop : kClickSlop); // a finger wobbles more
            const bool near = dx * dx + dy * dy <= slop * slop;
            const bool soon = m_time - m_lastPressTime <= kClickInterval;
            m_clickCount = (button == m_lastPressButton && near && soon && m_clickCount > 0) ? m_clickCount + 1 : 1;
            m_lastPressTime = m_time;
            m_lastPressAt = position;
            m_lastPressButton = button;
        }
        static constexpr f32 kClickInterval = 0.45f; // seconds between the presses of a double click
        // Lengths in DIPs, matching what a hand does, scaled by the display (Dips) to the device
        // pixels that positions arrive in (Android's touch slop is 8dp, scaled the same way).
        static constexpr f32 kClickSlop = 6.0f;      // between the presses of a double click
        static constexpr f32 kTapSlop = 24.0f;       // between two taps
        static constexpr f32 kStickDeadZone = 0.2f;  // a stick at rest wobbles within this
        static constexpr f32 kStickSpeed = 1400.0f;  // pixels a second, pushed all the way

        // Scrolls by the right stick. Past the dead zone, the deflection is rescaled from the dead
        // zone and squared (fine near the centre, fast at the rim), and each axis moves the nearest
        // scroller that scrolls that way.
        void ScrollByStick(f32 seconds)
        {
            const auto shape = [](f32 v)
            {
                const f32 a = core::Abs(v);
                if (a <= kStickDeadZone)
                    return 0.0f;
                const f32 t = core::Min(1.0f, (a - kStickDeadZone) / (1.0f - kStickDeadZone));
                return (v < 0.0f ? -t : t) * t;
            };
            const core::Float2 push{shape(m_scrollStick.x), shape(m_scrollStick.y)};
            if (push.x == 0.0f && push.y == 0.0f)
                return;
            Node* from = m_focusNode != nullptr ? m_focusNode : (m_hoverPath.IsEmpty() ? nullptr : m_hoverPath[m_hoverPath.Size() - 1]);
            for (u32 axis = 0; axis < 2; ++axis)
            {
                const f32 p = axis == 0 ? push.x : push.y;
                if (p == 0.0f)
                    continue;
                for (Node* n = from; n != nullptr; n = n->GetParent())
                {
                    if (!n->IsScrollContainer() || (axis == 0 ? n->GetScrollRange().x : n->GetScrollRange().y) <= 0.0f)
                        continue;
                    const f32 by = p * kStickSpeed * seconds;
                    n->ScrollBy(axis == 0 ? core::Float2{by, 0.0f} : core::Float2{0.0f, by});
                    break;
                }
            }
        }
        static constexpr f32 kPanSlop = 8.0f;        // DIPs a finger must move to pan instead of tap
        static constexpr f32 kFlingMinSpeed = 60.0f; // DIPs a second; a slower release just stops
        static constexpr f32 kPanWindow = 0.1f;      // seconds of the finger's path its speed is measured over

        // Converts a move of `delta` at world point `at` into `node`'s own units. A finger moves in
        // the window's pixels, while a scroller scrolls in its logical units, which differ under a
        // display scale or a transform.
        [[nodiscard]] static core::Float2 InNodeUnits(const Node& node, core::Float2 at, core::Float2 delta)
        {
            const core::Float2 from = node.ConvertToNodeSpace(at);
            const core::Float2 to = node.ConvertToNodeSpace(core::Float2{at.x + delta.x, at.y + delta.y});
            return core::Float2{to.x - from.x, to.y - from.y};
        }
        // The nearest scroller at or above where the finger went down that can move along the
        // finger's main direction (the larger axis of `moved`).
        [[nodiscard]] Node* PanScrollerFor(core::Float2 moved) const
        {
            const bool across = core::Abs(moved.x) > core::Abs(moved.y);
            for (Node* n = HitTest(m_pressAt); n != nullptr; n = n->GetParent())
            {
                if (!n->IsScrollContainer())
                    continue;
                const core::Float2 range = n->GetScrollRange();
                if ((across ? range.x : range.y) > 0.0f)
                    return n;
            }
            return nullptr;
        }
        void AddPanSample(core::Float2 position)
        {
            // A ring of the last few points on the dispatcher's clock.
            m_panSamples[m_panSampleCount % kPanSamples] = PanSample{m_time, position};
            ++m_panSampleCount;
        }
        // The finger's speed over the last kPanWindow, from the oldest sample in that window to the
        // latest.
        [[nodiscard]] core::Float2 PanVelocity() const
        {
            if (m_panSampleCount < 2)
                return core::Float2{0.0f, 0.0f};
            const PanSample& last = m_panSamples[(m_panSampleCount - 1) % kPanSamples];
            const PanSample* first = &last;
            const usize kept = core::Min<usize>(m_panSampleCount, kPanSamples);
            for (usize i = 1; i < kept; ++i)
            {
                const PanSample& s = m_panSamples[(m_panSampleCount - 1 - i) % kPanSamples];
                if (last.Time - s.Time > kPanWindow)
                    break;
                first = &s;
            }
            const f32 dt = last.Time - first->Time;
            if (dt <= 0.004f)
                return core::Float2{0.0f, 0.0f};
            return core::Float2{(last.Position.x - first->Position.x) / dt, (last.Position.y - first->Position.y) / dt};
        }

        // True if `node` is `ancestor` or a descendant of it.
        [[nodiscard]] static bool IsInSubtree(const Node* node, const Node* ancestor)
        {
            if (ancestor == nullptr)
                return false;
            for (const Node* n = node; n != nullptr; n = n->GetParent())
                if (n == ancestor)
                    return true;
            return false;
        }

        // === Drag and drop internals ===
        // The effect the modifiers choose among those the source allows (Ctrl copies, Shift moves,
        // both link), else the first of move, copy or link that the source allows.
        [[nodiscard]] DropEffect PreferredEffect() const
        {
            const bool ctrl = (m_dragModifiers & KeyModCtrl) != 0;
            const bool shift = (m_dragModifiers & KeyModShift) != 0;
            const DropEffect asked = ctrl && shift ? DropEffect::Link : ctrl ? DropEffect::Copy : shift ? DropEffect::Move : DropEffect::None;
            if (asked != DropEffect::None && (m_dragPayload.Allowed & DropEffectFlag(asked)) != 0)
                return asked;
            const DropEffect order[] = {DropEffect::Move, DropEffect::Copy, DropEffect::Link};
            for (DropEffect e : order)
                if ((m_dragPayload.Allowed & DropEffectFlag(e)) != 0)
                    return e;
            return DropEffect::None;
        }

        // Whether a press on `node` would offer a drag that some source fills in. It asks without
        // starting the drag, so a source's answer must not change anything.
        [[nodiscard]] bool HasDragSource(Node* node) const
        {
            for (Node* n = node; n != nullptr; n = n->GetParent())
            {
                const core::Float2 local = n->ConvertToNodeSpace(m_pressAt);
                if (n->OwnsPressDrag(local))
                    return false;
                DragRequest request;
                request.Press = local;
                request.Pointer = PointerType::Touch;
                if (n->HandleDragRequested(request))
                    return true;
            }
            return false;
        }

        // Called for a press that moved past the threshold: offers the drag to the pressed node and
        // its ancestors.
        bool TryDrag(core::Float2 position, u32 modifiers, PointerType pointer)
        {
            m_dragTried = true;
            for (Node* n = m_downNode; n != nullptr; n = n->GetParent())
            {
                if (!n->IsTreeEnabled())
                    return false;
                const core::Float2 local = n->ConvertToNodeSpace(m_pressAt);
                if (n->OwnsPressDrag(local))
                    return false; // the node owns its press (a slider, a scroll bar, a field)
                DragRequest request;
                request.Press = local;
                request.Modifiers = modifiers;
                request.Pointer = pointer;
                if (n->HandleDragRequested(request))
                {
                    m_dragModifiers = modifiers;
                    BeginDrag(*n, core::Move(request), position);
                    return true;
                }
            }
            return false;
        }

        void BeginDrag(Node& source, DragRequest request, core::Float2 at)
        {
            if (m_dragActive)
                CancelDrag();
            const core::RefPtr<Node> pin{&source};
            CancelPointer(); // the press now belongs to the drag, so no release or click follows
            HideTooltip();
            m_dragActive = true;
            m_dragSource = &source;
            m_dragFromKeyboard = request.FromKeyboard;
            m_dragPayload = core::Move(request.Payload);
            if (m_dragPayload.Label.IsEmpty())
                m_dragPayload.Label = m_dragPayload.Type;
            source.SetDragSourceState(true);

            // The preview stays at the offset where the press was on the source, or else beside the
            // pointer.
            m_dragPreview = request.Preview;
            if (request.HasGrabOffset)
                m_grabOffset = request.GrabOffset;
            else if (m_dragPreview)
                m_grabOffset = request.Press;
            else
                m_grabOffset = core::Float2{-14.0f, -12.0f};
            m_dragFollow = core::Move(request.Follow);
            if (!m_dragPreview && !m_dragFollow)
                m_dragPreview = source.CreateDragPreview(m_dragPayload);
            if (m_root != nullptr)
            {
                m_dragLayer = core::MakeRef<DragLayer>(m_root->MemoryAllocator());
                m_dragLayer->SetPosition(core::Float2{0.0f, 0.0f});
                m_dragLayer->SetSize(m_root->GetSize());
                if (m_dragPreview)
                {
                    m_dragPreview->SetHitTestVisible(false);
                    m_dragLayer->AddChild(m_dragPreview.Get());
                }
                PopupOptions options;
                options.Kind = PopupKind::Manual;
                options.PassesPointer = true;
                OpenPopup(*m_root, *m_dragLayer, core::Move(options));
                m_dragLayerIn = this;
            }
            m_dragOver = this;
            m_dragOutside = false;

            DragEvent started(EventType::DragStart, &source, m_dragPayload);
            started.Position = RootPoint(at);
            started.Modifiers = m_dragModifiers;
            source.SendEvent(started);
            QueryDrop(at);
        }

        // Finds where a drag's pointer is: over this root, over another root of the GUI (a window
        // of its own), or outside every root. Where the roots' places are not known (Wayland, or no
        // host), past this root's edge counts as outside, and a drop there that nothing takes ends
        // outside.
        void LocateDrag(core::Float2 at)
        {
            m_dragOver = this;
            m_dragOverAt = at;
            m_dragOutside = false;
            m_dragScreenKnown = false;
            if (m_dragFromKeyboard)
                return; // a keyboard drag queries the focus, not a point
            core::Float2 screen;
            if (m_rootPlaces != nullptr && m_rootPlaces->ToScreen(*this, at, screen))
            {
                m_dragScreenKnown = true;
                m_dragScreen = screen;
                core::Float2 local;
                if (EventDispatcher* over = m_rootPlaces->DispatcherAt(screen, local))
                {
                    m_dragOver = over;
                    m_dragOverAt = local;
                }
                else
                    m_dragOutside = true;
                return;
            }
            if (m_root != nullptr)
            {
                const core::Float2 p = RootPoint(at);
                const core::Float2 size = m_root->GetSize();
                m_dragOutside = p.x < 0.0f || p.y < 0.0f || p.x >= size.x || p.y >= size.y;
            }
        }
        // Moves the drag's layer (its preview and hint) into the root the pointer is over, as a
        // popup there.
        void MoveDragLayerTo(EventDispatcher& over)
        {
            if (!m_dragLayer || m_dragLayerIn == &over || over.m_root == nullptr)
                return;
            if (m_dragLayerIn != nullptr && m_dragLayerIn->IsPopupOpen(*m_dragLayer))
                (void)m_dragLayerIn->ClosePopup(*m_dragLayer, PopupCloseReason::Code);
            m_dragLayer->SetSize(over.m_root->GetSize());
            PopupOptions options;
            options.Kind = PopupKind::Manual;
            options.PassesPointer = true;
            over.OpenPopup(*over.m_root, *m_dragLayer, core::Move(options));
            m_dragLayerIn = &over;
        }

        // Queries what a drop at `at` would do, asking the node there and then its ancestors, in
        // the root the pointer is over (which may be another root of the GUI).
        void QueryDrop(core::Float2 at)
        {
            m_mousePos = at;
            if (m_dragFollow)
                m_dragFollow(RootPoint(at)); // the source moves first, so the query sees what is left under it
            LocateDrag(at);
            if (!m_dragOutside)
                MoveDragLayerTo(*m_dragOver);
            const core::Float2 overAt = m_dragOverAt;
            // Over none of the placed roots, there is nothing to ask. Past this root's edge when
            // the places are not known, the node there is still asked (a node may reach past the
            // edge).
            Node* hit = m_dragOutside && m_dragScreenKnown ? nullptr : m_dragOver->HitTest(overAt);
            NoteDragRest(hit);
            DropQuery query{m_dragPayload, core::Float2{0.0f, 0.0f}, 0, DropEffect::Move, false, DropEffect::None, core::String{}, false, 0};
            query.Modifiers = m_dragModifiers;
            query.Preferred = PreferredEffect();
            Node* target = nullptr;
            for (Node* n = hit; n != nullptr; n = n->GetParent())
            {
                if (!n->IsTreeEnabled())
                    break;
                query.Local = n->ConvertToNodeSpace(overAt);
                query.Effect = DropEffect::None;
                n->HandleDragQuery(query);
                if (query.Effect != DropEffect::None)
                {
                    target = n;
                    break;
                }
                if (query.Refused)
                    break; // a refusal with a reason ends the walk and shows the reason
            }
            SetDropTarget(target, query, at);
        }

        void SetDropTarget(Node* target, const DropQuery& query, core::Float2 at)
        {
            (void)at;
            const core::Float2 rootAt = DragRootPoint();
            const auto event = [&](EventType type, Node* node)
            {
                DragEvent e(type, node, m_dragPayload);
                e.Effect = query.Effect;
                e.Position = rootAt;
                e.Modifiers = m_dragModifiers;
                e.Place = query.Place;
                return e;
            };
            if (target != m_dropTarget)
            {
                if (Node* left = m_dropTarget)
                {
                    m_dropTarget = nullptr;
                    left->SetDropTargetState(false);
                    left->HandleDragEvent(event(EventType::DragLeave, left));
                }
                m_dropTarget = target;
                if (target != nullptr)
                {
                    target->SetDropTargetState(true);
                    target->HandleDragEvent(event(EventType::DragEnter, target));
                }
            }
            if (m_dropTarget != nullptr)
                m_dropTarget->HandleDragEvent(event(EventType::DragOver, m_dropTarget));
            m_dropEffect = target != nullptr ? query.Effect : DropEffect::None;
            m_dropPlace = query.Place;
            ShowDropState(query.Reason.AsView(), query.Refused, rootAt);
        }

        // Shows the preview at the pointer with the effect, and the reason's hint beside it.
        void ShowDropState(core::StringView reason, bool refused, core::Float2 rootAt)
        {
            if (!m_dragLayer)
                return;
            if (m_dragPreview)
            {
                m_dragPreview->OnDragPreviewEffect(m_dropEffect);
                m_dragLayer->Place(*m_dragPreview, core::Float2{rootAt.x - m_grabOffset.x, rootAt.y - m_grabOffset.y});
            }
            const bool showHint = reason.Size() != 0;
            if (showHint && (m_dropReason.AsView() != reason || m_dropRefused != refused || !m_dropHint))
            {
                if (m_dropHint)
                    m_dropHint->RemoveFromParent();
                m_dropHint = m_dragSource != nullptr ? m_dragSource->CreateDropHint(reason, refused) : core::RefPtr<Node>{};
                if (m_dropHint)
                {
                    m_dropHint->SetHitTestVisible(false);
                    m_dragLayer->AddChild(m_dropHint.Get());
                }
            }
            else if (!showHint && m_dropHint)
            {
                m_dropHint->RemoveFromParent();
                m_dropHint = core::RefPtr<Node>{};
            }
            m_dropReason = core::String(reason);
            m_dropRefused = refused;
            if (m_dropHint)
            {
                // Place the hint below the preview (beside the pointer when there is none), never
                // over it.
                core::Float2 at{rootAt.x + 16.0f, rootAt.y + 22.0f};
                if (m_dragPreview)
                {
                    const core::Float2 size = m_dragPreview->Measure(LayoutConstraints{});
                    at = core::Float2{rootAt.x - m_grabOffset.x, rootAt.y - m_grabOffset.y + size.y + 4.0f};
                }
                m_dragLayer->Place(*m_dropHint, at);
            }
        }

        void Drop()
        {
            Node* target = m_dropTarget;
            const DropEffect effect = m_dropEffect;
            if (target == nullptr || effect == DropEffect::None)
            {
                FinishDrag(DropEffect::None, nullptr, false);
                return;
            }
            const core::RefPtr<Node> pin{target};
            DragEvent dropped(EventType::Drop, target, m_dragPayload);
            dropped.Effect = effect;
            dropped.Position = DragRootPoint();
            dropped.Modifiers = m_dragModifiers;
            dropped.Place = m_dropPlace;
            target->HandleDragEvent(dropped);
            FinishDrag(effect, target, false);
        }

        // Ends the drag: removes the layer, clears the drag states and tells the source.
        void FinishDrag(DropEffect effect, Node* target, bool cancelled)
        {
            if (!m_dragActive)
                return;
            m_dragActive = false;
            const core::RefPtr<Node> source{m_dragSource};
            const DragPayload payload = core::Move(m_dragPayload);
            if (Node* left = m_dropTarget)
            {
                m_dropTarget = nullptr;
                left->SetDropTargetState(false);
                if (target == nullptr)
                {
                    DragEvent leave(EventType::DragLeave, left, payload);
                    leave.Position = DragRootPoint();
                    left->HandleDragEvent(leave);
                }
            }
            if (m_dragLayer && m_dragLayerIn != nullptr && m_dragLayerIn->IsPopupOpen(*m_dragLayer))
                (void)m_dragLayerIn->ClosePopup(*m_dragLayer, PopupCloseReason::Code);
            m_dragLayer = core::RefPtr<DragLayer>{};
            m_dragLayerIn = nullptr;
            const bool outside = m_dragOutside && target == nullptr && !cancelled;
            const bool screenKnown = m_dragScreenKnown;
            const core::Float2 screen = m_dragScreen;
            m_dragOver = nullptr;
            m_dragOutside = false;
            m_dragPreview = core::RefPtr<Node>{};
            m_dragFollow = {};
            m_dropHint = core::RefPtr<Node>{};
            m_dragSource = nullptr;
            m_dropEffect = DropEffect::None;
            m_dropReason = core::String{};
            m_dropRefused = false;
            m_dragFromKeyboard = false;
            m_dragRestNode = nullptr;
            if (source)
            {
                source->SetDragSourceState(false);
                DragResult result{effect, target, cancelled, outside, screenKnown, screen};
                DragEvent ended(EventType::DragEnd, source.Get(), payload);
                ended.Effect = effect;
                ended.Position = RootPoint(m_mousePos);
                source->HandleDragEnd(result, ended);
            }
        }

        // Ticks a drag. Near the edge of a scroller under the pointer, the scroller scrolls: the
        // innermost one that can move that way, and only one the drag can land in (the drop
        // target is the scroller or inside it, so a panel dragged to dock past a list does not
        // scroll the list). A drag that rests on a node for a moment opens it.
        void TickDrag(f32 seconds)
        {
            if (m_dragOver != this || m_dragOutside)
                return; // over another root (whose content this root does not scroll) or over none
            Node* hit = HitTest(m_mousePos);
            bool moved = false;
            for (Node* n = hit; n != nullptr && !moved; n = n->GetParent())
            {
                if (!n->IsScrollContainer() || !IsInSubtree(m_dropTarget, n))
                    continue;
                const core::Float2 p = n->ConvertToNodeSpace(m_mousePos);
                const core::Float2 size = n->GetSize();
                const core::Float2 range = n->GetScrollRange();
                const core::Float2 offset = n->GetScrollOffset();
                const core::Float2 low = n->GetScrollMin(), high = n->GetScrollMax();
                const auto speed = [&](f32 at, f32 length, f32 now, f32 min, f32 max) -> f32
                {
                    if (at < m_dragScrollBand && now > min)
                        return -m_dragScrollSpeed * (1.0f - core::Max(0.0f, at) / m_dragScrollBand);
                    if (at > length - m_dragScrollBand && now < max)
                        return m_dragScrollSpeed * (1.0f - core::Max(0.0f, length - at) / m_dragScrollBand);
                    return 0.0f;
                };
                const core::Float2 v{range.x > 0.0f ? speed(p.x, size.x, offset.x, low.x, high.x) : 0.0f,
                                     range.y > 0.0f ? speed(p.y, size.y, offset.y, low.y, high.y) : 0.0f};
                if (v.x != 0.0f || v.y != 0.0f)
                {
                    n->ScrollBy(core::Float2{v.x * seconds, v.y * seconds});
                    moved = true;
                }
            }
            // Track resting: whether the same node has been under the pointer since it arrived.
            NoteDragRest(hit);
            if (!moved && !m_dragRestOpened)
            {
                m_dragRest += seconds;
                if (m_dragRest >= m_dragHoverOpenDelay)
                {
                    m_dragRestOpened = true;
                    for (Node* n = hit; n != nullptr; n = n->GetParent())
                        if (n->HandleDragHoverOpen(n->ConvertToNodeSpace(m_mousePos)))
                        {
                            moved = true;
                            break;
                        }
                }
            }
            if (moved)
                QueryDrop(m_mousePos); // the content may have moved under the pointer
        }

        // The point of a keyboard drag: the focused node's centre, in device pixels as input
        // arrives.
        [[nodiscard]] core::Float2 FocusPoint() const
        {
            if (m_focusNode == nullptr)
                return m_mousePos;
            return m_focusNode->ConvertToWorldSpace(core::Float2{m_focusNode->GetSize().x * 0.5f, m_focusNode->GetSize().y * 0.5f});
        }
        // A keyboard drag queries the focused node and then its ancestors, not what lies under a
        // point.
        void QueryAtFocus()
        {
            if (!m_dragActive)
                return;
            const core::Float2 at = FocusPoint();
            m_mousePos = at;
            DropQuery query{m_dragPayload, core::Float2{0.0f, 0.0f}, 0, PreferredEffect(), true, DropEffect::None, core::String{}, false, 0};
            Node* target = nullptr;
            for (Node* n = m_focusNode; n != nullptr; n = n->GetParent())
            {
                if (!n->IsTreeEnabled())
                    break;
                query.Local = n->ConvertToNodeSpace(at);
                query.Effect = DropEffect::None;
                n->HandleDragQuery(query);
                if (query.Effect != DropEffect::None)
                {
                    target = n;
                    break;
                }
                if (query.Refused)
                    break;
            }
            SetDropTarget(target, query, at);
        }

        // Notes the node a drag rests on; a different node restarts the rest timer.
        void NoteDragRest(Node* hit)
        {
            if (hit == m_dragRestNode)
                return;
            m_dragRestNode = hit;
            m_dragRest = 0.0f;
            m_dragRestOpened = false;
        }

        // Converts a point from the device pixels input arrives in to the root's space, which
        // hit-testing and the preview's placement use.
        [[nodiscard]] core::Float2 RootPoint(core::Float2 at) const
        {
            return m_root != nullptr ? m_root->ConvertToNodeSpace(at) : at;
        }
        // The drag's pointer in the root it is over (in this root when outside every root).
        [[nodiscard]] core::Float2 DragRootPoint() const
        {
            if (m_dragOver != nullptr && m_dragOver != this && !m_dragOutside)
                return m_dragOver->RootPoint(m_dragOverAt);
            return RootPoint(m_mousePos);
        }

        struct PopupEntry
        {
            core::RefPtr<Node> Popup;
            PopupOptions Options;
            mutable core::Float2 Size{0.0f, 0.0f}; // the popup's own size, which placement may shrink
            Array<Node::ElementRef> SavedFocus;  // the node focused before it opened, then that node's ancestors
            NavScope ScopeBefore = NavScope::None;
            // Where the popup goes back to when it closes (ReturnHome). It is weak because it may be
            // this root itself, which owns this dispatcher; a strong reference would keep the root
            // alive.
            Node::ElementRef Home{};
            usize HomeIndex = 0;
        };
        Array<PopupEntry> m_popups; // the top layer, bottom first
        Keystroke m_captureRelease{static_cast<u32>(KeyCode::F1), static_cast<u32>(KeyModShift), false};
        // Closed popups playing their exit (LeavesOnClose). They are drawn under the open ones and
        // take no input.
        struct LeavingPopup
        {
            core::RefPtr<Node> Popup;
            Node::ElementRef Home{};
            usize HomeIndex = 0;
            bool Modal = false;
            core::Function<void()> OnGone;
        };
        Array<LeavingPopup> m_leaving;
        bool m_swallowing = false;  // set by a press that closed a popup, so its release is consumed too
        // Tooltip state: the node whose text is showing or waiting to show, the content shown, and
        // the timers.
        Node* m_tipProvider = nullptr;
        core::RefPtr<Node> m_tip;
        core::String m_tipText; // the shown tooltip's text, to detect a change in its provider's text
        f32 m_tipTimer = 0.0f;
        f32 m_tipDelay = 0.5f;
        f32 m_tipHiddenAt = -1000.0f;
        bool m_tipSuppressed = false;
        static constexpr f32 kTooltipWarm = 0.6f; // seconds after a tooltip hides during which the next shows at once
        static constexpr f32 kTooltipGrace = 0.3f; // seconds an interactive tooltip waits for the pointer to reach it
        bool m_tipInteractive = false;  // the shown tooltip takes the pointer
        f32 m_tipGrace = 0.0f;
        bool m_keyboardTooltips = true;
        bool m_keysMovedFocus = false;  // keys moved the focus since the pointer was last used
        // A long press is armed by a finger's press and disarmed when the finger moves or lifts.
        bool m_longPressArmed = false;
        f32 m_pressHeld = 0.0f;
        core::Float2 m_pressAt{0.0f, 0.0f};
        static constexpr f32 kLongPress = 0.5f;
        // === Tooltips and long presses ===
        [[nodiscard]] static Node* TooltipProvider(Node* node)
        {
            for (Node* n = node; n != nullptr; n = n->GetParent())
                if (n->GetTooltipText().Size() != 0)
                    return n;
            return nullptr;
        }
        void UpdateTooltip(f32 seconds)
        {
            if (m_dragActive)
                return; // no tooltips during a drag (the hint says what a drop would do)
            Node* provider = m_touchActive ? m_tipProvider : TooltipProvider(m_overNode);
            // While the keys move the focus, show the focused control's tooltip; the pointer's
            // tooltip takes precedence.
            bool fromFocus = false;
            if (provider == nullptr && !m_touchActive && m_keyboardTooltips && m_inputKind != InputKind::Pointer && m_keysMovedFocus && m_focusNode != nullptr)
            {
                provider = TooltipProvider(m_focusNode);
                fromFocus = provider != nullptr;
            }
            // An interactive tooltip holds while the pointer is in it, and a moment after the pointer
            // leaves both it and its control, so the pointer can cross the gap into it.
            if (Node* shown = GetShownTooltip(); shown != nullptr && m_tipInteractive && provider != m_tipProvider)
            {
                if (m_overNode != nullptr && IsInSubtree(m_overNode, shown))
                {
                    m_tipGrace = 0.0f;
                    return;
                }
                m_tipGrace += seconds;
                if (m_tipGrace < kTooltipGrace)
                    return;
            }
            m_tipGrace = 0.0f;
            if (provider != m_tipProvider)
            {
                HideTooltip();
                m_tipProvider = provider;
                m_tipTimer = 0.0f;
                m_tipSuppressed = false;
            }
            // When the same provider has new text (a code editor's word under the pointer), hide the
            // tooltip, so the new text shows at once (warm) by the pointer's current position.
            if (provider != nullptr && !m_tipInteractive && GetShownTooltip() != nullptr && provider->GetTooltipText() != m_tipText.AsView())
                HideTooltip();
            if (provider == nullptr || m_tipSuppressed || m_touchActive || GetShownTooltip() != nullptr)
                return;
            m_tipTimer += seconds;
            const bool warm = m_time - m_tipHiddenAt <= kTooltipWarm; // a tooltip just hid, so the next shows at once
            if (m_tipTimer >= (warm ? 0.0f : m_tipDelay))
                ShowTooltip(*provider, m_mousePos, fromFocus);
        }
        void ShowTooltip(Node& provider, core::Float2 at, bool belowProvider = false)
        {
            if (m_root == nullptr)
                return;
            core::RefPtr<Node> content = provider.CreateTooltipContent(provider.GetTooltipText());
            if (!content)
                return;
            m_tipText = core::String(provider.GetTooltipText());
            // The root holds the tooltip, but it takes its writing direction from its provider.
            const bool rightToLeft = provider.IsRightToLeft();
            content->SetLayoutDirection(rightToLeft ? Node::LayoutDirection::RightToLeft : Node::LayoutDirection::LeftToRight);
            PopupOptions options;
            // An interactive tooltip is a popup the pointer may enter and press in, closed by a
            // press outside or Escape, rather than a hint that any press or key hides.
            m_tipInteractive = provider.IsTooltipInteractive();
            options.Kind = m_tipInteractive ? PopupKind::Auto : PopupKind::Hint;
            if (m_tipInteractive)
                content->SetHitTestVisible(true);
            options.HasAnchorRect = true;
            if (belowProvider)
            {
                // The focused control's tooltip goes below the control, away from the pointer.
                const core::Float2 p0 = provider.ConvertToRootSpace(core::Float2{0.0f, 0.0f});
                const core::Float2 p1 = provider.ConvertToRootSpace(provider.GetSize());
                options.AnchorRect = Rect{core::Min(p0.x, p1.x), core::Min(p0.y, p1.y), core::Abs(p1.x - p0.x), core::Abs(p1.y - p0.y)};
                options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 4.0f, false};
            }
            else
            {
                const core::Float2 p = m_root->ConvertToNodeSpace(at);
                // Offset from the pointer toward the inline end.
                options.AnchorRect = Rect{rightToLeft ? p.x - 12.0f : p.x + 12.0f, p.y, 0.0f, 0.0f};
                options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 18.0f, false};
            }
            options.FitContent = true; // sized to its text once its sheet styles it
            options.OnClosed = [this](PopupCloseReason reason)
            {
                m_tipHiddenAt = m_time;
                if (reason != PopupCloseReason::Code)
                    m_tipSuppressed = true; // closed by a press or a key: not again until the pointer moves on
            };
            HideTooltip();
            m_tip = core::Move(content);
            m_tipProvider = &provider;
            OpenPopup(*m_root, *m_tip, core::Move(options));
        }
        void HideTooltip()
        {
            if (m_tip && IsPopupOpen(*m_tip))
                (void)ClosePopup(*m_tip, PopupCloseReason::Code);
        }
        // A finger held still opens a context menu at the pressed node; if none is taken, it shows
        // the node's tooltip.
        void LongPress()
        {
            m_longPressArmed = false;
            const core::RefPtr<Node> target{m_downNode};
            if (!target || !target->IsTreeEnabled())
                return;
            // Over a drag source, a finger held still arms its drag (otherwise a finger's move pans
            // a scroller). The next move past the threshold drags, and no context menu opens.
            if (HasDragSource(target.Get()))
            {
                m_touchDragArmed = true;
                return;
            }
            MouseEvent event(EventType::ContextMenu, target.Get(), m_pressAt);
            event.Pointer = PointerType::Touch;
            event.PointerId = m_touchFinger;
            const bool taken = target->RouteEvent(event);
            CancelPointer(); // no release or click follows the long press
            if (!taken)
                if (Node* provider = TooltipProvider(target.Get()))
                    ShowTooltip(*provider, m_pressAt);
        }

        // === Top layer internals ===
        [[nodiscard]] i32 FindPopup(const Node& popup) const
        {
            for (usize i = 0; i < m_popups.Size(); ++i)
                if (m_popups[i].Popup.Get() == &popup)
                    return static_cast<i32>(i);
            return -1;
        }
        // The topmost auto popup's index, skipping hints and not looking past a manual popup, or
        // -1.
        [[nodiscard]] i32 TopAutoPopup() const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
            {
                if (m_popups[i].Options.Kind == PopupKind::Hint)
                    continue;
                return m_popups[i].Options.Kind == PopupKind::Auto ? static_cast<i32>(i) : -1;
            }
            return -1;
        }
        // The topmost modal popup's index, or -1.
        [[nodiscard]] i32 TopModal() const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if (m_popups[i].Options.Modal)
                    return static_cast<i32>(i);
            return -1;
        }
        // Whether `node` takes input: any node while no modal is open, else only a node in the
        // topmost modal popup or in a popup above it. A leaving or inert node never does.
        [[nodiscard]] bool IsLive(const Node* node) const
        {
            if (node != nullptr && node->IsTreeInert())
                return false;
            const i32 modal = TopModal();
            if (modal < 0 || node == nullptr)
                return true;
            for (usize i = static_cast<usize>(modal); i < m_popups.Size(); ++i)
                if (IsInSubtree(node, m_popups[i].Popup.Get()))
                    return true;
            return false;
        }
        // The node that focus, Tab and the arrows stay inside: the topmost popup that takes focus,
        // else the root.
        [[nodiscard]] Node* FocusScopeRoot() const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if ((m_popups[i].Options.TakesFocus || m_popups[i].Options.Modal) && m_popups[i].Options.Kind != PopupKind::Hint)
                    return m_popups[i].Popup.Get();
            return m_root;
        }
        // Closes the auto popups above the one `hit` is in (all of them when it is in none), without
        // going past a modal one. Returns true when the press is to be consumed, meaning it closed
        // some and lands in no popup that stays open (a press in a parent menu that closes its
        // submenu goes on to the parent).
        bool DismissOutside(Node* hit)
        {
            i32 inside = -1;
            for (usize i = m_popups.Size(); i-- > 0;)
                if (IsInSubtree(hit, m_popups[i].Popup.Get()))
                {
                    inside = static_cast<i32>(i);
                    break;
                }
            bool closed = false;
            for (usize i = m_popups.Size(); i-- > 0 && static_cast<i32>(i) > inside;)
            {
                if (i >= m_popups.Size())
                    continue; // closed along with the popup containing it
                const PopupOptions& o = m_popups[i].Options;
                if (o.Modal && o.Kind != PopupKind::Auto)
                    break;
                if (o.Kind != PopupKind::Auto)
                    continue;
                ClosePopupAt(i, PopupCloseReason::OutsidePress);
                closed = true;
            }
            return closed && inside < 0;
        }
        void ClosePopupsOfKind(PopupKind kind, PopupCloseReason reason)
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if (i < m_popups.Size() && m_popups[i].Options.Kind == kind)
                    ClosePopupAt(i, reason);
        }
        // Closes the popup at `index`: first the popups it contains, then the popup itself (it is
        // told, and leaves the top layer and the tree). Focus is given back if it was inside.
        void ClosePopupAt(usize index, PopupCloseReason reason)
        {
            Node* const closing = m_popups[index].Popup.Get();
            for (usize i = m_popups.Size(); i-- > index + 1;)
                if (i < m_popups.Size() && IsInSubtree(m_popups[i].Popup.Get(), closing))
                    ClosePopupAt(i, reason);
            const i32 at = FindPopup(*closing);
            if (at < 0)
                return; // a handler already closed it
            const core::RefPtr<Node> popup = m_popups[static_cast<usize>(at)].Popup;
            ToggleEvent closed(popup.Get(), false, reason);
            (void)popup->RouteEvent(closed);
            const i32 still = FindPopup(*popup);
            if (still < 0)
                return;
            PopupEntry entry = core::Move(m_popups[static_cast<usize>(still)]);
            m_popups.RemoveAt(static_cast<usize>(still));
            const bool focusInside = m_focusNode == nullptr || IsInSubtree(m_focusNode, popup.Get());
            // A popup that plays an exit stays drawn on top, taking no input, until the exit ends.
            const bool leaves = entry.Options.LeavesOnClose && popup->GetParent() != nullptr && popup->GetRootNode()->HasStylePass();
            Demote(*popup, entry);
            if (leaves)
            {
                popup->SetInTopLayer(true); // drawn over the tree during its exit
                m_leaving.PushBack(LeavingPopup{popup, entry.Home, entry.HomeIndex, entry.Options.Modal, core::Move(entry.Options.OnGone)});
                popup->Leave();
            }
            else
            {
                popup->RemoveFromParent(); // a closed popup leaves the tree; its owner keeps a reference
                // With ReturnHome, it goes back to where it was, if that node still exists.
                if (Node* home = entry.Home.table != nullptr ? entry.Home.table->Resolve(entry.Home.slot, entry.Home.generation) : nullptr)
                    (void)home->AddChildAt(popup.Get(), core::Min(entry.HomeIndex, home->ChildCount()));
            }
            if (focusInside && !entry.SavedFocus.IsEmpty())
                RestoreFocus(entry, nullptr);
            if (entry.Options.OnClosed)
                entry.Options.OnClosed(reason);
            if (!leaves && entry.Options.OnGone)
                entry.Options.OnGone();
        }
        // Settles the popups whose exit has ended (they have left their parent): takes them out of
        // the top layer, returns them home if they have one, and calls OnGone.
        void SettleLeavingPopups()
        {
            for (usize i = 0; i < m_leaving.Size();)
            {
                Node& popup = *m_leaving[i].Popup;
                if (popup.GetParent() != nullptr && popup.IsLeaving())
                {
                    ++i;
                    continue;
                }
                LeavingPopup gone = core::Move(m_leaving[i]);
                m_leaving.RemoveAt(i);
                popup.SetInTopLayer(false);
                if (popup.GetParent() == nullptr)
                    if (Node* home = gone.Home.table != nullptr ? gone.Home.table->Resolve(gone.Home.slot, gone.Home.generation) : nullptr)
                        (void)home->AddChildAt(&popup, core::Min(gone.HomeIndex, home->ChildCount()));
                if (gone.OnGone)
                    gone.OnGone();
            }
        }
        // Drops a popup leaving with a subtree: removes it from the stack, tells it, and gives
        // focus back when possible.
        void DropPopupAt(usize index, PopupCloseReason reason, const Node* leaving)
        {
            PopupEntry entry = core::Move(m_popups[index]);
            m_popups.RemoveAt(index);
            const bool focusInside = m_focusNode == nullptr || IsInSubtree(m_focusNode, entry.Popup.Get());
            Demote(*entry.Popup, entry);
            if (focusInside && !entry.SavedFocus.IsEmpty())
                RestoreFocus(entry, leaving);
            if (entry.Options.OnClosed)
                entry.Options.OnClosed(reason);
        }
        static void Demote(Node& popup, const PopupEntry& entry)
        {
            popup.SetInTopLayer(false);
            popup.SetRouteStop(false);
            if (!entry.SavedFocus.IsEmpty() || entry.Options.TakesFocus || entry.Options.Modal)
                popup.SetNavScope(entry.ScopeBefore);
        }
        // Gives focus back to the node the popup took it from, else to that node's nearest ancestor
        // that is still in the tree, enabled and focusable. If there is none, nothing is focused.
        void RestoreFocus(const PopupEntry& entry, const Node* leaving)
        {
            for (const Node::ElementRef& ref : entry.SavedFocus)
            {
                Node* n = ref.table != nullptr ? ref.table->Resolve(ref.slot, ref.generation) : nullptr;
                if (n == nullptr || n->GetRootNode() != m_root || (leaving != nullptr && IsInSubtree(n, leaving)))
                    continue;
                if (n->IsTabFocusable() && n->IsTreeEnabled() && n->IsTreeVisible() && IsLive(n))
                {
                    SetFocusNode(n);
                    return;
                }
            }
            SetFocusNode(nullptr);
        }

        // The outermost focus group holding `node` below `within` (null when none).
        [[nodiscard]] static Node* OutermostFocusGroup(Node* node, Node* within)
        {
            Node* group = nullptr;
            for (Node* n = node != nullptr ? node->GetParent() : nullptr; n != nullptr && n != within; n = n->GetParent())
                if (n->IsFocusGroup())
                    group = n;
            return group;
        }

        // Gathers tab-focusable nodes under `node` in pre-order (skipping hidden subtrees, those
        // out of the layout too, and disabled nodes).
        static void CollectTabStops(Node* node, core::Array<Node*>& out)
        {
            if (node == nullptr || !node->IsVisible() || !node->IsDisplayed())
                return;
            if (node->IsTabFocusable() && node->IsTreeEnabled())
                out.PushBack(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                CollectTabStops(node->GetChildAt(i), out);
        }

        bool MoveTabFocus(i32 direction)
        {
            // nav-next and nav-prev take precedence.
            if (m_focusNode != nullptr)
            {
                Node* root = FocusScopeRoot();
                bool stop = false;
                Node* overridden = root != nullptr ? FollowOverride(*root, *m_focusNode,
                                                                    direction > 0 ? Node::NavOverrideNext : Node::NavOverridePrevious,
                                                                    stop)
                                                   : nullptr;
                if (overridden != nullptr)
                {
                    SetFocusNode(overridden);
                    return true;
                }
                if (stop)
                    return false;
            }
            const core::Array<Node*> stops = TabStops();
            const usize count = stops.Size();
            if (count == 0)
                return false;

            usize current = count; // sentinel: "not in the list"
            for (usize i = 0; i < count; ++i)
                if (stops[i] == m_focusNode)
                {
                    current = i;
                    break;
                }

            usize next;
            if (current == count)
                next = (direction > 0) ? 0 : count - 1; // nothing focused: go to the first or the last
            else
                next = (direction > 0) ? ((current + 1) % count) : ((current + count - 1) % count);

            SetFocusNode(stops[next]);
            return true;
        }

        Node* m_root;                      // non-owning (the SceneRoot owns this dispatcher)
        core::Array<Finger> m_fingers;     // every finger down (the first, while active, drives the pointer)
        // The gesture in progress: its node, its two fingers, and its running totals.
        bool m_gesturing = false;
        core::RefPtr<Node> m_gestureTarget;
        u64 m_gestureIds[2]{};
        PointerType m_gesturePointer = PointerType::Touch; // two fingers, or a trackpad's pinch
        static constexpr u64 kNoFinger = ~u64{0};
        f32 m_gestureSpan0 = 1.0f;
        f32 m_gestureAngle = 0.0f; // angle of the line between the fingers, as last seen
        core::Float2 m_gestureFocus0{0.0f, 0.0f};
        f32 m_gestureScale = 1.0f;
        f32 m_gestureRotation = 0.0f;
        core::Float2 m_gestureTranslation{0.0f, 0.0f};
        IClipboard* m_clipboard = nullptr; // non-owning system-clipboard adapter (optional)
        core::Function<bool(Node&)> m_modalInterceptor; // SetModalInterceptor
        Node* m_overNode = nullptr;        // non-owning: the deepest node of the hover path
        NavTrace m_lastNavigation;         // what the last directional move weighed
        Array<Node*> m_hoverPath;          // non-owning, root first: every node under the pointer
        Array<Node*> m_pressPath;          // non-owning: the pressed node and its ancestors
        Node* m_captureNode = nullptr;     // non-owning: explicit pointer capture (CapturePointer)
        f32 m_time = 0.0f;                 // seconds, advanced by Tick (click counting)
        f32 m_lastPressTime = -1000.0f;
        core::Float2 m_lastPressAt{0.0f, 0.0f};
        MouseButton m_lastPressButton = MouseButton::Left;
        u32 m_clickCount = 0;
        bool m_touchActive = false; // the first finger is down (it drives the pointer)
        u64 m_touchFinger = 0;
        Node* m_downNode = nullptr;        // non-owning
        struct PanSample
        {
            f32 Time;
            core::Float2 Position;
        };
        static constexpr usize kPanSamples = 8;
        PanSample m_panSamples[kPanSamples]{};
        usize m_panSampleCount = 0;
        core::Float2 m_scrollStick{0.0f, 0.0f}; // a pad's right stick (ScrollByStick)
        bool m_panning = false;            // the finger pans m_panScroller
        Node* m_panScroller = nullptr;     // non-owning (cleared when its subtree leaves)
        core::Float2 m_panLast{0.0f, 0.0f}; // where the finger was at the last pan step
        bool m_clickSuppressed = false;    // the press has already activated (SuppressClick)
        Node* m_focusNode = nullptr;       // non-owning
        bool m_dragActive = false;                   // drag-and-drop in progress
        bool m_dragTried = true;                     // this press has already offered its drag
        bool m_touchDragArmed = false;               // a finger held long enough over a source
        bool m_dragFromKeyboard = false;
        f32 m_dragThreshold = 6.0f;
        u32 m_dragModifiers = 0;
        DropEffect m_dropEffect = DropEffect::None;
        i32 m_dropPlace = 0;
        core::String m_dropReason;
        bool m_dropRefused = false;
        core::Float2 m_grabOffset{0.0f, 0.0f};
        core::RefPtr<DragLayer> m_dragLayer;
        core::RefPtr<Node> m_dragPreview;
        core::Function<void(core::Float2)> m_dragFollow; // a source's callback to show itself where the drag is
        core::RefPtr<Node> m_dropHint;
        core::Function<bool(const core::Array<core::String>&, core::Float2)> m_onFileDrop;
        f32 m_dragScrollBand = 24.0f;
        f32 m_dragScrollSpeed = 900.0f;
        f32 m_dragHoverOpenDelay = 0.6f;
        Node* m_dragRestNode = nullptr; // non-owning: compared, never dereferenced
        IRootPlaces* m_rootPlaces = nullptr;   // the GUI's, set by its host; not owned
        EventDispatcher* m_dragOver = nullptr; // the dispatcher of the root a drag is over (this one, or another root's)
        core::Float2 m_dragOverAt{};           // the pointer in that root
        bool m_dragOutside = false;            // over no root
        bool m_dragScreenKnown = false;        // the screen position is known (a host placed the roots)
        core::Float2 m_dragScreen{};
        EventDispatcher* m_dragLayerIn = nullptr; // the dispatcher whose top layer holds the drag's layer
        f32 m_dragRest = 0.0f;
        bool m_dragRestOpened = false;
        Node* m_dragSource = nullptr;                // non-owning
        Node* m_dropTarget = nullptr;                // non-owning current target
        DragPayload m_dragPayload;
        core::Float2 m_mousePos{0.0f, 0.0f};
        Observable m_focusChanged;   // notified when focus moves
        core::Function<bool(const Keystroke&)> m_keystrokes; // the keymap (SceneRoot)
        bool m_altHeld = false;         // access keys show while Alt is held
        bool m_mnemonicsAlways = false; // access keys always show
        void SetAltHeld(bool held)
        {
            if (held == m_altHeld)
                return;
            const bool was = AreMnemonicsShown();
            m_altHeld = held;
            if (was != AreMnemonicsShown())
                RedrawMnemonics();
        }
        // Redraws every node that draws an access key (and its parts), in the tree and its popups.
        void RedrawMnemonics()
        {
            const auto visit = [](auto& self, Node* node, bool marked) -> void
            {
                if (node == nullptr)
                    return;
                const bool here = marked || node->GetMnemonic() != 0;
                if (here)
                    node->Invalidate();
                for (usize i = 0; i < node->ChildCount(); ++i)
                    self(self, node->GetChildAt(i), here);
            };
            visit(visit, m_root, false);
            for (const PopupEntry& entry : m_popups)
                visit(visit, entry.Popup.Get(), false);
        }
        // The character a key types for an access key: its letter (lower case) or digit, else 0.
        [[nodiscard]] static char32_t MnemonicOfKey(KeyCode key) noexcept
        {
            for (usize i = 0; i < 26; ++i)
                if (kLetterKeys[i] == key)
                    return static_cast<char32_t>(U'a' + i);
            if (key >= KeyCode::Num0 && key <= KeyCode::Num9)
                return static_cast<char32_t>(U'0' + (static_cast<u32>(key) - static_cast<u32>(KeyCode::Num0)));
            return 0;
        }
        // The first shown, enabled widget marked with `key` in the open menu (else in the focus
        // scope) takes focus if it can, and acts. Returns false when no widget has that key.
        bool ActivateMnemonic(char32_t key)
        {
            if (key == 0)
                return false;
            Node* within = FocusScopeRoot();
            for (usize i = m_popups.Size(); i-- > 0;)
                if (m_popups[i].Options.Kind != PopupKind::Hint) // keys never go to a tooltip
                {
                    within = m_popups[i].Popup.Get();
                    break;
                }
            Node* found = nullptr;
            const auto find = [&](auto& self, Node* node) -> void
            {
                if (node == nullptr || found != nullptr || !node->IsVisible() || !node->IsDisplayed() || !node->IsTreeEnabled())
                    return;
                if (node->GetMnemonic() == key)
                {
                    found = node;
                    return;
                }
                for (usize i = 0; i < node->ChildCount(); ++i)
                    self(self, node->GetChildAt(i));
            };
            find(find, within);
            if (found == nullptr)
                return false;
            UseKeyboard();
            if (found->IsTabFocusable())
                SetFocusNode(found);
            (void)found->HandleActivate();
            return true;
        }
        // The state of a waiting CaptureNextKey.
        struct KeyCapture
        {
            core::Function<void(const Keystroke&)> OnKey;
            core::Function<void()> OnCancel;
            bool TakeEscape = false;
            bool Waiting = false;
            bool DropText = false;    // drop the text that the taken key types next
            u32 PendingModifier = 0;  // a modifier key down, taken alone if released before another key
        };
        KeyCapture m_capture;
        void TakeCapturedKey(const Keystroke& stroke)
        {
            m_capture.Waiting = false;
            core::Function<void(const Keystroke&)> onKey = core::Move(m_capture.OnKey);
            m_capture.OnCancel = {};
            if (onKey)
                onKey(stroke);
        }
        core::Function<void()> m_onInput;                    // called when this root takes input (GUIContext)
        void NoteInput()
        {
            if (m_onInput)
                m_onInput();
        }
        // What is driving the GUI. It starts as the keys until the pointer is used, so a menu that
        // opens with focus on its first button shows the focus (as games do).
        InputKind m_inputKind = InputKind::Keyboard;
        Observable m_inputKindChanged;
        bool m_holding = false;      // a direction held (PressDirection)
        NavDirection m_heldDirection = NavDirection::Down;
        f32 m_heldFor = 0.0f;
        f32 m_nextRepeatAt = 0.0f;

        // Called on a key or d-pad navigation: the focus ring shows, and the input kind switches
        // from the pointer to the keys.
        void UseKeyboard()
        {
            if (m_inputKind == InputKind::Pointer)
                ApplyInputKind(InputKind::Keyboard);
            m_keysMovedFocus = true;
            if (m_focusNode != nullptr)
                m_focusNode->SetFocusVisible(true);
        }
        void ApplyInputKind(InputKind kind)
        {
            if (kind == m_inputKind)
                return;
            m_inputKind = kind;
            if (kind == InputKind::Pointer)
                m_keysMovedFocus = false;
            if (m_focusNode != nullptr)
                m_focusNode->SetFocusVisible(kind != InputKind::Pointer);
            m_inputKindChanged.Notify();
            if (m_root != nullptr)
            {
                m_root->Invalidate(); // redraw for the cursor and for what a sheet shows for this input kind
                m_root->SendEvent(Event(EventType::InputKindChanged, m_root));
            }
        }
    };
}

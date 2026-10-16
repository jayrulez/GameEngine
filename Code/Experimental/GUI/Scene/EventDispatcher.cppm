// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :event_dispatcher partition
//
// EventDispatcher: routes abstract input into the node tree. Ported from eepp's
// Scene::EventDispatcher, but the core is platform-agnostic: instead of hooking a window's
// Input, it exposes an Inject* API that a gui.shell bridge feeds with already-abstracted
// events (from InputSurface/InputRouter). It hit-tests via the root's OverFind, tracks
// hover / press / focus, and calls the target Node's Handle* dispatch methods.
//
// Interaction refs (over/down/focus) are non-owning Node*; there is no node-removal
// cleanup hook, so a removed hovered/focused node does not clear these refs.

module;
#include "Core/Prelude.h"

export module experimental.gui:event_dispatcher;

import foundation.core; // Float2, StringView
import :node;
import :event;
import :clipboard;
import :navigation; // directional focus (G7)
import :keymap;     // Keystroke, PadButton
import :reactive;   // Observable (focus, for commands)

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class EventDispatcher
    {
    public:
        explicit EventDispatcher(Node* root) noexcept : m_root(root) {}

        [[nodiscard]] Node* GetRoot() const noexcept { return m_root; }

        // The system-clipboard adapter (non-owning; supplied by the gui.shell bridge / app).
        // Widgets reach it via GetEventDispatcher()->GetClipboard(); null = no clipboard, so
        // cut/copy/paste become no-ops.
        void SetClipboard(IClipboard* clipboard) noexcept { m_clipboard = clipboard; }
        [[nodiscard]] IClipboard* GetClipboard() const noexcept { return m_clipboard; }

        // Modal root: while set, pointer hit-testing and Tab traversal are confined to this
        // subtree, so a modal Window blocks interaction with everything behind it (clicks
        // outside it are ignored). Null = no modal. (A visual dim is the Window's concern.)
        void SetModalRoot(Node* modal) noexcept { m_modalRoot = modal; }
        [[nodiscard]] Node* GetModalRoot() const noexcept { return m_modalRoot; }

        [[nodiscard]] Node* GetOverNode() const noexcept { return m_overNode; }
        [[nodiscard]] Node* GetFocusNode() const noexcept { return m_focusNode; }
        /// The focused node, read as a binding's dependency (a command's availability follows it).
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

        // === Injection API (fed by the shell bridge) ===
        void InjectMouseMove(core::Float2 position)
        {
            m_mousePos = position;
            // While a drag-and-drop is in progress, route to the drop target (enter/leave/over)
            // instead of the normal hover/capture path.
            if (m_dragActive)
            {
                Node* target = FindDropTarget(HitTest(position));
                if (target != m_dropTarget)
                {
                    if (m_dropTarget)
                        m_dropTarget->HandleDragLeave(m_dragPayload);
                    m_dropTarget = target;
                    if (m_dropTarget)
                        m_dropTarget->HandleDragEnter(m_dragPayload);
                }
                if (m_dropTarget)
                    m_dropTarget->HandleDragOver(m_dragPayload);
                return;
            }
            Node* hit = HitTest(position);
            if (hit != m_overNode)
            {
                if (m_overNode)
                {
                    const core::RefPtr<Node> pin{m_overNode};
                    m_overNode->HandleMouseLeave(
                        MouseEvent(EventType::MouseLeave, m_overNode, position));
                }
                m_overNode = hit;
                if (m_overNode)
                {
                    const core::RefPtr<Node> pin{m_overNode};
                    m_overNode->HandleMouseEnter(
                        MouseEvent(EventType::MouseEnter, m_overNode, position));
                }
            }
            // Pointer capture: while a button is held, the pressed node keeps receiving moves
            // (so a drag continues even when the cursor leaves it). Otherwise the hovered node.
            Node* target = (m_downNode != nullptr) ? m_downNode : hit;
            if (target)
            {
                const core::RefPtr<Node> pin{target};
                target->HandleMouseMove(MouseEvent(EventType::MouseMove, target, position));
            }
        }

        void InjectMouseDown(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            NoteInput();
            // A pointer in use: the focus ring hides (shown again by the next key or d-pad move).
            m_keyboardMode = false;
            if (m_focusNode != nullptr)
                m_focusNode->SetFocusVisible(false);
            m_mousePos = position;
            Node* hit = HitTest(position);
            // A press outside the active popup (and its owner) dismisses it - the click still
            // proceeds normally afterwards. The optional `contains` predicate lets a popup claim
            // nodes outside its own subtree as still "inside" it (e.g. a menu's open submenus,
            // which are siblings in the tree, not descendants - eepp's isChildOrSubMenu).
            if (m_popup != nullptr && !IsInSubtree(hit, m_popup) &&
                !IsInSubtree(hit, m_popupOwner) && !(m_popupContains && m_popupContains(hit)))
                ClosePopup();
            m_downNode = hit;
            // Click-to-focus: the nearest widget that takes focus, at or above what was hit; none
            // (a click on a backdrop or a label) clears focus, as browsers do, so navigation
            // starts again from the GUI's entry rather than from the backdrop.
            Node* focusable = hit;
            while (focusable != nullptr && !(focusable->IsTabFocusable() && focusable->IsEnabled()))
                focusable = focusable->GetParent();
            SetFocusNode(focusable);
            if (hit)
            {
                const core::RefPtr<Node> pin{hit};
                hit->HandleMouseDown(
                    MouseEvent(EventType::MouseDown, hit, position, button, modifiers));
            }
        }

        void InjectMouseUp(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            m_mousePos = position;
            // A release while dragging completes the drop on the target under the cursor, then
            // ends the drag. The normal up (below) still fires so the source resets its state.
            if (m_dragActive)
            {
                Node* target = FindDropTarget(HitTest(position));
                if (target != nullptr)
                    target->HandleDrop(m_dragPayload);
                else if (m_dropTarget)
                    m_dropTarget->HandleDragLeave(m_dragPayload);
                EndDrag();
            }
            Node* hit = HitTest(position);
            // The captured (pressed) node gets the release, even if the cursor moved off it.
            // Both nodes are pinned for the rest of this call: a handler may mutate the tree
            // (e.g. a header click re-sorts a model and the view rebuilds its header cells),
            // dropping the last owning reference to the very node still on the call stack.
            const core::RefPtr<Node> hitPin{hit};
            Node* target = (m_downNode != nullptr) ? m_downNode : hit;
            const core::RefPtr<Node> targetPin{target};
            if (target)
                target->HandleMouseUp(
                    MouseEvent(EventType::MouseUp, target, position, button, modifiers));
            // A click (widget activation) only for the primary/left button, and only when the
            // release lands on the node that was pressed. Right/middle presses still deliver
            // Down/Up (e.g. for context menus) but never activate a control.
            if (hit != nullptr && hit == m_downNode && button == MouseButton::Left)
                hit->HandleMouseClick(
                    MouseEvent(EventType::MouseClick, hit, position, button, modifiers));
            m_downNode = nullptr;
        }

        void InjectMouseWheel(core::Float2 position, core::Float2 delta)
        {
            Node* hit = HitTest(position);
            // Bubble to the nearest ancestor that consumes the wheel (a ScrollView), so
            // scrolling works while hovering the scrolled content.
            for (Node* n = hit; n != nullptr; n = n->GetParent())
            {
                if (n->WantsWheel())
                {
                    const core::RefPtr<Node> pin{n};
                    n->HandleMouseWheel(WheelEvent(n, position, delta));
                    return;
                }
            }
            if (hit)
            {
                const core::RefPtr<Node> pin{hit};
                hit->HandleMouseWheel(
                    WheelEvent(hit, position, delta)); // fallback: listeners on the hit node
            }
        }

        void InjectKeyDown(u32 keyCode, u32 modifiers = 0)
        {
            NoteInput();
            // Escape dismisses an open popup (menu / dropdown / tooltip) and is consumed.
            if (m_popup != nullptr && keyCode == static_cast<u32>(KeyCode::Escape))
            {
                ClosePopup();
                return;
            }
            // Tab / Shift+Tab drive focus traversal at the dispatcher level (a widget never
            // sees a bare Tab), matching the common GUI convention.
            if (keyCode == static_cast<u32>(KeyCode::Tab) &&
                (modifiers & (~static_cast<u32>(KeyModShift))) == 0)
            {
                if (modifiers & KeyModShift)
                    FocusPrevious();
                else
                    FocusNext();
                return;
            }
            // The focused widget first; what it leaves is navigation's: an arrow moves focus, Return
            // or Space activates (G7).
            KeyEvent event(EventType::KeyDown, m_focusNode, keyCode, modifiers);
            if (m_focusNode)
                m_focusNode->HandleKeyDown(event);
            if (event.Handled)
                return;
            // Then the bindings (a keystroke bound to a command where the focus path's contexts allow).
            if (m_keystrokes && m_keystrokes(Keystroke{keyCode, modifiers, false}))
                return;
            if (modifiers != 0)
                return;
            switch (static_cast<KeyCode>(keyCode))
            {
            case KeyCode::Up: (void)MoveFocus(NavDirection::Up); break;
            case KeyCode::Down: (void)MoveFocus(NavDirection::Down); break;
            case KeyCode::Left: (void)MoveFocus(NavDirection::Left); break;
            case KeyCode::Right: (void)MoveFocus(NavDirection::Right); break;
            case KeyCode::Return:
            case KeyCode::Space: (void)Activate(); break;
            default: break;
            }
        }

        /// Moves focus to the best widget in `direction` (inside a modal while one is open, within
        /// focus scopes); with nothing focused, to the GUI's entry. True when focus moved. The
        /// gamepad's d-pad and stick arrive here as the arrows do.
        bool MoveFocus(NavDirection direction)
        {
            UseKeyboard();
            Node* root = m_modalRoot != nullptr ? m_modalRoot : m_root;
            if (root == nullptr)
                return false;
            (void)HitTest(core::Float2{}); // the layout as it is now (a pending pass runs first)
            Node* target = NavigateFrom(*root, m_focusNode, direction);
            if (target == nullptr || target == m_focusNode)
                return false;
            SetFocusNode(target);
            return true;
        }
        /// A gamepad button (the d-pad goes to PressDirection): the bindings first; unbound, South
        /// activates and East goes back (the `back` command).
        void InjectPadButton(PadButton button)
        {
            NoteInput();
            UseKeyboard();
            if (m_keystrokes && m_keystrokes(Keystroke{static_cast<u32>(button), 0, true}))
                return;
            if (button == PadButton::South)
                (void)Activate();
        }
        /// Told on input a user gave this root (a press, a key, the gamepad): its GUI makes it the
        /// active root.
        void SetOnInput(core::Function<void()> onInput) { m_onInput = core::Move(onInput); }
        /// Where unhandled keystrokes go: the SceneRoot's keymap (true when it took one).
        void SetKeystrokeRouter(core::Function<bool(const Keystroke&)> router) { m_keystrokes = core::Move(router); }

        /// Activates the focused widget (Return, Space, a gamepad's South): what a click does.
        bool Activate() { return m_focusNode != nullptr && m_focusNode->IsEnabled() && m_focusNode->HandleActivate(); }
        void InjectKeyUp(u32 keyCode, u32 modifiers = 0)
        {
            if (m_focusNode)
                m_focusNode->HandleKeyUp(
                    KeyEvent(EventType::KeyUp, m_focusNode, keyCode, modifiers));
        }
        void InjectText(core::StringView text)
        {
            if (m_focusNode)
                m_focusNode->HandleTextInput(TextInputEvent(m_focusNode, text));
        }

        // === Focus ===
        void SetFocusNode(Node* node)
        {
            if (node == m_focusNode)
                return;
            Node* previous = m_focusNode;
            m_focusNode = node;
            m_focusChanged.Notify(); // the focus path: which commands are available
            if (previous)
                previous->HandleFocusLost();
            if (m_focusNode)
            {
                RememberInScopes(*m_focusNode); // coming back to a scope returns here
                m_focusNode->SetFocusVisible(m_keyboardMode);
                m_focusNode->HandleFocusGained();
                // Scrollers around it bring it into view.
                for (Node* p = m_focusNode->GetParent(); p != nullptr; p = p->GetParent())
                    p->OnDescendantFocused(*m_focusNode);
            }
        }

        // === Drag-and-drop ===
        // A source widget starts a drag (typically from its OnMouseMove once a press has moved
        // past a threshold). The dispatcher then delivers drag-enter/over/leave to the nearest
        // accepting target under the cursor, and Drop on release.
        void BeginDrag(Node* source, DragPayload payload)
        {
            m_dragActive = true;
            m_dragSource = source;
            m_dragPayload = core::Move(payload);
            m_dropTarget = FindDropTarget(HitTest(m_mousePos));
            if (m_dropTarget)
                m_dropTarget->HandleDragEnter(m_dragPayload);
        }
        [[nodiscard]] bool IsDragging() const noexcept { return m_dragActive; }
        [[nodiscard]] const DragPayload& GetDragPayload() const noexcept { return m_dragPayload; }
        [[nodiscard]] Node* GetDragSource() const noexcept { return m_dragSource; }
        [[nodiscard]] Node* GetDropTarget() const noexcept { return m_dropTarget; }
        void CancelDrag()
        {
            if (!m_dragActive)
                return;
            if (m_dropTarget)
                m_dropTarget->HandleDragLeave(m_dragPayload);
            EndDrag();
        }

        // === Popups / overlays ===
        // Track an active popup (a dropdown / menu / tooltip the caller has already added to
        // the tree, typically as a top-level child of the root so it draws over everything).
        // A press outside it and its owner, or Escape, dismisses it via the onClose callback.
        // `owner` is the widget that opened it (e.g. the ComboBox) - clicks on it don't dismiss.
        void OpenPopup(Node* popup, Node* owner, core::Function<void()> onClose,
                       core::Function<bool(Node*)> contains = {})
        {
            if (m_popup != nullptr)
                ClosePopup();
            m_popup = popup;
            m_popupOwner = owner;
            m_onPopupClose = core::Move(onClose);
            m_popupContains = core::Move(contains);
        }
        void ClosePopup()
        {
            if (m_popup == nullptr)
                return;
            core::Function<void()> cb = core::Move(m_onPopupClose);
            m_popup = nullptr;
            m_popupOwner = nullptr;
            m_onPopupClose = {};
            m_popupContains = {};
            if (cb)
                cb(); // the owner hides/removes the popup here
        }
        [[nodiscard]] Node* GetPopup() const noexcept { return m_popup; }

        // === Tab navigation ===
        // Move focus to the next / previous tab-focusable node in tree pre-order (visible +
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

        // === A held direction (G7) ===
        // A gamepad's d-pad or stick held in a direction: a step at once, then, after a delay,
        // again and again, faster the longer it is held. Each step reaches the focused widget as an
        // arrow key does, so a slider moves its value and a list its selection, and only what it
        // leaves moves focus. (A keyboard's held arrow repeats by the
        // system's own key repeat.) The GUI's update (GUIContext::Update) ticks it.
        void PressDirection(NavDirection direction)
        {
            NoteInput();
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
        void Tick(f32 seconds)
        {
            if (!m_holding)
                return;
            m_heldFor += seconds;
            for (i32 guard = 0; guard < 8 && m_heldFor >= m_nextRepeatAt; ++guard)
            {
                StepDirection(m_heldDirection);
                // From the first interval down to the fastest, over the time held past the delay.
                const f32 ramp = core::Min(1.0f, (m_heldFor - kRepeatDelay) / kRepeatRampTime);
                m_nextRepeatAt += kRepeatSlowest + (kRepeatFastest - kRepeatSlowest) * ramp;
            }
        }
        static constexpr f32 kRepeatDelay = 0.4f;    // before the first repeat
        static constexpr f32 kRepeatSlowest = 0.16f; // the first repeats' interval
        static constexpr f32 kRepeatFastest = 0.05f; // once held long
        static constexpr f32 kRepeatRampTime = 1.2f; // from slowest to fastest

        // One step of a held direction, as an arrow key takes it: the focused widget first (a
        // slider steps its value, a list its selection), navigation with what it leaves.
        void StepDirection(NavDirection direction)
        {
            UseKeyboard();
            const KeyCode key = direction == NavDirection::Up     ? KeyCode::Up
                                : direction == NavDirection::Down ? KeyCode::Down
                                : direction == NavDirection::Left ? KeyCode::Left
                                                                  : KeyCode::Right;
            KeyEvent event(EventType::KeyDown, m_focusNode, static_cast<u32>(key), 0);
            if (m_focusNode)
                m_focusNode->HandleKeyDown(event);
            if (!event.Handled)
                (void)MoveFocus(direction);
        }


        // Clear any interaction refs pointing at `node` (call before removing/destroying it).
        void NotifyNodeRemoved(Node* node)
        {
            if (m_overNode == node)
                m_overNode = nullptr;
            if (m_downNode == node)
                m_downNode = nullptr;
            if (m_focusNode == node)
                m_focusNode = nullptr;
            if (m_dropTarget == node)
                m_dropTarget = nullptr;
            if (m_dragSource == node)
            {
                m_dragSource = nullptr;
                if (m_dragActive)
                    EndDrag();
            }
            if (m_modalRoot == node)
                m_modalRoot = nullptr;
        }

    private:
        [[nodiscard]] Node* HitTest(core::Float2 position) const
        {
            // Input sees the layout as it is now: a layout still waiting for the frame's pass runs
            // first (nothing to do when none waits).
            for (i32 round = 0; m_root != nullptr && round < 8 && (m_root->IsLayoutDirty() || m_root->HasLayoutDirtyDescendant());
                 ++round)
                (void)m_root->RunLayout();
            Node* hit = m_root ? m_root->OverFind(position) : nullptr;
            // A modal confines the pointer: hits outside the modal subtree are swallowed.
            if (m_modalRoot != nullptr && !IsInSubtree(hit, m_modalRoot))
                return nullptr;
            return hit;
        }

        // True if `node` is `ancestor` or a descendant of it.
        [[nodiscard]] static bool IsInSubtree(Node* node, Node* ancestor)
        {
            if (ancestor == nullptr)
                return false;
            for (Node* n = node; n != nullptr; n = n->GetParent())
                if (n == ancestor)
                    return true;
            return false;
        }

        // Nearest node (self or ancestor) that accepts the current drag payload; null if none.
        [[nodiscard]] Node* FindDropTarget(Node* hit) const
        {
            for (Node* n = hit; n != nullptr; n = n->GetParent())
                if (n->AcceptsDrop(m_dragPayload))
                    return n;
            return nullptr;
        }

        void EndDrag()
        {
            m_dragActive = false;
            m_dragSource = nullptr;
            m_dropTarget = nullptr;
            m_dragPayload = {};
        }

        // Gather tab-focusable nodes under `node` in pre-order (skipping invisible subtrees
        // and disabled nodes).
        static void CollectTabStops(Node* node, core::Array<Node*>& out)
        {
            if (node == nullptr || !node->IsVisible())
                return;
            if (node->IsTabFocusable() && node->IsEnabled())
                out.PushBack(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                CollectTabStops(node->GetChildAt(i), out);
        }

        bool MoveTabFocus(i32 direction)
        {
            // nav-next / nav-prev first (G7).
            if (m_focusNode != nullptr)
            {
                Node* root = m_modalRoot != nullptr ? m_modalRoot : m_root;
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
            core::Array<Node*> stops;
            // Confined to a modal, or to the containing scope (a dialog) focus is in.
            Node* within = m_modalRoot != nullptr ? m_modalRoot : m_root;
            if (Node* contain = ContainingScopeOf(m_focusNode))
                within = contain;
            CollectTabStops(within, stops);
            const usize count = stops.Size();
            if (count == 0)
                return false;

            // Find the current focus among the stops.
            usize current = count; // sentinel: "not in the list"
            for (usize i = 0; i < count; ++i)
                if (stops[i] == m_focusNode)
                {
                    current = i;
                    break;
                }

            usize next;
            if (current == count)
                next = (direction > 0) ? 0 : count - 1; // nothing focused -> first / last
            else
                next = (direction > 0) ? ((current + 1) % count) : ((current + count - 1) % count);

            SetFocusNode(stops[next]);
            return true;
        }

        Node* m_root;                      // non-owning (the SceneRoot owns this dispatcher)
        Node* m_modalRoot = nullptr;       // non-owning; confines input while a modal is open
        IClipboard* m_clipboard = nullptr; // non-owning system-clipboard adapter (optional)
        Node* m_overNode = nullptr;        // non-owning
        Node* m_downNode = nullptr;        // non-owning
        Node* m_focusNode = nullptr;       // non-owning
        Node* m_popup = nullptr;           // non-owning active popup
        Node* m_popupOwner = nullptr;      // non-owning opener (clicks on it don't dismiss)
        core::Function<void()> m_onPopupClose;
        core::Function<bool(Node*)> m_popupContains; // optional: extends the popup's "inside" set
        bool m_dragActive = false;                   // drag-and-drop in progress
        Node* m_dragSource = nullptr;                // non-owning
        Node* m_dropTarget = nullptr;                // non-owning current target
        DragPayload m_dragPayload;
        core::Float2 m_mousePos{0.0f, 0.0f};
        Observable m_focusChanged;   // told when focus moves
        core::Function<bool(const Keystroke&)> m_keystrokes; // the keymap (SceneRoot)
        core::Function<void()> m_onInput;                    // this root took input (GUIContext)
        void NoteInput()
        {
            if (m_onInput)
                m_onInput();
        }
        // The last input was a key or a d-pad move, not the pointer; true until the pointer is
        // used, so a menu opening with focus on its first button shows it (as games do).
        bool m_keyboardMode = true;
        bool m_holding = false;      // a direction held (PressDirection)
        NavDirection m_heldDirection = NavDirection::Down;
        f32 m_heldFor = 0.0f;
        f32 m_nextRepeatAt = 0.0f;

        // A key or d-pad move: the focus ring shows.
        void UseKeyboard()
        {
            m_keyboardMode = true;
            if (m_focusNode != nullptr)
                m_focusNode->SetFocusVisible(true);
        }
    };
}

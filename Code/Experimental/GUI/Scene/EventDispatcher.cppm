// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :event_dispatcher partition
//
// EventDispatcher: routes abstract input into the node tree. Ported from eepp's
// Scene::EventDispatcher, but the core is platform-agnostic: instead of hooking a window's
// Input, it exposes an Inject* API that a gui.shell bridge feeds with already-abstracted
// events (from InputSurface/InputRouter). It hit-tests via the root's OverFind, keeps the hover
// and press paths, the focus and the pointer capture, and starts each input event's route
// (Node::RouteEvent: capture, target, bubble). Its refs are non-owning Node*, dropped when a
// subtree leaves the tree (NotifySubtreeRemoved).

module;
#include "Core/Prelude.h"

export module experimental.gui:event_dispatcher;

import foundation.core; // Float2, StringView
import :node;
import :rect;
import :event;
import :clipboard;
import :navigation; // directional focus (G7)
import :popup_placement;
import :keymap;     // Keystroke, PadButton
import :reactive;   // Observable (focus, for commands)

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// How a popup closes (E5, after the web's popover kinds): `Auto` on a press outside it and on
    /// Escape, and when another auto popup that it does not hold opens; `Manual` only from code (a
    /// dialog); `Hint` (a tooltip) on any press or key, never closing the others, never taking focus
    /// or the pointer.
    enum class PopupKind : u8
    {
        Auto,
        Manual,
        Hint,
    };

    struct PopupOptions
    {
        PopupKind Kind = PopupKind::Auto;
        /// Everything below it is inert while it is open: no pointer, no keys, no bindings.
        bool Modal = false;
        /// Focus moves into it (its first focusable node, else itself if focusable) and comes back
        /// when it closes; it is a containing navigation scope meanwhile.
        bool TakesFocus = false;
        /// What it is placed against, again every layout pass: a node, else a rectangle in the
        /// root's space (a point for a context menu), else nothing (it keeps its own position).
        Node* Anchor = nullptr;
        Rect AnchorRect{};
        bool HasAnchorRect = false;
        PopupPlacement Placement{};
        /// Told when it closes, and why (after the `toggle` event).
        core::Function<void(PopupCloseReason)> OnClosed;
    };

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
        // Each returns whether the GUI used the input: a handler took it, or the pointer is over a
        // node that takes it (IsPointerOver), so a game host knows a click was the UI's.
        bool InjectMouseMove(core::Float2 position) { return PointerMove(position, PointerType::Mouse, 0); }
        bool InjectMouseDown(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            return PointerDown(position, button, modifiers, PointerType::Mouse, 0);
        }
        bool InjectMouseUp(core::Float2 position, MouseButton button, u32 modifiers = 0)
        {
            return PointerUp(position, button, modifiers, PointerType::Mouse, 0);
        }

        // === Touch === The first finger down drives the pointer as the mouse does (a press,
        // moves, a release, a click), its events marked as touch; other fingers are ignored. A
        // finger hovers only while it is down, so nothing stays hovered after a tap.
        bool InjectTouchDown(u64 finger, core::Float2 position)
        {
            if (m_touchActive)
                return false; // a second finger
            m_touchActive = true;
            m_touchFinger = finger;
            return PointerDown(position, MouseButton::Left, 0, PointerType::Touch, finger);
        }
        bool InjectTouchMove(u64 finger, core::Float2 position)
        {
            if (!m_touchActive || finger != m_touchFinger)
                return false;
            return PointerMove(position, PointerType::Touch, finger);
        }
        bool InjectTouchUp(u64 finger, core::Float2 position)
        {
            if (!m_touchActive || finger != m_touchFinger)
                return false;
            m_touchActive = false;
            const bool used = PointerUp(position, MouseButton::Left, 0, PointerType::Touch, finger);
            UpdateHoverPath(nullptr, position); // the finger lifted: nothing is under it
            return used;
        }
        [[nodiscard]] bool IsTouchActive() const noexcept { return m_touchActive; }

        /// Cancels the press in progress: the pressed node hears PointerCancel (a widget drops its
        /// drag), no release or click follows, and the press path and any capture clear. A
        /// scroller that starts panning under a finger calls it, so the row under the finger is
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

        bool InjectMouseWheel(core::Float2 position, core::Float2 delta)
        {
            Node* hit = HitTest(position);
            // Up from the node under the pointer: a scroller takes it, one at its end leaves it to
            // the one around it.
            bool used = false;
            if (hit != nullptr && hit->IsTreeEnabled())
            {
                WheelEvent event(hit, position, delta);
                used = hit->RouteEvent(event);
            }
            return used || IsPointerTaken(hit);
        }

        bool InjectKeyDown(u32 keyCode, u32 modifiers = 0)
        {
            NoteInput();
            ClosePopupsOfKind(PopupKind::Hint, PopupCloseReason::Code); // a key hides a tooltip
            // The focused widget, and its ancestors (capture, then bubble from the focus), first.
            if (RouteKey(EventType::KeyDown, keyCode, modifiers))
                return true;
            // Escape no widget took closes the topmost auto popup (E5).
            if (keyCode == static_cast<u32>(KeyCode::Escape) && modifiers == 0)
                for (usize i = m_popups.Size(); i-- > 0;)
                {
                    if (m_popups[i].Options.Kind == PopupKind::Hint)
                        continue;
                    if (m_popups[i].Options.Kind != PopupKind::Auto)
                        break; // a dialog's Escape is its own
                    ClosePopupAt(i, PopupCloseReason::Escape);
                    return true;
                }
            // Tab and Shift+Tab traverse focus when nothing took them.
            if (keyCode == static_cast<u32>(KeyCode::Tab) &&
                (modifiers & (~static_cast<u32>(KeyModShift))) == 0)
                return (modifiers & KeyModShift) != 0 ? FocusPrevious() : FocusNext();
            // Then the bindings (a keystroke bound to a command where the focus path's contexts allow).
            if (m_keystrokes && m_keystrokes(Keystroke{keyCode, modifiers, false}))
                return true;
            if (modifiers != 0)
                return false;
            // What is left is navigation's: an arrow moves focus, Return or Space activates (G7).
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

        /// Moves focus to the best widget in `direction` (inside a modal while one is open, within
        /// focus scopes); with nothing focused, to the GUI's entry. True when focus moved. The
        /// gamepad's d-pad and stick arrive here as the arrows do.
        bool MoveFocus(NavDirection direction)
        {
            UseKeyboard();
            Node* root = FocusScopeRoot();
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
        bool Activate() { return m_focusNode != nullptr && m_focusNode->IsTreeEnabled() && m_focusNode->HandleActivate(); }
        bool InjectKeyUp(u32 keyCode, u32 modifiers = 0) { return RouteKey(EventType::KeyUp, keyCode, modifiers); }
        bool InjectText(core::StringView text)
        {
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            TextInputEvent event(target, text);
            return target->RouteEvent(event);
        }
        /// An IME's composition so far (empty: it ended), to the focused node, which shows it in
        /// place; the committed text follows as InjectText. `cursor` is in characters.
        bool InjectComposition(core::StringView text, i32 cursor = 0)
        {
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            CompositionEvent event(target, text, cursor);
            return target->RouteEvent(event);
        }
        /// Where the focused node has text typed, in input space (as positions are injected), and
        /// the caret's x there: where a host's IME shows its candidates. False when it types none.
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
        /// Sends the pointer's moves and its release to `node` wherever the pointer goes, until
        /// released or the press ends (a press captures the pressed node on its own). A node that
        /// loses the capture (released, taken by another, or leaving the tree) is told
        /// (PointerCaptureLost).
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

        /// The pointer's look now: the nearest set `cursor` up from the node under the pointer
        /// (from the captured node while one holds the pointer, so a drag keeps its look), else
        /// Default. The host shows it (the shell bridge does).
        [[nodiscard]] Cursor GetCursor() const
        {
            Node* from = m_captureNode != nullptr ? m_captureNode : m_downNode != nullptr ? m_downNode : m_overNode;
            for (Node* n = from; n != nullptr; n = n->GetParent())
                if (n->GetCursor() != Cursor::Auto)
                    return n->GetCursor();
            return Cursor::Default;
        }

        /// Whether a pointer at `position` (device pixels) is over the GUI: over a node that takes
        /// the pointer (Node::TakesPointer), not only over a layout container. A game host asks
        /// before treating a click as its own.
        [[nodiscard]] bool IsPointerOver(core::Float2 position) const { return IsPointerTaken(HitTest(position)); }

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

        // === Popups: the top layer (E5) ===
        // A popup stays in its owner's subtree (its styles, its events' route) and is promoted into
        // this root's top layer while open: drawn after the whole tree in opening order, unclipped,
        // hit-tested first, placed against its anchor every layout pass. Opening an auto popup
        // closes the open ones that do not hold its owner (nesting comes from the tree); closing one
        // closes those it holds. A closed popup leaves the tree (its owner keeps it).

        /// Opens `popup` as a child of `owner` (attached there if it is not yet).
        void OpenPopup(Node& owner, Node& popup, PopupOptions options)
        {
            if (FindPopup(popup) >= 0)
                return;
            if (options.Kind != PopupKind::Manual)
            {
                // An auto popup closes the popups that do not hold its owner (a hint, the other hints).
                for (usize i = m_popups.Size(); i-- > 0;)
                {
                    const PopupEntry& e = m_popups[i];
                    if (e.Options.Modal || e.Options.Kind == PopupKind::Manual)
                        break; // what is below a dialog stays as it is
                    const bool closes = options.Kind == PopupKind::Hint ? e.Options.Kind == PopupKind::Hint
                                                                        : !IsInSubtree(&owner, e.Popup.Get());
                    if (closes && i < m_popups.Size())
                        ClosePopupAt(i, PopupCloseReason::Replaced);
                }
            }
            if (popup.GetParent() != &owner)
            {
                popup.RemoveFromParent();
                owner.AddChild(&popup);
            }
            PopupEntry entry;
            entry.Popup = core::RefPtr<Node>(&popup);
            entry.Size = popup.GetSize();
            const bool focusing = (options.TakesFocus || options.Modal) && options.Kind != PopupKind::Hint;
            if (focusing)
            {
                // The focus it takes, and the nodes holding it: given back on close to the first
                // that is still there.
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
            ToggleEvent opened(&popup, true, PopupCloseReason::Code);
            (void)popup.RouteEvent(opened);
            if (focusing && FindPopup(popup) >= 0)
            {
                core::Array<Node*> stops;
                CollectTabStops(&popup, stops);
                SetFocusNode(!stops.IsEmpty() ? stops[0] : (popup.IsTabFocusable() ? &popup : nullptr));
            }
        }
        /// Closes `popup` and the popups it holds. False when it is not open.
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
        /// Input went to another root of the GUI (another window): its auto popups and hints close,
        /// as a press outside them would close them (what is below a dialog stays).
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
        [[nodiscard]] usize PopupCount() const noexcept { return m_popups.Size(); }
        /// The open popups, bottom first.
        [[nodiscard]] Node* PopupAt(usize index) const { return index < m_popups.Size() ? m_popups[index].Popup.Get() : nullptr; }
        [[nodiscard]] const PopupOptions* PopupOptionsAt(usize index) const
        {
            return index < m_popups.Size() ? &m_popups[index].Options : nullptr;
        }
        [[nodiscard]] Node* TopPopup() const { return m_popups.IsEmpty() ? nullptr : m_popups.Back().Popup.Get(); }

        /// Places every anchored popup against its anchor within the root: on its side, flipped,
        /// shrunk and clamped (PlacePopup). Run every layout pass and before drawing, so a popup
        /// follows its anchor.
        void PlacePopups() const
        {
            if (m_root == nullptr)
                return;
            const Rect bounds{0.0f, 0.0f, m_root->GetSize().x, m_root->GetSize().y};
            for (const PopupEntry& e : m_popups)
            {
                Rect anchor{};
                if (e.Options.Anchor != nullptr)
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
                PlacedPopup placed = PlacePopup(anchor, e.Size, bounds, e.Options.Placement);
                Node& popup = *e.Popup;
                // On the device grid, as its arrange puts it, so the two agree.
                const f32 scale = core::Max(popup.GetPixelScale(), 0.01f);
                const auto snap = [scale](f32 v) { return core::Round(v * scale) / scale; };
                placed.Size = core::Float2{snap(placed.Position.x + placed.Size.x) - snap(placed.Position.x),
                                           snap(placed.Position.y + placed.Size.y) - snap(placed.Position.y)};
                placed.Position = core::Float2{snap(placed.Position.x), snap(placed.Position.y)};
                Node::StyleWriteScope writing(popup); // the layer's, not code's: no pins
                if (placed.Position.x != popup.GetPosition().x || placed.Position.y != popup.GetPosition().y)
                    popup.SetPosition(placed.Position);
                if (placed.Size.x != popup.GetSize().x || placed.Size.y != popup.GetSize().y)
                    popup.SetSize(placed.Size);
            }
        }
        /// A popup's size changed from code while it is open (a list grew): placed from it again.
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

        /// Draws the top layer (the root's draw does, after its tree): each popup in opening order,
        /// a modal one over its backdrop (its `::backdrop` part, over a dim of the whole root).
        void DrawPopups(DrawContext& ctx)
        {
            PlacePopups();
            for (usize i = 0; i < m_popups.Size(); ++i)
            {
                const core::RefPtr<Node> popup = m_popups[i].Popup; // held: drawing changes nothing, but be sure
                if (m_popups[i].Options.Modal && m_root != nullptr)
                {
                    const Rect all{0.0f, 0.0f, m_root->GetSize().x, m_root->GetSize().y};
                    popup->DrawBackdrop(ctx, all);
                }
                popup->Draw(ctx);
            }
        }
        /// Whether `node` takes input now: everything while no modal popup is open, else what is
        /// in the topmost modal one or above it.
        [[nodiscard]] bool IsInputLive(const Node* node) const { return IsLive(node); }
        /// Where focus, Tab and the arrows stay: the topmost popup taking focus, else the root.
        [[nodiscard]] Node* GetFocusScopeRoot() const { return FocusScopeRoot(); }

        /// The topmost popup under a world point, and in it the node there (hints pass the pointer
        /// through); null when the point is in none.
        [[nodiscard]] Node* FindInPopups(core::Float2 worldPoint) const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if (m_popups[i].Options.Kind != PopupKind::Hint)
                    if (Node* hit = m_popups[i].Popup->OverFind(worldPoint))
                        return hit;
            return nullptr;
        }

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
            m_time += seconds; // the clock click counting reads
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
            if (!RouteKey(EventType::KeyDown, static_cast<u32>(key), 0))
                (void)MoveFocus(direction);
        }


        // A subtree is leaving the tree (Node::RemoveChild tells the dispatcher before it
        // detaches): every ref into it is dropped, since the tree no longer keeps those nodes
        // alive. A sort that rebuilds a table's header frees the hovered header cell, and the next
        // mouse move would otherwise leave a freed node. A popup inside it is simply gone; a popup
        // whose opener leaves is dismissed while the opener still lives.
        void NotifySubtreeRemoved(Node& removed)
        {
            // Popups leaving with the subtree (their owner went): dropped, topmost first, focus
            // given back to what they took it from when that stays.
            for (usize i = m_popups.Size(); i-- > 0;)
                if (i < m_popups.Size() && IsInSubtree(m_popups[i].Popup.Get(), &removed))
                    DropPopupAt(i, PopupCloseReason::OwnerRemoved, &removed);
            PrunePath(m_hoverPath, removed, /*hover*/ true);
            m_overNode = m_hoverPath.IsEmpty() ? nullptr : m_hoverPath[m_hoverPath.Size() - 1];
            PrunePath(m_pressPath, removed, /*hover*/ false);
            if (IsInSubtree(m_downNode, &removed))
                m_downNode = nullptr;
            if (IsInSubtree(m_captureNode, &removed))
            {
                Node* lost = m_captureNode;
                m_captureNode = nullptr;
                SendCaptureLost(*lost); // told while it still lives
            }
            if (IsInSubtree(m_focusNode, &removed))
                SetFocusNode(nullptr);
            if (IsInSubtree(m_dropTarget, &removed))
                m_dropTarget = nullptr;
            if (IsInSubtree(m_dragSource, &removed))
            {
                m_dragSource = nullptr;
                if (m_dragActive)
                    EndDrag();
            }
        }

    private:
        [[nodiscard]] Node* HitTest(core::Float2 position) const
        {
            // Input sees the layout as it is now: a layout still waiting for the frame's pass runs
            // first (nothing to do when none waits).
            for (i32 round = 0; m_root != nullptr && round < 8 && (m_root->IsLayoutDirty() || m_root->HasLayoutDirtyDescendant());
                 ++round)
                m_root->ArrangeAgain();
            PlacePopups();
            Node* hit = FindInPopups(position);
            if (hit == nullptr && m_root != nullptr)
                hit = m_root->OverFind(position);
            // Below the topmost modal popup everything is inert: a hit there is swallowed.
            if (!IsLive(hit))
                return nullptr;
            return hit;
        }

        bool PointerMove(core::Float2 position, PointerType pointer, u64 id)
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
                return true;
            }
            Node* hit = HitTest(position);
            UpdateHoverPath(hit, position);
            // The captured node (explicitly, or the pressed one while a button is held) keeps the
            // moves, so a drag goes on when the pointer leaves it; otherwise the hovered node.
            Node* target = PointerTarget(hit);
            bool used = false;
            if (target != nullptr && target->IsTreeEnabled())
            {
                MouseEvent event(EventType::MouseMove, target, position);
                event.Pointer = pointer;
                event.PointerId = id;
                used = target->RouteEvent(event);
            }
            return used || IsPointerTaken(hit);
        }

        bool PointerDown(core::Float2 position, MouseButton button, u32 modifiers, PointerType pointer, u64 id)
        {
            NoteInput();
            // A pointer in use: the focus ring hides (shown again by the next key or d-pad move).
            m_keyboardMode = false;
            if (m_focusNode != nullptr)
                m_focusNode->SetFocusVisible(false);
            m_mousePos = position;
            Node* hit = HitTest(position);
            UpdateHoverPath(hit, position);
            // Any press hides a tooltip. A press closes the auto popups above the one it is in; one in
            // no popup is consumed with the rest of its gesture: closing a menu never acts on what
            // lies under it, and the opener pressed again closes without reopening (E5).
            ClosePopupsOfKind(PopupKind::Hint, PopupCloseReason::OutsidePress);
            if (DismissOutside(hit))
            {
                m_swallowing = true;
                m_downNode = nullptr;
                return true;
            }
            CountClick(position, button, pointer);
            m_downNode = hit;
            SetPressPath(hit);
            // Click-to-focus: the nearest widget that takes focus, at or above what was hit; none
            // (a click on a backdrop or a label) clears focus, as browsers do, so navigation
            // starts again from the GUI's entry rather than from the backdrop.
            Node* focusable = hit;
            while (focusable != nullptr && !(focusable->IsTabFocusable() && focusable->IsTreeEnabled()))
                focusable = focusable->GetParent();
            SetFocusNode(focusable);
            // A disabled widget still takes the press (nothing behind it does) but is not told.
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
            if (m_swallowing)
            {
                m_swallowing = false; // the end of a press that closed a popup: nothing more of it
                return true;
            }
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
            // A click (activation) for the left button only, on the nearest node holding both the
            // pressed node and the one released over (a press dragged onto a sibling clicks their
            // container, not either), as the DOM does. Right and middle presses still deliver
            // down and up (a context menu) but never activate a control. A cancelled press has no
            // pressed node, so no click.
            if (button == MouseButton::Left && m_downNode != nullptr && hit != nullptr)
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
                ReleasePointer(*m_captureNode); // a capture ends with the press, as the DOM's does
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
        // Keys go to the focused widget, or, with nothing focused, to the root (its capture and
        // bubble listeners still hear them); a disabled one hears nothing.
        [[nodiscard]] Node* KeyTarget() const
        {
            Node* target = m_focusNode != nullptr ? m_focusNode : m_root;
            if (!IsLive(target))
                target = FocusScopeRoot(); // below a modal: the keys are the modal's
            return target != nullptr && target->IsTreeEnabled() ? target : nullptr;
        }
        bool RouteKey(EventType type, u32 keyCode, u32 modifiers)
        {
            (void)HitTest(core::Float2{}); // keys see the layout as it is now, as the pointer does
            Node* target = KeyTarget();
            if (target == nullptr)
                return false;
            KeyEvent event(type, target, keyCode, modifiers);
            return target->RouteEvent(event);
        }
        // The nearest node holding both (each counting as holding itself); null in different trees.
        [[nodiscard]] static Node* CommonAncestor(Node* a, Node* b)
        {
            for (Node* n = a; n != nullptr; n = n->GetParent())
                if (IsInSubtree(b, n))
                    return n;
            return nullptr;
        }
        // The hover path follows the node under the pointer: the nodes that leave it are told
        // (deepest first) and the nodes that join it (outermost first), each one on its own, and
        // every node on it reads as hovered, so `:hover` holds for a card while a button inside
        // it is under the pointer, as CSS has it.
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
            // Held while their handlers run: an enter or a leave may change the tree.
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
        // The press path: the pressed node and the nodes holding it read as pressed (`:active`)
        // until the release.
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
        // A second press of the same button near in time and place is a double click, a third a
        // triple (positions in device pixels).
        void CountClick(core::Float2 position, MouseButton button, PointerType pointer)
        {
            const f32 dx = position.x - m_lastPressAt.x;
            const f32 dy = position.y - m_lastPressAt.y;
            const f32 slop = pointer == PointerType::Touch ? kTapSlop : kClickSlop; // a finger wobbles more
            const bool near = dx * dx + dy * dy <= slop * slop;
            const bool soon = m_time - m_lastPressTime <= kClickInterval;
            m_clickCount = (button == m_lastPressButton && near && soon && m_clickCount > 0) ? m_clickCount + 1 : 1;
            m_lastPressTime = m_time;
            m_lastPressAt = position;
            m_lastPressButton = button;
        }
        static constexpr f32 kClickInterval = 0.45f; // seconds between the presses of a double click
        static constexpr f32 kClickSlop = 6.0f;      // device pixels between them
        static constexpr f32 kTapSlop = 24.0f;       // device pixels between two taps

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

        struct PopupEntry
        {
            core::RefPtr<Node> Popup;
            PopupOptions Options;
            core::Float2 Size{0.0f, 0.0f};       // its own size, which placement shrinks from
            Array<Node::ElementRef> SavedFocus;  // the focus it took, then the nodes holding it
            NavScope ScopeBefore = NavScope::None;
        };
        Array<PopupEntry> m_popups; // the top layer, bottom first
        bool m_swallowing = false;  // a press that closed a popup: its release is consumed too
        // === The top layer's own ===
        [[nodiscard]] i32 FindPopup(const Node& popup) const
        {
            for (usize i = 0; i < m_popups.Size(); ++i)
                if (m_popups[i].Popup.Get() == &popup)
                    return static_cast<i32>(i);
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
        // Whether `node` takes input: anything while no modal is open; else what is in the topmost
        // modal popup or in a popup above it.
        [[nodiscard]] bool IsLive(const Node* node) const
        {
            const i32 modal = TopModal();
            if (modal < 0 || node == nullptr)
                return true;
            for (usize i = static_cast<usize>(modal); i < m_popups.Size(); ++i)
                if (IsInSubtree(node, m_popups[i].Popup.Get()))
                    return true;
            return false;
        }
        // Where focus moves and Tab and the arrows stay: the topmost popup taking focus, else the root.
        [[nodiscard]] Node* FocusScopeRoot() const
        {
            for (usize i = m_popups.Size(); i-- > 0;)
                if ((m_popups[i].Options.TakesFocus || m_popups[i].Options.Modal) && m_popups[i].Options.Kind != PopupKind::Hint)
                    return m_popups[i].Popup.Get();
            return m_root;
        }
        // Closes the auto popups above the one `hit` is in (all of them when it is in none), not
        // past a modal one. True when the press is to be consumed: it closed some and lands in no
        // popup that stays (a press in a parent menu closing its submenu goes on to the parent).
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
                    continue; // closed with the one holding it
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
        // Closes the popup at `index`: the popups it holds first, then it (told, out of the top
        // layer, out of the tree), focus given back while it was inside.
        void ClosePopupAt(usize index, PopupCloseReason reason)
        {
            Node* const closing = m_popups[index].Popup.Get();
            for (usize i = m_popups.Size(); i-- > index + 1;)
                if (i < m_popups.Size() && IsInSubtree(m_popups[i].Popup.Get(), closing))
                    ClosePopupAt(i, reason);
            const i32 at = FindPopup(*closing);
            if (at < 0)
                return; // closed meanwhile (a handler closed it)
            const core::RefPtr<Node> popup = m_popups[static_cast<usize>(at)].Popup;
            ToggleEvent closed(popup.Get(), false, reason);
            (void)popup->RouteEvent(closed);
            const i32 still = FindPopup(*popup);
            if (still < 0)
                return;
            PopupEntry entry = core::Move(m_popups[static_cast<usize>(still)]);
            m_popups.RemoveAt(static_cast<usize>(still));
            const bool focusInside = m_focusNode == nullptr || IsInSubtree(m_focusNode, popup.Get());
            Demote(*popup, entry);
            popup->RemoveFromParent(); // a closed popup leaves the tree; its owner keeps it
            if (focusInside && !entry.SavedFocus.IsEmpty())
                RestoreFocus(entry, nullptr);
            if (entry.Options.OnClosed)
                entry.Options.OnClosed(reason);
        }
        // A popup leaving with a subtree: out of the stack, told, focus given back when it can be.
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
        // Focus back to what the popup took it from, else the nearest holder of that still here,
        // enabled and focusable; none: no focus.
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

        // Gather tab-focusable nodes under `node` in pre-order (skipping invisible subtrees
        // and disabled nodes).
        static void CollectTabStops(Node* node, core::Array<Node*>& out)
        {
            if (node == nullptr || !node->IsVisible())
                return;
            if (node->IsTabFocusable() && node->IsTreeEnabled())
                out.PushBack(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                CollectTabStops(node->GetChildAt(i), out);
        }

        bool MoveTabFocus(i32 direction)
        {
            // nav-next / nav-prev first (G7).
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
            core::Array<Node*> stops;
            // Confined to a modal, or to the containing scope (a dialog) focus is in.
            Node* within = FocusScopeRoot();
            if (Node* contain = ContainingScopeOf(m_focusNode); contain != nullptr && IsInSubtree(contain, within))
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
        IClipboard* m_clipboard = nullptr; // non-owning system-clipboard adapter (optional)
        Node* m_overNode = nullptr;        // non-owning: the deepest node of the hover path
        Array<Node*> m_hoverPath;          // non-owning, root first: every node under the pointer
        Array<Node*> m_pressPath;          // non-owning: the pressed node and the nodes holding it
        Node* m_captureNode = nullptr;     // non-owning: explicit pointer capture (CapturePointer)
        f32 m_time = 0.0f;                 // seconds, advanced by Tick (click counting)
        f32 m_lastPressTime = -1000.0f;
        core::Float2 m_lastPressAt{0.0f, 0.0f};
        MouseButton m_lastPressButton = MouseButton::Left;
        u32 m_clickCount = 0;
        bool m_touchActive = false; // the first finger is down (it drives the pointer)
        u64 m_touchFinger = 0;
        Node* m_downNode = nullptr;        // non-owning
        Node* m_focusNode = nullptr;       // non-owning
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

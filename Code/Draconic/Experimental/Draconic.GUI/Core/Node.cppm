// Draconic GUI - :node partition
//
// Node: the retained-mode scene-graph node - the tree content base (eepp Scene::Node).
// Ported from eepp include/eepp/scene/node.hpp, adapted to Draconic: Object +
// Transformable bases (Cast<T> downcasts); the raw-pointer intrusive sibling list becomes
// an owning Array<RefPtr<Node>> children with a non-owning parent back-pointer. Draws its
// subtree through the DrawContext/VG seam (eepp's nodeDraw + matrix/clip, on VG).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:node;

import draconic.foundation; // Object, RefPtr, Array, Move, Float2
import :rect;
import :transform2d;
import :transformable;
import :event;
import :draw_context;
import :drawable;
import :control_state;
import :action;
import :action_manager;
import :mutation_queue;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class EventDispatcher; // owned by the SceneNode; reached via GetEventDispatcher()

    class Node : public Object, public Transformable
    {
        DRACONIC_OBJECT(Node, Object)
    public:
        static constexpr usize kInvalidIndex = static_cast<usize>(-1);

        Node() = default;

        // === Tree query ===
        [[nodiscard]] Node* GetParent() const noexcept { return m_parent; }
        [[nodiscard]] usize ChildCount() const noexcept { return m_children.Size(); }
        [[nodiscard]] Node* GetChildAt(usize index) const
        {
            return index < m_children.Size() ? m_children[index].Get() : nullptr;
        }
        [[nodiscard]] Node* GetFirstChild() const
        {
            return m_children.Size() ? m_children[0].Get() : nullptr;
        }
        [[nodiscard]] Node* GetLastChild() const
        {
            return m_children.Size() ? m_children[m_children.Size() - 1].Get() : nullptr;
        }
        [[nodiscard]] usize GetChildIndex(const Node* child) const
        {
            for (usize i = 0; i < m_children.Size(); ++i)
                if (m_children[i].Get() == child)
                    return i;
            return kInvalidIndex;
        }
        [[nodiscard]] bool HasChild(const Node* child) const
        {
            return GetChildIndex(child) != kInvalidIndex;
        }

        [[nodiscard]] Node* GetNextSibling() const
        {
            if (m_parent == nullptr)
                return nullptr;
            const usize i = m_parent->GetChildIndex(this);
            return (i != kInvalidIndex && i + 1 < m_parent->m_children.Size())
                       ? m_parent->m_children[i + 1].Get()
                       : nullptr;
        }
        [[nodiscard]] Node* GetPrevSibling() const
        {
            if (m_parent == nullptr)
                return nullptr;
            const usize i = m_parent->GetChildIndex(this);
            return (i != kInvalidIndex && i > 0) ? m_parent->m_children[i - 1].Get() : nullptr;
        }

        // === Tree mutation === (owning: the tree holds a RefPtr to each child)
        virtual Node* AddChild(Node* child)
        {
            if (child == nullptr || child == this || child->m_parent == this)
                return this;
            RefPtr<Node> keepAlive(child); // survive reparenting off the old parent
            if (child->m_parent != nullptr)
                child->m_parent->RemoveChild(child);
            child->m_parent = this;
            m_children.PushBack(foundation::Move(keepAlive));
            child->HandleParentChange();
            OnChildrenChanged();
            Invalidate();
            return this;
        }

        Node* AddChildAt(Node* child, usize index)
        {
            AddChild(child);
            SetChildIndex(child, index);
            return this;
        }

        void RemoveChild(Node* child)
        {
            const usize index = GetChildIndex(child);
            if (index == kInvalidIndex)
                return;
            RefPtr<Node> keepAlive = m_children[index]; // hold across detach + callback
            child->m_parent = nullptr;
            m_children.RemoveAt(index);
            child->HandleParentChange();
            OnChildrenChanged();
            Invalidate();
        }

        void RemoveAllChildren()
        {
            while (m_children.Size() != 0)
                RemoveChild(m_children[m_children.Size() - 1].Get());
        }

        void RemoveFromParent()
        {
            if (m_parent != nullptr)
                m_parent->RemoveChild(this);
        }

        // The topmost ancestor (the SceneNode root, once one is installed).
        [[nodiscard]] Node* GetRootNode()
        {
            Node* n = this;
            while (n->m_parent != nullptr)
                n = n->m_parent;
            return n;
        }

        // === Z-order (draw/hit order = child index; last = topmost) ===
        void SetChildIndex(Node* child, usize newIndex)
        {
            const usize cur = GetChildIndex(child);
            if (cur == kInvalidIndex)
                return;
            if (newIndex >= m_children.Size())
                newIndex = m_children.Size() - 1;
            if (cur == newIndex)
                return;
            RefPtr<Node> ref = m_children[cur];
            m_children.RemoveAt(cur);
            m_children.Insert(newIndex, foundation::Move(ref));
            Invalidate();
        }
        void ToFront()
        {
            if (m_parent != nullptr)
                m_parent->SetChildIndex(this, m_parent->m_children.Size());
        }
        void ToBack()
        {
            if (m_parent != nullptr)
                m_parent->SetChildIndex(this, 0);
        }

        // === Size / bounds ===
        void SetSize(foundation::Float2 size)
        {
            const foundation::Float2 clamped = ClampToSizeConstraints(size);
            if (clamped == m_size)
                return;
            m_size = clamped;
            HandleSizeChange();
        }
        [[nodiscard]] foundation::Float2 GetSize() const noexcept { return m_size; }
        [[nodiscard]] Rect GetLocalBounds() const noexcept
        {
            return Rect{0.0f, 0.0f, m_size.x, m_size.y};
        }

        // Min/max size constraints (CSS min/max-width/height). A negative max component means
        // "unbounded" on that axis. SetSize clamps to [min, max]; changing a bound re-clamps.
        void SetMinSize(foundation::Float2 minSize)
        {
            m_minSize = minSize;
            SetSize(m_size);
        }
        void SetMaxSize(foundation::Float2 maxSize)
        {
            m_maxSize = maxSize;
            SetSize(m_size);
        }
        [[nodiscard]] foundation::Float2 GetMinSize() const noexcept { return m_minSize; }
        [[nodiscard]] foundation::Float2 GetMaxSize() const noexcept { return m_maxSize; }

        // World transform accumulates the parent chain (parentWorld * local).
        [[nodiscard]] Transform2D GetWorldTransform() const
        {
            return m_parent != nullptr ? (m_parent->GetWorldTransform() * GetTransform())
                                       : GetTransform();
        }
        [[nodiscard]] foundation::Float2 ConvertToWorldSpace(foundation::Float2 nodePoint) const
        {
            return GetWorldTransform().TransformPoint(nodePoint);
        }
        [[nodiscard]] foundation::Float2 ConvertToNodeSpace(foundation::Float2 worldPoint) const
        {
            return GetWorldTransform().GetInverse().TransformPoint(worldPoint);
        }
        [[nodiscard]] foundation::Float2 GetScreenPosition() const
        {
            return ConvertToWorldSpace(foundation::Float2{0.0f, 0.0f});
        }
        [[nodiscard]] Rect GetScreenBounds() const
        {
            return GetWorldTransform().TransformRect(GetLocalBounds());
        }

        // === Visibility / enabled / alpha ===
        void SetVisible(bool visible)
        {
            if (visible == m_visible)
                return;
            m_visible = visible;
            HandleVisibilityChange();
        }
        [[nodiscard]] bool IsVisible() const noexcept { return m_visible; }
        [[nodiscard]] bool IsTreeVisible() const
        {
            return m_visible && (m_parent == nullptr || m_parent->IsTreeVisible());
        }
        void SetEnabled(bool enabled)
        {
            if (enabled == m_enabled)
                return;
            m_enabled = enabled;
            HandleEnabledChange();
        }
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        void SetAlpha(f32 alpha)
        {
            m_alpha = alpha;
            Invalidate();
        }
        [[nodiscard]] f32 GetAlpha() const noexcept { return m_alpha; }

        // === Hit testing ===
        // When false, this node is transparent to the pointer: OverFind skips it (clicks fall
        // through to whatever is behind), though it still draws. Useful for decorative overlays
        // (a title-bar caption, an image over a draggable area). Children are still hit-tested.
        void SetHitTestVisible(bool visible) noexcept { m_hitTestVisible = visible; }
        [[nodiscard]] bool IsHitTestVisible() const noexcept { return m_hitTestVisible; }

        [[nodiscard]] virtual bool PointInside(foundation::Float2 worldPoint) const
        {
            if (!m_hitTestVisible)
                return false;
            const foundation::Float2 p = ConvertToNodeSpace(worldPoint);
            return p.x >= 0.0f && p.x <= m_size.x && p.y >= 0.0f && p.y <= m_size.y;
        }
        // Topmost visible descendant (or self) under a world point; nullptr if none.
        [[nodiscard]] virtual Node* OverFind(foundation::Float2 worldPoint)
        {
            if (!m_visible)
                return nullptr;
            for (usize i = m_children.Size(); i-- > 0;)
                if (Node* hit = m_children[i]->OverFind(worldPoint))
                    return hit;
            return PointInside(worldPoint) ? this : nullptr;
        }

        // === Drawing ===
        void SetBackground(RefPtr<Drawable> background)
        {
            m_background = foundation::Move(background);
            Invalidate();
        }
        [[nodiscard]] Drawable* GetBackground() const noexcept { return m_background.Get(); }
        void SetForeground(RefPtr<Drawable> foreground)
        {
            m_foreground = foundation::Move(foreground);
            Invalidate();
        }
        [[nodiscard]] Drawable* GetForeground() const noexcept { return m_foreground.Get(); }

        void SetClipChildren(bool clip) noexcept { m_clipChildren = clip; }
        [[nodiscard]] bool ClipsChildren() const noexcept { return m_clipChildren; }

        // The visual state drawables render in (skins/StateList). Base: enabled-driven.
        [[nodiscard]] virtual ControlState GetControlState() const
        {
            return m_enabled ? ControlState::Normal : ControlState::Disabled;
        }

        // Draw this node and its subtree through the DrawContext, applying the node's
        // transform + opacity + optional child clip. Order: background -> OnDraw ->
        // children (back-to-front) -> foreground.
        void Draw(DrawContext& ctx)
        {
            if (!m_visible || m_alpha <= 0.0f)
                return;

            ctx.Save();
            ctx.ConcatTransform(GetTransform());
            const bool pushedOpacity = m_alpha < 1.0f;
            if (pushedOpacity)
                ctx.PushOpacity(m_alpha);

            const Rect local = GetLocalBounds();
            const ControlState state = GetControlState();

            if (m_background)
                m_background->Draw(ctx, local, state);
            OnDraw(ctx, local);

            if (m_clipChildren)
                ctx.PushClip(local);
            for (const RefPtr<Node>& child : m_children)
                child->Draw(ctx);
            if (m_clipChildren)
                ctx.PopClip();

            if (m_foreground)
                m_foreground->Draw(ctx, local, state);

            if (pushedOpacity)
                ctx.PopOpacity();
            ctx.Restore();
        }

        // Subclass-drawn content, between background and children.
        virtual void OnDraw(DrawContext& ctx, const Rect& localBounds)
        {
            (void)ctx;
            (void)localBounds;
        }

        // === Coordinator lookup + lifecycle ===
        // The SceneNode's ActionManager / MutationQueue, found by walking to the root.
        // Base returns the parent's; SceneNode overrides to return its own. Null if the
        // node is not yet attached under a SceneNode.
        [[nodiscard]] virtual ActionManager* GetActionManager()
        {
            return m_parent ? m_parent->GetActionManager() : nullptr;
        }
        [[nodiscard]] virtual MutationQueue* GetMutationQueue()
        {
            return m_parent ? m_parent->GetMutationQueue() : nullptr;
        }
        // The SceneNode's EventDispatcher (pointer walk to the root; SceneNode returns its own).
        [[nodiscard]] virtual EventDispatcher* GetEventDispatcher()
        {
            return m_parent ? m_parent->GetEventDispatcher() : nullptr;
        }

        // Programmatic focus (routes through the dispatcher; defined in the impl unit).
        void RequestFocus();
        void ReleaseFocus();

        // Run an action on this node (targets it, hands it to the SceneNode's manager).
        // No-op (the action is dropped) if the node is not attached under a SceneNode.
        void RunAction(RefPtr<Action> action)
        {
            if (!action)
                return;
            action->SetTarget(this);
            if (ActionManager* manager = GetActionManager())
                manager->AddAction(foundation::Move(action));
        }

        // Detach this node from its tree at the next safe sync point (SceneNode drain).
        // Falls back to immediate removal if there is no coordinator.
        void Close()
        {
            if (MutationQueue* queue = GetMutationQueue())
            {
                RefPtr<Node> self(this); // keep alive until the deferred op runs
                queue->Enqueue([self]() { self.Get()->RemoveFromParent(); });
            }
            else
            {
                RemoveFromParent();
            }
        }

        // === Input dispatch === (called by the EventDispatcher; each runs the virtual
        // On* hook then fires the typed event to listeners)
        void HandleMouseEnter(const MouseEvent& e)
        {
            OnMouseEnter(e);
            SendEvent(e);
        }
        void HandleMouseLeave(const MouseEvent& e)
        {
            OnMouseLeave(e);
            SendEvent(e);
        }
        void HandleMouseMove(const MouseEvent& e)
        {
            OnMouseMove(e);
            SendEvent(e);
        }
        void HandleMouseDown(const MouseEvent& e)
        {
            OnMouseDown(e);
            SendEvent(e);
        }
        void HandleMouseUp(const MouseEvent& e)
        {
            OnMouseUp(e);
            SendEvent(e);
        }
        void HandleMouseClick(const MouseEvent& e)
        {
            OnMouseClick(e);
            SendEvent(e);
        }
        void HandleMouseWheel(const WheelEvent& e)
        {
            OnMouseWheel(e);
            SendEvent(e);
        }
        void HandleKeyDown(const KeyEvent& e)
        {
            OnKeyDown(e);
            SendEvent(e);
        }
        void HandleKeyUp(const KeyEvent& e)
        {
            OnKeyUp(e);
            SendEvent(e);
        }
        void HandleTextInput(const TextInputEvent& e)
        {
            OnTextInput(e);
            SendEvent(e);
        }
        void HandleFocusGained()
        {
            m_focused = true;
            OnFocusGained();
            SendEvent(Event(EventType::FocusGained, this));
        }
        void HandleFocusLost()
        {
            m_focused = false;
            OnFocusLost();
            SendEvent(Event(EventType::FocusLost, this));
        }

        [[nodiscard]] bool IsFocused() const noexcept { return m_focused; }

        // === Drag-and-drop target ===
        // Set a predicate deciding which payloads this node accepts; when it returns true the
        // dispatcher delivers the drag-enter/over/leave and drop here (nearest accepting
        // ancestor of the cursor wins). Each also fires the matching DragEvent to listeners.
        void SetDropAcceptor(foundation::Function<bool(const DragPayload&)> predicate)
        {
            m_dropAcceptor = foundation::Move(predicate);
        }
        [[nodiscard]] bool AcceptsDrop(const DragPayload& payload) const
        {
            return m_dropAcceptor ? m_dropAcceptor(payload) : false;
        }
        void HandleDragEnter(const DragPayload& p)
        {
            OnDragEnter(p);
            SendEvent(DragEvent(EventType::DragEnter, this, p));
        }
        void HandleDragOver(const DragPayload& p)
        {
            OnDragOver(p);
            SendEvent(DragEvent(EventType::DragOver, this, p));
        }
        void HandleDragLeave(const DragPayload& p)
        {
            OnDragLeave(p);
            SendEvent(DragEvent(EventType::DragLeave, this, p));
        }
        void HandleDrop(const DragPayload& p)
        {
            OnDrop(p);
            SendEvent(DragEvent(EventType::Drop, this, p));
        }

        // True if this node edits text and wants the platform's text-input (IME) enabled
        // while it holds focus. The gui.shell bridge reads the focused node's answer and
        // drives the window's StartTextInput/StopTextInput accordingly. Default: false.
        [[nodiscard]] virtual bool WantsTextInput() const { return false; }

        // Optional hover-tooltip text. A TooltipManager reads the hovered node's answer and
        // shows a tooltip after a delay. Empty (the default) means no tooltip.
        [[nodiscard]] virtual foundation::StringView GetTooltipText() const { return {}; }

        // True if this node consumes the mouse wheel (e.g. a ScrollView). The dispatcher
        // bubbles a wheel event from the hit node up the ancestor chain to the nearest node
        // that answers true, so scrolling works while hovering the scrolled content. Default:
        // false.
        [[nodiscard]] virtual bool WantsWheel() const { return false; }

        // Tab-navigation: whether Tab / Shift+Tab focus traversal can land on this node
        // (distinct from click focus, which targets whatever is hit). Interactive widgets
        // opt in; static nodes (labels, layouts) stay false. Modeled on eepp's
        // UI_TAB_FOCUSABLE flag.
        void SetTabFocusable(bool focusable) noexcept { m_tabFocusable = focusable; }
        [[nodiscard]] bool IsTabFocusable() const noexcept { return m_tabFocusable; }

        // === Invalidation === (marks self + ancestors until an already-dirty one)
        void Invalidate()
        {
            m_needsRedraw = true;
            for (Node* p = m_parent; p != nullptr && !p->m_needsRedraw; p = p->m_parent)
                p->m_needsRedraw = true;
        }
        [[nodiscard]] bool NeedsRedraw() const noexcept { return m_needsRedraw; }
        void ClearNeedsRedraw() noexcept { m_needsRedraw = false; }

        // === Transformable overrides: geometry changes invalidate + notify ===
        void SetPosition(foundation::Float2 position) override
        {
            Transformable::SetPosition(position);
            HandlePositionChange();
        }
        void SetRotation(f32 radians) override
        {
            Transformable::SetRotation(radians);
            Invalidate();
        }
        void SetScale(foundation::Float2 factors) override
        {
            Transformable::SetScale(factors);
            Invalidate();
        }

        // === Event listeners ===
        u32 AddEventListener(EventType type, EventCallback callback)
        {
            const u32 id = ++m_nextListenerId;
            m_listeners.PushBack(Listener{id, type, foundation::Move(callback)});
            return id;
        }
        void RemoveEventListener(u32 id)
        {
            for (usize i = 0; i < m_listeners.Size(); ++i)
                if (m_listeners[i].Id == id)
                {
                    m_listeners.RemoveAt(i);
                    return;
                }
        }
        void SendEvent(const Event& event)
        {
            for (Listener& l : m_listeners)
                if (l.Type == event.Type)
                    l.Callback(event);
        }

        // === Change hooks (subclass overrides; the Handle* wrappers fire the event too) ===
        virtual void OnPositionChange() {}
        virtual void OnSizeChange() {}
        virtual void OnVisibilityChange() {}
        virtual void OnEnabledChange() {}
        virtual void OnParentChange() {}
        // Called on this node after a child is added or removed (containers re-layout here).
        virtual void OnChildrenChanged() {}

    protected:
        // Input handler hooks (override in subclasses; the Handle* wrappers call these).
        virtual void OnMouseEnter(const MouseEvent&) {}
        virtual void OnMouseLeave(const MouseEvent&) {}
        virtual void OnMouseMove(const MouseEvent&) {}
        virtual void OnMouseDown(const MouseEvent&) {}
        virtual void OnMouseUp(const MouseEvent&) {}
        virtual void OnMouseClick(const MouseEvent&) {}
        virtual void OnMouseWheel(const WheelEvent&) {}
        virtual void OnKeyDown(const KeyEvent&) {}
        virtual void OnKeyUp(const KeyEvent&) {}
        virtual void OnTextInput(const TextInputEvent&) {}
        virtual void OnFocusGained() {}
        virtual void OnFocusLost() {}
        virtual void OnDragEnter(const DragPayload&) {}
        virtual void OnDragOver(const DragPayload&) {}
        virtual void OnDragLeave(const DragPayload&) {}
        virtual void OnDrop(const DragPayload&) {}

        void HandlePositionChange()
        {
            OnPositionChange();
            Invalidate();
            SendEvent(Event(EventType::PositionChanged, this));
        }
        void HandleSizeChange()
        {
            OnSizeChange();
            Invalidate();
            SendEvent(Event(EventType::SizeChanged, this));
        }

        // Clamp a size to [m_minSize, m_maxSize] per axis (a negative max = unbounded).
        [[nodiscard]] foundation::Float2 ClampToSizeConstraints(foundation::Float2 size) const noexcept
        {
            foundation::Float2 out = size;
            if (out.x < m_minSize.x)
                out.x = m_minSize.x;
            if (out.y < m_minSize.y)
                out.y = m_minSize.y;
            if (m_maxSize.x >= 0.0f && out.x > m_maxSize.x)
                out.x = m_maxSize.x;
            if (m_maxSize.y >= 0.0f && out.y > m_maxSize.y)
                out.y = m_maxSize.y;
            return out;
        }
        void HandleVisibilityChange()
        {
            OnVisibilityChange();
            Invalidate();
            SendEvent(Event(EventType::VisibilityChanged, this));
        }
        void HandleEnabledChange()
        {
            OnEnabledChange();
            Invalidate();
            SendEvent(Event(EventType::EnabledChanged, this));
        }
        void HandleParentChange()
        {
            OnParentChange();
            Invalidate();
            SendEvent(Event(EventType::ParentChanged, this));
        }

        struct Listener
        {
            u32 Id;
            EventType Type;
            EventCallback Callback;
        };

        Node* m_parent = nullptr;       // non-owning back-pointer
        Array<RefPtr<Node>> m_children; // owning
        Array<Listener> m_listeners;
        Function<bool(const DragPayload&)>
            m_dropAcceptor; // drop-target predicate (empty = rejects)
        RefPtr<Drawable> m_background;
        RefPtr<Drawable> m_foreground;
        foundation::Float2 m_size{0.0f, 0.0f};
        foundation::Float2 m_minSize{0.0f, 0.0f};   // CSS min-width/height
        foundation::Float2 m_maxSize{-1.0f, -1.0f}; // CSS max-width/height (<0 = unbounded)
        f32 m_alpha = 1.0f;
        bool m_visible = true;
        bool m_enabled = true;
        bool m_needsRedraw = true;
        bool m_clipChildren = false;
        bool m_focused = false;
        bool m_tabFocusable = false;
        bool m_hitTestVisible = true;
        u32 m_nextListenerId = 0;
    };

    DRACONIC_DEFINE_OBJECT(Node, "draconic::gui")
}

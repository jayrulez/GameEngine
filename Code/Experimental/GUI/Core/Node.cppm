// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :node partition
//
// Node: the retained-mode scene-graph node - the tree content base (eepp Scene::Node).
// Ported from eepp include/eepp/scene/node.hpp, adapted to our Core: Object +
// Transformable bases (Cast<T> downcasts); the raw-pointer intrusive sibling list becomes
// an owning Array<RefPtr<Node>> children with a non-owning parent back-pointer. Draws its
// subtree through the DrawContext/VG seam (eepp's nodeDraw + matrix/clip, on VG).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:node;

import foundation.core; // Object, RefPtr, Array, Move, Float2
import foundation.script; // IScriptDelegate (the script surface's event handlers)
import foundation.vg;     // PathBuilder, ShapeBuilder (rounded clips), layers
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
import :node_box;
import :reactive; // Effect, EffectScheduler (bindings)
import :data_model; // DataModel (BindData)
import :css_values;  // LengthValue
import :style_value; // TransformFunction

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    /// What a focus scope does when navigation reaches its edge (G7 of Specs/vg-and-gui-lab.md):
    /// not a scope; pass to the neighbours outside it (the default for a scope); wrap around to its
    /// other side (menus, carousels); or contain focus (a dialog).
    enum class NavScope : u8
    {
        None,
        Pass,
        Wrap,
        Contain,
    };

    class EventDispatcher; // owned by the SceneRoot; reached via GetEventDispatcher()
    class ElementSet;      // what a GUI may create (:element_types); its SceneRoot holds one
    class SceneRoot;       // the root of a GUI (:scene_root)
    class Node;

    /// One GUI's element table (G1 of Specs/vg-and-gui-lab.md): what a script's handle to a node
    /// resolves through, a slot and a generation, so the tree owns its nodes and a script only
    /// ever holds them weakly. Each SceneRoot owns one, so two GUIs never share slots. Handles
    /// retain the table (an anchored resolve-mode Variant): when its SceneRoot goes, the table is
    /// closed and every handle into it reads as gone, never as freed memory.
    class ElementTable : public RefCounted
    {
    public:
        /// A slot for `node` (1-based), reusing a freed one under a new generation.
        u32 Take(Node& node)
        {
            u32 slot = 0;
            if (!m_free.IsEmpty())
            {
                slot = m_free.Back();
                m_free.PopBack();
            }
            else
            {
                m_slots.PushBack(Slot{});
                slot = static_cast<u32>(m_slots.Size());
            }
            m_slots[slot - 1].node = &node;
            return slot;
        }
        /// The node is gone: its handles stop resolving and the slot is free for another.
        void Free(u32 slot) noexcept
        {
            if (slot == 0 || slot > m_slots.Size())
                return;
            Slot& entry = m_slots[slot - 1];
            entry.node = nullptr;
            ++entry.generation;
            if (!m_closed)
                m_free.PushBack(slot);
        }
        /// The node in `slot` if `generation` is still its, else null.
        [[nodiscard]] Node* Resolve(u32 slot, u32 generation) const noexcept
        {
            if (m_closed || slot == 0 || slot > m_slots.Size())
                return nullptr;
            const Slot& entry = m_slots[slot - 1];
            return entry.generation == generation ? entry.node : nullptr;
        }
        [[nodiscard]] u32 Generation(u32 slot) const noexcept
        {
            return slot != 0 && slot <= m_slots.Size() ? m_slots[slot - 1].generation : 0;
        }
        /// The owning GUI is going: every handle into this table reads as gone from now on.
        void Close() noexcept { m_closed = true; }
        [[nodiscard]] bool IsClosed() const noexcept { return m_closed; }

    private:
        struct Slot
        {
            Node* node = nullptr;
            u32 generation = 1;
        };
        Array<Slot> m_slots;
        Array<u32> m_free;
        bool m_closed = false;
    };

    class Node : public Object, public Transformable
    {
        RTTI_OBJECT(Node, Object)
    public:
        static constexpr usize kInvalidIndex = static_cast<usize>(-1);

        /// Where a weak reference to a node resolves: its GUI's table, a slot and a generation.
        struct ElementRef
        {
            ElementTable* table = nullptr;
            u32 slot = 0;
            u32 generation = 0;
        };

        Node() = default;
        ~Node() override
        {
            if (m_elementTable)
                m_elementTable->Free(m_elementSlot);
        }

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
            m_children.PushBack(core::Move(keepAlive));
            child->MarkStyleDirty(true); // its selectors may match differently here
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

        /// The root of the GUI this node is in, or null when it is in none (a detached subtree:
        /// GetRootNode gives its topmost node).
        [[nodiscard]] SceneRoot* GetSceneRoot();

        // The topmost ancestor (the SceneRoot, when the node is in a GUI).
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
            m_children.Insert(newIndex, core::Move(ref));
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
        void SetSize(core::Float2 size)
        {
            const core::Float2 clamped = ClampToSizeConstraints(size);
            if (clamped == m_size)
                return;
            m_size = clamped;
            HandleSizeChange();
        }
        [[nodiscard]] core::Float2 GetSize() const noexcept { return m_size; }
        [[nodiscard]] Rect GetLocalBounds() const noexcept
        {
            return Rect{0.0f, 0.0f, m_size.x, m_size.y};
        }

        // Min/max size constraints (CSS min/max-width/height). A negative max component means
        // "unbounded" on that axis. SetSize clamps to [min, max]; changing a bound re-clamps.
        void SetMinSize(core::Float2 minSize)
        {
            m_minSize = minSize;
            SetSize(m_size);
        }
        void SetMaxSize(core::Float2 maxSize)
        {
            m_maxSize = maxSize;
            SetSize(m_size);
        }
        [[nodiscard]] core::Float2 GetMinSize() const noexcept { return m_minSize; }
        [[nodiscard]] core::Float2 GetMaxSize() const noexcept { return m_maxSize; }

        // World transform accumulates the parent chain (parentWorld * local).
        [[nodiscard]] Transform2D GetWorldTransform() const
        {
            return m_parent != nullptr ? (m_parent->GetWorldTransform() * GetTransform())
                                       : GetTransform();
        }
        [[nodiscard]] core::Float2 ConvertToWorldSpace(core::Float2 nodePoint) const
        {
            return GetWorldTransform().TransformPoint(nodePoint);
        }
        [[nodiscard]] core::Float2 ConvertToNodeSpace(core::Float2 worldPoint) const
        {
            return GetWorldTransform().GetInverse().TransformPoint(worldPoint);
        }
        [[nodiscard]] core::Float2 GetScreenPosition() const
        {
            return ConvertToWorldSpace(core::Float2{0.0f, 0.0f});
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

        [[nodiscard]] virtual bool PointInside(core::Float2 worldPoint) const
        {
            if (!m_hitTestVisible)
                return false;
            const core::Float2 p = ConvertToNodeSpace(worldPoint);
            return p.x >= 0.0f && p.x <= m_size.x && p.y >= 0.0f && p.y <= m_size.y;
        }
        // Topmost visible descendant (or self) under a world point; nullptr if none.
        [[nodiscard]] virtual Node* OverFind(core::Float2 worldPoint)
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
            m_background = core::Move(background);
            Invalidate();
        }
        [[nodiscard]] Drawable* GetBackground() const noexcept { return m_background.Get(); }
        void SetForeground(RefPtr<Drawable> foreground)
        {
            m_foreground = core::Move(foreground);
            Invalidate();
        }
        [[nodiscard]] Drawable* GetForeground() const noexcept { return m_foreground.Get(); }

        void SetClipChildren(bool clip)
        {
            if (clip == m_clipChildren)
                return;
            m_clipChildren = clip;
            Invalidate();
        }
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
            if (m_box.TextEffectsSet != 0)
                ctx.VG().SetTextEffects(m_box.MergeTextEffects(ctx.VG().GetTextEffects()));
            // A node with children fades as one picture (CSS's opacity), as does one with a filter
            // or a blend: through a VG layer, composited once. A leaf's opacity multiplies into
            // what it draws (no offscreen target for a label).
            const bool layered = m_box.NeedsLayer() || (m_alpha < 1.0f && !m_children.IsEmpty());
            const bool pushedOpacity = !layered && m_alpha < 1.0f;
            if (layered)
                ctx.VG().PushLayer(m_alpha, m_box.BlendMode, m_box.FilterBlur * ctx.DeviceScale());
            else if (pushedOpacity)
                ctx.PushOpacity(m_alpha);

            const Rect local = GetLocalBounds();
            const ControlState state = GetControlState();

            m_box.DrawUnder(ctx, local);
            if (m_background)
                m_background->Draw(ctx, local, state);
            OnDraw(ctx, local);

            // Children clip to the box, its rounded corners included (a path clip; a host without
            // stencil fills gets the bounds' scissor).
            const bool roundedClip = m_clipChildren && !m_box.Radii.IsZero();
            if (roundedClip)
            {
                vg::PathBuilder shape;
                vg::ShapeBuilder::BuildRoundedRect(local.ToRectangle(), m_box.Radii, shape);
                ctx.VG().PushClipPath(shape.ToPath());
            }
            else if (m_clipChildren)
                ctx.PushClip(local);
            for (const RefPtr<Node>& child : m_children)
                child->Draw(ctx);
            if (roundedClip)
                ctx.VG().PopClipPath();
            else if (m_clipChildren)
                ctx.PopClip();

            if (m_foreground)
                m_foreground->Draw(ctx, local, state);

            if (layered)
                ctx.VG().PopLayer();
            else if (pushedOpacity)
                ctx.PopOpacity();
            ctx.Restore();
            m_needsRedraw = false; // drawn: clean until something invalidates it again
        }

        // === What a frame must redo (only what changed: a clean subtree costs nothing) ===

        /// This node must be restyled (with its subtree when `withDescendants`: a selector with a
        /// combinator may match them differently): a class, id, tag or state changed, or it joined
        /// a tree. Its ancestors learn a descendant waits, so the style pass finds it.
        void MarkStyleDirty(bool withDescendants)
        {
            m_styleDirty = true;
            m_styleSubtreeDirty = m_styleSubtreeDirty || withDescendants;
            for (Node* p = m_parent; p != nullptr && !p->m_childStyleDirty; p = p->m_parent)
                p->m_childStyleDirty = true;
        }
        [[nodiscard]] bool IsStyleDirty() const noexcept { return m_styleDirty; }
        [[nodiscard]] bool IsStyleSubtreeDirty() const noexcept { return m_styleSubtreeDirty; }
        [[nodiscard]] bool HasStyleDirtyDescendant() const noexcept { return m_childStyleDirty; }
        void ClearStyleDirty() noexcept
        {
            m_styleDirty = false;
            m_styleSubtreeDirty = false;
            m_childStyleDirty = false;
        }

        /// This node's layout must run again. In a GUI it waits for its SceneRoot's layout pass, so
        /// a burst of changes in a frame costs one layout; a detached node lays out at once.
        void RequestLayout()
        {
            if (GetElementTable() == nullptr) // in no GUI: no pass to wait for
            {
                if (!m_layingOut) // a layout resizing its own children asks for nothing more
                    LayOut();
                return;
            }
            m_layoutDirty = true;
            for (Node* p = m_parent; p != nullptr && !p->m_childLayoutDirty; p = p->m_parent)
                p->m_childLayoutDirty = true;
            Invalidate();
        }
        [[nodiscard]] bool IsLayoutDirty() const noexcept { return m_layoutDirty; }
        // Runs OnLayout, marking the sizes it sets on children as its own.
        void LayOut()
        {
            m_layingOut = true;
            OnLayout();
            m_layingOut = false;
        }
        [[nodiscard]] bool HasLayoutDirtyDescendant() const noexcept { return m_childLayoutDirty; }

        /// Lays out the dirty containers of this subtree, parents before children (the pass a
        /// SceneRoot runs); how many containers laid out.
        u32 RunLayout()
        {
            u32 count = 0;
            if (m_layoutDirty)
            {
                m_layoutDirty = false;
                LayOut();
                ++count;
            }
            if (m_childLayoutDirty)
            {
                m_childLayoutDirty = false;
                for (usize i = 0; i < m_children.Size(); ++i)
                    count += m_children[i]->RunLayout();
            }
            return count;
        }

        // === The box (G2 of Specs/vg-and-gui-lab.md) ===
        // Background colour and layers, a border per side, radii and shadows, drawn under the
        // node's content. A sheet sets it (fromStyle: the next restyle replaces or clears it);
        // native code may set it directly.
        void SetBox(NodeBox box, bool fromStyle = false)
        {
            m_box = core::Move(box);
            m_boxFromStyle = fromStyle;
            Invalidate();
        }
        [[nodiscard]] const NodeBox& GetBox() const noexcept { return m_box; }
        [[nodiscard]] bool IsBoxFromStyle() const noexcept { return m_boxFromStyle; }

        /// CSS's `transform` about `transform-origin`: functions applied left to right after the
        /// node's own position, scale and rotation; translations and the origin in percentages
        /// resolve against the node's size, now and whenever it changes.
        void SetStyleTransform(Array<TransformFunction> functions, LengthValue originX, LengthValue originY,
                               bool fromStyle = true)
        {
            m_styleTransforms = core::Move(functions);
            m_transformOriginX = originX;
            m_transformOriginY = originY;
            m_transformFromStyle = fromStyle;
            UpdateStyleTransform();
        }
        [[nodiscard]] bool IsTransformFromStyle() const noexcept { return m_transformFromStyle; }
        [[nodiscard]] const Array<TransformFunction>& GetStyleTransforms() const noexcept { return m_styleTransforms; }

        /// A container places its children here (RequestLayout schedules it).
        virtual void OnLayout() {}
        /// A child's size (`resized`) or visibility changed, by anything but this node's own layout
        /// (a sheet, code, a script showing a panel, the child's own layout): a container lays out
        /// again.
        virtual void OnChildLayoutChange(Node& child, bool resized)
        {
            (void)child;
            (void)resized;
        }

        // Subclass-drawn content, between background and children.
        virtual void OnDraw(DrawContext& ctx, const Rect& localBounds)
        {
            (void)ctx;
            (void)localBounds;
        }

        // === Coordinator lookup + lifecycle ===
        // The SceneRoot's ActionManager / MutationQueue, found by walking to the root.
        // Base returns the parent's; SceneRoot overrides to return its own. Null if the
        // node is not yet attached under a SceneRoot.
        /// Where this node's bindings wait for the frame: its SceneRoot's scheduler, or none
        /// outside a scene (a binding then runs at once).
        [[nodiscard]] virtual EffectScheduler* GetEffectScheduler()
        {
            return m_parent ? m_parent->GetEffectScheduler() : nullptr;
        }
        [[nodiscard]] virtual ActionManager* GetActionManager()
        {
            return m_parent ? m_parent->GetActionManager() : nullptr;
        }
        [[nodiscard]] virtual MutationQueue* GetMutationQueue()
        {
            return m_parent ? m_parent->GetMutationQueue() : nullptr;
        }
        // The SceneRoot's EventDispatcher (pointer walk to the root; SceneRoot returns its own).
        /// What the GUI this node is in may create (its SceneRoot's set), or null when detached.
        [[nodiscard]] virtual const ElementSet* GetElementSet() const
        {
            return m_parent ? m_parent->GetElementSet() : nullptr;
        }
        /// The element table of the GUI this node is in (its SceneRoot's), or null when detached.
        [[nodiscard]] virtual ElementTable* GetElementTable()
        {
            return m_parent ? m_parent->GetElementTable() : nullptr;
        }
        [[nodiscard]] virtual EventDispatcher* GetEventDispatcher()
        {
            return m_parent ? m_parent->GetEventDispatcher() : nullptr;
        }

        // Programmatic focus (routes through the dispatcher; defined in the impl unit).
        void RequestFocus();
        void ReleaseFocus();

        // Run an action on this node (targets it, hands it to the SceneRoot's manager).
        // No-op (the action is dropped) if the node is not attached under a SceneRoot.
        void RunAction(RefPtr<Action> action)
        {
            if (!action)
                return;
            action->SetTarget(this);
            if (ActionManager* manager = GetActionManager())
                manager->AddAction(core::Move(action));
        }

        // === Weak references and the script surface (G1 of Specs/vg-and-gui-lab.md) ===
        // The tree owns its nodes. Anything else holds one weakly: a slot and a generation in its
        // GUI's element table, resolved on every use (Element<T> natively, a handle in a script),
        // reading as gone once the node is destroyed. Reflected in GUIReflectionImpl.cpp.

        /// This node's place in its GUI's element table, taking a slot the first time; an empty
        /// ref (null table) for a node in no GUI.
        [[nodiscard]] ElementRef TakeElementRef();

        /// A handle to this node typed as `type` (this node's type or a base of it); empty when the
        /// node is in no GUI (a detached element has no owner a handle could resolve through).
        [[nodiscard]] Variant HandleAs(const TypeInfo& type);
        /// The node a handle resolves to now, or null (gone, or not a node's handle).
        [[nodiscard]] static Node* FromHandle(const Variant& handle);
        /// Creates the element `tag` names in this GUI's element set as this node's last child,
        /// from this node's allocator; its handle, or empty when the set does not allow `tag`.
        Variant AddElement(core::StringView tag);
        /// The first widget in this subtree (depth first, this node first) whose id is `id`.
        [[nodiscard]] Variant FindElement(core::StringView id);
        [[nodiscard]] Variant ParentElement();
        [[nodiscard]] Variant ChildElement(i32 index);
        [[nodiscard]] i32 ElementChildCount() const noexcept { return static_cast<i32>(m_children.Size()); }
        /// Calls `handler` on each event named `eventName` (click, mousedown, mouseup,
        /// mouseenter, mouseleave, mousemove, wheel, keydown, keyup, textinput, focus, blur).
        void On(core::StringView eventName, RefPtr<foundation::script::IScriptDelegate> handler);
        /// Binds `property` to what `expression` returns, run again whenever what it read changed
        /// (gui.getNumber and the rest, widget properties). False when no such property.
        bool BindScript(core::StringView property, RefPtr<foundation::script::IScriptDelegate> expression);
        /// Binds `property` to this GUI's data value `key` (BindData with its scene's model); false
        /// outside a scene or with no such property.
        bool BindSceneData(core::StringView property, core::StringView key);
        /// Removes this node from its tree at the end of the UI's update (Close): never under a
        /// running handler.
        void Destroy() { Close(); }

        [[nodiscard]] f32 GetWidth() const noexcept { return m_size.x; }
        void SetWidth(f32 width) { SetSize(core::Float2{width, m_size.y}); }
        [[nodiscard]] f32 GetHeight() const noexcept { return m_size.y; }
        void SetHeight(f32 height) { SetSize(core::Float2{m_size.x, height}); }
        [[nodiscard]] f32 GetX() const noexcept { return GetPosition().x; }
        void SetX(f32 x) { SetPosition(core::Float2{x, GetPosition().y}); }
        [[nodiscard]] f32 GetY() const noexcept { return GetPosition().y; }
        void SetY(f32 y) { SetPosition(core::Float2{GetPosition().x, y}); }

        // Detach this node from its tree at the next safe sync point (SceneRoot drain).
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
        bool HandleActivate() { return OnActivate(); }
        void HandleTextInput(const TextInputEvent& e)
        {
            OnTextInput(e);
            SendEvent(e);
        }
        void HandleFocusGained()
        {
            m_focused = true;
            MarkStyleDirty(true); // :focus
            OnFocusGained();
            SendEvent(Event(EventType::FocusGained, this));
        }
        void HandleFocusLost()
        {
            m_focused = false;
            MarkStyleDirty(true);
            OnFocusLost();
            SendEvent(Event(EventType::FocusLost, this));
        }

        [[nodiscard]] bool IsFocused() const noexcept { return m_focused; }

        // === Drag-and-drop target ===
        // Set a predicate deciding which payloads this node accepts; when it returns true the
        // dispatcher delivers the drag-enter/over/leave and drop here (nearest accepting
        // ancestor of the cursor wins). Each also fires the matching DragEvent to listeners.
        void SetDropAcceptor(core::Function<bool(const DragPayload&)> predicate)
        {
            m_dropAcceptor = core::Move(predicate);
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
        [[nodiscard]] virtual core::StringView GetTooltipText() const { return {}; }

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

        // === Focus scopes (G7) ===
        // A scope groups what directional and tab navigation move between: it says what happens
        // at its edge, remembers what was last focused inside it (so coming back returns there),
        // and may name the widget focus starts on (its id).
        void SetNavScope(NavScope scope) noexcept { m_navScope = scope; }
        [[nodiscard]] NavScope GetNavScope() const noexcept { return m_navScope; }
        void SetDefaultFocusId(core::StringView id) { m_defaultFocusId = core::String(id); }
        [[nodiscard]] core::StringView GetDefaultFocusId() const noexcept { return m_defaultFocusId.AsView(); }
        /// What was last focused inside this scope (weakly: null once it is gone).
        [[nodiscard]] Node* GetRememberedFocus() const noexcept
        {
            return m_rememberedFocus.table != nullptr
                       ? m_rememberedFocus.table->Resolve(m_rememberedFocus.slot, m_rememberedFocus.generation)
                       : nullptr;
        }
        void RememberFocus(Node& focused) { m_rememberedFocus = focused.TakeElementRef(); }

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
        // An unchanged value is no change: hosts and layouts set geometry every pass, and an idle
        // frame must find nothing to redraw.
        void SetPosition(core::Float2 position) override
        {
            if (position == GetPosition())
                return;
            Transformable::SetPosition(position);
            HandlePositionChange();
        }
        void SetRotation(f32 radians) override
        {
            if (radians == GetRotation())
                return;
            Transformable::SetRotation(radians);
            Invalidate();
        }
        void SetScale(core::Float2 factors) override
        {
            if (factors == GetScale())
                return;
            Transformable::SetScale(factors);
            Invalidate();
        }

        // === Bindings (G5 of Specs/vg-and-gui-lab.md) ===

        /// Runs `body` now, and again on this GUI's frame whenever something it read changed (at
        /// once outside a scene). The node owns it: it ends with the node, or at ClearBindings.
        Effect& Bind(core::Function<void()> body)
        {
            m_bindings.PushBack(
                MakeUnique<Effect>(MemoryAllocator(), core::Move(body), [this]() { return GetEffectScheduler(); }));
            return *m_bindings[m_bindings.Size() - 1];
        }
        /// Binds the reflected property named `name` (this node's type's or a base's) to
        /// `expression`'s value, converted to the property's type: native code, markup and scripts
        /// bind any widget's properties this way, a library's included. False when the node has no
        /// such writable property.
        bool BindProperty(core::StringView name, core::Function<Variant()> expression);
        /// The reverse: `write` takes the property's value now and whenever it changes (the user
        /// moved a slider, typed). Only for the properties a widget's user changes (the `twoWay`
        /// property attribute); false for any other.
        bool BindPropertyBack(core::StringView name, core::Function<void(const Variant&)> write);
        /// Binds `property` to `model`'s value `key`, as markup's `{{key}}` does: it follows the
        /// value, and writes it back when the property is one the user changes. False when the node
        /// has no such writable property.
        bool BindData(core::StringView property, RefPtr<DataModel> model, core::StringView key)
        {
            const core::String name(key);
            if (!BindProperty(property, [model, name] { return model->Get(name.AsView()); }))
                return false;
            (void)BindPropertyBack(property, [model, name](const Variant& v) { model->Set(name.AsView(), v); });
            return true;
        }
        void ClearBindings() { m_bindings.Clear(); }
        [[nodiscard]] usize BindingCount() const noexcept { return m_bindings.Size(); }

        // === Event listeners ===
        u32 AddEventListener(EventType type, EventCallback callback)
        {
            const u32 id = ++m_nextListenerId;
            m_listeners.PushBack(Listener{id, type, core::Move(callback)});
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
        /// Return or Space (a gamepad's South) on the focused widget, when it used neither: what a
        /// click does, for the widgets a click acts on. True when it did something.
        virtual bool OnActivate() { return false; }

        // A click at the centre, as activation delivers it (listeners see a click, as with a mouse).
        bool ActivateAsClick()
        {
            HandleMouseClick(MouseEvent(EventType::MouseClick, this,
                                        ConvertToWorldSpace(core::Float2{m_size.x * 0.5f, m_size.y * 0.5f})));
            return true;
        }
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
            if (!m_styleTransforms.IsEmpty())
                UpdateStyleTransform();
            OnSizeChange();
            Invalidate();
            SendEvent(Event(EventType::SizeChanged, this));
            if (m_parent != nullptr && !m_parent->m_layingOut)
                m_parent->OnChildLayoutChange(*this, true);
        }

        // Clamp a size to [m_minSize, m_maxSize] per axis (a negative max = unbounded).
        [[nodiscard]] core::Float2 ClampToSizeConstraints(core::Float2 size) const noexcept
        {
            core::Float2 out = size;
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
            if (m_parent != nullptr && !m_parent->m_layingOut)
                m_parent->OnChildLayoutChange(*this, false); // shown or hidden: the others move
        }
        void HandleEnabledChange()
        {
            OnEnabledChange();
            MarkStyleDirty(true); // :disabled
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
        Array<UniquePtr<Effect>> m_bindings; // this node's bindings (Bind, BindProperty)
        Function<bool(const DragPayload&)>
            m_dropAcceptor; // drop-target predicate (empty = rejects)
        RefPtr<Drawable> m_background;
        RefPtr<Drawable> m_foreground;
        core::Float2 m_size{0.0f, 0.0f};
        core::Float2 m_minSize{0.0f, 0.0f};   // CSS min-width/height
        core::Float2 m_maxSize{-1.0f, -1.0f}; // CSS max-width/height (<0 = unbounded)
        f32 m_alpha = 1.0f;
        bool m_visible = true;
        bool m_enabled = true;
        bool m_needsRedraw = true;
        bool m_styleDirty = true;          // restyle this node
        bool m_styleSubtreeDirty = true;   // ... and every descendant
        bool m_childStyleDirty = false;    // some descendant waits for a restyle
        bool m_layoutDirty = false;        // this container's layout must run
        bool m_childLayoutDirty = false;   // some descendant's must
        bool m_layingOut = false;          // inside OnLayout: the sizes it sets are its own
        bool m_clipChildren = false;
        NavScope m_navScope = NavScope::None;
        core::String m_defaultFocusId;
        ElementRef m_rememberedFocus{}; // weak: resolved through the GUI's element table
        bool m_focused = false;
        bool m_tabFocusable = false;
        bool m_hitTestVisible = true;
        u32 m_nextListenerId = 0;
        // The style transform's matrix and origin in local units, from the functions and the size.
        void UpdateStyleTransform()
        {
            SetExtraTransform(ComposeTransforms(m_styleTransforms, m_size),
                              core::Float2{ResolveAgainst(m_transformOriginX, m_size.x),
                                           ResolveAgainst(m_transformOriginY, m_size.y)});
            Invalidate();
        }

        NodeBox m_box;
        bool m_boxFromStyle = false;
        Array<TransformFunction> m_styleTransforms;
        LengthValue m_transformOriginX{50.0f, LengthUnit::Percent};
        LengthValue m_transformOriginY{50.0f, LengthUnit::Percent};
        bool m_transformFromStyle = false;
        RefPtr<ElementTable> m_elementTable; // the table a handle took a slot in (null: none yet)
        u32 m_elementSlot = 0;
    };
}

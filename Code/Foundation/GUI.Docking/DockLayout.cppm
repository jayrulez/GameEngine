// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :layout partition: DockLayout, a dock layout as plain data.
//
// The layout is data, not widgets: what the dock widgets show is reconciled from it, a drop's
// preview is an operation run on a copy of it, and it is tested without a GUI. There is one
// surface for the main window and one for each floating window, and each surface holds a whole
// tree. Panels can be docked into a floating window, and a floating window can dock whole: its
// panels become a stack's tabs, or its tree goes beside a stack or along an edge. Nodes live in one
// table by stable id. A split has an axis, children and their weights (one share per child,
// summing to one). A stack has its panels (by registered id, never by title), the active one and
// its flags. The main surface has one central stack (the documents' area), which is kept when it
// is empty. A closed panel keeps its last place, so it can be shown there again.
//
// An operation either does what it says or refuses with its reason and changes nothing; none falls
// back to doing something else (such as splitting another pane when its target is gone). After
// each operation the layout is normalized: an empty stack is removed (except the central one), a
// split left with one child is replaced by that child, a split inside a split of the same axis is
// merged into it, weights are made to sum to one, and an empty floating surface is removed. A
// removed child's share goes to its neighbours, and the rest keep theirs.

module;
#include "Core/Prelude.h"

export module foundation.gui.docking:layout;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    using DockNodeId = u32;
    using DockSurfaceId = u32;
    inline constexpr u32 kNoDock = 0; ///< no node, no surface

    enum class DockAxis : u8
    {
        Horizontal, ///< side by side, from the inline start
        Vertical,   ///< one above another
    };
    /// Where beside a stack (or at a surface's edge) a panel goes: the inline start or end, the top
    /// or the bottom.
    enum class DockSide : u8
    {
        Start,
        End,
        Top,
        Bottom,
    };
    [[nodiscard]] constexpr DockAxis AxisOf(DockSide side) noexcept
    {
        return side == DockSide::Start || side == DockSide::End ? DockAxis::Horizontal : DockAxis::Vertical;
    }
    [[nodiscard]] constexpr bool IsBefore(DockSide side) noexcept { return side == DockSide::Start || side == DockSide::Top; }

    enum class DockNodeKind : u8
    {
        Split,
        Stack,
    };
    /// What a stack allows.
    enum DockStackFlag : u32
    {
        DockStackCentral = 1u << 0, ///< the main surface's documents area: kept when empty
        DockStackLocked = 1u << 1,  ///< no panel may be tabbed into it or leave it
        DockStackNoSplit = 1u << 2, ///< nothing docks beside it
        DockStackNoTabs = 1u << 3,  ///< holds one panel; no more may be tabbed into it
    };

    struct DockNode
    {
        DockNodeId Id = kNoDock;
        DockNodeId Parent = kNoDock; ///< kNoDock for a surface's root
        DockNodeKind Kind = DockNodeKind::Stack;
        // Split fields.
        DockAxis Axis = DockAxis::Horizontal;
        Array<DockNodeId> Children;
        Array<f32> Weights; ///< one share per child, summing to one
        // Stack fields.
        Array<String> Panels;
        u32 Active = 0;
        u32 Flags = 0;

        [[nodiscard]] bool IsStack() const noexcept { return Kind == DockNodeKind::Stack; }
        [[nodiscard]] bool Has(u32 flag) const noexcept { return (Flags & flag) != 0; }
    };

    enum class DockSurfaceKind : u8
    {
        Main,
        Floating,
    };
    struct DockSurface
    {
        DockSurfaceId Id = kNoDock;
        DockSurfaceKind Kind = DockSurfaceKind::Main;
        DockNodeId Root = kNoDock;
        Rectangle Bounds{0.0f, 0.0f, 0.0f, 0.0f}; ///< a floating surface's, in logical units on its screen
        String Screen;                         ///< the screen it was on (by name)
        f32 Scale = 1.0f;                      ///< that screen's scale
        DockNodeId Maximized = kNoDock;        ///< a stack shown alone over the surface
    };

    /// Where a panel was: its stack, its position among the stack's tabs, and the panels around it
    /// at the time, used to find a place near them when the stack is gone.
    struct DockPlace
    {
        DockSurfaceId Surface = kNoDock;
        DockNodeId Stack = kNoDock;
        u32 Index = 0;
        Array<String> Neighbours;
    };
    struct DockHidden
    {
        String Panel;
        DockPlace Last;
    };

    /// A registered panel's place when a restored layout does not name it: a tab of the central
    /// stack, or a stack of its own along the main surface's `Side` with `Share` of it.
    struct DockDefault
    {
        String Panel;
        bool Central = false;
        DockSide Side = DockSide::End;
        f32 Share = 0.25f;
    };

    class DockLayoutDocument;

    /// The result of an operation: Refused is empty when it succeeded, or holds the reason it was
    /// refused.
    struct DockOutcome
    {
        String Refused;
        [[nodiscard]] bool Ok() const noexcept { return Refused.IsEmpty(); }
        [[nodiscard]] static DockOutcome Refuse(core::StringView why) { return DockOutcome{String(why)}; }
    };

    class DockLayout
    {
    public:
        /// Makes a layout of a main surface holding an empty central stack.
        DockLayout()
        {
            DockNode& central = MakeNode(DockNodeKind::Stack);
            central.Flags = DockStackCentral;
            DockSurface main;
            main.Id = m_nextSurface++;
            main.Kind = DockSurfaceKind::Main;
            main.Root = central.Id;
            m_surfaces.PushBack(main);
        }

        // === What it holds ===

        [[nodiscard]] const Array<DockSurface>& Surfaces() const noexcept { return m_surfaces; }
        [[nodiscard]] const DockSurface* FindSurface(DockSurfaceId id) const noexcept
        {
            for (const DockSurface& surface : m_surfaces)
                if (surface.Id == id)
                    return &surface;
            return nullptr;
        }
        [[nodiscard]] DockSurfaceId MainSurface() const noexcept { return m_surfaces.IsEmpty() ? kNoDock : m_surfaces[0].Id; }
        [[nodiscard]] const Array<DockNode>& Nodes() const noexcept { return m_nodes; }
        [[nodiscard]] const DockNode* FindNode(DockNodeId id) const noexcept
        {
            for (const DockNode& node : m_nodes)
                if (node.Id == id)
                    return &node;
            return nullptr;
        }
        [[nodiscard]] DockNodeId CentralStack() const noexcept
        {
            for (const DockNode& node : m_nodes)
                if (node.IsStack() && node.Has(DockStackCentral))
                    return node.Id;
            return kNoDock;
        }
        /// The stack a panel is docked in; kNoDock when it is not.
        [[nodiscard]] DockNodeId StackOf(core::StringView panel) const noexcept
        {
            for (const DockNode& node : m_nodes)
                if (node.IsStack())
                    for (const String& p : node.Panels)
                        if (p.AsView() == panel)
                            return node.Id;
            return kNoDock;
        }
        [[nodiscard]] const Array<DockHidden>& Hidden() const noexcept { return m_hidden; }
        [[nodiscard]] const DockHidden* FindHidden(core::StringView panel) const noexcept
        {
            for (const DockHidden& hidden : m_hidden)
                if (hidden.Panel.AsView() == panel)
                    return &hidden;
            return nullptr;
        }
        /// The surface a node is in (through its parents to a root).
        [[nodiscard]] DockSurfaceId SurfaceOf(DockNodeId id) const noexcept
        {
            const DockNode* node = FindNode(id);
            while (node != nullptr && node->Parent != kNoDock)
                node = FindNode(node->Parent);
            if (node != nullptr)
                for (const DockSurface& surface : m_surfaces)
                    if (surface.Root == node->Id)
                        return surface.Id;
            return kNoDock;
        }

        // === What it does ===

        /// Docks a panel that is not docked yet (or is hidden) as a tab of `stack` at `index`, or
        /// last when `index` is past the end, and makes it the stack's active tab.
        DockOutcome InsertTab(core::StringView panel, DockNodeId stack, i32 index = -1)
        {
            if (DockOutcome refused = CanEnter(panel); !refused.Ok())
                return refused;
            if (DockOutcome refused = CanTabInto(stack); !refused.Ok())
                return refused;
            Unhide(panel);
            PlaceTab(panel, stack, index);
            Normalize();
            return {};
        }
        /// Docks a panel that is not docked yet (or is hidden) in a new stack beside `stack`, taking
        /// `share` of the room `stack` had.
        DockOutcome InsertBeside(core::StringView panel, DockNodeId stack, DockSide side, f32 share = 0.5f)
        {
            if (DockOutcome refused = CanEnter(panel); !refused.Ok())
                return refused;
            if (DockOutcome refused = CanDockBeside(stack); !refused.Ok())
                return refused;
            Unhide(panel);
            PlaceBeside(panel, stack, side, share);
            Normalize();
            return {};
        }
        /// Docks a panel that is not docked yet (or is hidden) in a new stack along the edge of a
        /// whole surface, taking `share` of the surface.
        DockOutcome InsertAtEdge(core::StringView panel, DockSurfaceId surface, DockSide side, f32 share = 0.25f)
        {
            if (DockOutcome refused = CanEnter(panel); !refused.Ok())
                return refused;
            if (FindSurface(surface) == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            Unhide(panel);
            PlaceAtEdge(panel, surface, side, share);
            Normalize();
            return {};
        }

        /// Moves a docked panel to a tab of `stack`. When `stack` is the panel's own stack, the panel
        /// moves to another place among its tabs.
        DockOutcome MoveToTab(core::StringView panel, DockNodeId stack, i32 index = -1)
        {
            if (DockOutcome refused = CanLeave(panel); !refused.Ok())
                return refused;
            const DockNodeId from = StackOf(panel);
            if (from != stack)
                if (DockOutcome refused = CanTabInto(stack); !refused.Ok())
                    return refused;
            if (from == stack && index >= 0)
            {
                // Reordered among its own tabs: the index is where it ends up.
                DockNode& node = *Node(stack);
                const u32 at = IndexIn(node, panel);
                node.Panels.RemoveAt(at);
                node.Panels.Insert(static_cast<usize>(Min(static_cast<u32>(index), static_cast<u32>(node.Panels.Size()))), String(panel));
                node.Active = IndexIn(node, panel);
                return {};
            }
            TakeOut(panel);
            PlaceTab(panel, stack, index);
            Normalize();
            return {};
        }
        /// Moves a docked panel into a new stack beside `stack`.
        DockOutcome MoveBeside(core::StringView panel, DockNodeId stack, DockSide side, f32 share = 0.5f)
        {
            if (DockOutcome refused = CanLeave(panel); !refused.Ok())
                return refused;
            if (DockOutcome refused = CanDockBeside(stack); !refused.Ok())
                return refused;
            if (StackOf(panel) == stack && Node(stack)->Panels.Size() == 1)
                return DockOutcome::Refuse(u8"a panel alone in its stack cannot go beside itself");
            TakeOut(panel);
            PlaceBeside(panel, stack, side, share);
            Normalize();
            return {};
        }
        /// Moves a docked panel to the edge of a whole surface.
        DockOutcome MoveToEdge(core::StringView panel, DockSurfaceId surface, DockSide side, f32 share = 0.25f)
        {
            if (DockOutcome refused = CanLeave(panel); !refused.Ok())
                return refused;
            if (FindSurface(surface) == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            const DockNodeId from = StackOf(panel);
            if (Node(from)->Panels.Size() == 1 && Node(from)->Parent == kNoDock && SurfaceOf(from) == surface)
                return DockOutcome::Refuse(u8"a surface's only panel cannot go to its own edge");
            TakeOut(panel);
            PlaceAtEdge(panel, surface, side, share);
            Normalize();
            return {};
        }
        /// Puts a panel (docked, hidden or new) in a new floating surface at `bounds`, and sets
        /// `made` to that surface.
        DockOutcome Float(core::StringView panel, Rectangle bounds, DockSurfaceId* made = nullptr)
        {
            if (panel.IsEmpty())
                return DockOutcome::Refuse(u8"a panel needs an id");
            if (StackOf(panel) != kNoDock)
            {
                if (DockOutcome refused = CanLeave(panel); !refused.Ok())
                    return refused;
                TakeOut(panel);
            }
            Unhide(panel);
            DockNode& stack = MakeNode(DockNodeKind::Stack);
            stack.Panels.PushBack(String(panel));
            DockSurface surface;
            surface.Id = m_nextSurface++;
            surface.Kind = DockSurfaceKind::Floating;
            surface.Root = stack.Id;
            surface.Bounds = bounds;
            m_surfaces.PushBack(surface);
            if (made != nullptr)
                *made = surface.Id;
            Normalize();
            return {};
        }
        // === Docking a whole floating surface ===
        /// Moves every panel of floating `surface` into `stack` as tabs, in the order of the
        /// surface's stacks, and makes the float's active panel active there. The surface is
        /// removed.
        DockOutcome DockSurfaceInto(DockSurfaceId surface, DockNodeId stack)
        {
            if (DockOutcome refused = CanDockWhole(surface, stack); !refused.Ok())
                return refused;
            if (DockOutcome refused = CanTabInto(stack); !refused.Ok())
                return refused;
            Array<String> panels;
            String active;
            CollectPanels(SurfaceRef(surface)->Root, panels, active);
            if (FindNode(stack)->Has(DockStackNoTabs) && panels.Size() > 1)
                return DockOutcome::Refuse(u8"that stack holds one panel");
            for (const DockNodeId id : StacksOf(surface))
                if (FindNode(id)->Has(DockStackLocked))
                    return DockOutcome::Refuse(u8"a stack in it is locked");
            for (const String& panel : panels)
            {
                TakeOut(panel.AsView());
                PlaceTab(panel.AsView(), stack, -1);
            }
            DockNode& into = *Node(stack);
            into.Active = IndexIn(into, active.AsView());
            Normalize(); // removes the emptied surface
            return {};
        }
        /// Docks the whole tree of floating `surface` beside `stack`, taking `share` of the stack's
        /// room. The surface is removed.
        DockOutcome DockSurfaceBeside(DockSurfaceId surface, DockNodeId stack, DockSide side, f32 share = 0.5f)
        {
            if (DockOutcome refused = CanDockWhole(surface, stack); !refused.Ok())
                return refused;
            if (DockOutcome refused = CanDockBeside(stack); !refused.Ok())
                return refused;
            const DockNodeId root = Lift(surface);
            PlaceNodeBeside(root, stack, side, share);
            Normalize();
            return {};
        }
        /// Docks the whole tree of floating `surface` along the edge of surface `into`. The floating
        /// surface is removed.
        DockOutcome DockSurfaceAtEdge(DockSurfaceId surface, DockSurfaceId into, DockSide side, f32 share = 0.25f)
        {
            const DockSurface* from = FindSurface(surface);
            if (from == nullptr || from->Kind != DockSurfaceKind::Floating)
                return DockOutcome::Refuse(u8"not a floating surface");
            if (FindSurface(into) == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            if (into == surface)
                return DockOutcome::Refuse(u8"a surface cannot dock into itself");
            const DockNodeId root = Lift(surface);
            PlaceNodeAtEdge(root, into, side, share);
            Normalize();
            return {};
        }

        // === Moving a whole stack (dragged by the grip on its strip) ===
        /// Moves every panel of `stack`, in order, into `into` as tabs at `index` (-1 for last), and
        /// makes its active panel active there. The emptied stack is removed, except the central
        /// stack, which stays empty.
        DockOutcome MoveStackInto(DockNodeId stack, DockNodeId into, i32 index = -1)
        {
            if (DockOutcome refused = CanMoveStack(stack); !refused.Ok())
                return refused;
            if (stack == into)
                return DockOutcome::Refuse(u8"a stack cannot go into itself");
            if (DockOutcome refused = CanTabInto(into); !refused.Ok())
                return refused;
            const DockNode& from = *FindNode(stack);
            if (FindNode(into)->Has(DockStackNoTabs) && from.Panels.Size() > 1)
                return DockOutcome::Refuse(u8"that stack holds one panel");
            const Array<String> panels = from.Panels;
            const String active = panels[Min(static_cast<usize>(from.Active), panels.Size() - 1)];
            i32 at = index < 0 ? static_cast<i32>(FindNode(into)->Panels.Size()) : Min(index, static_cast<i32>(FindNode(into)->Panels.Size()));
            for (const String& panel : panels)
            {
                TakeOut(panel.AsView());
                PlaceTab(panel.AsView(), into, at++);
            }
            DockNode& target = *Node(into);
            target.Active = IndexIn(target, active.AsView());
            Normalize();
            return {};
        }
        /// Moves `stack` beside `target`, taking `share` of the target's room. A floating surface
        /// that held only `stack` is removed.
        DockOutcome MoveStackBeside(DockNodeId stack, DockNodeId target, DockSide side, f32 share = 0.5f)
        {
            if (DockOutcome refused = CanMoveStack(stack); !refused.Ok())
                return refused;
            if (stack == target)
                return DockOutcome::Refuse(u8"a stack cannot go beside itself");
            if (DockOutcome refused = CanDockBeside(target); !refused.Ok())
                return refused;
            PlaceNodeBeside(TakeStack(stack), target, side, share);
            Normalize();
            return {};
        }
        /// Moves `stack` along the whole edge of `surface`.
        DockOutcome MoveStackToEdge(DockNodeId stack, DockSurfaceId surface, DockSide side, f32 share = 0.25f)
        {
            if (DockOutcome refused = CanMoveStack(stack); !refused.Ok())
                return refused;
            const DockSurface* into = FindSurface(surface);
            if (into == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            if (into->Root == stack)
                return DockOutcome::Refuse(u8"a surface's only stack cannot go to its own edge");
            PlaceNodeAtEdge(TakeStack(stack), surface, side, share);
            Normalize();
            return {};
        }
        /// Moves `stack` into a new floating surface at `bounds`, and sets `made` to that surface.
        DockOutcome FloatStack(DockNodeId stack, Rectangle bounds, DockSurfaceId* made = nullptr)
        {
            if (DockOutcome refused = CanMoveStack(stack); !refused.Ok())
                return refused;
            const DockSurfaceId from = SurfaceOf(stack);
            if (FindSurface(from)->Kind == DockSurfaceKind::Floating && FindSurface(from)->Root == stack)
                return DockOutcome::Refuse(u8"it floats alone already");
            const DockNodeId moved = TakeStack(stack);
            DockSurface surface;
            surface.Id = m_nextSurface++;
            surface.Kind = DockSurfaceKind::Floating;
            surface.Root = moved;
            surface.Bounds = bounds;
            m_surfaces.PushBack(surface);
            if (made != nullptr)
                *made = surface.Id;
            Normalize();
            return {};
        }

        /// Closes a docked panel: it is no longer docked, and its place is kept so it can be shown
        /// there again.
        DockOutcome Close(core::StringView panel)
        {
            if (DockOutcome refused = CanLeave(panel); !refused.Ok())
                return refused;
            const DockNodeId stackId = StackOf(panel);
            const DockNode& stack = *FindNode(stackId);
            DockHidden hidden;
            hidden.Panel = String(panel);
            hidden.Last.Surface = SurfaceOf(stackId);
            hidden.Last.Stack = stackId;
            hidden.Last.Index = IndexIn(stack, panel);
            for (const String& other : stack.Panels)
                if (other.AsView() != panel)
                    hidden.Last.Neighbours.PushBack(other);
            if (hidden.Last.Neighbours.IsEmpty())
                NeighboursBeside(stackId, hidden.Last.Neighbours); // alone in its stack: remember the panels in the stack beside it
            TakeOut(panel);
            m_hidden.PushBack(core::Move(hidden));
            Normalize();
            return {};
        }
        /// Shows a closed panel again where it was, among its stack's tabs. When that stack is gone,
        /// the panel goes beside the first of the panels around it then that is still docked in a
        /// stack that allows it; when none is, it goes in the central stack.
        DockOutcome Show(core::StringView panel)
        {
            const DockHidden* hidden = FindHidden(panel);
            if (hidden == nullptr)
                return DockOutcome::Refuse(StackOf(panel) != kNoDock ? u8"it is docked" : u8"it was never closed");
            const DockPlace last = hidden->Last;
            const DockNode* stack = FindNode(last.Stack);
            if (stack != nullptr && stack->IsStack() && CanTabInto(last.Stack).Ok())
                return InsertTab(panel, last.Stack, static_cast<i32>(last.Index));
            for (const String& neighbour : last.Neighbours)
                if (const DockNodeId near = StackOf(neighbour.AsView()); near != kNoDock && CanDockBeside(near).Ok())
                    return InsertBeside(panel, near, DockSide::End);
            const DockNodeId central = CentralStack();
            if (central != kNoDock)
                return InsertTab(panel, central);
            return DockOutcome::Refuse(u8"there is nowhere to show it");
        }
        /// Forgets the remembered place of a closed panel.
        DockOutcome Forget(core::StringView panel)
        {
            if (FindHidden(panel) == nullptr)
                return DockOutcome::Refuse(u8"it is not closed");
            Unhide(panel);
            return {};
        }
        /// Makes a docked panel its stack's active tab.
        DockOutcome Activate(core::StringView panel)
        {
            const DockNodeId stack = StackOf(panel);
            if (stack == kNoDock)
                return DockOutcome::Refuse(u8"it is not docked");
            DockNode& node = *Node(stack);
            node.Active = IndexIn(node, panel);
            return {};
        }
        /// Sets the shares of a split's children, one per child. They may be at any scale, since the
        /// layout is normalized afterwards.
        DockOutcome SetWeights(DockNodeId split, Span<const f32> weights)
        {
            DockNode* node = Node(split);
            if (node == nullptr || node->IsStack())
                return DockOutcome::Refuse(u8"not a split");
            if (weights.Size() != node->Children.Size())
                return DockOutcome::Refuse(u8"a share for each child");
            for (usize i = 0; i < weights.Size(); ++i)
                node->Weights[i] = Max(weights[i], 0.0f);
            Normalize();
            return {};
        }
        /// Shows a stack alone over its surface. Restore clears it; a surface's Maximized is kNoDock
        /// when no stack is maximized.
        DockOutcome Maximize(DockNodeId stack)
        {
            const DockNode* node = FindNode(stack);
            if (node == nullptr || !node->IsStack())
                return DockOutcome::Refuse(u8"not a stack");
            SurfaceRef(SurfaceOf(stack))->Maximized = stack;
            return {};
        }
        DockOutcome Restore(DockSurfaceId surface)
        {
            DockSurface* found = SurfaceRef(surface);
            if (found == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            found->Maximized = kNoDock;
            return {};
        }
        DockOutcome SetStackFlags(DockNodeId stack, u32 flags)
        {
            DockNode* node = Node(stack);
            if (node == nullptr || !node->IsStack())
                return DockOutcome::Refuse(u8"not a stack");
            if (node->Has(DockStackCentral) != ((flags & DockStackCentral) != 0))
                return DockOutcome::Refuse(u8"the central stack is the layout's own");
            node->Flags = flags;
            return {};
        }
        /// Sets a floating surface's bounds and screen, as a host reports them (the controller notes
        /// the user's moves through this).
        DockOutcome SetSurfaceBounds(DockSurfaceId surface, Rectangle bounds, core::StringView screen, f32 scale)
        {
            DockSurface* found = SurfaceRef(surface);
            if (found == nullptr)
                return DockOutcome::Refuse(u8"no such surface");
            found->Bounds = bounds;
            found->Screen = String(screen);
            found->Scale = scale;
            return {};
        }

        /// Replaces this layout with `read` (a layout restored from text) if `read` holds together.
        /// Otherwise it refuses with what is wrong and leaves this layout unchanged. A panel `read`
        /// names that is not registered stays as a placeholder for when it is registered. A
        /// registered panel (in `known`) that `read` names nowhere goes to its default place. A
        /// floating surface off every one of `screens` (in logical units; an empty list skips the
        /// check) comes back onto the first screen, no larger than it.
        DockOutcome Adopt(DockLayout read, Span<const DockDefault> known, Span<const Rectangle> screens = {})
        {
            String why;
            if (!read.CheckInvariants(&why))
                return DockOutcome::Refuse(why.AsView());
            for (const DockDefault& panel : known)
            {
                if (read.StackOf(panel.Panel.AsView()) != kNoDock || read.FindHidden(panel.Panel.AsView()) != nullptr)
                    continue;
                const DockOutcome placed = panel.Central ? read.InsertTab(panel.Panel.AsView(), read.CentralStack())
                                                         : read.InsertAtEdge(panel.Panel.AsView(), read.MainSurface(), panel.Side, panel.Share);
                if (!placed.Ok())
                    return placed;
            }
            if (!screens.IsEmpty())
                for (DockSurface& surface : read.m_surfaces)
                {
                    if (surface.Kind != DockSurfaceKind::Floating)
                        continue;
                    bool on = false;
                    for (const Rectangle& screen : screens)
                        on = on || (surface.Bounds.x < screen.x + screen.width && surface.Bounds.x + surface.Bounds.width > screen.x &&
                                    surface.Bounds.y < screen.y + screen.height && surface.Bounds.y + surface.Bounds.height > screen.y);
                    if (on)
                        continue;
                    const Rectangle& first = screens[0];
                    surface.Bounds.width = Min(surface.Bounds.width, first.width);
                    surface.Bounds.height = Min(surface.Bounds.height, first.height);
                    surface.Bounds.x = first.x + (first.width - surface.Bounds.width) * 0.5f;
                    surface.Bounds.y = first.y + (first.height - surface.Bounds.height) * 0.5f;
                }
            *this = core::Move(read);
            return {};
        }

        /// Whether the layout holds together. Every node is in one place in one surface's tree.
        /// Every split has two or more children whose shares sum to one, and no child split of its
        /// own axis. No stack is empty except the central one, which is in the main surface. A panel
        /// is in one stack at most and never also hidden. No floating surface is empty. `why`
        /// receives what is wrong, for a test.
        [[nodiscard]] bool CheckInvariants(String* why = nullptr) const;

    private:
        friend class DockLayoutDocument; // reads and writes it whole

        // === Checks ===
        [[nodiscard]] DockOutcome CanEnter(core::StringView panel) const
        {
            if (panel.IsEmpty())
                return DockOutcome::Refuse(u8"a panel needs an id");
            if (StackOf(panel) != kNoDock)
                return DockOutcome::Refuse(u8"it is docked already: move it");
            return {};
        }
        [[nodiscard]] DockOutcome CanLeave(core::StringView panel) const
        {
            const DockNodeId stack = StackOf(panel);
            if (stack == kNoDock)
                return DockOutcome::Refuse(u8"it is not docked");
            if (FindNode(stack)->Has(DockStackLocked))
                return DockOutcome::Refuse(u8"its stack is locked");
            return {};
        }
        [[nodiscard]] DockOutcome CanTabInto(DockNodeId stack) const
        {
            const DockNode* node = FindNode(stack);
            if (node == nullptr || !node->IsStack())
                return DockOutcome::Refuse(u8"not a stack");
            if (node->Has(DockStackLocked))
                return DockOutcome::Refuse(u8"that stack is locked");
            if (node->Has(DockStackNoTabs) && !node->Panels.IsEmpty())
                return DockOutcome::Refuse(u8"that stack holds one panel");
            return {};
        }
        // Checks that floating `surface` can dock whole at `stack` of another surface.
        [[nodiscard]] DockOutcome CanDockWhole(DockSurfaceId surface, DockNodeId stack) const
        {
            const DockSurface* from = FindSurface(surface);
            if (from == nullptr || from->Kind != DockSurfaceKind::Floating)
                return DockOutcome::Refuse(u8"not a floating surface");
            if (FindNode(stack) == nullptr)
                return DockOutcome::Refuse(u8"no such stack");
            if (SurfaceOf(stack) == surface)
                return DockOutcome::Refuse(u8"a surface cannot dock into itself");
            return {};
        }
        // Checks that `stack` can be picked up whole: it exists, has panels and is not locked.
        [[nodiscard]] DockOutcome CanMoveStack(DockNodeId stack) const
        {
            const DockNode* node = FindNode(stack);
            if (node == nullptr || !node->IsStack())
                return DockOutcome::Refuse(u8"not a stack");
            if (node->Panels.IsEmpty())
                return DockOutcome::Refuse(u8"the stack is empty");
            if (node->Has(DockStackLocked))
                return DockOutcome::Refuse(u8"its stack is locked");
            return {};
        }
        [[nodiscard]] DockOutcome CanDockBeside(DockNodeId stack) const
        {
            const DockNode* node = FindNode(stack);
            if (node == nullptr || !node->IsStack())
                return DockOutcome::Refuse(u8"not a stack");
            if (node->Has(DockStackNoSplit))
                return DockOutcome::Refuse(u8"nothing docks beside that stack");
            return {};
        }

        // === Changes (call Normalize after them) ===
        DockNode& MakeNode(DockNodeKind kind)
        {
            DockNode node;
            node.Id = m_nextNode++;
            node.Kind = kind;
            m_nodes.PushBack(core::Move(node));
            return m_nodes.Back();
        }
        [[nodiscard]] DockNode* Node(DockNodeId id) noexcept { return const_cast<DockNode*>(FindNode(id)); }
        [[nodiscard]] DockSurface* SurfaceRef(DockSurfaceId id) noexcept { return const_cast<DockSurface*>(FindSurface(id)); }
        [[nodiscard]] static u32 IndexIn(const DockNode& stack, core::StringView panel) noexcept
        {
            for (usize i = 0; i < stack.Panels.Size(); ++i)
                if (stack.Panels[i].AsView() == panel)
                    return static_cast<u32>(i);
            return 0;
        }
        void Unhide(core::StringView panel)
        {
            for (usize i = 0; i < m_hidden.Size(); ++i)
                if (m_hidden[i].Panel.AsView() == panel)
                {
                    m_hidden.RemoveAt(i);
                    return;
                }
        }
        // Takes `panel` out of its stack, which may be left empty until Normalize.
        void TakeOut(core::StringView panel)
        {
            DockNode& stack = *Node(StackOf(panel));
            const u32 at = IndexIn(stack, panel);
            stack.Panels.RemoveAt(at);
            if (stack.Active > at || stack.Active >= stack.Panels.Size())
                stack.Active = stack.Active > 0 ? stack.Active - 1 : 0;
        }
        void PlaceTab(core::StringView panel, DockNodeId stackId, i32 index)
        {
            DockNode& stack = *Node(stackId);
            const usize at = index < 0 ? stack.Panels.Size() : Min(static_cast<usize>(index), stack.Panels.Size());
            stack.Panels.Insert(at, String(panel));
            stack.Active = static_cast<u32>(at);
        }
        // Makes a new stack holding `panel` on `side` of `targetId`, with `share` of its room.
        void PlaceBeside(core::StringView panel, DockNodeId targetId, DockSide side, f32 share)
        {
            DockNode& made = MakeNode(DockNodeKind::Stack);
            made.Panels.PushBack(String(panel));
            PlaceNodeBeside(made.Id, targetId, side, share);
        }
        // Places node `madeId`, which is attached nowhere yet, on `side` of `targetId` with `share`
        // of its room.
        void PlaceNodeBeside(DockNodeId madeId, DockNodeId targetId, DockSide side, f32 share)
        {
            share = Clamp(share, 0.05f, 0.95f);
            DockNode* target = Node(targetId);
            const DockAxis axis = AxisOf(side);
            DockNode* parent = Node(target->Parent);
            if (parent != nullptr && parent->Axis == axis)
            {
                // The target's parent split has the same axis: insert the node beside the target
                // there and divide the target's share between them.
                usize at = 0;
                while (parent->Children[at] != targetId)
                    ++at;
                const f32 had = parent->Weights[at];
                parent->Weights[at] = had * (1.0f - share);
                const usize place = IsBefore(side) ? at : at + 1;
                parent->Children.Insert(place, madeId);
                parent->Weights.Insert(place, had * share);
                Node(madeId)->Parent = parent->Id;
                return;
            }
            Wrap(targetId, madeId, axis, IsBefore(side), share);
        }
        // Makes a new stack holding `panel` along the edge of the whole surface.
        void PlaceAtEdge(core::StringView panel, DockSurfaceId surfaceId, DockSide side, f32 share)
        {
            DockNode& made = MakeNode(DockNodeKind::Stack);
            made.Panels.PushBack(String(panel));
            PlaceNodeAtEdge(made.Id, surfaceId, side, share);
        }
        // Places node `madeId`, which is attached nowhere yet, along the edge of the whole surface.
        void PlaceNodeAtEdge(DockNodeId madeId, DockSurfaceId surfaceId, DockSide side, f32 share)
        {
            share = Clamp(share, 0.05f, 0.95f);
            const DockAxis axis = AxisOf(side);
            DockNode* root = Node(SurfaceRef(surfaceId)->Root);
            if (!root->IsStack() && root->Axis == axis)
            {
                for (f32& weight : root->Weights)
                    weight *= 1.0f - share;
                const usize place = IsBefore(side) ? 0 : root->Children.Size();
                root->Children.Insert(place, madeId);
                root->Weights.Insert(place, share);
                Node(madeId)->Parent = root->Id;
                return;
            }
            Wrap(root->Id, madeId, axis, IsBefore(side), share);
        }
        // Puts `targetId` and `madeId` in a new split of `axis` where `targetId` was, `madeId` first
        // when `before`. `madeId` takes `share` of the room.
        void Wrap(DockNodeId targetId, DockNodeId madeId, DockAxis axis, bool before, f32 share)
        {
            DockNode& split = MakeNode(DockNodeKind::Split);
            const DockNodeId splitId = split.Id;
            split.Axis = axis;
            DockNode* target = Node(targetId);
            split.Parent = target->Parent;
            if (DockNode* parent = Node(target->Parent))
            {
                for (DockNodeId& child : parent->Children)
                    if (child == targetId)
                        child = splitId;
            }
            else
                for (DockSurface& surface : m_surfaces)
                    if (surface.Root == targetId)
                        surface.Root = splitId;
            DockNode& made = *Node(splitId);
            made.Children.PushBack(before ? madeId : targetId);
            made.Children.PushBack(before ? targetId : madeId);
            made.Weights.PushBack(before ? share : 1.0f - share);
            made.Weights.PushBack(before ? 1.0f - share : share);
            Node(targetId)->Parent = splitId;
            Node(madeId)->Parent = splitId;
        }
        // Removes child `index` from `split`, its share to its neighbours in proportion to theirs.
        void RemoveChild(DockNode& split, usize index)
        {
            const f32 gone = split.Weights[index];
            split.Children.RemoveAt(index);
            split.Weights.RemoveAt(index);
            const f32 before = index > 0 ? split.Weights[index - 1] : 0.0f;
            const f32 after = index < split.Weights.Size() ? split.Weights[index] : 0.0f;
            const f32 both = before + after;
            if (index > 0)
                split.Weights[index - 1] += both > 0.0f ? gone * before / both : gone;
            if (index < split.Weights.Size())
                split.Weights[index] += both > 0.0f ? gone * after / both : (index > 0 ? 0.0f : gone);
        }
        void Erase(DockNodeId id)
        {
            for (usize i = 0; i < m_nodes.Size(); ++i)
                if (m_nodes[i].Id == id)
                {
                    m_nodes.RemoveAt(i);
                    return;
                }
        }
        // Detaches `id` from its parent, or from its surface when it is a root, in which case a
        // floating surface is removed. The main surface is never left without a root, since its
        // central stack is never removed.
        void Detach(DockNodeId id)
        {
            DockNode* node = Node(id);
            if (DockNode* parent = Node(node->Parent))
            {
                for (usize i = 0; i < parent->Children.Size(); ++i)
                    if (parent->Children[i] == id)
                    {
                        RemoveChild(*parent, i);
                        break;
                    }
                return;
            }
            for (usize i = 0; i < m_surfaces.Size(); ++i)
                if (m_surfaces[i].Root == id && m_surfaces[i].Kind == DockSurfaceKind::Floating)
                {
                    m_surfaces.RemoveAt(i);
                    return;
                }
        }
        // Puts `childId` where `splitId` was (in its parent, with its share; or as a surface's root).
        void Replace(DockNodeId splitId, DockNodeId childId)
        {
            DockNode* split = Node(splitId);
            const DockNodeId parentId = split->Parent;
            if (DockNode* parent = Node(parentId))
            {
                for (DockNodeId& child : parent->Children)
                    if (child == splitId)
                        child = childId;
            }
            else
                for (DockSurface& surface : m_surfaces)
                    if (surface.Root == splitId)
                        surface.Root = childId;
            Node(childId)->Parent = parentId;
        }
        // Takes a stack out of where it is, attached nowhere and ready to be placed; a floating
        // surface that held only this stack is removed. The central stack itself stays where it is
        // and is emptied: a new stack takes its panels and is the one returned.
        DockNodeId TakeStack(DockNodeId stack)
        {
            if (Node(stack)->Has(DockStackCentral))
            {
                DockNode& made = MakeNode(DockNodeKind::Stack);
                DockNode& central = *Node(stack);
                made.Panels = core::Move(central.Panels);
                made.Active = central.Active;
                central.Panels.Clear();
                central.Active = 0;
                return made.Id;
            }
            Detach(stack);
            Node(stack)->Parent = kNoDock;
            return stack;
        }
        // Takes the root off a floating surface, leaving the root attached nowhere, and removes the
        // surface.
        DockNodeId Lift(DockSurfaceId surface)
        {
            const DockNodeId root = SurfaceRef(surface)->Root;
            Detach(root); // detaching a floating surface's root removes the surface
            Node(root)->Parent = kNoDock;
            return root;
        }
        // Collects the panels under `id` into `out` in order (stacks depth first), and the first
        // stack's active panel into `active`.
        void CollectPanels(DockNodeId id, Array<String>& out, String& active) const
        {
            const DockNode* node = FindNode(id);
            if (node == nullptr)
                return;
            if (node->IsStack())
            {
                if (active.IsEmpty() && !node->Panels.IsEmpty())
                    active = node->Panels[Min(static_cast<usize>(node->Active), node->Panels.Size() - 1)];
                for (const String& panel : node->Panels)
                    out.PushBack(panel);
                return;
            }
            for (const DockNodeId child : node->Children)
                CollectPanels(child, out, active);
        }
        [[nodiscard]] Array<DockNodeId> StacksOf(DockSurfaceId surface) const
        {
            Array<DockNodeId> out;
            for (const DockNode& node : m_nodes)
                if (node.IsStack() && SurfaceOf(node.Id) == surface)
                    out.PushBack(node.Id);
            return out;
        }
        void Normalize();
        void NeighboursBeside(DockNodeId stack, Array<String>& out) const;

        Array<DockNode> m_nodes;
        Array<DockSurface> m_surfaces;
        Array<DockHidden> m_hidden;
        DockNodeId m_nextNode = 1;
        DockSurfaceId m_nextSurface = 1;
    };

    inline void DockLayout::Normalize()
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            for (usize i = 0; i < m_nodes.Size() && !changed; ++i)
            {
                DockNode& node = m_nodes[i];
                if (node.IsStack())
                {
                    // An empty stack is removed, except the central one.
                    if (node.Panels.IsEmpty() && !node.Has(DockStackCentral))
                    {
                        const DockNodeId id = node.Id;
                        Detach(id);
                        Erase(id);
                        changed = true;
                    }
                    continue;
                }
                const DockNodeId id = node.Id;
                if (node.Children.Size() <= 1)
                {
                    // A split with one child is replaced by it; a split with none is removed.
                    if (node.Children.Size() == 1)
                        Replace(id, node.Children[0]);
                    else
                        Detach(id);
                    Erase(id);
                    changed = true;
                    continue;
                }
                // A child split of the same axis is merged in: its children take its place and
                // divide its share.
                for (usize c = 0; c < node.Children.Size(); ++c)
                {
                    DockNode* child = Node(node.Children[c]);
                    if (child == nullptr || child->IsStack() || child->Axis != node.Axis)
                        continue;
                    const DockNodeId childId = child->Id;
                    const Array<DockNodeId> grand = child->Children;
                    Array<f32> shares = child->Weights;
                    f32 sum = 0.0f;
                    for (const f32 share : shares)
                        sum += share;
                    DockNode& outer = *Node(id);
                    const f32 had = outer.Weights[c];
                    outer.Children.RemoveAt(c);
                    outer.Weights.RemoveAt(c);
                    for (usize g = 0; g < grand.Size(); ++g)
                    {
                        outer.Children.Insert(c + g, grand[g]);
                        outer.Weights.Insert(c + g, sum > 0.0f ? had * shares[g] / sum : had / static_cast<f32>(grand.Size()));
                        Node(grand[g])->Parent = id;
                    }
                    Erase(childId);
                    changed = true;
                    break;
                }
            }
        }
        // Make the shares sum to one, keep each stack's active index within its panels, and clear
        // a surface's maximized stack when it is no longer on that surface.
        for (DockNode& node : m_nodes)
        {
            if (node.IsStack())
            {
                if (node.Active >= node.Panels.Size())
                    node.Active = node.Panels.IsEmpty() ? 0 : static_cast<u32>(node.Panels.Size() - 1);
                continue;
            }
            f32 sum = 0.0f;
            for (const f32 weight : node.Weights)
                sum += weight;
            for (f32& weight : node.Weights)
                weight = sum > 0.0f ? weight / sum : 1.0f / static_cast<f32>(node.Weights.Size());
        }
        for (DockSurface& surface : m_surfaces)
            if (surface.Maximized != kNoDock && SurfaceOf(surface.Maximized) != surface.Id)
                surface.Maximized = kNoDock;
    }

    inline void DockLayout::NeighboursBeside(DockNodeId stack, Array<String>& out) const
    {
        const DockNode* node = FindNode(stack);
        const DockNode* parent = node != nullptr ? FindNode(node->Parent) : nullptr;
        if (parent == nullptr)
            return;
        for (const DockNodeId sibling : parent->Children)
        {
            if (sibling == stack)
                continue;
            // The stacks in the sibling's subtree, nearest first, which is enough to find a place
            // near the panel.
            Array<DockNodeId> todo;
            todo.PushBack(sibling);
            while (!todo.IsEmpty())
            {
                const DockNode* at = FindNode(todo[0]);
                todo.RemoveAt(0);
                if (at == nullptr)
                    continue;
                if (at->IsStack())
                    for (const String& panel : at->Panels)
                        out.PushBack(panel);
                else
                    for (const DockNodeId child : at->Children)
                        todo.PushBack(child);
            }
        }
    }

    inline bool DockLayout::CheckInvariants(String* why) const
    {
        const auto fail = [why](core::StringView what)
        {
            if (why != nullptr)
                *why = String(what);
            return false;
        };
        if (m_surfaces.IsEmpty() || m_surfaces[0].Kind != DockSurfaceKind::Main)
            return fail(u8"the main surface is not first");
        for (const DockNode& node : m_nodes)
            if (node.Id == kNoDock || node.Id >= m_nextNode)
                return fail(u8"a node's id past the next one made"); // the next id made would collide with it
        for (const DockSurface& surface : m_surfaces)
            if (surface.Id == kNoDock || surface.Id >= m_nextSurface)
                return fail(u8"a surface's id past the next one made");
        Array<DockNodeId> seen;
        Array<String> panels;
        for (const DockSurface& surface : m_surfaces)
        {
            const DockNode* root = FindNode(surface.Root);
            if (root == nullptr || root->Parent != kNoDock)
                return fail(u8"a surface's root is missing or has a parent");
            Array<DockNodeId> todo;
            todo.PushBack(root->Id);
            usize surfacePanels = 0;
            while (!todo.IsEmpty())
            {
                const DockNodeId id = todo.Back();
                todo.PopBack();
                for (const DockNodeId s : seen)
                    if (s == id)
                        return fail(u8"a node is in two places");
                seen.PushBack(id);
                const DockNode* node = FindNode(id);
                if (node == nullptr)
                    return fail(u8"a child is missing");
                if (node->IsStack())
                {
                    if (node->Panels.IsEmpty() && !node->Has(DockStackCentral))
                        return fail(u8"an empty stack");
                    if (!node->Panels.IsEmpty() && node->Active >= node->Panels.Size())
                        return fail(u8"an active tab past the end");
                    for (const String& panel : node->Panels)
                    {
                        for (const String& p : panels)
                            if (p == panel)
                                return fail(u8"a panel docked twice");
                        panels.PushBack(panel);
                        ++surfacePanels;
                    }
                    continue;
                }
                if (node->Children.Size() < 2 || node->Weights.Size() != node->Children.Size())
                    return fail(u8"a split of fewer than two, or shares not one each");
                f32 sum = 0.0f;
                for (usize c = 0; c < node->Children.Size(); ++c)
                {
                    sum += node->Weights[c];
                    const DockNode* child = FindNode(node->Children[c]);
                    if (child == nullptr || child->Parent != id)
                        return fail(u8"a child's parent is not its split");
                    if (!child->IsStack() && child->Axis == node->Axis)
                        return fail(u8"a split inside a split of its axis");
                    todo.PushBack(child->Id);
                }
                if (Abs(sum - 1.0f) > 1.0e-3f)
                    return fail(u8"shares that do not sum to one");
            }
            if (surface.Kind == DockSurfaceKind::Floating && surfacePanels == 0)
                return fail(u8"an empty floating surface");
        }
        if (seen.Size() != m_nodes.Size())
            return fail(u8"a node in no surface");
        const DockNodeId central = CentralStack();
        if (central == kNoDock || SurfaceOf(central) != MainSurface())
            return fail(u8"the central stack is not in the main surface");
        for (const DockHidden& hidden : m_hidden)
            for (const String& p : panels)
                if (p == hidden.Panel)
                    return fail(u8"a panel both docked and hidden");
        return true;
    }
}

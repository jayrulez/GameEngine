// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :controller partition: DockController, a layout and the panels it places.
//
// Panels are registered by id with a description: the title and icon their tab shows, a factory
// for their content, what closing them does, whether they can be closed, and where they go when a
// restored layout does not place them. The controller owns each panel's content once made, so a
// panel moved to another stack or window, or hidden, keeps it. Closing a panel follows its policy:
// Hide keeps its content and its place, Rebuild lets its content go and keeps its place (the
// factory makes it again when shown), Destroy lets both go. A panel may ask before it closes (a
// document with unsaved changes), and its title and tab classes change while it is docked.
//
// The active panel is the one the user works in: the chosen panel of the stack the focus was last
// in. An application follows it (its active document, say) through SetOnActiveChanged.
//
// The layout changes through the controller, which tells every DockSpace showing it to show it
// again. Changes the user already made in the widgets (a separator dragged, a tab chosen or dragged
// to another place in its strip) are noted into the layout without showing it again, because the
// widgets already show them.

module;
#include "Core/Prelude.h"

export module foundation.gui.docking:controller;

import foundation.core;
import foundation.gui; // Node
import :layout;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    enum class DockClosePolicy : u8
    {
        Hide,    ///< content kept, place kept
        Rebuild, ///< content released, place kept; the factory makes it again when shown
        Destroy, ///< content released and place forgotten
    };

    struct DockPanelDescription
    {
        String Id;
        String Title; ///< the text of the panel's tab (the id when empty)
        String Icon;  ///< the path of the tab's icon
        String TabClasses; ///< classes for the tab, space-separated (such as a document's unsaved mark)
        Function<RefPtr<Node>()> Factory;
        DockClosePolicy Close = DockClosePolicy::Hide;
        bool Closable = true;
        /// Called before the panel closes, whether the close comes from its tab, a menu, a key or
        /// its window. Returning false keeps it open: a document with unsaved changes asks the user
        /// first and then closes with CloseNow. It must not register a panel while it asks.
        Function<bool()> CanClose;
        DockDefault Default; ///< where the panel goes when a restored layout does not place it (Panel is the panel's id)
    };

    /// A widget that shows a controller's layout. The controller tells it to show the layout again
    /// when the layout changes.
    class IDockView
    {
    public:
        virtual ~IDockView() = default;
        virtual void ShowLayout() = 0;
        /// Returns the panel that `page` belongs to, or null for a page this view does not show.
        [[nodiscard]] virtual const String* PanelOf(const Node& page) const = 0;
    };

    /// Where a dragged panel goes when dropped: into a stack's tab strip at a tab position, into a
    /// stack (dropped on its middle), beside a stack (dropped on its edge band), or along the edge of
    /// a whole surface.
    enum class DockDropKind : u8
    {
        Tab,
        Middle,
        Beside,
        Edge,
    };
    struct DockDrop
    {
        DockDropKind Kind = DockDropKind::Middle;
        DockNodeId Stack = kNoDock;      ///< Tab, Middle, Beside: the stack dropped on
        DockSurfaceId Surface = kNoDock; ///< Edge: the surface
        i32 Index = -1;                  ///< Tab: the position among the stack's tabs before the drop
        DockSide Side = DockSide::Start; ///< Beside, Edge
    };
    /// What a dock drag carries: a docked panel (a tab torn out), a whole floating surface (a
    /// float dragged by its title bar) or a whole stack (dragged by the grip on its strip).
    struct DockDragged
    {
        String Panel;
        DockSurfaceId Surface = kNoDock;
        DockNodeId Stack = kNoDock;
        [[nodiscard]] bool IsSurface() const noexcept { return Surface != kNoDock; }
        [[nodiscard]] bool IsStack() const noexcept { return Stack != kNoDock; }
    };
    /// The drag type of a whole floating surface; the drag's value is the surface's id.
    inline constexpr const char8_t* kDockSurfaceDragType = u8"gui/dock-surface";
    /// The drag type of a whole stack; the drag's value is the stack's id.
    inline constexpr const char8_t* kDockStackDragType = u8"gui/dock-stack";

    /// Applies the drop of a docked panel to `layout` as the move it stands for. `layout` is either
    /// a copy, to preview the move and report why it is refused, or the controller's own layout.
    inline DockOutcome ApplyDrop(DockLayout& layout, core::StringView panel, const DockDrop& drop)
    {
        switch (drop.Kind)
        {
        case DockDropKind::Tab:
        {
            i32 index = drop.Index;
            // Dropped among its own tabs: the index counts the panel at its current place, so an
            // index past it is one less once the panel is taken out.
            if (const DockNode* stack = layout.FindNode(drop.Stack); stack != nullptr && layout.StackOf(panel) == drop.Stack)
                for (usize i = 0; i < stack->Panels.Size(); ++i)
                    if (stack->Panels[i].AsView() == panel && index > static_cast<i32>(i))
                        --index;
            return layout.MoveToTab(panel, drop.Stack, index);
        }
        case DockDropKind::Middle: return layout.MoveToTab(panel, drop.Stack);
        case DockDropKind::Beside: return layout.MoveBeside(panel, drop.Stack, drop.Side);
        case DockDropKind::Edge: return layout.MoveToEdge(panel, drop.Surface, drop.Side);
        }
        return DockOutcome::Refuse(u8"no such drop");
    }
    /// Applies the drop of whatever a drag carries. A panel is handled as above. A floating surface
    /// docks whole: into the stack when dropped over its strip or middle, beside the stack, or along
    /// the surface's edge. A stack moves whole the same ways, at a tab position when dropped over a
    /// strip.
    inline DockOutcome ApplyDrop(DockLayout& layout, const DockDragged& dragged, const DockDrop& drop)
    {
        if (dragged.IsStack())
            switch (drop.Kind)
            {
            case DockDropKind::Tab: return layout.MoveStackInto(dragged.Stack, drop.Stack, drop.Index);
            case DockDropKind::Middle: return layout.MoveStackInto(dragged.Stack, drop.Stack);
            case DockDropKind::Beside: return layout.MoveStackBeside(dragged.Stack, drop.Stack, drop.Side);
            case DockDropKind::Edge: return layout.MoveStackToEdge(dragged.Stack, drop.Surface, drop.Side);
            }
        if (!dragged.IsSurface())
            return ApplyDrop(layout, dragged.Panel.AsView(), drop);
        switch (drop.Kind)
        {
        case DockDropKind::Tab:
        case DockDropKind::Middle: return layout.DockSurfaceInto(dragged.Surface, drop.Stack);
        case DockDropKind::Beside: return layout.DockSurfaceBeside(dragged.Surface, drop.Stack, drop.Side);
        case DockDropKind::Edge: return layout.DockSurfaceAtEdge(dragged.Surface, drop.Surface, drop.Side);
        }
        return DockOutcome::Refuse(u8"no such drop");
    }
    /// Returns the panels a drag carries, as `layout` has them; for a surface, in the order of its
    /// stacks.
    [[nodiscard]] inline Array<String> PanelsOf(const DockLayout& layout, const DockDragged& dragged)
    {
        Array<String> out;
        if (dragged.IsStack())
        {
            if (const DockNode* stack = layout.FindNode(dragged.Stack))
                for (const String& panel : stack->Panels)
                    out.PushBack(panel);
            return out;
        }
        if (!dragged.IsSurface())
        {
            out.PushBack(dragged.Panel);
            return out;
        }
        for (const DockNode& node : layout.Nodes())
            if (node.IsStack() && layout.SurfaceOf(node.Id) == dragged.Surface)
                for (const String& panel : node.Panels)
                    out.PushBack(panel);
        return out;
    }

    class DockController
    {
    public:
        explicit DockController(IAllocator& allocator) noexcept : m_panels(allocator), m_contents(allocator), m_views(allocator) {}
        DockController(const DockController&) = delete;
        DockController& operator=(const DockController&) = delete;

        /// Registers a panel the layout may place. Returns false when its id is empty or taken.
        bool Register(DockPanelDescription description)
        {
            if (description.Id.IsEmpty() || Describe(description.Id.AsView()) != nullptr)
                return false;
            description.Default.Panel = description.Id;
            m_panels.PushBack(core::Move(description));
            ShowEverywhere(); // a placeholder left for the panel may show it now
            return true;
        }
        /// Removes a panel for good (for example when a document is closed): takes it out of the
        /// layout without asking, forgets its content and its place, and drops its description.
        /// Returns false for a panel that is not registered.
        bool Unregister(core::StringView panel)
        {
            usize index = m_panels.Size();
            for (usize i = 0; i < m_panels.Size(); ++i)
                if (m_panels[i].Id.AsView() == panel)
                    index = i;
            if (index == m_panels.Size())
                return false;
            const String id(panel); // `panel` may be the description's own id
            ForgetContent(id.AsView());
            if (m_layout.StackOf(id.AsView()) != kNoDock)
                (void)m_layout.Close(id.AsView());
            (void)m_layout.Forget(id.AsView());
            m_panels.RemoveAt(index);
            ShowEverywhere();
            return true;
        }
        /// The registered panels, in the order they registered.
        [[nodiscard]] Span<const DockPanelDescription> Panels() const noexcept { return Span<const DockPanelDescription>(m_panels.Data(), m_panels.Size()); }
        [[nodiscard]] const DockPanelDescription* Describe(core::StringView panel) const noexcept
        {
            for (const DockPanelDescription& description : m_panels)
                if (description.Id.AsView() == panel)
                    return &description;
            return nullptr;
        }

        [[nodiscard]] const DockLayout& GetLayout() const noexcept { return m_layout; }
        /// Changes the layout through `edit`; when the edit changed something, every view shows the
        /// new layout.
        DockOutcome Edit(const Function<DockOutcome(DockLayout&)>& edit)
        {
            DockOutcome outcome = edit(m_layout);
            if (outcome.Ok())
                ShowEverywhere();
            return outcome;
        }
        /// Closes a docked panel as its description's policy says, once its CanClose agrees.
        DockOutcome Close(core::StringView panel)
        {
            const DockPanelDescription* description = Describe(panel);
            if (description != nullptr && !description->Closable)
                return DockOutcome::Refuse(u8"it cannot be closed");
            if (description != nullptr && description->CanClose && !description->CanClose())
                return DockOutcome::Refuse(u8"it asked to stay open");
            return CloseNow(panel);
        }
        /// Closes a docked panel as its description's policy says without calling CanClose. A panel
        /// that asked the user calls this once the user agrees.
        DockOutcome CloseNow(core::StringView panel)
        {
            const DockPanelDescription* description = Describe(panel);
            if (description != nullptr && !description->Closable)
                return DockOutcome::Refuse(u8"it cannot be closed");
            DockOutcome outcome = m_layout.Close(panel);
            if (!outcome.Ok())
                return outcome;
            const DockClosePolicy policy = description != nullptr ? description->Close : DockClosePolicy::Hide;
            if (policy != DockClosePolicy::Hide)
                ForgetContent(panel);
            if (policy == DockClosePolicy::Destroy)
                (void)m_layout.Forget(panel);
            ShowEverywhere();
            return outcome;
        }
        /// Shows a registered panel: where it was closed, else at its default place.
        DockOutcome Show(core::StringView panel)
        {
            if (m_layout.StackOf(panel) != kNoDock)
                return DockOutcome::Refuse(u8"it is docked");
            if (m_layout.FindHidden(panel) != nullptr)
                return Edit([panel](DockLayout& layout) { return layout.Show(panel); });
            const DockPanelDescription* description = Describe(panel);
            if (description == nullptr)
                return DockOutcome::Refuse(u8"it is not registered");
            const DockDefault where = description->Default;
            return Edit(
                [panel, where](DockLayout& layout)
                {
                    return where.Central ? layout.InsertTab(panel, layout.CentralStack())
                                         : layout.InsertAtEdge(panel, layout.MainSurface(), where.Side, where.Share);
                });
        }
        /// Adopts a restored layout through DockLayout::Adopt, with the registered panels' default
        /// places.
        DockOutcome Adopt(DockLayout read, Span<const Rectangle> screens = {})
        {
            Array<DockDefault> defaults;
            for (const DockPanelDescription& description : m_panels)
                defaults.PushBack(description.Default);
            DockOutcome outcome = m_layout.Adopt(core::Move(read), Span<const DockDefault>(defaults.Data(), defaults.Size()), screens);
            if (outcome.Ok())
                ShowEverywhere();
            return outcome;
        }

        /// Sets a registered panel's tab text, shown at once wherever it is docked. Returns false for
        /// a panel that is not registered.
        bool SetTitle(core::StringView panel, core::StringView title)
        {
            DockPanelDescription* description = Find(panel);
            if (description == nullptr)
                return false;
            if (description->Title.AsView() != title)
            {
                description->Title = String(title);
                ShowEverywhere();
            }
            return true;
        }
        /// Sets a registered panel's tab classes (space-separated), replacing the previous ones and
        /// shown at once wherever it is docked. Returns false for a panel that is not registered.
        bool SetTabClasses(core::StringView panel, core::StringView classes)
        {
            DockPanelDescription* description = Find(panel);
            if (description == nullptr)
                return false;
            if (description->TabClasses.AsView() != classes)
            {
                description->TabClasses = String(classes);
                ShowEverywhere();
            }
            return true;
        }

        // === The active panel ===
        /// Returns the panel the user works in: the chosen panel of the stack the focus was last in,
        /// or empty before the focus has been in any stack. It changes when a tab is chosen in that
        /// stack or the focus goes into another stack. When the active panel closes, the stack's next
        /// chosen panel becomes active, or none does when the stack is gone.
        [[nodiscard]] core::StringView ActivePanel() const noexcept { return m_active.AsView(); }
        /// Sets the handler called each time the active panel changes, with the panel's id (empty
        /// when no panel is active).
        void SetOnActiveChanged(Function<void(core::StringView)> handler) { m_onActiveChanged = core::Move(handler); }

        /// Returns a registered panel's content. The factory makes it the first time it is asked for
        /// and the controller keeps it from then on. Returns null for a panel that is not registered
        /// or whose factory makes nothing.
        RefPtr<Node> Content(core::StringView panel)
        {
            for (const Made& made : m_contents)
                if (made.Panel.AsView() == panel)
                    return made.Content;
            const DockPanelDescription* description = Describe(panel);
            if (description == nullptr || !description->Factory)
                return {};
            RefPtr<Node> content = description->Factory();
            if (content)
                m_contents.PushBack(Made{String(panel), content});
            return content;
        }
        /// Whether a panel's content has been made and is kept.
        [[nodiscard]] bool HasContent(core::StringView panel) const noexcept
        {
            for (const Made& made : m_contents)
                if (made.Panel.AsView() == panel)
                    return true;
            return false;
        }

        // === Changes the widgets already show, noted without showing the layout again ===
        void NoteWeights(DockNodeId split, Span<const f32> weights) { (void)m_layout.SetWeights(split, weights); }
        void NoteActive(core::StringView panel)
        {
            (void)m_layout.Activate(panel);
            const DockNodeId stack = m_layout.StackOf(panel);
            if (stack != kNoDock && stack == m_activeStack)
                SetActive(panel, stack);
        }
        /// Called when the focus goes into a stack: its chosen panel becomes the active one.
        void NoteFocused(DockNodeId stack)
        {
            const DockNode* node = m_layout.FindNode(stack);
            if (node == nullptr || !node->IsStack() || node->Panels.IsEmpty())
                return;
            const String panel = node->Panels[Min(static_cast<usize>(node->Active), node->Panels.Size() - 1)];
            SetActive(panel.AsView(), stack);
        }
        void NoteTabPlace(core::StringView panel, DockNodeId stack, i32 index) { (void)m_layout.MoveToTab(panel, stack, index); }
        void NoteSurfaceBounds(DockSurfaceId surface, Rectangle bounds)
        {
            if (const DockSurface* found = m_layout.FindSurface(surface))
            {
                const String screen = found->Screen;
                (void)m_layout.SetSurfaceBounds(surface, bounds, screen.AsView(), found->Scale);
            }
        }

        /// Returns the panel that a page shown by some view belongs to (used for a dragged tab), or
        /// null when no view shows the page.
        [[nodiscard]] const String* PanelShownBy(const Node& page) const
        {
            for (const IDockView* view : m_views)
                if (const String* panel = view->PanelOf(page))
                    return panel;
            return nullptr;
        }

        // === The views ===
        void AddView(IDockView& view)
        {
            for (IDockView* known : m_views)
                if (known == &view)
                    return;
            m_views.PushBack(&view);
        }
        void RemoveView(IDockView& view)
        {
            for (usize i = 0; i < m_views.Size(); ++i)
                if (m_views[i] == &view)
                {
                    m_views.RemoveAt(i);
                    if (m_showing && static_cast<i64>(i) <= m_showAt)
                        --m_showAt; // the pass continues with the view that moved into the removed one's slot
                    return;
                }
        }

    private:
        struct Made
        {
            String Panel;
            RefPtr<Node> Content;
        };
        [[nodiscard]] DockPanelDescription* Find(core::StringView panel) noexcept
        {
            for (DockPanelDescription& description : m_panels)
                if (description.Id.AsView() == panel)
                    return &description;
            return nullptr;
        }
        void SetActive(core::StringView panel, DockNodeId stack)
        {
            m_activeStack = stack;
            if (m_active.AsView() == panel)
                return;
            m_active = String(panel);
            if (m_onActiveChanged)
                m_onActiveChanged(m_active.AsView()); // the handler may change the layout, so nothing runs after it
        }
        // Keeps the active panel right after a change. An active panel that moved stays active in
        // its new stack. One that closed gives way to its stack's chosen panel, or to none when the
        // stack is gone.
        void KeepActive()
        {
            if (m_active.IsEmpty())
                return;
            const DockNodeId stack = m_layout.StackOf(m_active.AsView());
            if (stack != kNoDock)
            {
                m_activeStack = stack;
                return;
            }
            const DockNode* node = m_layout.FindNode(m_activeStack);
            if (node != nullptr && node->IsStack() && !node->Panels.IsEmpty())
            {
                const String next = node->Panels[Min(static_cast<usize>(node->Active), node->Panels.Size() - 1)];
                SetActive(next.AsView(), m_activeStack);
                return;
            }
            m_activeStack = kNoDock;
            m_active = String();
            if (m_onActiveChanged)
                m_onActiveChanged(core::StringView{});
        }
        void ForgetContent(core::StringView panel)
        {
            for (usize i = 0; i < m_contents.Size(); ++i)
                if (m_contents[i].Panel.AsView() == panel)
                {
                    m_contents[i].Content->RemoveFromParent();
                    m_contents.RemoveAt(i);
                    return;
                }
        }
        // Has each view show the layout. A view may make or end other views while it does (a
        // floating surface's window and its dock space): a view made during the pass joins it, and
        // a view ended leaves it. A change requested during the pass runs the pass again afterwards.
        void ShowEverywhere()
        {
            if (m_showing)
            {
                m_showAgain = true;
                return;
            }
            m_showing = true;
            do
            {
                m_showAgain = false;
                for (m_showAt = 0; m_showAt < static_cast<i64>(m_views.Size()); ++m_showAt)
                    m_views[static_cast<usize>(m_showAt)]->ShowLayout();
            } while (m_showAgain);
            m_showing = false;
            KeepActive();
        }

        DockLayout m_layout;
        Array<DockPanelDescription> m_panels;
        Array<Made> m_contents;
        Array<IDockView*> m_views;
        String m_active;                 // the active panel (empty: none)
        DockNodeId m_activeStack = kNoDock; // the stack it is in, or was in when it closed
        Function<void(core::StringView)> m_onActiveChanged;
        bool m_showing = false;   // a ShowEverywhere pass is running
        bool m_showAgain = false; // the layout changed during the pass
        i64 m_showAt = 0;         // index of the view the pass is at
    };
}

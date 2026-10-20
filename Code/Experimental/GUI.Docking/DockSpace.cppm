// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :dock_space partition: DockSpace, the widget that shows one surface of a
// controller's layout (D3 of Specs/gui-lab-evolution-3.md).
//
// Its widgets are made from the layout, each kept by the id of the node it shows, so showing the
// layout again changes only what changed: a split is the core's Splitter (round three's ruling 6),
// a stack a TabView whose strip's group is `dock`, each panel a page whose content the controller
// makes the first time its tab is shown. A panel the layout names that is not registered shows a
// placeholder, filled when it is. A maximized stack is shown alone. What the user does in the
// widgets goes back into the layout: a separator dragged, a tab chosen or dragged to another place,
// a tab's close (through the controller, which closes it as its description says).
//
// Docking's default sheet (DockingSheetCSS) styles them through the theme's tokens: a theme or an
// application restyles docking by setting `--dock-*` (round three's ruling 4).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.docking:dock_space;

import foundation.core;
import experimental.gui;
import :layout;
import :controller;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// Docking's default sheet, written against the theme's tokens (an application puts it after the
    /// theme in its user agent sheet).
    [[nodiscard]] inline core::StringView DockingSheetCSS() noexcept
    {
        return core::StringView(u8R"(
            dockspace { background-color: var(--dock-background, var(--surface)); }
            tabview.dock-stack { background-color: var(--dock-stack-background, var(--panel)); }
            tabview.dock-central { background-color: var(--dock-central-background, var(--surface)); }
            label.dock-placeholder { color: var(--dock-placeholder, var(--muted)); padding: 16px; }
            dockspace { edge-band: var(--dock-edge, 64px); surface-band: var(--dock-surface-edge, 20px); }
            panel.dock-preview {
                background-color: var(--dock-preview, rgba(80, 150, 255, 0.25));
                border: 2px solid var(--dock-preview-border, var(--accent));
                border-radius: 4px;
            }
        )");
    }

    class DockSpace;

    /// A stack as a DockSpace shows it: a TabView whose drops are the layout's to decide (its
    /// middle tabs a dragged panel in, an edge band puts it beside).
    class DockStack : public TabView
    {
        RTTI_OBJECT(DockStack, TabView)
    public:
        DockStack(DockSpace& space, DockNodeId node) noexcept : m_space(&space), m_node(node) {}
        [[nodiscard]] DockNodeId GetNode() const noexcept { return m_node; }

    protected:
        void OnDragQuery(DropQuery& query) override;
        void OnDragLeave(const DragEvent& event) override;
        void OnDrop(const DragEvent& event) override;

    private:
        DockSpace* m_space;
        DockNodeId m_node;
    };

    class DockSpace : public UIWidget, public IDockView
    {
        RTTI_OBJECT(DockSpace, UIWidget)
        friend class DockStack;
    public:
        DockSpace()
        {
            SetTag(core::StringView(u8"dockspace"));
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Column;
                s.AlignItems = Align::Stretch;
            });
        }
        ~DockSpace() override
        {
            if (m_controller != nullptr)
                m_controller->RemoveView(*this);
        }
        DockSpace(const DockSpace&) = delete;
        DockSpace& operator=(const DockSpace&) = delete;

        /// Shows `surface` of `controller`'s layout, now and whenever it changes.
        void Show(DockController& controller, DockSurfaceId surface)
        {
            if (m_controller != nullptr)
                m_controller->RemoveView(*this);
            m_controller = &controller;
            m_surface = surface;
            controller.AddView(*this);
            ShowLayout();
        }
        [[nodiscard]] DockSurfaceId GetSurface() const noexcept { return m_surface; }

        /// The widget showing a node (a Splitter or a TabView); null when it shows none.
        [[nodiscard]] UIWidget* WidgetOf(DockNodeId node) const noexcept
        {
            for (const Shown& shown : m_shown)
                if (shown.Node == node)
                    return shown.Widget.Get();
            return nullptr;
        }
        /// The page showing a panel; null when none does.
        [[nodiscard]] TabPage* PageOf(core::StringView panel) const noexcept
        {
            for (const Shown& shown : m_shown)
                for (const Page& page : shown.Pages)
                    if (page.Panel.AsView() == panel)
                        return page.Widget.Get();
            return nullptr;
        }

        [[nodiscard]] const String* PanelOf(const Node& page) const override
        {
            for (const Shown& shown : m_shown)
                for (const Page& p : shown.Pages)
                    if (p.Widget.Get() == &page)
                        return &p.Panel;
            return nullptr;
        }

        // === Drops (D4) ===
        /// How far into a stack its edge band reaches (a third of it at most); the sheet's
        /// `edge-band`, the theme's `--dock-edge`.
        [[nodiscard]] f32 GetEdgeBand() const noexcept { return m_edgeBand; }
        void SetEdgeBand(f32 band) noexcept { m_edgeBand = Max(0.0f, band); }
        /// How far into the surface its own edge band reaches (`surface-band`, `--dock-surface-edge`).
        [[nodiscard]] f32 GetSurfaceBand() const noexcept { return m_surfaceBand; }
        void SetSurfaceBand(f32 band) noexcept { m_surfaceBand = Max(0.0f, band); }

        /// The docked panel a drag carries (a torn-out tab of a page some view of the controller
        /// shows); null for anything else.
        [[nodiscard]] const String* DraggedPanel(const DragPayload& payload) const
        {
            if (m_controller == nullptr || payload.Type.AsView() != core::StringView(u8"lab/tab"))
                return nullptr;
            const Tab* tab = Cast<Tab>(Node::FromHandle(payload.Value));
            const Node* page = tab != nullptr ? tab->GetPage() : nullptr;
            return page != nullptr ? m_controller->PanelShownBy(*page) : nullptr;
        }
        /// The drop a point means (in this widget's space), over `stack` (kNoDock: over none): the
        /// surface's edge within its band, else the stack's edge within its band, else its middle;
        /// false when it means none.
        [[nodiscard]] bool DropAt(core::Float2 at, DockNodeId stack, DockDrop& drop) const
        {
            const Rect bounds = GetScreenBounds();
            const f32 w = bounds.width;
            const f32 h = bounds.height;
            DockSide side{};
            if (NearestEdge(at, Rect{0.0f, 0.0f, w, h}, Min(m_surfaceBand, Min(w, h) / 3.0f), side))
            {
                drop = DockDrop{DockDropKind::Edge, kNoDock, m_surface, -1, side};
                return true;
            }
            const UIWidget* widget = stack != kNoDock ? WidgetOf(stack) : nullptr;
            if (widget == nullptr)
                return false;
            const Rect sb = widget->GetScreenBounds();
            const Rect r{sb.x - bounds.x, sb.y - bounds.y, sb.width, sb.height};
            if (NearestEdge(at, r, Min(m_edgeBand, Min(r.width, r.height) / 3.0f), side))
                drop = DockDrop{DockDropKind::Beside, stack, kNoDock, -1, side};
            else
                drop = DockDrop{DockDropKind::Middle, stack, kNoDock, -1, DockSide::Start};
            return true;
        }
        /// Where the preview of the drop being offered stands (this widget's space); false when none
        /// shows.
        [[nodiscard]] bool GetPreviewBounds(Rect& out) const
        {
            if (!m_preview || !m_preview->IsVisible())
                return false;
            out = m_previewBounds;
            return true;
        }

        /// The layout shown again: the widgets of nodes still there kept and brought in step, those of
        /// nodes gone let go.
        void ShowLayout() override
        {
            if (m_controller == nullptr)
                return;
            const DockLayout& layout = m_controller->GetLayout();
            const DockSurface* surface = layout.FindSurface(m_surface);
            for (Shown& shown : m_shown)
                shown.Used = false;
            RefPtr<UIWidget> top;
            if (surface != nullptr)
            {
                top = Build(surface->Maximized != kNoDock ? surface->Maximized : surface->Root);
                Keep(surface->Root); // a stack maximized: the rest kept for its restore
            }
            Node* const shown = ChildCount() > 0 && GetChildAt(0) != m_preview.Get() ? GetChildAt(0) : nullptr;
            if (shown != top.Get())
            {
                if (shown != nullptr)
                    RemoveChild(shown);
                if (top)
                {
                    top->EditLayoutStyle([](LayoutStyle& s) { s.Grow = 1.0f; });
                    if (top->GetParent() != nullptr)
                        top->RemoveFromParent();
                    AddChildAt(top.Get(), 0); // under the preview
                }
            }
            for (usize i = m_shown.Size(); i > 0; --i)
                if (!m_shown[i - 1].Used)
                {
                    m_shown[i - 1].Widget->RemoveFromParent();
                    m_shown.RemoveAt(i - 1);
                }
        }

    protected:
        void OnDragQuery(DropQuery& query) override
        {
            // Reached over a separator, or past a stack that left it the surface's band.
            const String* panel = DraggedPanel(query.Payload);
            DockDrop drop;
            if (panel != nullptr && DropAt(query.Local, kNoDock, drop))
                Offer(*this, query, panel->AsView(), drop);
        }
        void OnDragLeave(const DragEvent&) override { Withdraw(*this); }
        void OnDrop(const DragEvent&) override { TakeOffer(*this); }

    private:
        struct Page
        {
            String Panel;
            RefPtr<TabPage> Widget;
            bool Registered = false; ///< made for a registered panel, not a placeholder
        };
        struct Shown
        {
            DockNodeId Node = kNoDock;
            DockNodeKind Kind = DockNodeKind::Stack;
            RefPtr<UIWidget> Widget;
            Array<Page> Pages; // a stack's, by panel
            bool Used = false;
        };

        [[nodiscard]] Shown* Find(DockNodeId node) noexcept
        {
            for (Shown& shown : m_shown)
                if (shown.Node == node)
                    return &shown;
            return nullptr;
        }

        // The widgets of `id` and its descendants that are made stay, shown or not.
        void Keep(DockNodeId id)
        {
            const DockNode* node = m_controller->GetLayout().FindNode(id);
            if (node == nullptr)
                return;
            if (Shown* shown = Find(id))
                shown->Used = true;
            for (const DockNodeId child : node->Children)
                Keep(child);
        }

        // The widget for `id`, made or kept, brought in step with the layout.
        RefPtr<UIWidget> Build(DockNodeId id)
        {
            const DockNode* node = m_controller->GetLayout().FindNode(id);
            if (node == nullptr)
                return {};
            Shown* shown = Find(id);
            if (shown != nullptr && shown->Kind != node->Kind)
            {
                shown->Widget->RemoveFromParent();
                shown->Widget = {};
            }
            if (shown == nullptr)
            {
                m_shown.PushBack(Shown{id, node->Kind, {}, {}, false});
                shown = &m_shown.Back();
            }
            shown->Used = true;
            if (node->IsStack())
                return BuildStack(id);
            return BuildSplit(id);
        }

        RefPtr<UIWidget> BuildSplit(DockNodeId id)
        {
            if (!Find(id)->Widget)
            {
                RefPtr<Splitter> made = MakeRef<Splitter>(MemoryAllocator());
                DockSpace* self = this;
                made->SetOnResized(
                    [self, id, splitter = made.Get()]()
                    {
                        const Array<f32> weights = splitter->GetWeights();
                        self->m_controller->NoteWeights(id, Span<const f32>(weights.Data(), weights.Size()));
                    });
                Find(id)->Widget = made;
            }
            // Its children first (building them may grow the table and move entries).
            const DockNode node = *m_controller->GetLayout().FindNode(id);
            Array<RefPtr<UIWidget>> children;
            for (const DockNodeId child : node.Children)
                children.PushBack(Build(child));
            Splitter& splitter = *Cast<Splitter>(Find(id)->Widget.Get());
            splitter.SetAxis(node.Axis == DockAxis::Vertical ? SplitterAxis::Vertical : SplitterAxis::Horizontal);
            // Its panes, in order (the handles between them are the splitter's own).
            bool inOrder = splitter.GetPaneCount() == children.Size();
            for (usize i = 0; inOrder && i < children.Size(); ++i)
                inOrder = splitter.GetPane(i) == children[i].Get();
            if (!inOrder)
            {
                while (splitter.GetPaneCount() > 0)
                    splitter.RemoveChild(splitter.GetPane(splitter.GetPaneCount() - 1));
                for (const RefPtr<UIWidget>& child : children)
                {
                    if (child->GetParent() != nullptr)
                        child->RemoveFromParent();
                    splitter.AddChild(child.Get());
                }
            }
            // Its shares, unless they are what it shows already (a drag just noted them).
            const Array<f32> now = splitter.GetWeights();
            bool same = now.Size() == node.Weights.Size();
            for (usize i = 0; same && i < now.Size(); ++i)
                same = Abs(now[i] - node.Weights[i]) < 1.0e-4f;
            if (!same)
                splitter.SetWeights(Span<const f32>(node.Weights.Data(), node.Weights.Size()));
            return Find(id)->Widget;
        }

        RefPtr<UIWidget> BuildStack(DockNodeId id)
        {
            DockSpace* self = this;
            if (!Find(id)->Widget)
            {
                RefPtr<DockStack> made = MakeRef<DockStack>(MemoryAllocator(), *this, id);
                made->AddClass(core::StringView(u8"dock-stack"));
                // Its strip: a tab dropped there is the layout's move too.
                TabBar* bar = made->GetBar();
                bar->SetOnTabOffered([self, id, bar](DropQuery& query, Tab&, i32 index)
                {
                    if (const String* panel = self->DraggedPanel(query.Payload))
                    {
                        const String dragged = *panel;
                        // The surface's band wins over the strip (a strip along the surface's edge).
                        DockDrop drop{DockDropKind::Tab, id, kNoDock, index, DockSide::Start};
                        DockDrop edge;
                        if (self->DropAt(self->FromNode(*bar, query.Local), kNoDock, edge))
                            drop = edge;
                        self->Offer(*bar, query, dragged.AsView(), drop);
                    }
                    else
                        query.Refuse(core::StringView(u8"it is not a docked panel"));
                });
                bar->SetTabTaker([self, bar](Tab&, i32) { self->TakeOffer(*bar); });
                (void)bar->AddEventListener(EventType::DragLeave, [self, bar](const Event&) { self->Withdraw(*bar); });
                made->SetGroup(String(u8"dock"));
                made->SetReorderable(true);
                TabView* view = made.Get();
                made->SetOnSelected([self, id, view](i32 index) { self->NoteActive(id, *view, index); });
                made->SetOnPageMoved([self, id, view](i32 to) { self->NoteMoved(id, *view, to); });
                made->GetBar()->SetCloseHandler([self, id, view](i32 index) { self->CloseTab(id, *view, index); });
                Find(id)->Widget = made;
            }
            const DockNode node = *m_controller->GetLayout().FindNode(id);
            Shown& shown = *Find(id);
            TabView& view = *Cast<TabView>(shown.Widget.Get());
            if (node.Has(DockStackCentral))
                view.AddClass(core::StringView(u8"dock-central"));
            // Its pages: one a panel, in the panels' order; a panel gone takes its page with it.
            Array<Page> pages;
            for (const String& panel : node.Panels)
            {
                const bool registered = m_controller->Describe(panel.AsView()) != nullptr;
                RefPtr<TabPage> page;
                for (const Page& had : shown.Pages)
                    if (had.Panel == panel && (had.Registered || !registered)) // a placeholder whose panel came is made again
                        page = had.Widget;
                if (!page)
                    page = MakePage(panel.AsView());
                pages.PushBack(Page{panel, page, registered});
            }
            bool inOrder = view.GetPageCount() == static_cast<i32>(pages.Size());
            for (usize i = 0; inOrder && i < pages.Size(); ++i)
                inOrder = view.GetPage(static_cast<i32>(i)) == pages[i].Widget.Get();
            if (!inOrder)
            {
                while (view.GetPageCount() > 0)
                    view.RemovePage(view.GetPageCount() - 1);
                // The active tab chosen before the pages come, so the first to come is not shown
                // (and its content made) on the way.
                view.SetSelectedIndex(static_cast<i32>(node.Active));
                for (const Page& page : pages)
                {
                    if (page.Widget->GetParent() != nullptr)
                        page.Widget->RemoveFromParent();
                    view.AddPage(*page.Widget);
                }
            }
            shown.Pages = core::Move(pages);
            if (!node.Panels.IsEmpty() && view.GetSelectedIndex() != static_cast<i32>(node.Active))
                view.SetSelectedIndex(static_cast<i32>(node.Active));
            return shown.Widget;
        }
        // A panel's page: its title, icon and close from its description; its content made when its
        // tab is first shown, or a placeholder for a panel not registered.
        RefPtr<TabPage> MakePage(core::StringView panel)
        {
            RefPtr<TabPage> page = MakeRef<TabPage>(MemoryAllocator());
            const DockPanelDescription* description = m_controller->Describe(panel);
            page->SetTitle(description != nullptr && !description->Title.IsEmpty() ? description->Title.AsView() : panel);
            if (description != nullptr && !description->Icon.IsEmpty())
                page->SetIconSource(description->Icon);
            page->SetClosable(description == nullptr || description->Closable);
            DockController* controller = m_controller;
            IAllocator* allocator = &MemoryAllocator();
            const String id(panel);
            page->SetContentFactory(
                [controller, allocator, id](TabPage& into)
                {
                    RefPtr<Node> content = controller->Content(id.AsView());
                    if (!content)
                    {
                        RefPtr<Label> placeholder = MakeRef<Label>(*allocator);
                        placeholder->AddClass(core::StringView(u8"dock-placeholder"));
                        placeholder->SetText(Format(u8"{} is not available", id).AsView());
                        content = placeholder;
                    }
                    if (content->GetParent() != nullptr)
                        content->RemoveFromParent(); // it was shown elsewhere
                    into.AddChild(content.Get());
                });
            return page;
        }

        [[nodiscard]] const String* PanelAt(DockNodeId stack, TabView& view, i32 index) const
        {
            const Node* page = view.GetPage(index);
            for (const Shown& shown : m_shown)
                if (shown.Node == stack)
                    for (const Page& p : shown.Pages)
                        if (p.Widget.Get() == page)
                            return &p.Panel;
            return nullptr;
        }
        void NoteActive(DockNodeId stack, TabView& view, i32 index)
        {
            if (const String* panel = PanelAt(stack, view, index))
                m_controller->NoteActive(panel->AsView());
        }
        void NoteMoved(DockNodeId stack, TabView& view, i32 to)
        {
            if (const String* panel = PanelAt(stack, view, to))
            {
                const String moved = *panel;
                m_controller->NoteTabPlace(moved.AsView(), stack, to);
                // The pages' order is the strip's now: kept so, without showing the layout again.
                if (Shown* shown = Find(stack))
                {
                    Array<Page> ordered;
                    for (i32 i = 0; i < view.GetPageCount(); ++i)
                        for (const Page& p : shown->Pages)
                            if (p.Widget.Get() == view.GetPage(i))
                                ordered.PushBack(p);
                    shown->Pages = core::Move(ordered);
                }
            }
        }
        void CloseTab(DockNodeId stack, TabView& view, i32 index)
        {
            if (const String* panel = PanelAt(stack, view, index))
            {
                const String closing = *panel;
                (void)m_controller->Close(closing.AsView());
            }
        }

        // A point in `node`'s space, in this widget's.
        [[nodiscard]] core::Float2 FromNode(const Node& node, core::Float2 local) const
        {
            const Rect space = GetScreenBounds();
            const Rect from = node.GetScreenBounds();
            return core::Float2{local.x + from.x - space.x, local.y + from.y - space.y};
        }

        // The side of `r` whose band of `band` holds `at`, nearest first; false when none does.
        [[nodiscard]] bool NearestEdge(core::Float2 at, const Rect& r, f32 band, DockSide& side) const
        {
            if (band <= 0.0f || at.x < r.x || at.y < r.y || at.x > r.x + r.width || at.y > r.y + r.height)
                return false;
            const f32 left = at.x - r.x;
            const f32 right = r.x + r.width - at.x;
            const f32 top = at.y - r.y;
            const f32 bottom = r.y + r.height - at.y;
            const f32 nearest = Min(Min(left, right), Min(top, bottom));
            if (nearest > band)
                return false;
            const bool rtl = IsRightToLeft();
            if (nearest == left)
                side = rtl ? DockSide::End : DockSide::Start;
            else if (nearest == right)
                side = rtl ? DockSide::Start : DockSide::End;
            else
                side = nearest == top ? DockSide::Top : DockSide::Bottom;
            return true;
        }

        // A drop offered by `owner` (a stack, its strip, or this widget): tried on a copy of the
        // layout, refused with the layout's reason, or accepted with the preview where the panel
        // would be.
        void Offer(Node& owner, DropQuery& query, core::StringView panel, const DockDrop& drop)
        {
            DockLayout after = m_controller->GetLayout();
            const DockOutcome outcome = ApplyDrop(after, panel, drop);
            m_offerOwner = &owner;
            if (!outcome.Ok())
            {
                m_offer = {};
                query.Refuse(outcome.Refused.AsView());
                HidePreview();
                return;
            }
            query.Accept(DropEffect::Move);
            m_offer = Offered{String(panel), drop, true};
            Rect at;
            if (BoundsIn(after, after.StackOf(panel), at))
                ShowPreview(at);
            else
                HidePreview(); // it would go to another surface
        }
        void Withdraw(Node& owner)
        {
            if (m_offerOwner != &owner)
                return; // another took the drag over already
            m_offerOwner = nullptr;
            m_offer = {};
            HidePreview();
        }
        void TakeOffer(Node& owner)
        {
            const Offered offer = m_offerOwner == &owner ? m_offer : Offered{};
            m_offerOwner = nullptr;
            m_offer = {};
            HidePreview();
            if (offer.Valid && m_controller != nullptr)
                (void)m_controller->Edit([&offer](DockLayout& layout) { return ApplyDrop(layout, offer.Panel.AsView(), offer.Drop); });
        }

        // Where `node` stands in `layout`'s surface as this widget would show it: the shares of each
        // split from its root (the separators' room left out).
        [[nodiscard]] bool BoundsIn(const DockLayout& layout, DockNodeId node, Rect& out) const
        {
            const DockSurface* surface = layout.FindSurface(m_surface);
            if (surface == nullptr || node == kNoDock || layout.SurfaceOf(node) != m_surface)
                return false;
            const Rect bounds = GetScreenBounds();
            const Rect whole{0.0f, 0.0f, bounds.width, bounds.height};
            if (surface->Maximized != kNoDock)
            {
                out = whole;
                return node == surface->Maximized;
            }
            return BoundsUnder(layout, surface->Root, node, whole, out);
        }
        [[nodiscard]] bool BoundsUnder(const DockLayout& layout, DockNodeId at, DockNodeId node, const Rect& r, Rect& out) const
        {
            if (at == node)
            {
                out = r;
                return true;
            }
            const DockNode* split = layout.FindNode(at);
            if (split == nullptr || split->IsStack())
                return false;
            const bool vertical = split->Axis == DockAxis::Vertical;
            const bool mirrored = !vertical && IsRightToLeft();
            const f32 extent = vertical ? r.height : r.width;
            f32 from = 0.0f;
            for (usize i = 0; i < split->Children.Size(); ++i)
            {
                const f32 size = extent * split->Weights[i];
                Rect part = r;
                if (vertical)
                {
                    part.y = r.y + from;
                    part.height = size;
                }
                else
                {
                    part.x = mirrored ? r.x + r.width - from - size : r.x + from;
                    part.width = size;
                }
                if (BoundsUnder(layout, split->Children[i], node, part, out))
                    return true;
                from += size;
            }
            return false;
        }

        void ShowPreview(const Rect& at)
        {
            if (!m_preview)
            {
                m_preview = MakeRef<Panel>(MemoryAllocator());
                m_preview->AddClass(core::StringView(u8"dock-preview"));
                m_preview->SetHitTestVisible(false); // the drag sees what is under it
                AddChild(m_preview.Get());
            }
            m_previewBounds = at;
            m_preview->EditLayoutStyle([at](LayoutStyle& s)
            {
                s.Placement = Position::Absolute;
                s.Left = LayoutLength::Px(at.x);
                s.Top = LayoutLength::Px(at.y);
                s.Width = LayoutLength::Px(at.width);
                s.Height = LayoutLength::Px(at.height);
            });
            m_preview->SetVisible(true);
        }
        void HidePreview()
        {
            if (m_preview)
                m_preview->SetVisible(false);
        }

        struct Offered
        {
            String Panel;
            DockDrop Drop;
            bool Valid = false;
        };

        DockController* m_controller = nullptr;
        DockSurfaceId m_surface = kNoDock;
        Array<Shown> m_shown;
        f32 m_edgeBand = 64.0f;
        f32 m_surfaceBand = 20.0f;
        RefPtr<Panel> m_preview;
        Rect m_previewBounds{};
        Node* m_offerOwner = nullptr; // what offered the drop shown (non-owning; compared only)
        Offered m_offer;
    };

    // A stack's drops: the surface's band is the DockSpace's (left for the walk to reach it), the
    // rest the stack's own edge bands and middle.
    inline void DockStack::OnDragQuery(DropQuery& query)
    {
        const String* panel = m_space->DraggedPanel(query.Payload);
        if (panel == nullptr)
            return; // a tab of another kind: not the layout's
        DockDrop drop;
        if (!m_space->DropAt(m_space->FromNode(*this, query.Local), m_node, drop) || drop.Kind == DockDropKind::Edge)
            return;
        const String dragged = *panel;
        m_space->Offer(*this, query, dragged.AsView(), drop);
    }
    inline void DockStack::OnDragLeave(const DragEvent&) { m_space->Withdraw(*this); }
    inline void DockStack::OnDrop(const DragEvent&) { m_space->TakeOffer(*this); }
}

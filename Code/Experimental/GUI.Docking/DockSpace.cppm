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
        )");
    }

    class DockSpace : public UIWidget, public IDockView
    {
        RTTI_OBJECT(DockSpace, UIWidget)
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
            if (ChildCount() != (top ? 1u : 0u) || (top && GetChildAt(0) != top.Get()))
            {
                while (ChildCount() > 0)
                    RemoveChild(GetChildAt(ChildCount() - 1));
                if (top)
                {
                    top->EditLayoutStyle([](LayoutStyle& s) { s.Grow = 1.0f; });
                    AddChild(top.Get());
                }
            }
            for (usize i = m_shown.Size(); i > 0; --i)
                if (!m_shown[i - 1].Used)
                {
                    m_shown[i - 1].Widget->RemoveFromParent();
                    m_shown.RemoveAt(i - 1);
                }
        }

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
                RefPtr<TabView> made = MakeRef<TabView>(MemoryAllocator());
                made->AddClass(core::StringView(u8"dock-stack"));
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

        DockController* m_controller = nullptr;
        DockSurfaceId m_surface = kNoDock;
        Array<Shown> m_shown;
    };

    RTTI_DEFINE_OBJECT(DockSpace, "rtti::gui::docking")
}

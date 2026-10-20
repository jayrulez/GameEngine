// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :dock_area partition
//
// DockArea: floating surfaces within one window (D5 of Specs/gui-lab-evolution-3.md), the virtual
// surface host for the web and one-window platforms. The main surface's DockSpace fills it; each
// floating surface of the layout is a DockFloat over it, a Window holding a DockSpace of its own, so
// a float is a whole dock tree that is docked into like the main one. The area keeps the floats
// with the layout as it changes (one made for a surface that came, one ended for a surface that
// went), each at its surface's bounds (this widget's space), and notes back what the user does to a
// window: moved or resized, its bounds; closed, its panels closed as their policies say. A tab
// dragged with Shift held floats: the dock spaces pass the drop on, and the area shows the window
// at the pointer and makes it there. A float dragged by its title bar follows the pointer and docks
// whole where it is let go over a target (the preview the area its content would take); with Shift
// it is only moved; Escape puts it back. The dock keys reach across its surfaces: an arrow past a
// float's edge goes on to the stack beyond it, `dock.next-surface` and `dock.previous-surface` take
// the focus from one surface to the next, and the Move to menu lists every surface's stacks.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.docking:dock_area;

import foundation.core;
import experimental.gui;
import :layout;
import :controller;
import :dock_space;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class DockArea;

    /// A floating surface's window (`window.dock-float`): its title its panels', its content a
    /// DockSpace showing the surface; closing it closes its panels as their policies say.
    class DockFloat : public Window
    {
        RTTI_OBJECT(DockFloat, Window)
    public:
        DockFloat(DockArea& area, DockSurfaceId surface);
        [[nodiscard]] DockSurfaceId GetSurface() const noexcept { return m_surface; }
        [[nodiscard]] DockSpace* GetSpace() const noexcept { return m_space.Get(); }
        [[nodiscard]] DockArea* GetArea() const noexcept { return m_area; }
        /// Its close button's: its panels closed as their policies say.
        void RequestClose() override;
        /// Being dragged by its title bar (D5): it follows the pointer, out of hit tests with all it
        /// holds so the targets under it answer (`.dock-dragging`).
        [[nodiscard]] bool IsBeingDragged() const noexcept { return m_dragging; }
        /// While it is dragged, nothing of it is under the pointer: the drop is asked of what is below.
        [[nodiscard]] Node* OverFind(core::Float2 worldPoint) override { return m_dragging ? nullptr : Window::OverFind(worldPoint); }

    private:
        friend class DockArea;
        // Its title bar's drag: the whole surface, the window following the pointer.
        bool StartTitleDrag(DragRequest& request);
        void FollowTo(core::Float2 rootAt);
        void EndTitleDrag(const DragResult& result);

        DockArea* m_area;
        DockSurfaceId m_surface;
        RefPtr<DockSpace> m_space;
        bool m_dragging = false;
        core::Float2 m_grab{0.0f, 0.0f}; // where on it the press took it
        core::Float2 m_dragFrom{0.0f, 0.0f}; // where it was (Escape puts it back)
    };

    class DockArea : public UIWidget, public IDockView, public IDockSurroundings
    {
        RTTI_OBJECT(DockArea, UIWidget)
    public:
        /// A float placed with no bounds of its own (a layout written by hand) takes this size.
        static constexpr core::Float2 kFloatSize{360.0f, 260.0f};
        /// How much of a float's title bar stays in the area however far it is moved.
        static constexpr f32 kKeepInView = 48.0f;

        DockArea()
        {
            SetTag(core::StringView(u8"dockarea"));
            SetClipChildren(true);
            m_main = MakeRef<DockSpace>(MemoryAllocator());
            AddChild(m_main.Get());
            Placing(*m_main);
            m_preview = MakeRef<Panel>(MemoryAllocator());
            m_preview->AddClass(core::StringView(u8"dock-preview"));
            m_preview->SetHitTestVisible(false);
            m_preview->SetVisible(false);
            AddChild(m_preview.Get());
            DockArea* self = this;
            (void)OnCommand(core::StringView(u8"dock.next-surface"), [self](CommandEvent& e) { e.Propagate = !self->FocusNextSurface(false); });
            (void)OnCommand(core::StringView(u8"dock.previous-surface"), [self](CommandEvent& e) { e.Propagate = !self->FocusNextSurface(true); });
        }
        ~DockArea() override
        {
            if (m_controller != nullptr)
                m_controller->RemoveView(*this);
        }
        DockArea(const DockArea&) = delete;
        DockArea& operator=(const DockArea&) = delete;

        /// Shows `controller`'s layout: its main surface, and its floating surfaces over it.
        void Show(DockController& controller)
        {
            if (m_controller != nullptr)
                m_controller->RemoveView(*this);
            m_controller = &controller;
            controller.AddView(*this);
            m_main->Show(controller, controller.GetLayout().MainSurface());
            ShowLayout();
        }
        [[nodiscard]] DockController* GetController() const noexcept { return m_controller; }
        /// The main surface's DockSpace.
        [[nodiscard]] DockSpace* GetMainSpace() const noexcept { return m_main.Get(); }
        [[nodiscard]] usize GetFloatCount() const noexcept { return m_floats.Size(); }
        [[nodiscard]] DockFloat* GetFloat(usize index) const noexcept { return index < m_floats.Size() ? m_floats[index].Get() : nullptr; }
        /// The window of a floating surface; null for a surface it does not show.
        [[nodiscard]] DockFloat* FloatOf(DockSurfaceId surface) const noexcept
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSurface() == surface)
                    return window.Get();
            return nullptr;
        }

        /// Where a float of `size` let go at `at` (this widget's space) stands: its title bar under
        /// the pointer, `grab` along it, kept in the area.
        [[nodiscard]] Rectangle FloatBoundsAt(core::Float2 at, core::Float2 size, core::Float2 grab) const
        {
            const core::Float2 place = KeptInView(core::Float2{at.x - grab.x, at.y - grab.y}, size);
            return Rectangle{place.x, place.y, size.x, size.y};
        }

        // === IDockView ===
        void ShowLayout() override
        {
            if (m_controller == nullptr)
                return;
            const DockLayout& layout = m_controller->GetLayout();
            // The floats whose surfaces went (docked away, emptied) end; their dock spaces leave
            // the controller's views as they do.
            for (usize i = m_floats.Size(); i > 0; --i)
                if (layout.FindSurface(m_floats[i - 1]->GetSurface()) == nullptr)
                {
                    const RefPtr<DockFloat> gone = m_floats[i - 1];
                    m_floats.RemoveAt(i - 1);
                    gone->RemoveFromParent();
                }
            usize cascade = 0; // its place among the floats
            for (const DockSurface& surface : layout.Surfaces())
            {
                if (surface.Kind != DockSurfaceKind::Floating)
                    continue;
                ++cascade;
                DockFloat* window = FloatOf(surface.Id);
                if (window == nullptr)
                {
                    RefPtr<DockFloat> made = MakeRef<DockFloat>(MemoryAllocator(), *this, surface.Id);
                    made->AddClass(core::StringView(u8"dock-float"));
                    DockArea* self = this;
                    DockFloat* raw = made.Get();
                    made->SetOnUserArranged([self, raw]() { self->NoteArranged(*raw); });
                    AddChild(made.Get());
                    m_floats.PushBack(made);
                    window = made.Get();
                    Placing(*window->GetSpace());
                    window->GetSpace()->Show(*m_controller, surface.Id);
                }
                Rectangle bounds = surface.Bounds;
                if (bounds.width <= 0.0f || bounds.height <= 0.0f)
                {
                    // None of its own: a cascade from the top left, noted as its own from then.
                    const f32 step = 28.0f * static_cast<f32>(cascade - 1);
                    bounds = Rectangle{40.0f + step, 40.0f + step, kFloatSize.x, kFloatSize.y};
                    m_controller->NoteSurfaceBounds(surface.Id, bounds);
                }
                window->SetPosition(core::Float2{bounds.x, bounds.y});
                window->SetSize(core::Float2{bounds.width, bounds.height});
                window->SetTitle(TitleOf(layout, surface).AsView());
            }
            if (GetChildAt(ChildCount() - 1) != m_preview.Get())
                m_preview->ToFront(); // over the floats
            RequestLayout();
        }
        [[nodiscard]] const String* PanelOf(const Node&) const override { return nullptr; } // its dock spaces answer

        // === IDockSurroundings ===
        /// What is just past the surface's edge (its window's, for a float) the way `way` goes, level
        /// with the middle of `stack`: the topmost other float there, else the main surface; of it,
        /// the stack there or the one nearest. Past the main surface's edge is outside the area:
        /// nothing (`dock.next-surface` reaches the floats from it).
        [[nodiscard]] DockNodeId StackBeyond(const DockSpace& from, DockNodeId stack, DockDirection way) const override
        {
            const UIWidget* shown = from.WidgetOf(stack);
            const DockFloat* own = FloatShowing(from);
            if (shown == nullptr || (own == nullptr && &from != m_main.Get()))
                return kNoDock;
            const Rect edge = own != nullptr ? own->GetScreenBounds() : from.GetScreenBounds();
            const Rect a = shown->GetScreenBounds();
            constexpr f32 kPast = 2.0f;
            core::Float2 probe{a.x + a.width * 0.5f, a.y + a.height * 0.5f};
            switch (way)
            {
            case DockDirection::Left: probe.x = edge.x - kPast; break;
            case DockDirection::Right: probe.x = edge.x + edge.width + kPast; break;
            case DockDirection::Up: probe.y = edge.y - kPast; break;
            case DockDirection::Down: probe.y = edge.y + edge.height + kPast; break;
            }
            const auto contains = [&probe](const Rect& r)
            { return probe.x >= r.x && probe.x < r.x + r.width && probe.y >= r.y && probe.y < r.y + r.height; };
            // The floats front to back, as they are drawn.
            for (usize i = ChildCount(); i > 0; --i)
            {
                const DockFloat* window = Cast<DockFloat>(GetChildAt(i - 1));
                if (window != nullptr && window != own && window->IsVisible() && contains(window->GetScreenBounds()))
                    return window->GetSpace()->StackAt(probe);
            }
            return own != nullptr && contains(m_main->GetScreenBounds()) ? m_main->StackAt(probe) : kNoDock;
        }
        void OtherStacks(const DockSpace& from, Array<DockNodeId>& out) const override
        {
            if (&from != m_main.Get())
                m_main->StacksShown(out);
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSpace() != &from)
                    window->GetSpace()->StacksShown(out);
        }
        bool FocusStackAnywhere(DockNodeId stack) override
        {
            NoteFocusedStack();
            if (m_main->FocusStack(stack))
                return true;
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSpace()->WidgetOf(stack) != nullptr)
                {
                    const RefPtr<DockFloat> raised = window;
                    raised->ToFront(); // the float it is in comes to the front
                    m_preview->ToFront();
                    return raised->GetSpace()->FocusStack(stack);
                }
            return false;
        }

        /// The focus taken to the next surface (`backward`, the one before): the main one, then the
        /// floats as they were made, around; to the stack last focused there, else its first. With
        /// the focus in none of them, to the first (the last). False when there is no other surface.
        bool FocusNextSurface(bool backward)
        {
            Array<DockSpace*> spaces;
            spaces.PushBack(m_main.Get());
            for (const RefPtr<DockFloat>& window : m_floats)
                spaces.PushBack(window->GetSpace());
            usize at = spaces.Size(); // none focused
            for (usize i = 0; i < spaces.Size(); ++i)
                if (spaces[i]->FocusedStack() != kNoDock)
                    at = i;
            const usize count = spaces.Size();
            for (usize step = 1; step <= count; ++step)
            {
                usize next = 0;
                if (at == count)
                    next = backward ? count - step : step - 1;
                else if (step == count)
                    break; // round to where it is
                else
                    next = backward ? (at + count - step) % count : (at + step) % count;
                DockSpace& space = *spaces[next];
                DockNodeId stack = LastFocusedIn(space.GetSurface());
                if (stack == kNoDock || space.WidgetOf(stack) == nullptr)
                {
                    Array<DockNodeId> stacks;
                    space.StacksShown(stacks);
                    stack = stacks.IsEmpty() ? kNoDock : stacks[0];
                }
                if (stack != kNoDock && FocusStackAnywhere(stack))
                    return true;
            }
            return false;
        }

        /// Where a float being asked for would stand (this widget's space); false when none is.
        [[nodiscard]] bool GetFloatPreview(Rectangle& out) const
        {
            if (!m_preview->IsVisible())
                return false;
            out = m_previewBounds;
            return true;
        }
        /// Where a panel floated from its stack stands: over the stack, a little in, at most the
        /// stack's size (and at least a usable one).
        [[nodiscard]] Rectangle FloatBoundsFor(core::StringView panel) const
        {
            const DockSpace* space = SpaceOf(panel);
            const UIWidget* stack = space != nullptr ? space->WidgetOf(m_controller->GetLayout().StackOf(panel)) : nullptr;
            if (stack == nullptr)
                return Rectangle{0.0f, 0.0f, 0.0f, 0.0f}; // cascaded
            const Rect at = stack->GetScreenBounds();
            const core::Float2 origin = ConvertToNodeSpace(core::Float2{at.x, at.y});
            const core::Float2 size = FloatSizeOf(core::Float2{at.width, at.height});
            const core::Float2 place = KeptInView(core::Float2{origin.x + 24.0f, origin.y + 24.0f}, size);
            return Rectangle{place.x, place.y, size.x, size.y};
        }

    protected:
        // A float asked for (Shift held): the drop the dock spaces passed on lands here, the
        // window at the pointer, its title bar under it.
        void OnDragQuery(DropQuery& query) override
        {
            if (!DockSpace::AsksToFloat(query))
                return;
            // A float dragged by its title with Shift: only moved, where it is let go.
            if (const DockSurfaceId surface = m_main->DraggedSurface(query.Payload); surface != kNoDock)
            {
                HidePreview();
                m_moving = surface;
                query.Accept(DropEffect::Move);
                return;
            }
            const String* panel = m_main->DraggedPanel(query.Payload);
            if (panel == nullptr)
                return;
            const String dragged = *panel;
            DockLayout after = m_controller->GetLayout();
            const core::Float2 size = FloatSizeFrom(dragged.AsView());
            const Rectangle bounds = FloatBoundsAt(query.Local, size, core::Float2{Min(60.0f, size.x * 0.25f), 14.0f});
            if (DockOutcome refused = after.Float(dragged.AsView(), bounds); !refused.Ok())
            {
                HidePreview();
                query.Refuse(refused.Refused.AsView());
                return;
            }
            query.Accept(DropEffect::Move);
            m_floatPanel = dragged;
            m_previewBounds = bounds;
            m_preview->SetVisible(true);
            m_preview->ToFront();
            RequestLayout();
        }
        void OnDragLeave(const DragEvent&) override
        {
            HidePreview();
            m_moving = kNoDock;
        }
        void OnDrop(const DragEvent&) override
        {
            if (const DockSurfaceId moving = m_moving; moving != kNoDock)
            {
                m_moving = kNoDock;
                if (DockFloat* window = FloatOf(moving))
                    NoteArranged(*window); // where it was let go
                return;
            }
            const String panel = core::Move(m_floatPanel);
            const Rectangle bounds = m_previewBounds;
            HidePreview();
            if (!panel.IsEmpty() && m_controller != nullptr)
                (void)m_controller->Edit([&](DockLayout& layout) { return layout.Float(panel.AsView(), bounds); });
        }

    protected:
        // The main surface fills it; each float at its surface's place (where the pointer has it
        // while it is dragged), kept in view.
        void OnLayout() override
        {
            const Rect box = GetContentBounds();
            m_main->Arrange(box);
            if (m_preview->IsVisible())
                m_preview->Arrange(Rect{m_previewBounds.x, m_previewBounds.y, m_previewBounds.width, m_previewBounds.height});
            for (const RefPtr<DockFloat>& window : m_floats)
            {
                core::Float2 place = window->GetPosition();
                if (const DockSurface* surface = m_controller != nullptr ? m_controller->GetLayout().FindSurface(window->GetSurface()) : nullptr;
                    surface != nullptr && !window->IsBeingDragged())
                    place = core::Float2{surface->Bounds.x, surface->Bounds.y};
                const core::Float2 at = KeptInView(place, window->GetSize());
                window->Arrange(Rect{at.x, at.y, window->GetSize().x, window->GetSize().y});
            }
        }

    private:
        friend class DockFloat;

        // A dock space of the area places its floats as the area does.
        void Placing(DockSpace& space)
        {
            DockArea* self = this;
            space.SetFloatPlacer([self](core::StringView panel) { return self->FloatBoundsFor(panel); });
            space.SetSurroundings(this);
        }
        [[nodiscard]] const DockFloat* FloatShowing(const DockSpace& space) const
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSpace() == &space)
                    return window.Get();
            return nullptr;
        }
        // The stack the focus is in noted as its surface's last, for the focus to come back to.
        void NoteFocusedStack()
        {
            DockSpace* focused = m_main->FocusedStack() != kNoDock ? m_main.Get() : nullptr;
            for (const RefPtr<DockFloat>& window : m_floats)
                if (focused == nullptr && window->GetSpace()->FocusedStack() != kNoDock)
                    focused = window->GetSpace();
            if (focused == nullptr)
                return;
            const DockSurfaceId surface = focused->GetSurface();
            for (LastFocused& last : m_lastFocused)
                if (last.Surface == surface)
                {
                    last.Stack = focused->FocusedStack();
                    return;
                }
            m_lastFocused.PushBack(LastFocused{surface, focused->FocusedStack()});
        }
        [[nodiscard]] DockNodeId LastFocusedIn(DockSurfaceId surface) const
        {
            for (const LastFocused& last : m_lastFocused)
                if (last.Surface == surface)
                    return last.Stack;
            return kNoDock;
        }
        [[nodiscard]] DockSpace* SpaceOf(core::StringView panel) const
        {
            if (m_main->PageOf(panel) != nullptr)
                return m_main.Get();
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSpace()->PageOf(panel) != nullptr)
                    return window->GetSpace();
            return nullptr;
        }
        // A float's size from what it came from: no larger than most of the area, no smaller than a
        // window can be used at.
        [[nodiscard]] core::Float2 FloatSizeOf(core::Float2 from) const
        {
            const core::Float2 room = GetSize();
            return core::Float2{Clamp(from.x, 200.0f, Max(200.0f, room.x * 0.7f)), Clamp(from.y, 140.0f, Max(140.0f, room.y * 0.7f))};
        }
        [[nodiscard]] core::Float2 FloatSizeFrom(core::StringView panel) const
        {
            const DockSpace* space = SpaceOf(panel);
            const UIWidget* stack = space != nullptr ? space->WidgetOf(m_controller->GetLayout().StackOf(panel)) : nullptr;
            return FloatSizeOf(stack != nullptr ? stack->GetSize() : kFloatSize);
        }
        void HidePreview()
        {
            m_floatPanel = String{};
            if (m_preview->IsVisible())
                m_preview->SetVisible(false);
        }
        // A window's place moved so a part of its title bar stays in the area.
        [[nodiscard]] core::Float2 KeptInView(core::Float2 at, core::Float2 size) const
        {
            const core::Float2 room = GetSize();
            const f32 keep = Min(kKeepInView, size.x);
            return core::Float2{Clamp(at.x, keep - size.x, Max(0.0f, room.x - keep)), Clamp(at.y, 0.0f, Max(0.0f, room.y - 28.0f))};
        }
        // Its first stack's panel shown, and how many more it holds.
        [[nodiscard]] String TitleOf(const DockLayout& layout, const DockSurface& surface) const
        {
            usize count = 0;
            String first;
            for (const DockNode& node : layout.Nodes())
                if (node.IsStack() && layout.SurfaceOf(node.Id) == surface.Id)
                {
                    if (first.IsEmpty() && !node.Panels.IsEmpty())
                        first = node.Panels[Min(static_cast<usize>(node.Active), node.Panels.Size() - 1)];
                    count += node.Panels.Size();
                }
            const DockPanelDescription* description = m_controller->Describe(first.AsView());
            const String title = description != nullptr && !description->Title.IsEmpty() ? description->Title : first;
            return count > 1 ? core::Format(u8"{} +{}", title, count - 1) : title;
        }
        void NoteArranged(DockFloat& window)
        {
            if (m_controller == nullptr)
                return;
            const core::Float2 at = KeptInView(window.GetPosition(), window.GetSize());
            window.SetPosition(at); // let go out of reach: back where its title can be taken
            m_controller->NoteSurfaceBounds(window.GetSurface(), Rectangle{at.x, at.y, window.GetSize().x, window.GetSize().y});
        }
        // Its panels closed as their policies say (one that may not close stays, and the window with it).
        void CloseFloat(DockSurfaceId surface)
        {
            if (m_controller == nullptr)
                return;
            Array<String> panels;
            for (const DockNode& node : m_controller->GetLayout().Nodes())
                if (node.IsStack() && m_controller->GetLayout().SurfaceOf(node.Id) == surface)
                    for (const String& panel : node.Panels)
                        panels.PushBack(panel);
            for (const String& panel : panels)
                (void)m_controller->Close(panel.AsView());
        }

        DockController* m_controller = nullptr;
        RefPtr<DockSpace> m_main;
        Array<RefPtr<DockFloat>> m_floats;
        RefPtr<Panel> m_preview; // where a float asked for would stand
        Rectangle m_previewBounds{0.0f, 0.0f, 0.0f, 0.0f};
        String m_floatPanel; // the panel it is for
        DockSurfaceId m_moving = kNoDock; // a float dragged with Shift: only moved
        struct LastFocused
        {
            DockSurfaceId Surface;
            DockNodeId Stack;
        };
        Array<LastFocused> m_lastFocused; // each surface's stack the focus last left
    };

    inline DockFloat::DockFloat(DockArea& area, DockSurfaceId surface) : m_area(&area), m_surface(surface)
    {
        // Its title bar's press is a dock drag of the whole surface.
        DragHandle* bar = GetTitleBar();
        bar->SetDragsItself(false);
        DockFloat* self = this;
        bar->SetDragSource([self](DragRequest& request) { return self->StartTitleDrag(request); });
        // When the drag ends the window may be gone (docked away): its title bar, which the drag
        // holds, says whether it is still in one.
        bar->SetOnDragEnd([bar](const DragResult& result)
        {
            if (DockFloat* window = Cast<DockFloat>(bar->GetParent()))
                window->EndTitleDrag(result);
        });
        m_space = MakeRef<DockSpace>(MemoryAllocator());
        m_space->EditLayoutStyle([](LayoutStyle& s)
        {
            s.Width = LayoutLength::Percent(100.0f);
            s.Height = LayoutLength::Percent(100.0f);
        });
        GetContent()->AddChild(m_space.Get());
        SetMinSize(core::Float2{160.0f, 100.0f});
    }
    inline void DockFloat::RequestClose() { m_area->CloseFloat(m_surface); }

    inline bool DockFloat::StartTitleDrag(DragRequest& request)
    {
        if (request.FromKeyboard)
            return false;
        request.Payload.Type = String(kDockSurfaceDragType);
        request.Payload.Value = core::Variant::From<u64>(static_cast<u64>(m_surface));
        request.Payload.Label = String(GetTitle());
        request.Payload.Allowed = DropEffectsMove;
        m_grab = request.Press; // the title bar is at the window's top left: the same place
        m_dragFrom = GetPosition();
        DockFloat* self = this;
        request.Follow = [self](core::Float2 rootAt) { self->FollowTo(rootAt); };
        return true;
    }
    inline void DockFloat::FollowTo(core::Float2 rootAt)
    {
        if (!m_dragging)
        {
            m_dragging = true;
            AddClass(core::StringView(u8"dock-dragging"));
            ToFront();
        }
        const core::Float2 at = m_area->ConvertToNodeSpace(rootAt);
        SetPosition(core::Float2{at.x - m_grab.x, at.y - m_grab.y});
        m_area->RequestLayout();
    }
    inline void DockFloat::EndTitleDrag(const DragResult& result)
    {
        m_dragging = false;
        RemoveClass(core::StringView(u8"dock-dragging"));
        if (result.Cancelled)
        {
            SetPosition(m_dragFrom); // Escape: put back as it was
            m_area->RequestLayout();
        }
        else if (result.Target == nullptr)
            m_area->NoteArranged(*this); // let go over nothing (off the area): left there, in reach
    }
}

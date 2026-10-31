// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - :dock_area partition
//
// DockArea: floating surfaces within one window. It is the virtual surface host for the web and
// for platforms with a single window. The main surface's DockSpace fills the area, and each
// floating surface of the layout is a DockFloat over it: a Window holding its own DockSpace, so a
// float is a whole dock tree that panels dock into just as they dock into the main one. As the
// layout changes, the area makes a float for each surface that appears and ends the float of each
// surface that goes, and places each float at its surface's bounds (in this widget's space). It
// reports back what the user does to a window: a move or resize updates the surface's bounds, and
// a close closes the window's panels as their policies say.
//
// A tab dragged with Shift held floats: the dock spaces pass the drop on, and the area previews
// the window at the pointer and makes it there. A float dragged by its title bar follows the
// pointer and, when let go over a target, docks whole there; the preview shows the area its
// content would take. With Shift held the float is only moved, and Escape puts it back. The dock
// keys reach across surfaces: an arrow key past a float's edge moves on to the stack beyond it,
// `dock.next-surface` and `dock.previous-surface` move the focus from one surface to the next, and
// the Move to menu lists the stacks of every surface.
//
// Floats as OS windows: given an IDockWindows from a host, the area shows each floating surface as
// a dock space in a window of its own instead, with its bounds in screen units. The window's moves,
// resizes and close come back through NoteWindowBounds and CloseFloat. A tab or stack let go
// outside every window floats where it was let go; with Shift held it floats at the pointer, and
// the Move to menu's Float command floats it over its stack. No window follows the pointer during a
// drag, because the system moves a window by its own frame. Switching between the two modes
// converts the floats' bounds between the area's units and the screen's.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.docking:dock_area;

import foundation.core;
import foundation.gui;
import :layout;
import :controller;
import :dock_space;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class DockArea;

    /// What a DockArea needs from a host to show its floats as windows of their own (for a
    /// GuiHost, DockHostWindows implements it). Bounds are in screen units. The host reports what
    /// the user does to a window through DockArea::NoteWindowBounds and DockArea::CloseFloat.
    class IDockWindows
    {
    public:
        virtual ~IDockWindows() = default;
        /// Opens a window for `surface` with `content` filling it. Where the platform keeps windows
        /// at the places they are given, the window opens at `bounds`; elsewhere it opens at the size
        /// of `bounds` wherever the system puts it. Returns false when no window opened.
        virtual bool Open(DockSurfaceId surface, Node& content, const Rectangle& bounds, core::StringView title) = 0;
        virtual void Close(DockSurfaceId surface) = 0;
        /// Moves and sizes the window of `surface`. Called when a layout is adopted, not when the
        /// user moves the window.
        virtual void Place(DockSurfaceId surface, const Rectangle& bounds) = 0;
        virtual void SetTitle(DockSurfaceId surface, core::StringView title) = 0;
        virtual void Raise(DockSurfaceId surface) = 0;
        /// Converts `local`, in `node`'s space, to screen coordinates in `screen`. Returns false
        /// where the screen position is not known (Wayland).
        virtual bool ToScreen(const Node& node, core::Float2 local, core::Float2& screen) = 0;
    };

    /// The window of a floating surface (`window.dock-float`). Its title names its panels and its
    /// content is a DockSpace showing the surface. Closing it closes its panels as their policies
    /// say.
    class DockFloat : public Window
    {
        RTTI_OBJECT(DockFloat, Window)
    public:
        DockFloat(DockArea& area, DockSurfaceId surface);
        [[nodiscard]] DockSurfaceId GetSurface() const noexcept { return m_surface; }
        [[nodiscard]] DockSpace* GetSpace() const noexcept { return m_space.Get(); }
        [[nodiscard]] DockArea* GetArea() const noexcept { return m_area; }
        /// Called by the close button: closes the window's panels as their policies say.
        void RequestClose() override;
        /// Whether the window is being dragged by its title bar. While dragged it follows the
        /// pointer, and it and everything it holds are left out of hit tests (`.dock-dragging`) so
        /// the drop targets under it answer.
        [[nodiscard]] bool IsBeingDragged() const noexcept { return m_dragging; }
        /// While the window is dragged, no part of it is found under the pointer, so the drop goes
        /// to what is below it.
        [[nodiscard]] Node* OverFind(core::Float2 worldPoint) override { return m_dragging ? nullptr : Window::OverFind(worldPoint); }

    private:
        friend class DockArea;
        // Starts a drag of the whole surface from the title bar; the window follows the pointer.
        bool StartTitleDrag(DragRequest& request);
        void FollowTo(core::Float2 rootAt);
        void EndTitleDrag(const DragResult& result);

        DockArea* m_area;
        DockSurfaceId m_surface;
        RefPtr<DockSpace> m_space;
        bool m_dragging = false;
        core::Float2 m_grab{0.0f, 0.0f}; // where on the window the press grabbed it
        core::Float2 m_dragFrom{0.0f, 0.0f}; // where it was before the drag (Escape puts it back)
    };

    /// DockArea::kFloatSize, kept at namespace scope so another library can read it: MSVC does not
    /// export a class's static constexpr member from a shared library.
    inline constexpr core::Float2 kDockAreaFloatSize{360.0f, 260.0f};

    /// DockArea::kKeepInView, kept at namespace scope so another library can read it: MSVC does not
    /// export a class's static constexpr member from a shared library.
    inline constexpr f32 kDockAreaKeepInView = 48.0f;

    class DockArea : public UIWidget, public IDockView, public IDockSurroundings
    {
        RTTI_OBJECT(DockArea, UIWidget)
    public:
        /// A float placed with no bounds of its own (a layout written by hand) takes this size.
        static constexpr core::Float2 kFloatSize = kDockAreaFloatSize;
        /// How much of a float's title bar stays in the area however far it is moved.
        static constexpr f32 kKeepInView = kDockAreaKeepInView;

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
            if (m_windows != nullptr)
                EndFloats(); // the floats' windows close with the area
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
        /// Returns the window of a floating surface, or null for a surface this area does not show.
        [[nodiscard]] DockFloat* FloatOf(DockSurfaceId surface) const noexcept
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSurface() == surface)
                    return window.Get();
            return nullptr;
        }

        /// Shows the floats as windows of their own through `windows`, or inside the area when
        /// `windows` is null. `windows` comes from a host and must outlive the area or be taken back
        /// first. The floats' bounds are converted between the area's units and the screen's.
        void SetWindows(IDockWindows* windows)
        {
            if (windows == m_windows)
                return;
            IDockWindows* via = windows != nullptr ? windows : m_windows;
            core::Float2 origin{0.0f, 0.0f};
            const bool known = via->ToScreen(*this, core::Float2{0.0f, 0.0f}, origin);
            EndFloats();
            m_windows = windows;
            if (m_controller != nullptr)
            {
                const f32 sign = windows != nullptr ? 1.0f : -1.0f;
                Array<DockSurface> floating;
                for (const DockSurface& surface : m_controller->GetLayout().Surfaces())
                    if (surface.Kind == DockSurfaceKind::Floating && surface.Bounds.width > 0.0f && surface.Bounds.height > 0.0f)
                        floating.PushBack(surface);
                for (const DockSurface& surface : floating)
                {
                    Rectangle bounds = surface.Bounds;
                    if (known)
                    {
                        bounds.x += sign * origin.x;
                        bounds.y += sign * origin.y;
                    }
                    else
                        bounds.x = bounds.y = 0.0f; // position unknown: the system places it
                    m_controller->NoteSurfaceBounds(surface.Id, bounds);
                }
            }
            ShowLayout();
        }
        [[nodiscard]] IDockWindows* GetWindows() const noexcept { return m_windows; }
        /// Called by the host when the user moves or resizes a floating surface's window. `bounds`
        /// is in screen units.
        void NoteWindowBounds(DockSurfaceId surface, const Rectangle& bounds)
        {
            if (m_controller == nullptr)
                return;
            if (OsFloat* shown = OsFloatOf(surface))
                shown->Placed = bounds; // the window is already there, so it is not placed again
            m_controller->NoteSurfaceBounds(surface, bounds);
        }
        /// Returns the dock space showing a floating surface in its own window, or null when none
        /// does.
        [[nodiscard]] DockSpace* WindowSpaceOf(DockSurfaceId surface) const noexcept
        {
            for (const OsFloat& shown : m_osFloats)
                if (shown.Surface == surface)
                    return shown.Space.Get();
            return nullptr;
        }
        /// Closes the panels of a floating surface as their policies say, for a float's close button
        /// or for its own window's. A panel that may not close stays, and so does the window.
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

        /// Returns the bounds of a float of `size` let go at `at` (in this widget's space). Its title
        /// bar sits under the pointer, grabbed at `grab`, and the float is kept in the area.
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
            if (m_windows != nullptr)
            {
                ShowWindows(layout);
                return;
            }
            // End the floats whose surfaces are gone (docked away or emptied); their dock spaces
            // leave the controller's views as they end.
            for (usize i = m_floats.Size(); i > 0; --i)
                if (layout.FindSurface(m_floats[i - 1]->GetSurface()) == nullptr)
                {
                    const RefPtr<DockFloat> gone = m_floats[i - 1];
                    m_floats.RemoveAt(i - 1);
                    gone->RemoveFromParent();
                }
            usize cascade = 0; // the float's place in the cascade
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
                    // The surface has no bounds of its own: cascade it from the top left, and note
                    // those bounds as its own from then on.
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
        [[nodiscard]] const String* PanelOf(const Node&) const override { return nullptr; } // the dock spaces answer this

        // === IDockSurroundings ===
        /// Returns the stack just past the edge of `from`'s surface (its window, for a float) in
        /// direction `way`, level with the middle of `stack`. The surface found there is the topmost
        /// other float, else the main surface, and the result is its stack at that point or the
        /// nearest one. Past the main surface's edge lies outside the area, so there is nothing
        /// there; `dock.next-surface` reaches the floats from the main surface.
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
            Array<DockSpace*> spaces;
            FloatSpaces(spaces);
            for (DockSpace* space : spaces)
                if (space != &from)
                    space->StacksShown(out);
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
            for (const OsFloat& shown : m_osFloats)
                if (shown.Space->WidgetOf(stack) != nullptr)
                {
                    const RefPtr<DockSpace> space = shown.Space;
                    m_windows->Raise(shown.Surface); // its window comes to the front
                    return space->FocusStack(stack);
                }
            return false;
        }

        /// Moves the focus to the next surface, or to the previous one when `backward`. The order is
        /// the main surface, then the floats in the order they were made, wrapping around. The focus
        /// goes to the stack last focused on that surface, else to its first stack. When no surface
        /// has the focus, it goes to the first surface (the last, when `backward`). Returns false
        /// when there is no other surface.
        bool FocusNextSurface(bool backward)
        {
            Array<DockSpace*> spaces;
            spaces.PushBack(m_main.Get());
            FloatSpaces(spaces);
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
                    break; // wrapped round to the surface that has the focus
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

        /// Gets where a requested float would stand, in this widget's space. Returns false when no
        /// float is being requested.
        [[nodiscard]] bool GetFloatPreview(Rectangle& out) const
        {
            if (!m_preview->IsVisible())
                return false;
            out = m_previewBounds;
            return true;
        }
        /// Returns the bounds of a panel floated from its stack: over the stack, inset a little, no
        /// larger than the stack and no smaller than a usable size.
        [[nodiscard]] Rectangle FloatBoundsFor(core::StringView panel) const
        {
            const DockSpace* space = SpaceOf(panel);
            const UIWidget* stack = space != nullptr ? space->WidgetOf(m_controller->GetLayout().StackOf(panel)) : nullptr;
            if (stack == nullptr)
                return Rectangle{0.0f, 0.0f, 0.0f, 0.0f}; // no bounds: the float is cascaded
            if (m_windows != nullptr)
            {
                // In its own window: over the stack, inset a little, in screen units.
                const core::Float2 size = FloatSizeOf(stack->GetSize());
                core::Float2 screen;
                if (m_windows->ToScreen(*stack, core::Float2{24.0f, 24.0f}, screen))
                    return Rectangle{screen.x, screen.y, size.x, size.y};
                return Rectangle{0.0f, 0.0f, size.x, size.y}; // position unknown: the system places it
            }
            const Rect at = stack->GetScreenBounds();
            const core::Float2 origin = ConvertToNodeSpace(core::Float2{at.x, at.y});
            const core::Float2 size = FloatSizeOf(core::Float2{at.width, at.height});
            const core::Float2 place = KeptInView(core::Float2{origin.x + 24.0f, origin.y + 24.0f}, size);
            return Rectangle{place.x, place.y, size.x, size.y};
        }

    protected:
        // A float requested with Shift held: the dock spaces pass the drop on to the area, which
        // places the window at the pointer with its title bar under it.
        void OnDragQuery(DropQuery& query) override
        {
            if (!DockSpace::AsksToFloat(query))
                return;
            // A float dragged by its title bar with Shift held is only moved to where it is let go.
            if (const DockSurfaceId surface = m_main->DraggedSurface(query.Payload); surface != kNoDock)
            {
                HidePreview();
                m_moving = surface;
                query.Accept(DropEffect::Move);
                return;
            }
            // A whole stack dragged by its grip floats as it is.
            if (const DockNodeId stack = m_main->DraggedStack(query.Payload); stack != kNoDock)
            {
                const DockNode* node = m_controller->GetLayout().FindNode(stack);
                const core::Float2 size = node->Panels.IsEmpty() ? FloatSizeOf(kFloatSize) : FloatSizeFrom(node->Panels[0].AsView());
                const Rectangle bounds = FloatBoundsAt(query.Local, size, core::Float2{Min(60.0f, size.x * 0.25f), 14.0f});
                DockLayout after = m_controller->GetLayout();
                if (DockOutcome refused = after.FloatStack(stack, bounds); !refused.Ok())
                {
                    HidePreview();
                    query.Refuse(refused.Refused.AsView());
                    return;
                }
                query.Accept(DropEffect::Move);
                m_floatStack = stack;
                ShowFloatPreview(bounds);
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
            ShowFloatPreview(bounds);
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
            const DockNodeId stack = m_floatStack;
            const Rectangle bounds = OnScreen(m_previewBounds); // in its own window, where the preview was
            HidePreview();
            if (m_controller == nullptr)
                return;
            if (stack != kNoDock)
                (void)m_controller->Edit([&](DockLayout& layout) { return layout.FloatStack(stack, bounds); });
            else if (!panel.IsEmpty())
                (void)m_controller->Edit([&](DockLayout& layout) { return layout.Float(panel.AsView(), bounds); });
        }

    protected:
        // The main surface fills the area. Each float goes at its surface's place (or where the
        // pointer has it while it is dragged) and is kept in view.
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

        // Hooks a dock space up to the area: the area places the space's floats, tells it what lies
        // past its edges and floats what is dragged outside it.
        void Placing(DockSpace& space)
        {
            DockArea* self = this;
            space.SetFloatPlacer([self](core::StringView panel) { return self->FloatBoundsFor(panel); });
            space.SetSurroundings(this);
            space.SetOnDraggedOutside([self](const DockDragged& dragged, const DragResult& result) { self->FloatOutside(dragged, result); });
        }
        // Collects the dock spaces of the floats, in the area or in their own windows, in the order
        // they were made.
        void FloatSpaces(Array<DockSpace*>& out) const
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                out.PushBack(window->GetSpace());
            for (const OsFloat& shown : m_osFloats)
                out.PushBack(shown.Space.Get());
        }
        struct OsFloat
        {
            DockSurfaceId Surface = kNoDock;
            RefPtr<Panel> Content; // held by the window's root
            RefPtr<DockSpace> Space;
            String Title;
            Rectangle Placed{0.0f, 0.0f, 0.0f, 0.0f}; // where the window was placed or last reported
        };
        [[nodiscard]] OsFloat* OsFloatOf(DockSurfaceId surface) noexcept
        {
            for (OsFloat& shown : m_osFloats)
                if (shown.Surface == surface)
                    return &shown;
            return nullptr;
        }
        // Converts bounds in the area to screen units when the floats are windows of their own.
        // Where the screen position is not known, only the size is kept and the system places the
        // window.
        [[nodiscard]] Rectangle OnScreen(const Rectangle& bounds) const
        {
            if (m_windows == nullptr)
                return bounds;
            core::Float2 screen;
            if (m_windows->ToScreen(*this, core::Float2{bounds.x, bounds.y}, screen))
                return Rectangle{screen.x, screen.y, bounds.width, bounds.height};
            return Rectangle{0.0f, 0.0f, bounds.width, bounds.height};
        }
        // Ends every float, in the area or in its own window; the layout keeps the surfaces.
        void EndFloats()
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                window->RemoveFromParent();
            m_floats.Clear();
            while (!m_osFloats.IsEmpty())
            {
                const OsFloat gone = m_osFloats.Back(); // kept alive while its window closes
                m_osFloats.PopBack();
                m_windows->Close(gone.Surface);
            }
        }
        // Shows each floating surface in a window of its own: opens a window for each new surface,
        // closes the window of each surface that is gone, and retitles and places each window as the
        // layout has it. When a window cannot be opened, the area goes back to showing all the
        // floats inside itself.
        void ShowWindows(const DockLayout& layout)
        {
            for (usize i = m_osFloats.Size(); i > 0; --i)
                if (layout.FindSurface(m_osFloats[i - 1].Surface) == nullptr)
                {
                    const OsFloat gone = m_osFloats[i - 1];
                    m_osFloats.RemoveAt(i - 1);
                    m_windows->Close(gone.Surface);
                }
            usize cascade = 0;
            Array<DockSurface> floating;
            for (const DockSurface& surface : layout.Surfaces())
                if (surface.Kind == DockSurfaceKind::Floating)
                    floating.PushBack(surface);
            for (const DockSurface& surface : floating)
            {
                ++cascade;
                Rectangle bounds = surface.Bounds;
                if (bounds.width <= 0.0f || bounds.height <= 0.0f)
                {
                    // The surface has no bounds of its own: cascade it from the area's top left, in
                    // screen units.
                    const f32 step = 40.0f + 28.0f * static_cast<f32>(cascade - 1);
                    bounds = OnScreen(Rectangle{step, step, kFloatSize.x, kFloatSize.y});
                    m_controller->NoteSurfaceBounds(surface.Id, bounds);
                }
                const String title = TitleOf(m_controller->GetLayout(), surface);
                if (OsFloat* shown = OsFloatOf(surface.Id))
                {
                    if (shown->Title != title)
                    {
                        shown->Title = title;
                        m_windows->SetTitle(surface.Id, title.AsView());
                    }
                    const Rectangle& was = shown->Placed;
                    if (was.x != bounds.x || was.y != bounds.y || was.width != bounds.width || was.height != bounds.height)
                    {
                        shown->Placed = bounds;
                        m_windows->Place(surface.Id, bounds);
                    }
                    continue;
                }
                OsFloat made;
                made.Surface = surface.Id;
                made.Title = title;
                made.Placed = bounds;
                made.Content = MakeRef<Panel>(MemoryAllocator());
                made.Content->AddClass(core::StringView(u8"dock-window"));
                made.Content->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Width = LayoutLength::Percent(100.0f);
                    s.Height = LayoutLength::Percent(100.0f);
                });
                made.Space = MakeRef<DockSpace>(MemoryAllocator());
                made.Space->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Width = LayoutLength::Percent(100.0f);
                    s.Height = LayoutLength::Percent(100.0f);
                });
                made.Content->AddChild(made.Space.Get());
                Placing(*made.Space);
                made.Space->Show(*m_controller, surface.Id);
                m_osFloats.PushBack(made);
                if (!m_windows->Open(surface.Id, *made.Content, bounds, title.AsView()))
                {
                    m_osFloats.PopBack();
                    SetWindows(nullptr); // a window failed to open: show all the floats in the area
                    return;
                }
            }
        }
        // Floats a panel or a stack let go outside every window into a window of its own, with its
        // title bar under the pointer. Where the pointer's screen position is not known, the system
        // places the window.
        void FloatOutside(const DockDragged& dragged, const DragResult& result)
        {
            if (m_windows == nullptr || m_controller == nullptr || dragged.IsSurface())
                return; // floats inside the area cannot go outside it
            const DockLayout& layout = m_controller->GetLayout();
            String first = dragged.Panel;
            if (dragged.IsStack())
                if (const DockNode* node = layout.FindNode(dragged.Stack); node != nullptr && !node->Panels.IsEmpty())
                    first = node->Panels[0];
            const core::Float2 size = first.IsEmpty() ? FloatSizeOf(kFloatSize) : FloatSizeFrom(first.AsView());
            const Rectangle bounds = result.ScreenKnown ? Rectangle{result.Screen.x - Min(60.0f, size.x * 0.25f), result.Screen.y - 14.0f, size.x, size.y}
                                                        : Rectangle{0.0f, 0.0f, size.x, size.y};
            const DockDragged moving = dragged;
            if (moving.IsStack())
                (void)m_controller->Edit([&](DockLayout& edit) { return edit.FloatStack(moving.Stack, bounds); });
            else
                (void)m_controller->Edit([&](DockLayout& edit) { return edit.Float(moving.Panel.AsView(), bounds); });
        }
        [[nodiscard]] const DockFloat* FloatShowing(const DockSpace& space) const
        {
            for (const RefPtr<DockFloat>& window : m_floats)
                if (window->GetSpace() == &space)
                    return window.Get();
            return nullptr;
        }
        // Notes the stack that has the focus as its surface's last focused stack, for the focus to
        // return to.
        void NoteFocusedStack()
        {
            DockSpace* focused = m_main->FocusedStack() != kNoDock ? m_main.Get() : nullptr;
            Array<DockSpace*> spaces;
            FloatSpaces(spaces);
            for (DockSpace* space : spaces)
                if (focused == nullptr && space->FocusedStack() != kNoDock)
                    focused = space;
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
            Array<DockSpace*> spaces;
            FloatSpaces(spaces);
            for (DockSpace* space : spaces)
                if (space->PageOf(panel) != nullptr)
                    return space;
            return nullptr;
        }
        // Returns a float's size from the size of what it came from: no larger than most of the
        // area, and no smaller than a usable window.
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
        // Shows the float preview at `bounds`, above everything.
        void ShowFloatPreview(const Rectangle& bounds)
        {
            m_previewBounds = bounds;
            m_preview->SetVisible(true);
            m_preview->ToFront();
            RequestLayout();
        }
        void HidePreview()
        {
            m_floatPanel = String{};
            m_floatStack = kNoDock;
            if (m_preview->IsVisible())
                m_preview->SetVisible(false);
        }
        // Returns the window position `at`, moved so part of its title bar stays in the area.
        [[nodiscard]] core::Float2 KeptInView(core::Float2 at, core::Float2 size) const
        {
            const core::Float2 room = GetSize();
            const f32 keep = Min(kKeepInView, size.x);
            return core::Float2{Clamp(at.x, keep - size.x, Max(0.0f, room.x - keep)), Clamp(at.y, 0.0f, Max(0.0f, room.y - 28.0f))};
        }
        // Returns a float's title: the panel shown in its first stack, then how many more panels
        // it holds.
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
            window.SetPosition(at); // let go out of reach: moved back to where its title bar can be grabbed
            m_controller->NoteSurfaceBounds(window.GetSurface(), Rectangle{at.x, at.y, window.GetSize().x, window.GetSize().y});
        }
        DockController* m_controller = nullptr;
        RefPtr<DockSpace> m_main;
        Array<RefPtr<DockFloat>> m_floats;
        IDockWindows* m_windows = nullptr; // floats as windows of their own; not owned
        Array<OsFloat> m_osFloats;
        RefPtr<Panel> m_preview; // where a requested float would stand
        Rectangle m_previewBounds{0.0f, 0.0f, 0.0f, 0.0f};
        String m_floatPanel; // the panel the preview is for
        DockNodeId m_floatStack = kNoDock; // or the stack it is for
        DockSurfaceId m_moving = kNoDock; // a float dragged with Shift: only moved
        struct LastFocused
        {
            DockSurfaceId Surface;
            DockNodeId Stack;
        };
        Array<LastFocused> m_lastFocused; // the stack on each surface that last had the focus
    };

    inline DockFloat::DockFloat(DockArea& area, DockSurfaceId surface) : m_area(&area), m_surface(surface)
    {
        // A press on the title bar starts a dock drag of the whole surface.
        DragHandle* bar = GetTitleBar();
        bar->SetDragsItself(false);
        DockFloat* self = this;
        bar->SetDragSource([self](DragRequest& request) { return self->StartTitleDrag(request); });
        // When the drag ends the window may be gone (docked away). The drag holds the title bar, so
        // the title bar tells whether it is still in a window.
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
        m_grab = request.Press; // the title bar is at the window's top left, so the grab point is the same on both
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
            m_area->NoteArranged(*this); // let go over no target (off the area): left there, kept in reach
    }
}

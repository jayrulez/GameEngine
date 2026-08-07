// Draconic UI Toolkit - :docking partition (mutually-recursive docking cluster)
//
// The six docking types that reference one another cyclically live in ONE partition (the established
// "mutually-recursive types share one partition" rule). Ported from Sedulous.UI.Toolkit/src/Docking/
// {DockPanelDragData, DockablePanel, DockableWindow, DockTabGroup, DockManager}.bf.
//
// Ownership adaptation (Beef raw `new`/`delete` tree -> draconic RefPtr tree):
//   * The view tree is the strong owner of its children (AddView adds a RefPtr, RemoveView drops it -
//     which can DESTROY the child if it was the last ref). So any node that is detached and re-attached
//     must be pinned with a local RefPtr<View> across the operation.
//   * DockManager.mPanels OWNS its panels via RefPtr (Beef's raw registry was the de-facto owner; an
//     undocked-but-tracked panel must survive between AddPanel and DockPanel). The tree also refs a
//     docked panel - RefPtr shared ownership handles this.
//   * DockManager.mDockableWindows is a NON-owning raw list (PopupLayer owns floating windows, ownsView).
//   * QueueDeleteNode keeps the node alive across the deferred boundary (captures a RefPtr) so the
//     deferred tree-removal + release is UAF-free; with no Context it relies on RAII.
//
// Beef `IDockableWindow` interface is DEAD (not ported); DetachPanel() is a plain method on DockableWindow.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:docking;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import draconic.fonts;
import :dock_position;
import :dock_layout_node;
import :idock_host;
import :idockable_window_host;
import :dock_zone_indicator;
import :dock_split;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    // Forward declarations for the cyclic cluster.
    class DockDragPreview;
    class DockPanelDragData;
    class DockablePanel;
    class DockableWindow;
    class DockTabGroup;
    class DockManager;

    // ============================================================================================
    // DockDragPreview - adorner visual that looks like a mini dockable window.
    // ============================================================================================
    class DockDragPreview : public View
    {
        DRACONIC_OBJECT(DockDragPreview, View)
    public:
        void SetTitle(StringView title) { m_title = String(title); }

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 headerH = 24.0f;

            const Color borderColor = Rgb(65, 70, 85, 255);
            const Color contentBg = Rgb(42, 44, 54, 255);
            ctx.VG().FillRoundedRect(Rectangle{0, 0, Width(), Height()}, 4, contentBg);

            const Color headerBg = Rgb(40, 44, 55, 255);
            ctx.VG().FillRoundedRect(Rectangle{0, 0, Width(), headerH}, 4, headerBg);
            // Square off header bottom corners.
            ctx.VG().FillRect(Rectangle{0, headerH - 4, Width(), 4}, headerBg);

            // Title text.
            if (ctx.FontService() != nullptr)
            {
                fonts::CachedFont* font = ctx.FontService()->GetFont(11.0f);
                if (font != nullptr)
                {
                    const Color textColor = Rgb(220, 225, 235, 255);
                    ctx.VG().DrawText(m_title, font, Rectangle{8, 0, Width() - 16, headerH},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }

            // Border outline.
            ctx.VG().StrokeRoundedRect(Rectangle{0, 0, Width(), Height()}, 4, borderColor, 1);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(m_previewWidth),
                                  constraints.ConstrainHeight(m_previewHeight)};
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        String m_title;
        f32 m_previewWidth = 200;
        f32 m_previewHeight = 120;
    };

    // ============================================================================================
    // DockPanelDragData - drag payload carrying a reference to the DockablePanel being dragged.
    // ============================================================================================
    class DockPanelDragData : public DragData
    {
        DRACONIC_OBJECT(DockPanelDragData, DragData)
    public:
        explicit DockPanelDragData(DockablePanel* panel) : DragData(u8"dock/panel"), Panel(panel) {}

        DockablePanel* Panel = nullptr;
        /// If dragging from an existing DockableWindow, set so DockManager can move it during drag.
        DockableWindow* SourceWindow = nullptr;
        /// Mouse offset within the dockable window at drag start (for smooth repositioning).
        f32 DragOffsetX = 0.0f;
        f32 DragOffsetY = 0.0f;
    };

    // ============================================================================================
    // DockablePanel - content panel with title bar, close button, and drag support.
    // ============================================================================================
    class DockablePanel : public ViewGroup, public IDragSource
    {
        DRACONIC_OBJECT(DockablePanel, ViewGroup)
    public:
        DockablePanel() = default;
        explicit DockablePanel(StringView title) { m_title = String(title); }
        DockablePanel(StringView title, View* content)
        {
            m_title = String(title);
            SetContent(content);
        }

        // Last dock position for re-dock after floating.
        DockPosition mLastDockPosition = DockPosition::Center;
        ViewId mLastRelativeToId = ViewId::Invalid;

        f32 HeaderHeight = 24;
        IDockHost* DockHost = nullptr;

        Event<void(DockablePanel*)> OnCloseRequested;

        /// Veto hook for close requests (the editor prompts on dirty pages): return false to
        /// swallow the request (the close buttons route through RequestClose; a handler that
        /// later decides to close invokes OnCloseRequested directly, bypassing the veto).
        Function<bool(DockablePanel*)> OnCloseInterceptor;

        /// The user-gesture close path (panel/tab close buttons): interceptor first, then the
        /// close event. Programmatic closers that must not be vetoed invoke the event directly.
        void RequestClose()
        {
            if (OnCloseInterceptor && !OnCloseInterceptor(this))
            {
                return;
            }
            OnCloseRequested.Invoke(this);
        }

        /// ANY press inside this panel's subtree activates it (capture/tunnel phase: this runs
        /// before the target handles the click and never consumes it). This is what makes the
        /// ACTIVE panel follow interaction - with side-by-side tab groups, a panel can be
        /// visible (already its group's selected tab, so SetSelectedIndex early-outs) while a
        /// DIFFERENT panel is the app-active one; tab clicks alone can't re-announce it.
        void OnMouseDownCapture(MouseEventArgs&) override; // out-of-line (needs DockManager)

        /// Stable identifier for layout persistence.
        [[nodiscard]] StringView PersistenceId() const { return m_persistenceId.AsView(); }
        void SetPersistenceId(StringView id) { m_persistenceId = String(id); }

        [[nodiscard]] StringView Title() const { return m_title.AsView(); }
        void SetTitle(StringView title)
        {
            m_title = String(title);
            Invalidate();
        }

        [[nodiscard]] bool Closable() const { return m_closable; }
        void SetClosable(bool value) { m_closable = value; }

        /// Whether to show the panel's own header bar (false when inside a DockTabGroup).
        [[nodiscard]] bool ShowHeader() const { return m_showHeader; }
        void SetShowHeader(bool value)
        {
            m_showHeader = value;
            Invalidate();
        }

        [[nodiscard]] View* ContentView() const { return m_content; }

        /// Set the content view (replaces existing).
        void SetContent(View* content, LayoutParamsPtr lp = {})
        {
            if (m_content != nullptr)
            {
                RemoveView(m_content, true);
            }
            m_content = content;
            if (content != nullptr)
            {
                AddView(content, Move(lp));
            }
            Invalidate();
        }

        /// Save the current dock position for re-docking after floating.
        void SaveDockPosition(DockPosition position, View* relativeTo)
        {
            mLastDockPosition = position;
            mLastRelativeToId = (relativeTo != nullptr) ? relativeTo->Id : ViewId::Invalid;
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 headerH = m_showHeader ? HeaderHeight : 0;

            if (m_showHeader)
            {
                // Header background.
                if (Drawable* headerDrawable = ResolvePartDrawable(
                        u8"header", StyleProperty::Background, ControlState::Normal))
                {
                    headerDrawable->Draw(ctx, Rectangle{0, 0, w, HeaderHeight});
                }
                else
                {
                    ctx.VG().FillRect(Rectangle{0, 0, w, HeaderHeight}, Rgb(40, 44, 55, 255));
                }

                // Header text.
                if (ctx.FontService() != nullptr)
                {
                    fonts::CachedFont* font = ctx.FontService()->GetFont(12.0f);
                    if (font != nullptr)
                    {
                        const Color textColor =
                            ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235, 255));
                        ctx.VG().DrawText(m_title, font, Rectangle{8, 0, w - 30, HeaderHeight},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Middle, textColor);
                    }
                }

                // Close button (X).
                if (m_closable)
                {
                    const f32 cx = w - 14;
                    const f32 cy = HeaderHeight * 0.5f;
                    const f32 sz = 4.0f;

                    const Color closeColor =
                        ResolvePartColor(u8"close-button", StyleProperty::TextColor,
                                         ControlState::Normal, Rgb(180, 185, 200, 150));
                    if (Drawable* closeIcon = ResolvePartDrawable(
                            u8"close-button", StyleProperty::Background, ControlState::Normal))
                    {
                        const f32 iconSize = 2.0f * (sz + 2.0f);
                        ctx.VG().PushOpacity(closeColor.a);
                        closeIcon->Draw(ctx, Rectangle{cx - iconSize * 0.5f, cy - iconSize * 0.5f,
                                                       iconSize, iconSize});
                        ctx.VG().PopOpacity();
                    }
                    else
                    {
                        ctx.VG().DrawLine(Float2{cx - sz, cy - sz}, Float2{cx + sz, cy + sz},
                                          closeColor, 1.5f);
                        ctx.VG().DrawLine(Float2{cx + sz, cy - sz}, Float2{cx - sz, cy + sz},
                                          closeColor, 1.5f);
                    }
                }
            }

            // Content background.
            if (Drawable* contentDrawable = ResolvePartDrawable(
                    u8"content", StyleProperty::Background, ControlState::Normal))
            {
                contentDrawable->Draw(ctx, Rectangle{0, headerH, w, Height() - headerH});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, headerH, w, Height() - headerH},
                                  Rgb(42, 44, 54, 255));
            }

            DrawChildren(ctx);
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            if (m_showHeader)
            {
                // Close button hit-test.
                if (m_closable && e.X >= Width() - 22 && e.Y <= HeaderHeight)
                {
                    RequestClose();
                    e.Handled = true;
                    return;
                }

                // Track header click for drag.
                m_headerDrag = (e.Y <= HeaderHeight);
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            (void)e;
            m_headerDrag = false;
        }

        // === IDragSource ===

        [[nodiscard]] IDragSource* AsDragSource() override { return this; }

        [[nodiscard]] RefPtr<DragData> CreateDragData() override
        {
            if (!m_headerDrag)
            {
                return RefPtr<DragData>{};
            }
            return MakeRef<DockPanelDragData>(DefaultAllocator(), this);
        }

        [[nodiscard]] RefPtr<View>
        CreateDragVisual(DragData* data) override;   // out-of-line (needs DockableWindow)
        void OnDragStarted(DragData* data) override; // out-of-line (needs DockableWindow)

        void OnDragCompleted(DragData* data, DragDropEffects effect, bool cancelled) override
        {
            (void)effect;
            Opacity = 1.0f;

            // Restore floating window state only when cancelled.
            if (cancelled)
            {
                if (auto* panelData = Cast<DockPanelDragData>(data))
                {
                    if (panelData->SourceWindow != nullptr)
                    {
                        RestoreSourceWindow(panelData->SourceWindow);
                    }
                }
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 headerH = m_showHeader ? HeaderHeight : 0;
            if (m_content != nullptr && m_content->Visibility != VisibilityValue::Gone)
            {
                // A docked panel fills its dock-allocated region; measure content within the incoming
                // constraints (minus the header), never unbounded.
                m_content->Measure(BoxConstraints(constraints.MinWidth, constraints.MaxWidth,
                                                  Max(0.0f, constraints.MinHeight - headerH),
                                                  Max(0.0f, constraints.MaxHeight - headerH)));
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(0), constraints.ConstrainHeight(0)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 headerH = m_showHeader ? HeaderHeight : 0;
            if (m_content != nullptr && m_content->Visibility != VisibilityValue::Gone)
            {
                const f32 contentH = height - headerH;
                m_content->Measure(BoxConstraints::Tight(width, contentH));
                m_content->Layout(0, headerH, width, contentH);
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        // Defined out-of-line (needs DockableWindow complete).
        static void RestoreSourceWindow(DockableWindow* fw);

        String m_title = String(u8"Panel");
        String m_persistenceId;
        View* m_content = nullptr; // in the tree via AddView
        bool m_closable = true;
        bool m_showHeader = true;
        bool m_headerDrag = false; // true if mouse-down was on header (enables drag)
    };

    // ============================================================================================
    // DockableWindow - virtual/OS window wrapping a DockablePanel; draggable, resizable overlay.
    // ============================================================================================

    /// Resize edge/corner identification.
    enum class ResizeEdge
    {
        None,
        Top,
        Bottom,
        Left,
        Right,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    class DockableWindow : public ViewGroup
    {
        DRACONIC_OBJECT(DockableWindow, ViewGroup)
    public:
        bool IsOSWindow = false;
        IDockableWindowHost* WindowHost = nullptr;

        Event<void(DockableWindow*)> OnDockRequested;
        Event<void(DockableWindow*)> OnCloseRequested;

        explicit DockableWindow(DockablePanel* panel) : m_panel(panel)
        {
            if (panel != nullptr)
            {
                AddView(panel);
            }
        }

        /// The panel contained in this floating window.
        [[nodiscard]] DockablePanel* Panel() const { return m_panel; }

        /// Explicit size for the window. Set during resize or initial float.
        [[nodiscard]] f32 RequestedWidth() const { return m_requestedWidth; }
        void SetRequestedWidth(f32 value)
        {
            m_requestedWidth = Max(value, kMinWidth);
            Invalidate();
        }

        [[nodiscard]] f32 RequestedHeight() const { return m_requestedHeight; }
        void SetRequestedHeight(f32 value)
        {
            m_requestedHeight = Max(value, kMinHeight);
            Invalidate();
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            if (!IsOSWindow)
            {
                if (Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background))
                {
                    bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), Height()});
                }
                else
                {
                    ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(42, 44, 54, 255));
                }

                const Color borderColor =
                    ResolveStyleColor(StyleProperty::BorderColor, Rgb(65, 70, 85, 255));
                ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, borderColor, 2);
            }

            DrawChildren(ctx);
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            // Title bar double-click to re-dock.
            if (e.Y < m_titleBarHeight && e.ClickCount >= 2)
            {
                OnDockRequested.Invoke(this);
                e.Handled = true;
                return;
            }

            // Edge/corner resize.
            const ResizeEdge edge = HitTestEdge(e.X, e.Y);
            if (edge != ResizeEdge::None)
            {
                m_resizing = true;
                m_resizeEdge = edge;

                if (IsOSWindow && WindowHost != nullptr &&
                    WindowHost->TryGetDockableWindowBounds(this, m_resizeStartX, m_resizeStartY,
                                                           m_resizeStartW, m_resizeStartH))
                {
                    WindowHost->GetGlobalMousePosition(m_resizeStartMouseX, m_resizeStartMouseY);
                }
                else
                {
                    const Float2 screenPos = LocalToScreen(Float2{0, 0});
                    m_resizeStartX = screenPos.x;
                    m_resizeStartY = screenPos.y;
                    m_resizeStartW = Width();
                    m_resizeStartH = Height();
                    const Float2 screenMouse = LocalToScreen(Float2{e.X, e.Y});
                    m_resizeStartMouseX = screenMouse.x;
                    m_resizeStartMouseY = screenMouse.y;
                }

                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_resizing)
            {
                f32 curX, curY;
                if (IsOSWindow && WindowHost != nullptr)
                {
                    WindowHost->GetGlobalMousePosition(curX, curY);
                }
                else
                {
                    const Float2 screenMouse = LocalToScreen(Float2{e.X, e.Y});
                    curX = screenMouse.x;
                    curY = screenMouse.y;
                }
                const f32 dx = curX - m_resizeStartMouseX;
                const f32 dy = curY - m_resizeStartMouseY;

                f32 newX = m_resizeStartX;
                f32 newY = m_resizeStartY;
                f32 newW = m_resizeStartW;
                f32 newH = m_resizeStartH;

                switch (m_resizeEdge)
                {
                case ResizeEdge::Right:
                    newW = Max(kMinWidth, m_resizeStartW + dx);
                    break;
                case ResizeEdge::Bottom:
                    newH = Max(kMinHeight, m_resizeStartH + dy);
                    break;
                case ResizeEdge::Left:
                {
                    const f32 dw = Min(dx, m_resizeStartW - kMinWidth);
                    newX = m_resizeStartX + dw;
                    newW = m_resizeStartW - dw;
                    break;
                }
                case ResizeEdge::Top:
                {
                    const f32 dh = Min(dy, m_resizeStartH - kMinHeight);
                    newY = m_resizeStartY + dh;
                    newH = m_resizeStartH - dh;
                    break;
                }
                case ResizeEdge::BottomRight:
                    newW = Max(kMinWidth, m_resizeStartW + dx);
                    newH = Max(kMinHeight, m_resizeStartH + dy);
                    break;
                case ResizeEdge::BottomLeft:
                {
                    const f32 dw = Min(dx, m_resizeStartW - kMinWidth);
                    newX = m_resizeStartX + dw;
                    newW = m_resizeStartW - dw;
                    newH = Max(kMinHeight, m_resizeStartH + dy);
                    break;
                }
                case ResizeEdge::TopRight:
                {
                    newW = Max(kMinWidth, m_resizeStartW + dx);
                    const f32 dh = Min(dy, m_resizeStartH - kMinHeight);
                    newY = m_resizeStartY + dh;
                    newH = m_resizeStartH - dh;
                    break;
                }
                case ResizeEdge::TopLeft:
                {
                    const f32 dw = Min(dx, m_resizeStartW - kMinWidth);
                    newX = m_resizeStartX + dw;
                    newW = m_resizeStartW - dw;
                    const f32 dh = Min(dy, m_resizeStartH - kMinHeight);
                    newY = m_resizeStartY + dh;
                    newH = m_resizeStartH - dh;
                    break;
                }
                case ResizeEdge::None:
                    break;
                }

                m_requestedWidth = newW;
                m_requestedHeight = newH;

                if (IsOSWindow)
                {
                    if (WindowHost != nullptr)
                    {
                        WindowHost->ResizeDockableWindow(this, newX, newY, newW, newH);
                    }
                }
                else
                {
                    if (RootView* root = Root())
                    {
                        if (PopupLayer* pl = root->GetPopupLayer())
                        {
                            pl->UpdatePopupPosition(this, newX, newY);
                        }
                    }
                }

                e.Handled = true;
                return;
            }

            // Update cursor based on edge proximity.
            const ResizeEdge edge = HitTestEdge(e.X, e.Y);
            Cursor = EdgeToCursor(edge);
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_resizing && e.Button == MouseButton::Left)
            {
                m_resizing = false;
                m_resizeEdge = ResizeEdge::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }

        void OnMouseLeave() override
        {
            if (!m_resizing)
            {
                Cursor = CursorType::Default;
            }
        }

        // === Hit testing ===

        [[nodiscard]] View* HitTest(Float2 localPoint) override
        {
            if (!IsInteractionEnabled || Visibility != VisibilityValue::Visible)
            {
                return nullptr;
            }
            if (localPoint.x < 0 || localPoint.y < 0 || localPoint.x >= Width() ||
                localPoint.y >= Height())
            {
                return nullptr;
            }

            // During active resize, consume all input.
            if (m_resizing)
            {
                return this;
            }

            // Intercept edge zones for resize.
            if (HitTestEdge(localPoint.x, localPoint.y) != ResizeEdge::None)
            {
                return this;
            }

            // Otherwise delegate to children normally.
            return ViewGroup::HitTest(localPoint);
        }

        // === Panel detach (Beef IDockableWindow.DetachPanel; ported as a plain method) ===

        /// Detach and return the panel. Caller takes ownership (tracking).
        DockablePanel* DetachPanel()
        {
            DockablePanel* panel = m_panel;
            if (panel != nullptr)
            {
                RemoveView(panel);
                m_panel = nullptr;
            }
            return panel;
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 w = (m_requestedWidth > 0) ? constraints.ConstrainWidth(m_requestedWidth)
                                                 : constraints.ConstrainWidth(250);
            const f32 h = (m_requestedHeight > 0) ? constraints.ConstrainHeight(m_requestedHeight)
                                                  : constraints.ConstrainHeight(200);

            if (m_panel != nullptr)
            {
                m_panel->Measure(BoxConstraints::Tight(w, h));
            }

            MeasuredSize = Float2{w, h};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_panel != nullptr)
            {
                m_panel->Layout(0, 0, width, height);
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] ResizeEdge HitTestEdge(f32 x, f32 y) const
        {
            const f32 s = kResizeHitSize;
            const bool onLeft = x < s;
            const bool onRight = x >= Width() - s;
            const bool onTop = y < s;
            const bool onBottom = y >= Height() - s;

            if (onTop && onLeft)
            {
                return ResizeEdge::TopLeft;
            }
            if (onTop && onRight)
            {
                return ResizeEdge::TopRight;
            }
            if (onBottom && onLeft)
            {
                return ResizeEdge::BottomLeft;
            }
            if (onBottom && onRight)
            {
                return ResizeEdge::BottomRight;
            }
            if (onLeft)
            {
                return ResizeEdge::Left;
            }
            if (onRight)
            {
                return ResizeEdge::Right;
            }
            if (onTop)
            {
                return ResizeEdge::Top;
            }
            if (onBottom)
            {
                return ResizeEdge::Bottom;
            }
            return ResizeEdge::None;
        }

        [[nodiscard]] static CursorType EdgeToCursor(ResizeEdge edge)
        {
            switch (edge)
            {
            case ResizeEdge::Top:
            case ResizeEdge::Bottom:
                return CursorType::SizeNS;
            case ResizeEdge::Left:
            case ResizeEdge::Right:
                return CursorType::SizeWE;
            case ResizeEdge::TopLeft:
            case ResizeEdge::BottomRight:
                return CursorType::SizeNWSE;
            case ResizeEdge::TopRight:
            case ResizeEdge::BottomLeft:
                return CursorType::SizeNESW;
            case ResizeEdge::None:
                return CursorType::Default;
            }
            return CursorType::Default;
        }

        static constexpr f32 kResizeHitSize = 5.0f;
        static constexpr f32 kMinWidth = 150.0f;
        static constexpr f32 kMinHeight = 100.0f;

        DockablePanel* m_panel = nullptr;
        f32 m_titleBarHeight = 24;
        f32 m_requestedWidth = 0;
        f32 m_requestedHeight = 0;
        bool m_resizing = false;
        ResizeEdge m_resizeEdge = ResizeEdge::None;
        f32 m_resizeStartMouseX = 0;
        f32 m_resizeStartMouseY = 0;
        f32 m_resizeStartX = 0;
        f32 m_resizeStartY = 0;
        f32 m_resizeStartW = 0;
        f32 m_resizeStartH = 0;
    };

    // --- DockablePanel members that need DockableWindow complete -------------------------------
    inline RefPtr<View> DockablePanel::CreateDragVisual(DragData* data)
    {
        (void)data;
        // If dragging from a dockable window, suppress the adorner (we'll move the window instead).
        if (Cast<DockableWindow>(Parent) != nullptr)
        {
            return RefPtr<View>{};
        }

        RefPtr<DockDragPreview> preview = MakeRef<DockDragPreview>(DefaultAllocator());
        preview->SetTitle(m_title);
        return preview;
    }

    inline void DockablePanel::OnDragStarted(DragData* data)
    {
        if (auto* panelData = Cast<DockPanelDragData>(data))
        {
            if (auto* fw = Cast<DockableWindow>(Parent))
            {
                // Floating panel: move the actual window during drag. Dim + disable interaction so the
                // DockManager underneath receives drop events.
                panelData->SourceWindow = fw;
                fw->Opacity = 0.5f;
                fw->IsInteractionEnabled = false;

                if (Context != nullptr)
                {
                    DragDropManager* ddm = Context->DragDrop();
                    if (fw->IsOSWindow)
                    {
                        panelData->DragOffsetX = ddm->LastScreenX();
                        panelData->DragOffsetY = ddm->LastScreenY();
                    }
                    else
                    {
                        const Float2 windowPos = fw->LocalToScreen(Float2{0, 0});
                        panelData->DragOffsetX = ddm->LastScreenX() - windowPos.x;
                        panelData->DragOffsetY = ddm->LastScreenY() - windowPos.y;
                    }
                    ddm->AdornerOffsetX = 0;
                    ddm->AdornerOffsetY = 0;
                }
                return;
            }
        }

        // Docked panel: dim while dragging.
        Opacity = 0.4f;
        if (Context != nullptr)
        {
            DragDropManager* ddm = Context->DragDrop();
            ddm->AdornerOffsetX = -30.0f;
            ddm->AdornerOffsetY = -12.0f;
        }
    }

    inline void DockablePanel::RestoreSourceWindow(DockableWindow* fw)
    {
        fw->Opacity = 1.0f;
        fw->IsInteractionEnabled = true;
    }

    // ============================================================================================
    // DockTabGroup - tab container for docked panels. Implements IDragSource for tab dragging.
    // ============================================================================================
    class DockTabGroup : public ViewGroup, public IDragSource
    {
        DRACONIC_OBJECT(DockTabGroup, ViewGroup)
    public:
        DockTabGroup() = default;

        /// Fired when the selected tab changes (user click or programmatic).
        Event<void(DockablePanel*)> OnTabSelected;

        [[nodiscard]] i32 SelectedIndex() const { return m_selectedIndex; }
        void SetSelectedIndex(i32 value); // out-of-line (needs DockManager for ancestor notify)

        [[nodiscard]] i32 PanelCount() const { return static_cast<i32>(m_panels.Size()); }
        [[nodiscard]] f32 TabHeight() const { return m_tabHeight; }
        void SetTabHeight(f32 value)
        {
            m_tabHeight = Max(16.0f, value);
            Invalidate();
        }

        [[nodiscard]] DockablePanel* SelectedPanel() const
        {
            return (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_panels.Size()))
                       ? m_panels[static_cast<usize>(m_selectedIndex)]
                       : nullptr;
        }

        /// Add a panel as a tab. DockTabGroup does NOT take ownership (tree holds it via AddView).
        void AddPanel(DockablePanel* panel)
        {
            m_panels.PushBack(panel);
            panel->Visibility = VisibilityValue::Gone;
            panel->SetShowHeader(false); // Tab strip replaces panel header.
            AddView(panel);

            if (m_selectedIndex < 0)
            {
                SetSelectedIndex(0);
            }
            else
            {
                Invalidate();
            }
        }

        /// Insert a panel at a specific index.
        void InsertPanel(i32 index, DockablePanel* panel)
        {
            const i32 idx = Clamp(index, 0, static_cast<i32>(m_panels.Size()));
            m_panels.Insert(static_cast<usize>(idx), panel);
            panel->Visibility = VisibilityValue::Gone;
            panel->SetShowHeader(false);
            AddView(panel);

            if (m_selectedIndex < 0)
            {
                SetSelectedIndex(0);
            }
            else
            {
                if (idx <= m_selectedIndex)
                {
                    m_selectedIndex++;
                }
                Invalidate();
            }
        }

        /// Remove a panel from this group. Returns the panel (caller manages lifecycle).
        DockablePanel* RemovePanel(DockablePanel* panel)
        {
            const i32 idx = IndexOfPanel(panel);
            if (idx < 0)
            {
                return nullptr;
            }

            m_panels.RemoveAt(static_cast<usize>(idx));
            RemoveView(panel);
            panel->SetShowHeader(true); // Restore header for standalone/floating.

            if (m_selectedIndex >= static_cast<i32>(m_panels.Size()))
            {
                SetSelectedIndex(static_cast<i32>(m_panels.Size()) - 1);
            }
            else if (idx <= m_selectedIndex && m_selectedIndex > 0)
            {
                SetSelectedIndex(m_selectedIndex - 1);
            }
            else
            {
                Invalidate();
            }

            return panel;
        }

        /// Get the panel at the given index.
        [[nodiscard]] DockablePanel* GetPanel(i32 index) const
        {
            if (index >= 0 && index < static_cast<i32>(m_panels.Size()))
            {
                return m_panels[static_cast<usize>(index)];
            }
            return nullptr;
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 contentY = m_tabHeight;
            const f32 contentH = Height() - m_tabHeight;

            // Tab bar background (top).
            if (Drawable* stripDrawable =
                    ResolvePartDrawable(u8"strip", StyleProperty::Background, ControlState::Normal))
            {
                stripDrawable->Draw(ctx, Rectangle{0, 0, Width(), m_tabHeight});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), m_tabHeight}, Rgb(35, 37, 46, 255));
            }

            // Content area (below tabs).
            if (Drawable* contentDrawable = ResolvePartDrawable(
                    u8"content", StyleProperty::Background, ControlState::Normal))
            {
                contentDrawable->Draw(ctx, Rectangle{0, contentY, Width(), contentH});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, contentY, Width(), contentH}, Rgb(42, 44, 54, 255));
            }

            // Draw selected panel.
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_panels.Size()))
            {
                DockablePanel* panel = m_panels[static_cast<usize>(m_selectedIndex)];
                if (panel->Visibility != VisibilityValue::Gone)
                {
                    ctx.VG().PushState();
                    ctx.VG().Translate(panel->Bounds.x, panel->Bounds.y);
                    panel->OnDraw(ctx);
                    ctx.VG().PopState();
                }
            }

            // Draw tabs.
            m_tabRects.Clear();
            m_closeRects.Clear();
            if (ctx.FontService() == nullptr)
            {
                return;
            }

            // Tab label font from the theme (family + size). The theme sets a compact per-type FontSize
            // on DockTabGroup (12px) which now overrides the global View default via CSS tie-breaking.
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font =
                ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize);
            if (font == nullptr)
            {
                return;
            }

            const Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(35, 37, 46, 255));
            const Color closeColor =
                ResolvePartColor(u8"close-button", StyleProperty::TextColor, ControlState::Normal,
                                 Rgb(180, 185, 200, 150));

            // Overflow handling: measure the full strip first, clamp the scroll offset (and
            // bring the selected tab into view when selection just changed), THEN draw every
            // tab at its scrolled position clipped to the strip. Hit-testing reuses the drawn
            // m_tabRects, so clicks/hover/close stay aligned with what's on screen for free.
            Array<f32> tabWidths;
            f32 stripWidth = 2.0f;
            for (i32 i = 0; i < static_cast<i32>(m_panels.Size()); ++i)
            {
                DockablePanel* panel = m_panels[static_cast<usize>(i)];
                // INTEGER width: MSDF advances are fractional, and accumulated fractional
                // widths put every later tab (and its close icon) on a different subpixel
                // phase - the per-tab shimmer the icon bake exists to kill.
                f32 tabW = Round(font->font->MeasureString(panel->Title()) + 16);
                if (panel->Closable())
                {
                    tabW += kCloseButtonWidth;
                }
                tabWidths.PushBack(tabW);
                stripWidth += tabW + 2;
            }
            const f32 maxScroll = Max(0.0f, stripWidth - Width());
            m_tabScroll = Clamp(m_tabScroll, 0.0f, maxScroll);
            if (m_scrollSelectedIntoView && m_selectedIndex >= 0 &&
                m_selectedIndex < static_cast<i32>(tabWidths.Size()))
            {
                f32 selX = 2.0f;
                for (i32 i = 0; i < m_selectedIndex; ++i)
                {
                    selX += tabWidths[static_cast<usize>(i)] + 2;
                }
                const f32 selW = tabWidths[static_cast<usize>(m_selectedIndex)];
                if (selX - m_tabScroll < 0.0f)
                {
                    m_tabScroll = selX - 2.0f;
                }
                else if (selX + selW - m_tabScroll > Width())
                {
                    m_tabScroll = selX + selW - Width();
                }
                m_tabScroll = Clamp(m_tabScroll, 0.0f, maxScroll);
            }
            m_scrollSelectedIntoView = false;
            m_tabOverflow = maxScroll > 0.0f;

            ctx.VG().PushClipRect(Rectangle{0, 0, Width(), m_tabHeight});
            // Draw a tab background masked to top-rounded corners at the theme's resolved CornerRadius
            // (matches ui::TabView: rounded in the rounded theme, square in the flat one).
            const f32 tabCr = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
            const auto drawTabBg = [&](Drawable* d, const Rectangle& rect, Color fallback)
            {
                if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(d))
                {
                    const draconic::vg::CornerRadii saved = rrd->Radii;
                    rrd->Radii = draconic::vg::CornerRadii{tabCr, tabCr, 0.0f, 0.0f};
                    rrd->Draw(ctx, rect);
                    rrd->Radii = saved;
                }
                else if (d != nullptr)
                {
                    d->Draw(ctx, rect);
                }
                else
                {
                    ctx.VG().FillRect(rect, fallback);
                }
            };
            f32 tabX = 2 - m_tabScroll;
            for (i32 i = 0; i < static_cast<i32>(m_panels.Size()); ++i)
            {
                DockablePanel* panel = m_panels[static_cast<usize>(i)];
                const f32 tabW = tabWidths[static_cast<usize>(i)];
                const f32 textW = tabW - 16 - (panel->Closable() ? kCloseButtonWidth : 0.0f);
                const Rectangle tabRect{tabX, 0, tabW, m_tabHeight};
                m_tabRects.PushBack(tabRect);

                // Tab background.
                if (i == m_selectedIndex)
                {
                    drawTabBg(ResolvePartDrawable(u8"tab", StyleProperty::Background,
                                                  ControlState::Checked),
                              tabRect, Rgb(42, 44, 54, 255));
                    // Selected-tab accent strip (the same 2px indicator ui::TabView draws) -
                    // dock tabs read as "active" the way regular tabs do.
                    const Color accentColor = ResolveStyleColor(
                        StyleProperty::AccentColor,
                        Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
                    ctx.VG().FillRect(Rectangle{tabRect.x, tabRect.y + tabRect.height - 2.0f,
                                                tabRect.width, 2.0f},
                                      accentColor);
                }
                else if (i == m_hoveredTabIndex)
                {
                    drawTabBg(ResolvePartDrawable(u8"tab", StyleProperty::Background,
                                                  ControlState::Hover),
                              tabRect, Palette::Lighten(borderColor, 0.1f));
                }

                // Tab text.
                const Color textColor =
                    (i == m_selectedIndex)
                        ? ResolvePartColor(u8"tab", StyleProperty::TextColor, ControlState::Checked,
                                           Rgb(220, 225, 235, 255))
                        : ResolvePartColor(u8"tab", StyleProperty::TextColor, ControlState::Normal,
                                           Rgb(180, 185, 200, 153));
                ctx.VG().DrawText(panel->Title(), font, Rectangle{tabX + 8, 0, textW, m_tabHeight},
                                  fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                  textColor);

                // Close button - show on active tab always, on hovered inactive tab.
                if (panel->Closable())
                {
                    const f32 cbX = tabX + tabW - kCloseButtonPadding - kCloseButtonSize;
                    const f32 cbY = (m_tabHeight - kCloseButtonSize) * 0.5f;
                    const Rectangle closeRect{tabX + tabW - kCloseButtonWidth, 0, kCloseButtonWidth,
                                              m_tabHeight};
                    m_closeRects.PushBack(closeRect);

                    const bool showClose = (i == m_selectedIndex) || (i == m_hoveredTabIndex);
                    if (showClose)
                    {
                        if (Drawable* closeIcon = ResolvePartDrawable(
                                u8"close-button", StyleProperty::Background, ControlState::Normal))
                        {
                            ctx.VG().PushOpacity(closeColor.a);
                            closeIcon->Draw(
                                ctx, Rectangle{cbX, cbY, kCloseButtonSize, kCloseButtonSize});
                            ctx.VG().PopOpacity();
                        }
                        else
                        {
                            // Fallback: draw X lines.
                            const f32 cx = cbX + kCloseButtonSize * 0.5f;
                            const f32 cy = cbY + kCloseButtonSize * 0.5f;
                            const f32 sz = 3.0f;
                            ctx.VG().DrawLine(Float2{cx - sz, cy - sz}, Float2{cx + sz, cy + sz},
                                              closeColor, 1.5f);
                            ctx.VG().DrawLine(Float2{cx + sz, cy - sz}, Float2{cx - sz, cy + sz},
                                              closeColor, 1.5f);
                        }
                    }
                }
                else
                {
                    m_closeRects.PushBack(Rectangle{}); // Empty rect for non-closable tabs.
                }

                tabX += tabW + 2;
            }
            ctx.VG().PopClip();
        }

        // === Input ===

        // Wheel over the strip scrolls clipped tabs into view (there is no room for a
        // scrollbar in a 24px strip; selection changes also auto-scroll into view).
        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            // Wheel args arrive in ROOT space (unlike the localized mouse events) - convert
            // before testing the strip band, or the check only passes at the window's top.
            const Float2 origin = LocalToScreen(Float2{0.0f, 0.0f});
            const f32 localX = e.X - origin.x;
            const f32 localY = e.Y - origin.y;
            if (!m_tabOverflow || localY < 0.0f || localY >= m_tabHeight || localX < 0.0f ||
                localX >= Width())
            {
                return;
            }
            const f32 delta = (e.DeltaY != 0.0f) ? e.DeltaY : e.DeltaX;
            if (delta == 0.0f)
            {
                return;
            }
            m_tabScroll -= delta * 40.0f; // clamped in the next draw's pre-pass
            m_hoveredTabIndex = -1;
            Invalidate();
            e.Handled = true;
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            // Check close buttons first.
            for (i32 i = 0; i < static_cast<i32>(m_closeRects.Size()); ++i)
            {
                const Rectangle cr = m_closeRects[static_cast<usize>(i)];
                if (cr.width > 0 && i < static_cast<i32>(m_panels.Size()) &&
                    m_panels[static_cast<usize>(i)]->Closable())
                {
                    if (e.X >= cr.x && e.X < cr.x + cr.width && e.Y >= cr.y &&
                        e.Y < cr.y + cr.height)
                    {
                        m_panels[static_cast<usize>(i)]->RequestClose();
                        e.Handled = true;
                        return;
                    }
                }
            }

            // Check tab selection.
            m_dragTabIndex = -1;
            for (i32 i = 0; i < static_cast<i32>(m_tabRects.Size()); ++i)
            {
                const Rectangle r = m_tabRects[static_cast<usize>(i)];
                if (e.X >= r.x && e.X < r.x + r.width && e.Y >= r.y && e.Y < r.y + r.height)
                {
                    SetSelectedIndex(i);
                    m_dragTabIndex = i;
                    e.Handled = true;
                    return;
                }
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            i32 hovered = -1;
            for (i32 i = 0; i < static_cast<i32>(m_tabRects.Size()); ++i)
            {
                const Rectangle r = m_tabRects[static_cast<usize>(i)];
                if (e.X >= r.x && e.X < r.x + r.width && e.Y >= r.y && e.Y < r.y + r.height)
                {
                    hovered = i;
                    break;
                }
            }

            if (hovered != m_hoveredTabIndex)
            {
                m_hoveredTabIndex = hovered;
                Invalidate();
            }
        }

        void OnMouseLeave() override
        {
            if (m_hoveredTabIndex != -1)
            {
                m_hoveredTabIndex = -1;
                Invalidate();
            }
        }

        // === IDragSource ===

        [[nodiscard]] IDragSource* AsDragSource() override { return this; }

        [[nodiscard]] RefPtr<DragData> CreateDragData() override
        {
            if (m_dragTabIndex < 0 || m_dragTabIndex >= static_cast<i32>(m_panels.Size()))
            {
                return RefPtr<DragData>{};
            }
            return MakeRef<DockPanelDragData>(DefaultAllocator(),
                                              m_panels[static_cast<usize>(m_dragTabIndex)]);
        }

        [[nodiscard]] RefPtr<View> CreateDragVisual(DragData* data) override
        {
            if (auto* panelData = Cast<DockPanelDragData>(data))
            {
                RefPtr<DockDragPreview> preview = MakeRef<DockDragPreview>(DefaultAllocator());
                preview->SetTitle(panelData->Panel->Title());
                return preview;
            }
            return RefPtr<View>{};
        }

        void OnDragStarted(DragData* data) override
        {
            if (auto* panelData = Cast<DockPanelDragData>(data))
            {
                m_draggedPanel = panelData->Panel;
                m_dragOriginalIndex = m_dragTabIndex;
                RemovePanel(m_draggedPanel);

                // Position preview so the title bar is under the cursor.
                if (Context != nullptr)
                {
                    DragDropManager* ddm = Context->DragDrop();
                    ddm->AdornerOffsetX = -30.0f;
                    ddm->AdornerOffsetY = -12.0f;
                }
            }
        }

        void OnDragCompleted(DragData* data, DragDropEffects effect,
                             bool cancelled) override; // out-of-line (needs IDockHost use)

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            PurgeDeletedPanels();

            const f32 w = constraints.ConstrainWidth(150);
            const f32 h = constraints.ConstrainHeight(100);

            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_panels.Size()))
            {
                DockablePanel* panel = m_panels[static_cast<usize>(m_selectedIndex)];
                if (panel->Visibility != VisibilityValue::Gone)
                {
                    panel->Measure(BoxConstraints::Tight(w, Max(0.0f, h - m_tabHeight)));
                }
            }

            MeasuredSize = Float2{w, h};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            PurgeDeletedPanels();

            const f32 contentY = m_tabHeight;
            const f32 contentH = Max(0.0f, height - m_tabHeight);

            for (i32 i = 0; i < static_cast<i32>(m_panels.Size()); ++i)
            {
                DockablePanel* panel = m_panels[static_cast<usize>(i)];
                if (i == m_selectedIndex)
                {
                    panel->Visibility = VisibilityValue::Visible;
                    panel->Measure(BoxConstraints::Tight(width, contentH));
                    panel->Layout(0, contentY, width, contentH);
                }
                else
                {
                    panel->Visibility = VisibilityValue::Gone;
                }
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] i32 IndexOfPanel(DockablePanel* panel) const
        {
            for (i32 i = 0; i < static_cast<i32>(m_panels.Size()); ++i)
            {
                if (m_panels[static_cast<usize>(i)] == panel)
                {
                    return i;
                }
            }
            return -1;
        }

        /// Purge any panels that are pending deletion (defense-in-depth).
        void PurgeDeletedPanels()
        {
            bool changed = false;
            for (i32 i = static_cast<i32>(m_panels.Size()) - 1; i >= 0; --i)
            {
                DockablePanel* p = m_panels[static_cast<usize>(i)];
                if (p->IsPendingDeletion || p->Parent != this)
                {
                    m_panels.RemoveAt(static_cast<usize>(i));
                    changed = true;
                }
            }
            if (changed && m_selectedIndex >= static_cast<i32>(m_panels.Size()))
            {
                m_selectedIndex = static_cast<i32>(m_panels.Size()) - 1;
            }
        }

        static constexpr f32 kCloseButtonSize = 10.0f;   // Icon draw size (X glyph rect).
        static constexpr f32 kCloseButtonPadding = 6.0f; // Space reserved for close area.
        static constexpr f32 kCloseButtonWidth =
            kCloseButtonSize + kCloseButtonPadding * 2; // Total width.

        Array<DockablePanel*> m_panels; // Non-owning refs (tree owns via AddView).
        i32 m_selectedIndex = -1;
        f32 m_tabHeight = 24;
        f32 m_tabScroll = 0;        // horizontal strip scroll (0 = leftmost)
        bool m_tabOverflow = false; // strip wider than the group (from last draw)
        bool m_scrollSelectedIntoView = false;
        i32 m_hoveredTabIndex = -1;
        Array<Rectangle> m_tabRects;
        Array<Rectangle> m_closeRects; // Per-tab close button rects.

        // Drag state for tab dragging.
        i32 m_dragTabIndex = -1;
        DockablePanel* m_draggedPanel = nullptr;
        i32 m_dragOriginalIndex = -1;
    };

    // ============================================================================================
    // DockManager - multi-window docking system.
    // ============================================================================================
    class DockManager : public ViewGroup, public IDropTarget, public IPopupOwner, public IDockHost
    {
        DRACONIC_OBJECT(DockManager, ViewGroup)
    public:
        /// Optional host for OS-level dockable windows.
        IDockableWindowHost* DockableWindowHost = nullptr;

        /// Fired when a dock tab is selected (user click or programmatic).
        Event<void(DockablePanel*)> OnPanelActivated;

        DockManager()
        {
            m_zoneIndicator = MakeRef<DockZoneIndicator>(DefaultAllocator());
            m_zoneIndicator->Visibility = VisibilityValue::Gone;
        }

        [[nodiscard]] View* RootNode() const { return m_rootNode; }

        // === IDockHost ===

        [[nodiscard]] UIContext* HostContext() override { return Context; }

        // === Public API ===

        /// Create and add a new dockable panel with content.
        DockablePanel* AddPanel(StringView title, View* content)
        {
            RefPtr<DockablePanel> panel =
                MakeRef<DockablePanel>(DefaultAllocator(), title, content);
            panel->OnCloseRequested.Add([this](DockablePanel* p) { ClosePanel(p); });
            panel->DockHost = this;
            DockablePanel* raw = panel.Get();
            m_panels.PushBack(Move(panel));
            return raw;
        }

        /// Dock a panel at the specified position relative to the root.
        void DockPanel(DockablePanel* panel, DockPosition position)
        {
            DockPanelRelativeTo(panel, position, m_rootNode);
        }

        /// Dock a panel at the specified position relative to another node.
        void DockPanelRelativeTo(DockablePanel* panel, DockPosition position, View* relativeTo)
        {
            // Remove panel from its current location first.
            RemoveFromTree(panel);

            // Save dock position for re-dock after floating.
            panel->SaveDockPosition(position, relativeTo);

            if (position == DockPosition::Float)
            {
                FloatPanel(panel, 100, 100);
                return;
            }

            // Clean up empty nodes left behind. Grab a safe ViewId reference in case cleanup invalidates.
            const ViewId relativeToId = (relativeTo != nullptr) ? relativeTo->Id : ViewId::Invalid;
            CleanupEmptyNodes();

            // Re-resolve relativeTo - it may have been collapsed by cleanup.
            View* target = relativeTo;
            if (relativeToId.IsValid() && Context != nullptr)
            {
                View* resolved = Context->GetViewById(relativeToId);
                if (resolved != nullptr && !resolved->IsPendingDeletion)
                {
                    target = resolved;
                }
                else
                {
                    target = m_rootNode;
                }
            }
            else if (target != nullptr && target->IsPendingDeletion)
            {
                target = m_rootNode;
            }

            if (position == DockPosition::Center)
            {
                if (auto* tabGroup = Cast<DockTabGroup>(target))
                {
                    tabGroup->AddPanel(panel);
                }
                else if (auto* existingPanel = Cast<DockablePanel>(target))
                {
                    if (auto* parentGroup = Cast<DockTabGroup>(existingPanel->Parent))
                    {
                        parentGroup->AddPanel(panel);
                    }
                    else
                    {
                        // Wrap standalone panel in a new tab group.
                        RefPtr<DockTabGroup> group = MakeRef<DockTabGroup>(DefaultAllocator());
                        ReplaceNode(existingPanel, group.Get());
                        group->AddPanel(existingPanel);
                        group->AddPanel(panel);
                    }
                }
                else
                {
                    // Target is a DockSplit or null - find first tab group in subtree.
                    DockTabGroup* targetGroup = nullptr;
                    if (target != nullptr)
                    {
                        targetGroup = FindFirstTabGroup(target);
                    }
                    if (targetGroup == nullptr && m_rootNode != nullptr)
                    {
                        targetGroup = FindFirstTabGroup(m_rootNode);
                    }

                    if (targetGroup != nullptr)
                    {
                        targetGroup->AddPanel(panel);
                    }
                    else
                    {
                        // Empty tree - create new root.
                        RefPtr<DockTabGroup> group = MakeRef<DockTabGroup>(DefaultAllocator());
                        group->AddPanel(panel);
                        m_rootNode = group.Get();
                        AddView(group.Get());
                    }
                }
                // A freshly docked tab becomes the ACTIVE tab (deliberate deviation from the
                // Sedulous port, user-approved 2026-07-11: upstream keeps the existing selection
                // and its editor activates by hand; mainstream-IDE behavior activates on dock).
                // Covers programmatic docking, interactive drag-drop, and window redock - all
                // funnel through here. Layout restore is unaffected (ApplyLayout rebuilds tab
                // groups directly and sets ActiveTabIndex itself).
                ActivatePanel(panel);
                Invalidate();
                return;
            }

            // Create split.
            InsertSplit(target, panel, position);
        }

        /// Undock a panel from its current position.
        void UndockPanel(DockablePanel* panel)
        {
            RemoveFromTree(panel);
            CleanupEmptyNodes();
            Invalidate();
        }

        /// Float a panel at the given position (OS window if supported, else PopupLayer).
        void FloatPanel(DockablePanel* panel, f32 x, f32 y) override
        {
            const f32 floatW = panel->Width() > 0 ? panel->Width() : 300;
            const f32 floatH = panel->Height() > 0 ? panel->Height() : 250;

            RemoveFromTree(panel);

            RefPtr<DockableWindow> dockable = MakeRef<DockableWindow>(DefaultAllocator(), panel);
            m_dockableWindows.PushBack(dockable.Get());

            dockable->OnDockRequested.Add([this](DockableWindow* fw) { RedockDockableWindow(fw); });
            dockable->OnCloseRequested.Add([this](DockableWindow* fw) { CloseDockableWindow(fw); });
            dockable->WindowHost = DockableWindowHost;

            const bool useOSWindow =
                (DockableWindowHost != nullptr && DockableWindowHost->SupportsOSWindows());

            if (useOSWindow)
            {
                dockable->IsOSWindow = true;
                DockableWindowHost->CreateDockableWindow(dockable.Get(), floatW, floatH, x, y,
                                                         [this](View* view)
                                                         {
                                                             if (auto* fw =
                                                                     Cast<DockableWindow>(view))
                                                             {
                                                                 CloseDockableWindow(fw);
                                                             }
                                                         });
            }
            else if (Context != nullptr)
            {
                // Virtual mode via PopupLayer.
                if (RootView* root = Root())
                {
                    if (PopupLayer* pl = root->GetPopupLayer())
                    {
                        pl->ShowPopup(dockable.Get(), this, x, y, false, false, true);
                    }
                }
            }

            CleanupEmptyNodes();
            Invalidate();
        }

        /// Close a panel (undock and delete).
        void ClosePanel(DockablePanel* panel)
        {
            UndockPanel(panel);
            QueueDeleteNode(panel); // keeps `panel` alive across the deferred boundary (see helper)
            ErasePanel(panel);      // drop the mPanels ownership ref
        }

        /// Activates (selects) a panel's tab in its parent tab group.
        void ActivatePanel(DockablePanel* panel)
        {
            if (auto* tabGroup = Cast<DockTabGroup>(panel->Parent))
            {
                for (i32 i = 0; i < tabGroup->PanelCount(); ++i)
                {
                    if (tabGroup->GetPanel(i) == panel)
                    {
                        tabGroup->SetSelectedIndex(i);
                        return;
                    }
                }
            }
        }

        /// Re-dock a dockable window back into the dock tree.
        void RedockDockableWindow(DockableWindow* dockable)
        {
            DockablePanel* panel = dockable->DetachPanel();
            if (panel == nullptr)
            {
                return;
            }

            DestroyDockableWindow(dockable);

            View* relativeTo = nullptr;
            if (panel->mLastRelativeToId.IsValid() && Context != nullptr)
            {
                relativeTo = Context->GetViewById(panel->mLastRelativeToId);
            }

            if (relativeTo != nullptr)
            {
                DockPanelRelativeTo(panel, panel->mLastDockPosition, relativeTo);
            }
            else
            {
                DockPanel(panel, DockPosition::Center);
            }
        }

        /// Close a dockable window.
        void CloseDockableWindow(DockableWindow* dockable)
        {
            DockablePanel* panel = dockable->DetachPanel();
            DestroyDockableWindow(dockable);

            if (panel != nullptr)
            {
                QueueDeleteNode(panel);
                ErasePanel(panel);
            }
        }

        /// Destroy a dockable window (OS or virtual).
        void DestroyDockableWindow(DockableWindow* dockable) override
        {
            EraseDockableWindow(dockable);

            if (dockable->IsOSWindow && DockableWindowHost != nullptr)
            {
                DockableWindowHost->DestroyDockableWindow(dockable);
                QueueDeleteNode(dockable);
            }
            else
            {
                // ClosePopup handles deletion (ownsView=true).
                if (RootView* root = Root())
                {
                    if (PopupLayer* pl = root->GetPopupLayer())
                    {
                        pl->ClosePopup(dockable);
                    }
                }
            }
        }

        // === Layout Persistence ===

        /// Exports the current dock tree as a serializable data structure. Returns null if empty.
        [[nodiscard]] UniquePtr<DockLayoutNode> ExportLayout()
        {
            if (m_rootNode == nullptr)
            {
                return UniquePtr<DockLayoutNode>{};
            }
            return ExportNode(m_rootNode);
        }

        /// Rebuilds the dock tree from a previously exported layout (matched by PersistenceId).
        void ApplyLayout(DockLayoutNode* layout)
        {
            if (layout == nullptr)
            {
                return;
            }

            // Collect all registered panels by PersistenceId.
            HashMap<StringView, DockablePanel*> panelMap;
            for (RefPtr<DockablePanel>& panel : m_panels)
            {
                if (panel->PersistenceId().Size() > 0)
                {
                    panelMap.InsertOrAssign(panel->PersistenceId(), panel.Get());
                }
            }

            // Detach all panels from the current tree (don't delete them).
            for (RefPtr<DockablePanel>& panel : m_panels)
            {
                if (panel->Parent != nullptr)
                {
                    if (auto* tabGroup = Cast<DockTabGroup>(panel->Parent))
                    {
                        tabGroup->RemovePanel(panel.Get());
                    }
                    else if (panel->Parent == this)
                    {
                        RemoveView(panel.Get());
                    }
                }
            }

            // Clear old tree structure.
            if (m_rootNode != nullptr)
            {
                ClearTreeStructure(m_rootNode);
                m_rootNode = nullptr;
            }

            // Rebuild from layout.
            RefPtr<View> newRoot = BuildNode(layout, panelMap);
            m_rootNode = newRoot.Get();
            if (m_rootNode != nullptr)
            {
                AddView(m_rootNode);
            }

            // Float any panels that weren't placed by the layout.
            f32 floatX = 100, floatY = 100;
            for (RefPtr<DockablePanel>& panel : m_panels)
            {
                if (panel->Parent == nullptr)
                {
                    FloatPanel(panel.Get(), floatX, floatY);
                    floatX += 30;
                    floatY += 30;
                }
            }

            Invalidate();
        }

        /// Finds a registered panel by its PersistenceId.
        [[nodiscard]] DockablePanel* FindPanelById(StringView persistenceId)
        {
            for (RefPtr<DockablePanel>& panel : m_panels)
            {
                if (panel->PersistenceId() == persistenceId)
                {
                    return panel.Get();
                }
            }
            return nullptr;
        }

        // === Layout / Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            if (Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background))
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(30, 30, 35, 255));
            }

            DrawChildren(ctx);

            // Draw zone indicator overlay.
            if (m_zoneIndicator->Visibility != VisibilityValue::Gone)
            {
                ctx.VG().PushState();
                m_zoneIndicator->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }

        // === IDropTarget ===

        [[nodiscard]] IDropTarget* AsDropTarget() override { return this; }

        [[nodiscard]] DragDropEffects CanAcceptDrop(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            (void)localY;
            return (data->Format() == u8"dock/panel") ? DragDropEffects::Move
                                                      : DragDropEffects::None;
        }

        void OnDragEnter(DragData* data, f32 localX, f32 localY) override
        {
            if (data->Format() == u8"dock/panel")
            {
                ShowZoneIndicators(localX, localY);
            }
        }

        void OnDragOver(DragData* data, f32 localX, f32 localY) override
        {
            // Move virtual dockable window to follow cursor (PopupLayer mode).
            if (auto* panelData = Cast<DockPanelDragData>(data))
            {
                if (panelData->SourceWindow != nullptr && !panelData->SourceWindow->IsOSWindow &&
                    Context != nullptr)
                {
                    if (RootView* root = Root())
                    {
                        if (PopupLayer* pl = root->GetPopupLayer())
                        {
                            const f32 screenX = Context->DragDrop()->LastScreenX();
                            const f32 screenY = Context->DragDrop()->LastScreenY();
                            pl->UpdatePopupPosition(panelData->SourceWindow,
                                                    screenX - panelData->DragOffsetX,
                                                    screenY - panelData->DragOffsetY);
                        }
                    }
                }
            }

            if (m_zoneIndicator->Visibility != VisibilityValue::Gone)
            {
                ShowZoneIndicators(localX, localY);
                m_zoneIndicator->UpdateHover(localX, localY);
            }
        }

        void OnDragLeave(DragData* data) override
        {
            (void)data;
            HideZoneIndicators();
        }

        [[nodiscard]] DragDropEffects OnDrop(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            (void)localY;
            if (auto* panelData = Cast<DockPanelDragData>(data))
            {
                const Optional<DockTarget> target = m_zoneIndicator->HoveredTarget();
                HideZoneIndicators();

                f32 floatX = 0, floatY = 0;
                if (Context != nullptr)
                {
                    floatX = Context->DragDrop()->LastScreenX();
                    floatY = Context->DragDrop()->LastScreenY();
                }

                if (target.HasValue())
                {
                    const DockTarget t = target.Value();
                    if (t.Position == DockPosition::Float)
                    {
                        FloatPanel(panelData->Panel, floatX, floatY);
                    }
                    else
                    {
                        DockPanelRelativeTo(panelData->Panel, t.Position, t.RelativeTo);
                    }
                    return DragDropEffects::Move;
                }
                else
                {
                    // Dropped inside DockManager but not on a zone - float.
                    if (panelData->SourceWindow != nullptr)
                    {
                        panelData->SourceWindow->Opacity = 1.0f;
                        panelData->SourceWindow->IsInteractionEnabled = true;
                    }
                    else
                    {
                        FloatPanel(panelData->Panel, floatX, floatY);
                    }
                    return DragDropEffects::Move;
                }
            }

            HideZoneIndicators();
            return DragDropEffects::None;
        }

        // === IPopupOwner ===

        void OnPopupClosed(View* popup) override
        {
            for (i32 i = static_cast<i32>(m_dockableWindows.Size()) - 1; i >= 0; --i)
            {
                if (m_dockableWindows[static_cast<usize>(i)] == popup)
                {
                    m_dockableWindows.RemoveAt(static_cast<usize>(i));
                    break;
                }
            }
        }

        [[nodiscard]] View* OwnerView() override { return this; }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 w = constraints.ConstrainWidth(0);
            const f32 h = constraints.ConstrainHeight(0);

            if (m_rootNode != nullptr)
            {
                m_rootNode->Measure(BoxConstraints::Tight(w, h));
            }

            MeasuredSize = Float2{w, h};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_rootNode != nullptr)
            {
                m_rootNode->Layout(0, 0, width, height);
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        void ErasePanel(DockablePanel* panel)
        {
            for (usize i = 0; i < m_panels.Size(); ++i)
            {
                if (m_panels[i].Get() == panel)
                {
                    m_panels.RemoveAt(i);
                    return;
                }
            }
        }

        void EraseDockableWindow(DockableWindow* dw)
        {
            for (usize i = 0; i < m_dockableWindows.Size(); ++i)
            {
                if (m_dockableWindows[i] == dw)
                {
                    m_dockableWindows.RemoveAt(i);
                    return;
                }
            }
        }

        // === Internal tree operations ===

        void InsertSplit(View* existingNode, DockablePanel* panel, DockPosition position)
        {
            // If the target is a panel inside a DockTabGroup, split relative to the tab group instead.
            View* target = existingNode;
            if (target != nullptr && Cast<DockTabGroup>(target->Parent) != nullptr)
            {
                target = target->Parent;
            }

            const ::draconic::ui::Orientation orientation =
                (position == DockPosition::Left || position == DockPosition::Right)
                    ? ::draconic::ui::Orientation::Horizontal
                    : ::draconic::ui::Orientation::Vertical;
            RefPtr<DockSplit> split = MakeRef<DockSplit>(DefaultAllocator(), orientation);

            RefPtr<DockTabGroup> group = MakeRef<DockTabGroup>(DefaultAllocator());
            group->AddPanel(panel);

            const bool panelFirst =
                (position == DockPosition::Left || position == DockPosition::Top);

            if (target == nullptr)
            {
                if (m_rootNode != nullptr)
                {
                    RefPtr<View> pinRoot(m_rootNode); // pin across detach
                    RemoveView(m_rootNode);
                    if (panelFirst)
                    {
                        split->SetChildren(group.Get(), m_rootNode);
                    }
                    else
                    {
                        split->SetChildren(m_rootNode, group.Get());
                    }
                }
                else
                {
                    split->SetChildren(group.Get(), nullptr);
                }
                m_rootNode = split.Get();
                AddView(split.Get());
            }
            else
            {
                View* parent = target->Parent;
                if (parent == this)
                {
                    RefPtr<View> pinTarget(target);
                    RemoveView(target);
                    if (panelFirst)
                    {
                        split->SetChildren(group.Get(), target);
                    }
                    else
                    {
                        split->SetChildren(target, group.Get());
                    }
                    m_rootNode = split.Get();
                    AddView(split.Get());
                }
                else if (auto* parentSplit = Cast<DockSplit>(parent))
                {
                    // Capture both children BEFORE detaching - DockSplit indices shift.
                    const bool isFirst = (parentSplit->First() == target);
                    View* otherChild = isFirst ? parentSplit->Second() : parentSplit->First();

                    RefPtr<View> pinTarget(target);
                    RefPtr<View> pinOther(otherChild);
                    parentSplit->RemoveView(target);
                    if (otherChild != nullptr)
                    {
                        parentSplit->RemoveView(otherChild);
                    }

                    if (panelFirst)
                    {
                        split->SetChildren(group.Get(), target);
                    }
                    else
                    {
                        split->SetChildren(target, group.Get());
                    }

                    if (isFirst)
                    {
                        parentSplit->SetChildren(split.Get(), otherChild);
                    }
                    else
                    {
                        parentSplit->SetChildren(otherChild, split.Get());
                    }
                }
            }

            Invalidate();
        }

        void RemoveFromTree(DockablePanel* panel)
        {
            // Check if in a tab group.
            if (auto* tabGroup = Cast<DockTabGroup>(panel->Parent))
            {
                tabGroup->RemovePanel(panel);
                return;
            }

            // Direct child of DockManager (root).
            if (panel->Parent == this && m_rootNode == panel)
            {
                RemoveView(panel);
                m_rootNode = nullptr;
                return;
            }

            // In a dockable window.
            for (usize i = 0; i < m_dockableWindows.Size(); ++i)
            {
                if (m_dockableWindows[i]->Panel() == panel)
                {
                    DockableWindow* dockable = m_dockableWindows[i];
                    dockable->DetachPanel();
                    DestroyDockableWindow(dockable);
                    return;
                }
            }
        }

        void ReplaceNode(View* oldNode, View* newNode)
        {
            if (oldNode == m_rootNode)
            {
                RefPtr<View> pinOld(oldNode);
                RemoveView(oldNode);
                m_rootNode = newNode;
                AddView(newNode);
            }
            else if (auto* parentSplit = Cast<DockSplit>(oldNode->Parent))
            {
                const bool isFirst = (parentSplit->First() == oldNode);
                View* other = isFirst ? parentSplit->Second() : parentSplit->First();

                RefPtr<View> pinOld(oldNode);
                RefPtr<View> pinOther(other);
                parentSplit->RemoveView(oldNode);
                if (other != nullptr)
                {
                    parentSplit->RemoveView(other);
                }

                if (isFirst)
                {
                    parentSplit->SetChildren(newNode, other);
                }
                else
                {
                    parentSplit->SetChildren(other, newNode);
                }
            }
        }

        void CleanupEmptyNodes()
        {
            if (m_isCleaningUp)
            {
                return;
            } // Prevent re-entrancy.
            m_isCleaningUp = true;
            if (m_rootNode != nullptr)
            {
                RefPtr<View> newRoot = CleanupNode(m_rootNode);
                m_rootNode = newRoot.Get();
            }
            m_isCleaningUp = false;
        }

        RefPtr<View> CleanupNode(View* node)
        {
            RefPtr<View> pinNode(node); // keep alive across detaches / QueueDeleteNode
            if (auto* split = Cast<DockSplit>(node))
            {
                View* first = split->First();
                View* second = split->Second();
                RefPtr<View> pinFirst(first);
                RefPtr<View> pinSecond(second);

                if (second != nullptr)
                {
                    split->RemoveView(second);
                }
                if (first != nullptr)
                {
                    split->RemoveView(first);
                }

                RefPtr<View> cleanFirst = (first != nullptr) ? CleanupNode(first) : RefPtr<View>{};
                RefPtr<View> cleanSecond =
                    (second != nullptr) ? CleanupNode(second) : RefPtr<View>{};

                // Queue originals for deletion if they were replaced.
                if (first != nullptr && cleanFirst.Get() != first)
                {
                    QueueDeleteNode(first);
                }
                if (second != nullptr && cleanSecond.Get() != second)
                {
                    QueueDeleteNode(second);
                }

                if (cleanFirst && cleanSecond)
                {
                    split->AddView(cleanFirst.Get());
                    split->AddView(cleanSecond.Get());
                    return pinNode;
                }
                else if (cleanFirst)
                {
                    if (split == m_rootNode)
                    {
                        RemoveView(split);
                        QueueDeleteNode(split);
                        AddView(cleanFirst.Get());
                    }
                    return cleanFirst;
                }
                else if (cleanSecond)
                {
                    if (split == m_rootNode)
                    {
                        RemoveView(split);
                        QueueDeleteNode(split);
                        AddView(cleanSecond.Get());
                    }
                    return cleanSecond;
                }
                else
                {
                    if (split == m_rootNode)
                    {
                        RemoveView(split);
                        QueueDeleteNode(split);
                    }
                    return RefPtr<View>{};
                }
            }
            else if (auto* tabGroup = Cast<DockTabGroup>(node))
            {
                if (tabGroup->PanelCount() == 0)
                {
                    if (tabGroup == m_rootNode)
                    {
                        RemoveView(tabGroup);
                        QueueDeleteNode(tabGroup);
                    }
                    return RefPtr<View>{};
                }
            }

            return pinNode;
        }

        /// Deletes tree structure nodes (DockSplit, DockTabGroup) without deleting panels.
        void ClearTreeStructure(View* node)
        {
            RefPtr<View> pinNode(node);
            if (auto* split = Cast<DockSplit>(node))
            {
                View* first = split->First();
                View* second = split->Second();
                RefPtr<View> pinFirst(first);
                RefPtr<View> pinSecond(second);

                if (second != nullptr)
                {
                    split->RemoveView(second);
                }
                if (first != nullptr)
                {
                    split->RemoveView(first);
                }

                if (first != nullptr)
                {
                    ClearTreeStructure(first);
                }
                if (second != nullptr)
                {
                    ClearTreeStructure(second);
                }

                if (node->Parent == this)
                {
                    RemoveView(node);
                }
                QueueDeleteNode(split);
            }
            else if (auto* tabGroup = Cast<DockTabGroup>(node))
            {
                // Remove panels without deleting them.
                while (tabGroup->PanelCount() > 0)
                {
                    tabGroup->RemovePanel(tabGroup->GetPanel(tabGroup->PanelCount() - 1));
                }

                if (node->Parent == this)
                {
                    RemoveView(node);
                }
                QueueDeleteNode(tabGroup);
            }
        }

        // Deferred deletion of a tree-owned structure node (or an orphaned panel). Keeps the node alive
        // across the mutation-queue boundary so the deferred tree-removal + release is UAF-free. With no
        // Context, dropping the caller's last ref frees it via RAII.
        void QueueDeleteNode(View* node)
        {
            if (node == nullptr || node->IsPendingDeletion)
            {
                return;
            }
            if (Context != nullptr)
            {
                node->IsPendingDeletion = true;
                RefPtr<View> keep(node);
                Context->MutationQueueRef().QueueAction(
                    [keep = Move(keep)]() mutable
                    {
                        if (keep && keep->Parent != nullptr)
                        {
                            if (auto* pg = Cast<ViewGroup>(keep->Parent))
                            {
                                pg->RemoveView(keep.Get(), false);
                            }
                        }
                        keep = nullptr; // release (deferred destruction)
                    });
            }
            // else: RAII / tree ownership frees it once the caller's last ref drops.
        }

        // === Persistence helpers ===

        UniquePtr<DockLayoutNode> ExportNode(View* node)
        {
            if (auto* split = Cast<DockSplit>(node))
            {
                UniquePtr<DockLayoutNode> ln = MakeUnique<DockLayoutNode>(DefaultAllocator());
                ln->Type = DockLayoutNodeType::Split;
                ln->Direction = split->Orientation();
                ln->SplitRatio = split->SplitRatio();
                if (split->First() != nullptr)
                {
                    ln->First = ExportNode(split->First());
                }
                if (split->Second() != nullptr)
                {
                    ln->Second = ExportNode(split->Second());
                }
                return ln;
            }
            else if (auto* tabGroup = Cast<DockTabGroup>(node))
            {
                UniquePtr<DockLayoutNode> ln = MakeUnique<DockLayoutNode>(DefaultAllocator());
                ln->Type = DockLayoutNodeType::TabGroup;
                ln->ActiveTabIndex = tabGroup->SelectedIndex();
                for (i32 i = 0; i < tabGroup->PanelCount(); ++i)
                {
                    DockablePanel* panel = tabGroup->GetPanel(i);
                    if (panel->PersistenceId().Size() > 0)
                    {
                        ln->PanelIds.PushBack(String(panel->PersistenceId()));
                    }
                }
                return ln;
            }
            else if (auto* panel = Cast<DockablePanel>(node))
            {
                // Standalone panel not in a tab group - wrap in a TabGroup node.
                UniquePtr<DockLayoutNode> ln = MakeUnique<DockLayoutNode>(DefaultAllocator());
                ln->Type = DockLayoutNodeType::TabGroup;
                ln->ActiveTabIndex = 0;
                if (panel->PersistenceId().Size() > 0)
                {
                    ln->PanelIds.PushBack(String(panel->PersistenceId()));
                }
                return ln;
            }

            return UniquePtr<DockLayoutNode>{};
        }

        RefPtr<View> BuildNode(DockLayoutNode* layoutNode,
                               HashMap<StringView, DockablePanel*>& panelMap)
        {
            if (layoutNode->Type == DockLayoutNodeType::Split)
            {
                RefPtr<DockSplit> split =
                    MakeRef<DockSplit>(DefaultAllocator(), layoutNode->Direction);
                split->SetSplitRatio(layoutNode->SplitRatio);

                RefPtr<View> first = layoutNode->First
                                         ? BuildNode(layoutNode->First.Get(), panelMap)
                                         : RefPtr<View>{};
                RefPtr<View> second = layoutNode->Second
                                          ? BuildNode(layoutNode->Second.Get(), panelMap)
                                          : RefPtr<View>{};

                if (first && second)
                {
                    split->SetChildren(first.Get(), second.Get());
                    return split;
                }
                else if (first)
                {
                    return first;
                }
                else if (second)
                {
                    return second;
                }
                else
                {
                    return RefPtr<View>{};
                }
            }
            else // TabGroup
            {
                RefPtr<DockTabGroup> tabGroup = MakeRef<DockTabGroup>(DefaultAllocator());

                for (const String& id : layoutNode->PanelIds)
                {
                    if (DockablePanel** found = panelMap.Find(id.AsView()))
                    {
                        tabGroup->AddPanel(*found);
                    }
                }

                if (tabGroup->PanelCount() == 0)
                {
                    return RefPtr<View>{};
                }

                if (layoutNode->ActiveTabIndex >= 0 &&
                    layoutNode->ActiveTabIndex < tabGroup->PanelCount())
                {
                    tabGroup->SetSelectedIndex(layoutNode->ActiveTabIndex);
                }

                return tabGroup;
            }
        }

        // === Zone indicators ===

        void ShowZoneIndicators(f32 cursorX, f32 cursorY)
        {
            m_zoneIndicator->ClearTargets();
            const f32 zoneSize = 40;

            if (m_rootNode == nullptr)
            {
                const f32 cx = Width() * 0.5f;
                const f32 cy = Height() * 0.5f;
                m_zoneIndicator->AddTarget(
                    DockPosition::Center,
                    Rectangle{cx - zoneSize * 0.5f, cy - zoneSize * 0.5f, zoneSize, zoneSize},
                    nullptr);
            }
            else
            {
                const f32 cx = Width() * 0.5f;
                const f32 cy = Height() * 0.5f;

                // Root-level edge zones.
                m_zoneIndicator->AddTarget(DockPosition::Top,
                                           Rectangle{cx - zoneSize * 0.5f, 8, zoneSize, zoneSize},
                                           m_rootNode);
                m_zoneIndicator->AddTarget(
                    DockPosition::Bottom,
                    Rectangle{cx - zoneSize * 0.5f, Height() - zoneSize - 8, zoneSize, zoneSize},
                    m_rootNode);
                m_zoneIndicator->AddTarget(DockPosition::Left,
                                           Rectangle{8, cy - zoneSize * 0.5f, zoneSize, zoneSize},
                                           m_rootNode);
                m_zoneIndicator->AddTarget(
                    DockPosition::Right,
                    Rectangle{Width() - zoneSize - 8, cy - zoneSize * 0.5f, zoneSize, zoneSize},
                    m_rootNode);

                // Walk tree to find hovered leaf node and add its zones.
                View* hoveredNode = FindHoveredDockNode(m_rootNode, cursorX, cursorY);
                if (hoveredNode != nullptr)
                {
                    const Rectangle bounds = GetNodeBounds(hoveredNode);
                    if (bounds.width > 0 && bounds.height > 0)
                    {
                        const f32 ncx = bounds.x + bounds.width * 0.5f;
                        const f32 ncy = bounds.y + bounds.height * 0.5f;
                        const f32 smallZone = 32;

                        m_zoneIndicator->AddTarget(DockPosition::Center,
                                                   Rectangle{ncx - smallZone * 0.5f,
                                                             ncy - smallZone * 0.5f, smallZone,
                                                             smallZone},
                                                   hoveredNode);

                        const f32 edgeOffset = smallZone + 4;
                        m_zoneIndicator->AddTarget(DockPosition::Top,
                                                   Rectangle{ncx - smallZone * 0.5f,
                                                             ncy - edgeOffset - smallZone * 0.5f,
                                                             smallZone, smallZone},
                                                   hoveredNode);
                        m_zoneIndicator->AddTarget(DockPosition::Bottom,
                                                   Rectangle{ncx - smallZone * 0.5f,
                                                             ncy + edgeOffset - smallZone * 0.5f,
                                                             smallZone, smallZone},
                                                   hoveredNode);
                        m_zoneIndicator->AddTarget(DockPosition::Left,
                                                   Rectangle{ncx - edgeOffset - smallZone * 0.5f,
                                                             ncy - smallZone * 0.5f, smallZone,
                                                             smallZone},
                                                   hoveredNode);
                        m_zoneIndicator->AddTarget(DockPosition::Right,
                                                   Rectangle{ncx + edgeOffset - smallZone * 0.5f,
                                                             ncy - smallZone * 0.5f, smallZone,
                                                             smallZone},
                                                   hoveredNode);
                    }
                }
            }

            // Hand the indicator our resolved theme accent (it's drawn manually, outside the styled tree).
            m_zoneIndicator->Accent =
                ResolveStyleColor(StyleProperty::AccentColor,
                                  Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
            m_zoneIndicator->Visibility = VisibilityValue::Visible;
            m_zoneIndicator->Layout(0, 0, Width(), Height());
        }

        void HideZoneIndicators()
        {
            m_zoneIndicator->ClearTargets();
            m_zoneIndicator->Visibility = VisibilityValue::Gone;
        }

        /// Find the leaf DockTabGroup or DockablePanel that the cursor is over.
        View* FindHoveredDockNode(View* node, f32 localX, f32 localY)
        {
            if (auto* split = Cast<DockSplit>(node))
            {
                if (split->First() != nullptr)
                {
                    const Rectangle bounds = GetNodeBounds(split->First());
                    if (localX >= bounds.x && localX < bounds.x + bounds.width &&
                        localY >= bounds.y && localY < bounds.y + bounds.height)
                    {
                        return FindHoveredDockNode(split->First(), localX, localY);
                    }
                }
                if (split->Second() != nullptr)
                {
                    const Rectangle bounds = GetNodeBounds(split->Second());
                    if (localX >= bounds.x && localX < bounds.x + bounds.width &&
                        localY >= bounds.y && localY < bounds.y + bounds.height)
                    {
                        return FindHoveredDockNode(split->Second(), localX, localY);
                    }
                }
                return node;
            }
            return node;
        }

        /// Find the first DockTabGroup in a subtree (depth-first).
        DockTabGroup* FindFirstTabGroup(View* node)
        {
            if (auto* tabGroup = Cast<DockTabGroup>(node))
            {
                return tabGroup;
            }

            if (auto* split = Cast<DockSplit>(node))
            {
                if (split->First() != nullptr)
                {
                    if (DockTabGroup* result = FindFirstTabGroup(split->First()))
                    {
                        return result;
                    }
                }
                if (split->Second() != nullptr)
                {
                    return FindFirstTabGroup(split->Second());
                }
            }

            return nullptr;
        }

        /// Get bounds of a dock tree node in DockManager local coordinates.
        Rectangle GetNodeBounds(View* node)
        {
            f32 x = 0, y = 0;
            View* current = node;
            while (current != nullptr && current != this)
            {
                x += current->Bounds.x;
                y += current->Bounds.y;
                current = current->Parent;
            }
            return Rectangle{x, y, node->Width(), node->Height()};
        }

        View* m_rootNode = nullptr;
        Array<RefPtr<DockablePanel>>
            m_panels; // Owning registry (survives undock; tree shares the ref).
        Array<DockableWindow*>
            m_dockableWindows; // Non-owning tracking (PopupLayer owns floating windows).
        RefPtr<DockZoneIndicator> m_zoneIndicator; // Owned, drawn manually, never AddView'd.
        bool m_isCleaningUp = false;
    };

    // --- DockTabGroup::SetSelectedIndex needs DockManager complete -----------------------------
    inline void DockablePanel::OnMouseDownCapture(MouseEventArgs&)
    {
        for (View* ancestor = Parent; ancestor != nullptr; ancestor = ancestor->Parent)
        {
            if (auto* dm = Cast<DockManager>(ancestor))
            {
                dm->OnPanelActivated.Invoke(this);
                return;
            }
        }
    }

    inline void DockTabGroup::SetSelectedIndex(i32 value)
    {
        if (value >= -1 && value < static_cast<i32>(m_panels.Size()) && m_selectedIndex != value)
        {
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_panels.Size()))
            {
                m_panels[static_cast<usize>(m_selectedIndex)]->Visibility = VisibilityValue::Gone;
            }

            m_selectedIndex = value;
            m_scrollSelectedIntoView = true; // clipped tab strips scroll the new tab into view

            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_panels.Size()))
            {
                m_panels[static_cast<usize>(m_selectedIndex)]->Visibility =
                    VisibilityValue::Visible;
            }

            Invalidate();
            OnTabSelected.Invoke(SelectedPanel());

            // Notify the DockManager (if any) about the tab change.
            View* ancestor = Parent;
            while (ancestor != nullptr)
            {
                if (auto* dm = Cast<DockManager>(ancestor))
                {
                    dm->OnPanelActivated.Invoke(SelectedPanel());
                    break;
                }
                ancestor = ancestor->Parent;
            }
        }
    }

    // --- DockTabGroup::OnDragCompleted needs IDockHost use -------------------------------------
    inline void DockTabGroup::OnDragCompleted(DragData* data, DragDropEffects effect,
                                              bool cancelled)
    {
        (void)data;
        (void)effect;
        if (cancelled && m_draggedPanel != nullptr)
        {
            IDockHost* dockHost = m_draggedPanel->DockHost;
            if (dockHost != nullptr)
            {
                // Re-apply the adorner offset used by the drag preview so the new window lands where the
                // preview appeared (avoids a "jump on drop").
                UIContext* hostCtx = dockHost->HostContext();
                DragDropManager* dragMgr = (hostCtx != nullptr) ? hostCtx->DragDrop() : nullptr;
                const f32 cursorX = dragMgr ? dragMgr->LastScreenX() : 100.0f;
                const f32 cursorY = dragMgr ? dragMgr->LastScreenY() : 100.0f;
                const f32 offsetX = dragMgr ? dragMgr->AdornerOffsetX : 0.0f;
                const f32 offsetY = dragMgr ? dragMgr->AdornerOffsetY : 0.0f;
                dockHost->FloatPanel(m_draggedPanel, cursorX + offsetX, cursorY + offsetY);
            }
            else
            {
                InsertPanel(m_dragOriginalIndex, m_draggedPanel);
            }
        }

        m_draggedPanel = nullptr;
        m_dragOriginalIndex = -1;
        m_dragTabIndex = -1;
    }

    DRACONIC_DEFINE_OBJECT(DockDragPreview, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DockPanelDragData, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DockablePanel, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DockableWindow, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DockTabGroup, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DockManager, "draconic::ui::toolkit")
}

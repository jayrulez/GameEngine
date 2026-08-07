// Draconic UI - :tab_view partition
//
// Tabbed container: clickable tab headers + switchable content, Top/Bottom/Left/Right strip placement,
// optional closable tabs. Ported from Sedulous.UI/src/Controls/TabView.bf. Each tab's content is a
// logical child (AddView -> ViewGroup RefPtr owns it); TabItem stores a BORROWED View* + an owned Title
// String (RAII, no manual dtor). Beef SelectedIndex/TabCount get/set -> methods; RectangleF ->
// foundation::Rectangle; CornerRadii fields are lowercase (topLeft/topRight/bottomRight/bottomLeft).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:tab_view;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import :view;
import :property;
import :event;
import :box_constraints;
import :thickness;
import :draw_context;
import :drawable;
import :rounded_rect_drawable;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :enums;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::ui
{
    enum class TabPlacement
    {
        Top,
        Bottom,
        Left,
        Right
    };

    class TabView : public ViewGroup
    {
        DRACONIC_OBJECT(TabView, ViewGroup)
    public:
        Property<f32> TabHeight{28.0f};
        Property<TabPlacement> Placement{TabPlacement::Top};
        Property<bool> TabsClosable{false};
        Property<f32> CloseButtonSize{12.0f};
        Property<f32> MinTabWidth{50.0f};

        Event<void(TabView*, i32)> OnTabChanged;
        Event<void(TabView*, i32)> OnTabCloseRequested;

        TabView()
        {
            IsFocusable = true;
            ClipsContent = true;
            WantsArrowKeys = true;
            TabHeight.SetOwner(this);
            Placement.SetOwner(this);
            TabsClosable.SetOwner(this, InvalidationKind::Visual);
            CloseButtonSize.SetOwner(this, InvalidationKind::Visual);
            MinTabWidth.SetOwner(this);
        }

        [[nodiscard]] i32 SelectedIndex() const noexcept { return m_selectedIndex; }
        void SetSelectedIndex(i32 value)
        {
            if (value == m_selectedIndex)
            {
                return;
            }
            if (value < 0 || value >= static_cast<i32>(m_tabs.Size()))
            {
                return;
            }
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_tabs.Size()))
            {
                m_tabs[static_cast<usize>(m_selectedIndex)].Content->Visibility =
                    VisibilityValue::Gone;
            }
            m_selectedIndex = value;
            m_scrollSelectedIntoView =
                true; // an overflowing strip scrolls the new tab into view on rebuild
            m_tabs[static_cast<usize>(m_selectedIndex)].Content->Visibility =
                VisibilityValue::Visible;
            Invalidate();
            OnTabChanged.Invoke(this, m_selectedIndex);
        }

        [[nodiscard]] usize TabCount() const noexcept { return m_tabs.Size(); }

        /// Index of the tab currently under the cursor, or -1 if none (drives the Hover visual state).
        [[nodiscard]] i32 HoveredTabIndex() const noexcept { return m_hoveredTabIndex; }

        /// Add a tab (content becomes a logical child). Returns the new tab index.
        i32 AddTab(StringView title, View* content, bool closable = false)
        {
            TabItem item;
            item.Title = String(title);
            item.Content = content;
            item.IsClosable = closable || TabsClosable.Value();
            m_tabs.PushBack(Move(item));

            const i32 index = static_cast<i32>(m_tabs.Size() - 1);
            content->Visibility = VisibilityValue::Gone;
            AddView(content);
            if (m_selectedIndex < 0)
            {
                SetSelectedIndex(0);
            }
            return index;
        }

        void RemoveTab(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_tabs.Size()))
            {
                return;
            }
            View* content = m_tabs[static_cast<usize>(index)].Content;
            RemoveView(content, true);
            m_tabs.RemoveAt(static_cast<usize>(index));
            if (m_selectedIndex >= static_cast<i32>(m_tabs.Size()))
            {
                m_selectedIndex = static_cast<i32>(m_tabs.Size()) - 1;
            }
            if (m_selectedIndex >= 0)
            {
                m_tabs[static_cast<usize>(m_selectedIndex)].Content->Visibility =
                    VisibilityValue::Visible;
            }
            Invalidate();
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            RebuildTabRects();
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font =
                ctx.FontService() != nullptr
                    ? ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize)
                    : nullptr;
            const ControlState controlState = GetControlState();

            Drawable* stripDrawable =
                ResolvePartDrawable(u8"strip", StyleProperty::Background, controlState);
            Drawable* contentDrawable =
                ResolvePartDrawable(u8"content", StyleProperty::Background, controlState);
            const Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor,
                                  Color{60.0f / 255.0f, 65.0f / 255.0f, 80.0f / 255.0f, 1.0f});
            const Color accentColor =
                ResolveStyleColor(StyleProperty::AccentColor,
                                  Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});

            const TabPlacement place = Placement.Value();
            const f32 th = TabHeight.Value();
            switch (place)
            {
            case TabPlacement::Top:
                DrawRegion(ctx, stripDrawable, Rectangle{0, 0, Width(), th});
                DrawContentRegion(ctx, contentDrawable, Rectangle{0, th, Width(), Height() - th});
                ctx.VG().DrawLine(Float2{0, th}, Float2{Width(), th}, borderColor, 1);
                break;
            case TabPlacement::Bottom:
                DrawContentRegion(ctx, contentDrawable, Rectangle{0, 0, Width(), Height() - th});
                DrawRegion(ctx, stripDrawable, Rectangle{0, Height() - th, Width(), th});
                ctx.VG().DrawLine(Float2{0, Height() - th}, Float2{Width(), Height() - th},
                                  borderColor, 1);
                break;
            case TabPlacement::Left:
            {
                const f32 stripW = ComputeStripWidth();
                DrawRegion(ctx, stripDrawable, Rectangle{0, 0, stripW, Height()});
                DrawContentRegion(ctx, contentDrawable,
                                  Rectangle{stripW, 0, Width() - stripW, Height()});
                ctx.VG().DrawLine(Float2{stripW, 0}, Float2{stripW, Height()}, borderColor, 1);
                break;
            }
            case TabPlacement::Right:
            {
                const f32 stripW = ComputeStripWidth();
                const f32 stripX = Width() - stripW;
                DrawContentRegion(ctx, contentDrawable, Rectangle{0, 0, stripX, Height()});
                DrawRegion(ctx, stripDrawable, Rectangle{stripX, 0, stripW, Height()});
                ctx.VG().DrawLine(Float2{stripX, 0}, Float2{stripX, Height()}, borderColor, 1);
                break;
            }
            }

            // Clip the tabs to the strip band so an overflowing, scrolled strip doesn't spill past it.
            Rectangle stripClip;
            switch (place)
            {
            case TabPlacement::Top:
                stripClip = Rectangle{0, 0, Width(), th};
                break;
            case TabPlacement::Bottom:
                stripClip = Rectangle{0, Height() - th, Width(), th};
                break;
            case TabPlacement::Left:
            {
                const f32 sw = ComputeStripWidth();
                stripClip = Rectangle{0, 0, sw, Height()};
                break;
            }
            case TabPlacement::Right:
            {
                const f32 sw = ComputeStripWidth();
                stripClip = Rectangle{Width() - sw, 0, sw, Height()};
                break;
            }
            }
            ctx.VG().PushClipRect(stripClip);
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (i >= m_tabRects.Size())
                {
                    break;
                }
                const Rectangle rect = m_tabRects[i];
                const bool isActive = static_cast<i32>(i) == m_selectedIndex;
                const bool isHovered = static_cast<i32>(i) == m_hoveredTabIndex;

                ControlState tabState = ControlState::Normal;
                if (isActive)
                {
                    tabState |= ControlState::Checked;
                }
                if (isHovered)
                {
                    tabState |= ControlState::Hover;
                }
                if (Drawable* tabDrawable =
                        ResolvePartDrawable(u8"tab", StyleProperty::Background, tabState))
                {
                    DrawTabRegion(ctx, tabDrawable, rect);
                }

                if (isActive)
                {
                    switch (place)
                    {
                    case TabPlacement::Top:
                        ctx.VG().FillRect(
                            Rectangle{rect.x, rect.y + rect.height - 2, rect.width, 2},
                            accentColor);
                        break;
                    case TabPlacement::Bottom:
                        ctx.VG().FillRect(Rectangle{rect.x, rect.y, rect.width, 2}, accentColor);
                        break;
                    case TabPlacement::Left:
                        ctx.VG().FillRect(
                            Rectangle{rect.x + rect.width - 2, rect.y, 2, rect.height},
                            accentColor);
                        break;
                    case TabPlacement::Right:
                        ctx.VG().FillRect(Rectangle{rect.x, rect.y, 2, rect.height}, accentColor);
                        break;
                    }
                }

                if (font != nullptr)
                {
                    const Color fallback =
                        isActive
                            ? Color{240.0f / 255.0f, 240.0f / 255.0f, 245.0f / 255.0f, 1.0f}
                            : (isHovered
                                   ? Color{200.0f / 255.0f, 205.0f / 255.0f, 215.0f / 255.0f, 1.0f}
                                   : Color{140.0f / 255.0f, 145.0f / 255.0f, 160.0f / 255.0f,
                                           1.0f});
                    const Color textColor =
                        ResolvePartColor(u8"tab", StyleProperty::TextColor, tabState, fallback);
                    Rectangle textRect = rect;
                    textRect.x += 8;
                    textRect.width -= 16;
                    if (m_tabs[i].IsClosable)
                    {
                        textRect.width -= ResolvePartFloat(u8"close-button", StyleProperty::Width,
                                                           controlState, 12) +
                                          4;
                    }
                    ctx.VG().DrawText(m_tabs[i].Title, font, textRect, fonts::TextAlignment::Left,
                                      fonts::VerticalAlignment::Middle, textColor);
                }

                if (m_tabs[i].IsClosable)
                {
                    const f32 cbSize =
                        ResolvePartFloat(u8"close-button", StyleProperty::Width, controlState, 12);
                    const f32 cbX = rect.x + rect.width - cbSize - 4;
                    const f32 cbY = rect.y + (rect.height - cbSize) * 0.5f;
                    ControlState cbState = ControlState::Normal;
                    if (isActive || isHovered)
                    {
                        cbState |= ControlState::Hover;
                    }
                    const Color cbColor = ResolvePartColor(
                        u8"close-button", StyleProperty::TextColor, cbState,
                        Color{120.0f / 255.0f, 125.0f / 255.0f, 140.0f / 255.0f, 1.0f});
                    if (Drawable* closeIcon = ResolvePartDrawable(
                            u8"close-button", StyleProperty::Background, cbState))
                    {
                        ctx.VG().PushOpacity(cbColor.a);
                        closeIcon->Draw(ctx, Rectangle{cbX, cbY, cbSize, cbSize});
                        ctx.VG().PopOpacity();
                    }
                    else if (font != nullptr)
                    {
                        ctx.VG().DrawText(u8"x", font, Rectangle{cbX, rect.y, cbSize, rect.height},
                                          fonts::TextAlignment::Center,
                                          fonts::VerticalAlignment::Middle, cbColor);
                    }
                }
            }
            ctx.VG().PopClip();

            DrawChildren(ctx);
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            const Float2 local = ScreenToLocal(MouseScreenPos());
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (i >= m_tabRects.Size())
                {
                    break;
                }
                const Rectangle rect = m_tabRects[i];
                if (local.x >= rect.x && local.x < rect.x + rect.width && local.y >= rect.y &&
                    local.y < rect.y + rect.height)
                {
                    if (m_tabs[i].IsClosable)
                    {
                        const f32 cbSize = CloseButtonSize.Value();
                        const f32 cbX = rect.x + rect.width - cbSize - 4;
                        const f32 cbY = rect.y + (rect.height - cbSize) * 0.5f;
                        if (local.x >= cbX && local.x <= cbX + cbSize && local.y >= cbY &&
                            local.y <= cbY + cbSize)
                        {
                            OnTabCloseRequested.Invoke(this, static_cast<i32>(i));
                            e.Handled = true;
                            return;
                        }
                    }
                    SetSelectedIndex(static_cast<i32>(i));
                    e.Handled = true;
                    return;
                }
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            (void)e;
            const Float2 local = ScreenToLocal(MouseScreenPos());
            i32 newHovered = -1;
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (i >= m_tabRects.Size())
                {
                    break;
                }
                const Rectangle rect = m_tabRects[i];
                if (local.x >= rect.x && local.x < rect.x + rect.width && local.y >= rect.y &&
                    local.y < rect.y + rect.height)
                {
                    newHovered = static_cast<i32>(i);
                    break;
                }
            }
            if (newHovered != m_hoveredTabIndex)
            {
                m_hoveredTabIndex = newHovered;
                Invalidate();
            }
        }

        // Tab hover is tracked in OnMouseMove, which stops firing once the cursor leaves the TabView - so
        // clear the hovered tab here or it stays visually stuck in the Hover state. (Divergence from
        // Sedulous, whose TabView has no OnMouseLeave and leaves the last tab highlighted.)
        void OnMouseLeave() override
        {
            if (m_hoveredTabIndex != -1)
            {
                m_hoveredTabIndex = -1;
                Invalidate();
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (m_tabs.Size() == 0)
            {
                return;
            }
            switch (e.Key)
            {
            case KeyCode::Left:
                if (m_selectedIndex > 0)
                {
                    SetSelectedIndex(m_selectedIndex - 1);
                }
                e.Handled = true;
                break;
            case KeyCode::Right:
                if (m_selectedIndex < static_cast<i32>(m_tabs.Size()) - 1)
                {
                    SetSelectedIndex(m_selectedIndex + 1);
                }
                e.Handled = true;
                break;
            default:
                break;
            }
        }

        // Wheel over an overflowing tab strip scrolls the clipped tabs into view (there is no room for a
        // scrollbar in the strip; selection changes auto-scroll too). Ported from the dock tab strip.
        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            if (!m_tabOverflow)
            {
                return;
            }
            // Wheel args arrive in root space (unlike the localized mouse events); convert before
            // testing the strip band, or the check only passes at the window's origin.
            const Float2 local = ScreenToLocal(Float2{e.X, e.Y});
            const TabPlacement place = Placement.Value();
            const f32 th = TabHeight.Value();
            Rectangle strip;
            switch (place)
            {
            case TabPlacement::Top:
                strip = Rectangle{0, 0, Width(), th};
                break;
            case TabPlacement::Bottom:
                strip = Rectangle{0, Height() - th, Width(), th};
                break;
            case TabPlacement::Left:
            {
                const f32 sw = ComputeStripWidth();
                strip = Rectangle{0, 0, sw, Height()};
                break;
            }
            case TabPlacement::Right:
            {
                const f32 sw = ComputeStripWidth();
                strip = Rectangle{Width() - sw, 0, sw, Height()};
                break;
            }
            }
            if (local.x < strip.x || local.x >= strip.x + strip.width || local.y < strip.y ||
                local.y >= strip.y + strip.height)
            {
                return;
            }
            const f32 delta = (e.DeltaY != 0.0f) ? e.DeltaY : e.DeltaX;
            if (delta == 0.0f)
            {
                return;
            }
            m_tabScroll -= delta * 40.0f; // clamped in the next rebuild
            m_hoveredTabIndex = -1;
            Invalidate();
            e.Handled = true;
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const TabPlacement place = Placement.Value();
            BoxConstraints contentConstraints =
                (place == TabPlacement::Top || place == TabPlacement::Bottom)
                    ? constraints.Deflate(Thickness{0, TabHeight.Value(), 0, 0})
                    : constraints.Deflate(Thickness{ComputeStripWidth(), 0, 0, 0});

            f32 contentW = 0, contentH = 0;
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_tabs.Size()))
            {
                View* content = m_tabs[static_cast<usize>(m_selectedIndex)].Content;
                if (content->Visibility != VisibilityValue::Gone)
                {
                    content->Measure(contentConstraints);
                    contentW = content->MeasuredSize.x;
                    contentH = content->MeasuredSize.y;
                }
            }

            if (place == TabPlacement::Top || place == TabPlacement::Bottom)
                MeasuredSize = Float2{constraints.ConstrainWidth(contentW),
                                      constraints.ConstrainHeight(contentH + TabHeight.Value())};
            else
                MeasuredSize = Float2{constraints.ConstrainWidth(contentW + ComputeStripWidth()),
                                      constraints.ConstrainHeight(contentH)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            // Rebuild the tab hit-rects at layout time too (not just in OnDraw), so mouse hit-testing is
            // valid immediately after layout - e.g. the first mouse-move before the first draw.
            RebuildTabRects();
            if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<i32>(m_tabs.Size()))
            {
                return;
            }
            View* content = m_tabs[static_cast<usize>(m_selectedIndex)].Content;
            if (content->Visibility == VisibilityValue::Gone)
            {
                return;
            }
            const f32 th = TabHeight.Value();
            switch (Placement.Value())
            {
            case TabPlacement::Top:
                content->Layout(0, th, width, foundation::Max(0.0f, height - th));
                break;
            case TabPlacement::Bottom:
                content->Layout(0, 0, width, foundation::Max(0.0f, height - th));
                break;
            case TabPlacement::Left:
            {
                const f32 stripW = ComputeStripWidth();
                content->Layout(stripW, 0, foundation::Max(0.0f, width - stripW), height);
                break;
            }
            case TabPlacement::Right:
            {
                const f32 stripW = ComputeStripWidth();
                content->Layout(0, 0, foundation::Max(0.0f, width - stripW), height);
                break;
            }
            }
        }

    private:
        struct TabItem
        {
            String Title;
            View* Content = nullptr;
            bool IsClosable = false;
        };

        [[nodiscard]] Float2 MouseScreenPos() const
        {
            if (Context == nullptr)
            {
                return Float2{0, 0};
            }
            return Float2{Context->GetInputManager()->MouseX(),
                          Context->GetInputManager()->MouseY()};
        }

        void RebuildTabRects()
        {
            m_tabRects.Clear();
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font =
                (Context != nullptr && Context->FontService() != nullptr)
                    ? Context->FontService()->GetFont(ResolveStyleFontFamily(), fontSize)
                    : nullptr;
            const TabPlacement place = Placement.Value();
            const bool horizontal = (place == TabPlacement::Top || place == TabPlacement::Bottom);
            const f32 th = TabHeight.Value();

            // Measure each tab's extent along the strip's main axis (x for Top/Bottom, y for Left/Right)
            // and the total strip length, so overflow can be detected and the scroll offset clamped.
            Array<f32> extents;
            f32 total = 0.0f;
            if (horizontal)
            {
                for (const TabItem& tab : m_tabs)
                {
                    f32 tabW = 80;
                    if (font != nullptr)
                    {
                        tabW = font->font->MeasureString(tab.Title) + 24;
                    }
                    if (tab.IsClosable)
                    {
                        tabW += CloseButtonSize.Value() + 4;
                    }
                    tabW = foundation::Max(MinTabWidth.Value(), tabW);
                    extents.PushBack(tabW);
                    total += tabW;
                }
            }
            else
            {
                for (usize i = 0; i < m_tabs.Size(); ++i)
                {
                    extents.PushBack(th);
                    total += th;
                }
            }

            // Clamp the scroll offset to the overflow, then bring the selected tab into view when a
            // selection change requested it (mirrors the dock tab strip). Both run on the measured
            // extents, so the drawn rects — reused for hit-testing — stay aligned with what's on screen.
            const f32 available = horizontal ? Width() : Height();
            const f32 maxScroll = foundation::Max(0.0f, total - available);
            m_tabScroll = foundation::Clamp(m_tabScroll, 0.0f, maxScroll);
            if (m_scrollSelectedIntoView && m_selectedIndex >= 0 &&
                m_selectedIndex < static_cast<i32>(extents.Size()))
            {
                f32 selStart = 0.0f;
                for (i32 i = 0; i < m_selectedIndex; ++i)
                {
                    selStart += extents[static_cast<usize>(i)];
                }
                const f32 selExtent = extents[static_cast<usize>(m_selectedIndex)];
                if (selStart - m_tabScroll < 0.0f)
                {
                    m_tabScroll = selStart;
                }
                else if (selStart + selExtent - m_tabScroll > available)
                {
                    m_tabScroll = selStart + selExtent - available;
                }
                m_tabScroll = foundation::Clamp(m_tabScroll, 0.0f, maxScroll);
            }
            m_scrollSelectedIntoView = false;
            m_tabOverflow = maxScroll > 0.0f;

            // Emit each tab's rect, offset by the scroll along the main axis.
            if (horizontal)
            {
                const f32 stripY = (place == TabPlacement::Top) ? 0.0f : Height() - th;
                f32 xPos = -m_tabScroll;
                for (usize i = 0; i < m_tabs.Size(); ++i)
                {
                    m_tabRects.PushBack(Rectangle{xPos, stripY, extents[i], th});
                    xPos += extents[i];
                }
            }
            else
            {
                const f32 stripW = ComputeStripWidth();
                const f32 stripX = (place == TabPlacement::Left) ? 0.0f : Width() - stripW;
                f32 yPos = -m_tabScroll;
                for (usize i = 0; i < m_tabs.Size(); ++i)
                {
                    m_tabRects.PushBack(Rectangle{stripX, yPos, stripW, th});
                    yPos += th;
                }
            }
        }

        [[nodiscard]] f32 ComputeStripWidth()
        {
            const TabPlacement place = Placement.Value();
            if (place == TabPlacement::Top || place == TabPlacement::Bottom)
            {
                return 0;
            }
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font =
                (Context != nullptr && Context->FontService() != nullptr)
                    ? Context->FontService()->GetFont(ResolveStyleFontFamily(), fontSize)
                    : nullptr;
            if (font == nullptr)
            {
                return 100;
            }
            f32 maxW = 0;
            for (const TabItem& tab : m_tabs)
            {
                maxW = foundation::Max(maxW, font->font->MeasureString(tab.Title));
            }
            return maxW + 24 + (TabsClosable.Value() ? CloseButtonSize.Value() + 4 : 0);
        }

        static void DrawRegion(UIDrawContext& ctx, Drawable* drawable, const Rectangle& bounds)
        {
            if (drawable != nullptr)
            {
                drawable->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{42.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f, 1.0f});
            }
        }

        void DrawTabRegion(UIDrawContext& ctx, Drawable* drawable, const Rectangle& bounds)
        {
            if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(drawable))
            {
                const vg::CornerRadii saved = rrd->Radii;
                rrd->Radii = MaskRadiiForTab(saved);
                rrd->Draw(ctx, bounds);
                rrd->Radii = saved;
            }
            else if (drawable != nullptr)
            {
                drawable->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{42.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f, 1.0f});
            }
        }

        void DrawContentRegion(UIDrawContext& ctx, Drawable* drawable, const Rectangle& bounds)
        {
            if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(drawable))
            {
                const vg::CornerRadii saved = rrd->Radii;
                rrd->Radii = MaskRadiiForContent(saved);
                rrd->Draw(ctx, bounds);
                rrd->Radii = saved;
            }
            else if (drawable != nullptr)
            {
                drawable->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{42.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f, 1.0f});
            }
        }

        [[nodiscard]] vg::CornerRadii MaskRadiiForTab(vg::CornerRadii r) const
        {
            switch (Placement.Value())
            {
            case TabPlacement::Top:
                return vg::CornerRadii{r.topLeft, r.topRight, 0, 0};
            case TabPlacement::Bottom:
                return vg::CornerRadii{0, 0, r.bottomRight, r.bottomLeft};
            case TabPlacement::Left:
                return vg::CornerRadii{r.topLeft, 0, 0, r.bottomLeft};
            case TabPlacement::Right:
                return vg::CornerRadii{0, r.topRight, r.bottomRight, 0};
            }
            return r;
        }

        [[nodiscard]] vg::CornerRadii MaskRadiiForContent(vg::CornerRadii r) const
        {
            switch (Placement.Value())
            {
            case TabPlacement::Top:
                return vg::CornerRadii{0, 0, r.bottomRight, r.bottomLeft};
            case TabPlacement::Bottom:
                return vg::CornerRadii{r.topLeft, r.topRight, 0, 0};
            case TabPlacement::Left:
                return vg::CornerRadii{0, r.topRight, r.bottomRight, 0};
            case TabPlacement::Right:
                return vg::CornerRadii{r.topLeft, 0, 0, r.bottomLeft};
            }
            return r;
        }

        Array<TabItem> m_tabs;
        i32 m_selectedIndex = -1;
        i32 m_hoveredTabIndex = -1;
        Array<Rectangle> m_tabRects;
        f32 m_tabScroll =
            0.0f; ///< Strip scroll along the main axis (0 = start); clamped in RebuildTabRects.
        bool m_tabOverflow =
            false; ///< Strip longer than the view along the main axis (set in RebuildTabRects).
        bool m_scrollSelectedIntoView =
            false; ///< Selection changed; bring the new tab into view on the next rebuild.
    };

    DRACONIC_DEFINE_OBJECT(TabView, "draconic::ui")
}

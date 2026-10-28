// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI.Toolkit - :floating_panel partition.
//
// A floating panel that hosts arbitrary content OVER another view (NOT an OS window and NOT
// docking-managed - that is the DockableWindow / IDockableWindowHost world). A self-contained tool
// window: a themed title bar (an Expander-style collapse chevron + title + a DockablePanel-style
// close X, all hand-drawn for consistency with those controls - no IconButtons), a content region,
// and a bottom-right resize grip. Corners follow the theme's CornerRadius (rounded in the editor's
// rounded theme, square in flat themes). Resize follows SplitView's pattern: the size is one
// authoritative field mapped ABSOLUTELY from the pointer (no delta accumulation / no measured-size
// read-back), so there is no jitter. Content fills the body, so resizing resizes the content.
//
// Positioning: the panel must be hosted in an AbsoluteLayout; a header drag moves it by setting its
// LayoutStyle Left/Top (NOT a render transform - ScreenToLocal ignores transforms, which would
// offset every later hit-test; and NOT a margin - a margin box inflates the parent's measure).
// Clamped to the parent so it can't leave it (a sibling pane would draw over it and steal input).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:floating_panel;

import foundation.core;
import foundation.vg;
import foundation.ui;
import foundation.fonts;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    class FloatingPanel : public ViewGroup
    {
        RTTI_OBJECT(FloatingPanel, ViewGroup)
    public:
        explicit FloatingPanel(StringView title) : m_title(title) {}

        /// Replace the body content (below the title bar). Null clears it.
        void SetContent(RefPtr<View> content)
        {
            if (m_content)
            {
                RemoveView(m_content.Get(), true);
            }
            m_content = Move(content);
            if (m_content)
            {
                AddView(m_content.Get());
                m_content->Visibility = m_collapsed ? Visibility::Gone : Visibility::Visible;
            }
            Invalidate();
        }

        void SetTitle(StringView title)
        {
            m_title = String(title);
            Invalidate();
        }

        /// The panel's initial/target size expressed as the desired CONTENT size (title bar and insets
        /// are added). Clamped to the panel minimum. Resizing overrides this afterward.
        void SetPreferredContentSize(f32 width, f32 height)
        {
            m_userW = Max(kMinWidth, width + 2.0f * kContentInset);
            m_userH = Max(kMinHeight, kHeaderHeight + height + kContentInset);
            Invalidate();
        }

        void SetCollapsed(bool collapsed)
        {
            if (m_collapsed == collapsed)
            {
                return;
            }
            m_collapsed = collapsed;
            if (m_content)
            {
                m_content->Visibility = collapsed ? Visibility::Gone : Visibility::Visible;
            }
            Invalidate();
        }
        [[nodiscard]] bool IsCollapsed() const noexcept { return m_collapsed; }

        /// Fired by the close button. The consumer decides the effect (e.g. deactivate the tool).
        Event<void()> OnClose;

    protected:
        // Authoritative size = m_userW/m_userH (clamped to the panel min and the parent's available
        // room). Content is a consequence of that size, never an input to it - the one-way flow that
        // keeps resize stable.
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 w = constraints.ConstrainWidth(Max(kMinWidth, m_userW));
            const f32 h = m_collapsed ? constraints.ConstrainHeight(kHeaderHeight)
                                      : constraints.ConstrainHeight(Max(kMinHeight, m_userH));
            if (m_content && !m_collapsed)
            {
                m_content->Measure(BoxConstraints::Tight(Max(0.0f, w - 2.0f * kContentInset),
                                                         Max(0.0f, h - kHeaderHeight - kContentInset)));
            }
            MeasuredSize = Float2{w, h};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            // Re-clamp against the CURRENT parent size (available here). If the viewport shrank - the
            // bottom dock expanded upward - pull the panel back inside; a changed X/Y needs another
            // layout pass to reposition it.
            ClampToParent(); // SetLayout marks layout damage when the position changed
            if (m_content && !m_collapsed)
            {
                m_content->Layout(kContentInset, kHeaderHeight, Max(0.0f, width - 2.0f * kContentInset),
                                  Max(0.0f, height - kHeaderHeight - kContentInset));
            }
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 h = Height();
            const f32 radius = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);

            // Body: themed rounded fill + border (theme CornerRadius; square in flat themes).
            const Color fill =
                ResolveStyleColor(StyleProperty::Background, Color{0.149f, 0.157f, 0.196f, 1.0f});
            const Color border =
                ResolveStyleColor(StyleProperty::BorderColor, Color{0.255f, 0.275f, 0.333f, 1.0f});
            ctx.VG().FillRoundedRect(Rectangle{0, 0, w, h}, radius, fill);

            // Title bar: rounded only at the top so it meets the body's top corners cleanly.
            const f32 headerH = m_collapsed ? h : kHeaderHeight;
            const Color headerFill = ResolvePartColor(u8"header", StyleProperty::Background,
                                                      ControlState::Normal, DarkenedFill(fill));
            ctx.VG().FillRoundedRect(Rectangle{0, 0, w, headerH},
                                     vg::CornerRadii(radius, radius, 0.0f, 0.0f), headerFill);

            // Collapse chevron (Expander idiom: down = expanded, right = collapsed).
            const Color chevronColor = ResolvePartColor(u8"chevron", StyleProperty::TextColor,
                                                        ControlState::Normal, border);
            DrawChevron(ctx, chevronColor);

            // Title text between the chevron and the close box.
            if (ctx.FontService() != nullptr)
            {
                const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 12.0f);
                if (fonts::CachedFont* font = ctx.FontService()->GetFont(fontSize))
                {
                    const Color textColor =
                        ResolveStyleColor(StyleProperty::TextColor, Color{0.863f, 0.882f, 0.922f, 1.0f});
                    const f32 textX = kChevronBoxW;
                    const f32 textW = Max(0.0f, w - textX - kCloseBoxW);
                    ctx.VG().DrawText(m_title.AsView(), font, Rectangle{textX, 0, textW, kHeaderHeight},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }

            // Close X (DockablePanel idiom): two strokes; hover -> error color.
            const Color closeColor = ResolvePartColor(
                u8"close-button", StyleProperty::TextColor,
                m_closeHover ? ControlState::Hover : ControlState::Normal, chevronColor);
            const f32 ccx = w - kCloseBoxW * 0.5f;
            const f32 ccy = kHeaderHeight * 0.5f;
            const f32 cs = 4.0f;
            ctx.VG().DrawLine(Float2{ccx - cs, ccy - cs}, Float2{ccx + cs, ccy + cs}, closeColor, 1.5f);
            ctx.VG().DrawLine(Float2{ccx + cs, ccy - cs}, Float2{ccx - cs, ccy + cs}, closeColor, 1.5f);

            // Clip the content to the body so it never spills past the panel (a narrow resize keeps
            // the content inside the frame rather than drawing over the border / outside the panel).
            if (!m_collapsed)
            {
                ctx.PushClip(Rectangle{kContentInset, kHeaderHeight, Max(0.0f, w - 2.0f * kContentInset),
                                       Max(0.0f, h - kHeaderHeight - kContentInset)});
                DrawChildren(ctx);
                ctx.PopClip();
            }

            // Body border on top of content edges, and the bottom-right resize grip.
            ctx.VG().DrawBorderRoundedRect(Rectangle{0, 0, w, h}, radius, border, 1.0f);
            if (!m_collapsed)
            {
                for (i32 i = 1; i <= 3; ++i)
                {
                    const f32 o = static_cast<f32>(i) * 4.0f;
                    ctx.VG().DrawLine(Float2{w - o, h - 2.0f}, Float2{w - 2.0f, h - o}, border, 1.0f);
                }
            }
        }

        // Intercept the resize band before children so the grip works even over content.
        [[nodiscard]] View* HitTest(Float2 p) override
        {
            if (Visibility == Visibility::Visible && IsInteractionEnabled && !m_collapsed &&
                p.x >= 0 && p.y >= 0 && p.x < Width() && p.y < Height())
            {
                bool right = false, bottom = false;
                if (InResizeBand(p, right, bottom))
                {
                    return this;
                }
            }
            return ViewGroup::HitTest(p);
        }

        [[nodiscard]] CursorType CursorAt(Float2 p) const override
        {
            if (!m_collapsed)
            {
                bool right = false, bottom = false;
                if (InResizeBand(p, right, bottom))
                {
                    if (right && bottom) return CursorType::SizeNWSE;
                    if (right) return CursorType::SizeWE;
                    return CursorType::SizeNS;
                }
            }
            if (p.y < kHeaderHeight)
            {
                if (InChevronBox(p) || InCloseBox(p)) return CursorType::Hand;
                return CursorType::Move; // draggable title strip
            }
            return CursorType::Default;
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            const Float2 p{e.X, e.Y};
            if (p.y < kHeaderHeight)
            {
                if (InCloseBox(p))
                {
                    OnClose.Invoke();
                    e.Handled = true;
                    return;
                }
                if (InChevronBox(p))
                {
                    SetCollapsed(!m_collapsed);
                    e.Handled = true;
                    return;
                }
            }
            bool right = false, bottom = false;
            if (!m_collapsed && InResizeBand(p, right, bottom))
            {
                m_resizing = true;
                m_resizeRight = right;
                m_resizeBottom = bottom;
                m_grabOffX = Width() - e.X; // keep the grabbed point under the corner during drag
                m_grabOffY = Height() - e.Y;
                Capture();
                e.Handled = true;
                return;
            }
            if (p.y < kHeaderHeight)
            {
                m_dragging = true;
                m_grabLocalX = e.X; // where in the panel the drag was grabbed (panel-local)
                m_grabLocalY = e.Y;
                Capture();
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_resizing)
            {
                // Top-left is fixed during a bottom-right resize, so e.X/e.Y map straight to the
                // target size (the grab offset keeps the corner under the cursor). Absolute, no drift.
                if (m_resizeRight)
                {
                    m_userW = Clamp(e.X + m_grabOffX, kMinWidth, MaxWidthInParent());
                }
                if (m_resizeBottom)
                {
                    m_userH = Clamp(e.Y + m_grabOffY, kMinHeight, MaxHeightInParent());
                }
                Invalidate();
                e.Handled = true;
                return;
            }
            if (m_dragging)
            {
                // Absolute mapping keeps the grabbed point under the cursor: new X = (mouse-in-parent)
                // - grab = (e.X + Bounds.x) - grabLocal. Using X/Y (not a margin or a render transform)
                // keeps Bounds - and thus every later hit-test coordinate - exact.
                LayoutStyle placement = Layout();
                placement.Left = e.X + Bounds.x - m_grabLocalX;
                placement.Top = e.Y + Bounds.y - m_grabLocalY;
                SetLayout(placement);
                ClampToParent();
                Invalidate();
                e.Handled = true;
                return;
            }
            // Hover feedback for the close X.
            const bool hover = e.Y < kHeaderHeight && InCloseBox(Float2{e.X, e.Y});
            if (hover != m_closeHover)
            {
                m_closeHover = hover;
                Invalidate();
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && (m_resizing || m_dragging))
            {
                m_resizing = false;
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }

        void OnMouseLeave() override
        {
            if (m_closeHover)
            {
                m_closeHover = false;
                Invalidate();
            }
        }

    private:
        static constexpr f32 kHeaderHeight = 24.0f;
        static constexpr f32 kContentInset = 6.0f;
        static constexpr f32 kResizeBand = 9.0f;   // grip stroke drawing (visual), not hit width
        static constexpr f32 kResizeCorner = 12.0f; // bottom-right corner grab square (hit)
        static constexpr f32 kChevronBoxW = 22.0f;
        static constexpr f32 kChevronX = 8.0f;
        static constexpr f32 kChevronSize = 8.0f;
        static constexpr f32 kCloseBoxW = 24.0f;
        static constexpr f32 kMinWidth = 240.0f; // wide enough for a small tool row + property fields
        static constexpr f32 kMinHeight = kHeaderHeight + 60.0f;

        void Capture()
        {
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetCapture(this);
            }
        }

        // The panel positions itself via its LayoutStyle Left/Top (it must be hosted in an
        // AbsoluteLayout). AbsoluteLayout places the child at exactly (Left, Top) with its measured
        // size, so Bounds.x/y == Left/Top and there is no margin box to inflate the parent's measure
        // or shift the hit-test coordinates.

        void DrawChevron(UIDrawContext& ctx, Color color)
        {
            const f32 cy = kHeaderHeight * 0.5f;
            ctx.VG().BeginPath();
            if (!m_collapsed)
            {
                // Down chevron (expanded).
                ctx.VG().MoveTo(kChevronX, cy - kChevronSize * 0.25f);
                ctx.VG().LineTo(kChevronX + kChevronSize * 0.5f, cy + kChevronSize * 0.25f);
                ctx.VG().LineTo(kChevronX + kChevronSize, cy - kChevronSize * 0.25f);
            }
            else
            {
                // Right chevron (collapsed).
                ctx.VG().MoveTo(kChevronX + kChevronSize * 0.25f, cy - kChevronSize * 0.5f);
                ctx.VG().LineTo(kChevronX + kChevronSize * 0.75f, cy);
                ctx.VG().LineTo(kChevronX + kChevronSize * 0.25f, cy + kChevronSize * 0.5f);
            }
            ctx.VG().Stroke(color, 2.0f);
        }

        [[nodiscard]] static Color DarkenedFill(Color c)
        {
            return Color{c.r * 0.85f, c.g * 0.85f, c.b * 0.85f, c.a};
        }

        [[nodiscard]] bool InChevronBox(Float2 p) const
        {
            return p.x >= 0 && p.x < kChevronBoxW && p.y >= 0 && p.y < kHeaderHeight;
        }
        [[nodiscard]] bool InCloseBox(Float2 p) const
        {
            return p.x >= Width() - kCloseBoxW && p.x < Width() && p.y >= 0 && p.y < kHeaderHeight;
        }
        [[nodiscard]] bool InResizeBand(Float2 p, bool& right, bool& bottom) const
        {
            // The EDGE bands live in the content inset (the border area OUTSIDE the hosted
            // content), so they never eat the content's own edge - otherwise a 9px band would claim
            // the outer ~3px of a hosted PropertyGrid's scrollbar. The bottom-right
            // CORNER keeps a larger grab square (the OS-window resize-grip convention; the
            // corner of a scrollable area is dead space).
            const bool inCorner =
                p.x >= Width() - kResizeCorner && p.y >= Height() - kResizeCorner;
            right = inCorner || p.x >= Width() - kContentInset;
            bottom = inCorner || p.y >= Height() - kContentInset;
            return (right || bottom) && p.y > kHeaderHeight;
        }

        // Resize can't push the panel past the parent's right/bottom edge (Bounds.x/y track the
        // LayoutStyle Left/Top).
        [[nodiscard]] f32 MaxWidthInParent() const
        {
            if (Parent == nullptr)
            {
                return Max(kMinWidth, m_userW);
            }
            return Max(kMinWidth, Parent->Bounds.width - Bounds.x);
        }
        [[nodiscard]] f32 MaxHeightInParent() const
        {
            if (Parent == nullptr)
            {
                return Max(kMinHeight, m_userH);
            }
            return Max(kMinHeight, Parent->Bounds.height - Bounds.y);
        }

        // Keep the WHOLE panel inside its parent by clamping its LayoutStyle Left/Top
        // (Bounds.x/y track them). Clamp against the INTENDED size (not the laid-out
        // Height()/Width(), which lags a frame). AbsoluteLayout does not clamp an overflowing
        // child, so this is the only guard; run it on every layout so the panel follows a
        // shrinking viewport (e.g. the bottom dock expanding upward) instead of sliding behind it.
        void ClampToParent()
        {
            if (Parent == nullptr)
            {
                return;
            }
            const f32 intendedW = Max(kMinWidth, m_userW);
            const f32 intendedH = m_collapsed ? kHeaderHeight : Max(kMinHeight, m_userH);
            LayoutStyle placement = Layout();
            placement.Left = Clamp(placement.Left.Value(), 0.0f, Max(0.0f, Parent->Bounds.width - intendedW));
            placement.Top = Clamp(placement.Top.Value(), 0.0f, Max(0.0f, Parent->Bounds.height - intendedH));
            SetLayout(placement);
        }

        String m_title;
        RefPtr<View> m_content;

        bool m_collapsed = false;
        bool m_closeHover = false;
        f32 m_userW = 240.0f;
        f32 m_userH = kHeaderHeight + 170.0f;

        bool m_dragging = false;
        bool m_resizing = false;
        bool m_resizeRight = false;
        bool m_resizeBottom = false;
        f32 m_grabLocalX = 0.0f; // drag: grab point in panel-local coords
        f32 m_grabLocalY = 0.0f;
        f32 m_grabOffX = 0.0f;   // resize: corner offset from the grab point
        f32 m_grabOffY = 0.0f;
    };

    RTTI_DEFINE_OBJECT(FloatingPanel, "rtti::ui::toolkit")
}
